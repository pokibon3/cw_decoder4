//
//	B 案 (ゲート長の列 → テキスト) の学習・評価データを作る
//
//	合成 CW をファームと同一の WASM 中核 (DSP + デコーダ) に通し、
//	  - ゲート (filteredstate) の切り替わりから求めたマーク長・スペース長の列
//	  - 現行デコーダの出力テキスト (比較用)
//	  - 送信した正解テキスト
//	を JSONL で書き出す。ML はゲート以降だけを置き換えるので、入力の揺れ
//	(ノイズブランカ・しきい値由来の欠け/偽マーク) は実物の前段で作る。
//
//	使い方:
//	  node web/tools/ml/gen.mjs synth <件数> <シード> > out.jsonl
//	  node web/tools/ml/gen.mjs audio <音源> <正解txt|-> > out.jsonl
//
//	入力 BPF は実機を再現する 760Hz / Q=2.2 (合成・録音とも)
//
import { readFileSync, existsSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const FS = 8000;
const HOP = 30;                         // DSP_HOP。1 ブロックずつ流して時刻を正確に取る
const BPF_F0 = 760, BPF_Q = 2.2;

export const MORSE = {
	A: '.-', B: '-...', C: '-.-.', D: '-..', E: '.', F: '..-.', G: '--.',
	H: '....', I: '..', J: '.---', K: '-.-', L: '.-..', M: '--', N: '-.',
	O: '---', P: '.--.', Q: '--.-', R: '.-.', S: '...', T: '-', U: '..-',
	V: '...-', W: '.--', X: '-..-', Y: '-.--', Z: '--..',
	0: '-----', 1: '.----', 2: '..---', 3: '...--', 4: '....-',
	5: '.....', 6: '-....', 7: '--...', 8: '---..', 9: '----.',
	'/': '-..-.', '?': '..--..', ',': '--..--', '.': '.-.-.-',
};

//==================================================================
//	乱数 (mulberry32)
//==================================================================
let rs = 1;
const seed = (s) => { rs = s >>> 0; };
function rnd() {
	rs = (rs + 0x6D2B79F5) >>> 0;
	let t = rs;
	t = Math.imul(t ^ (t >>> 15), t | 1);
	t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
	return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
}
const U = (a, b) => a + (b - a) * rnd();
const pick = (a) => a[Math.floor(rnd() * a.length)];
function gauss() {
	const u1 = rnd() || 1e-12, u2 = rnd();
	return Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
}

//==================================================================
//	送信テキスト
//==================================================================
//	QSO の定型・コールサイン・英単語・ランダム文字列を混ぜる。モデルが
//	語彙を丸暗記して未知のコールサインを読み違えないよう、ランダム列を多めに
const QSO = ('CQ DE K BK TU 73 GM GA GE FB OM OP NAME QTH RST UR 5NN 599 579 559 ' +
	'R ES HR PSE AGN QRZ QSL TNX FER CALL WX RIG ANT PWR W DR SRI QRM QSB QRN ' +
	'HW CPY CUL GL SK KN AR BT 73 TEST POTA SOTA JA JH JR 5W 100W DIPOLE YAGI ' +
	'TKS INFO OK SOLID NR QRP QRO').split(' ');
const WORDS = ('THE OF AND TO IN IS YOU THAT IT HE WAS FOR ON ARE AS WITH HIS THEY ' +
	'AT BE THIS HAVE FROM OR ONE HAD BY WORD BUT NOT WHAT ALL WERE WE WHEN YOUR ' +
	'CAN SAID THERE USE EACH WHICH SHE DO HOW THEIR IF WILL UP OTHER ABOUT OUT ' +
	'MANY THEN THEM THESE SO SOME HER WOULD MAKE LIKE HIM INTO TIME HAS LOOK TWO ' +
	'MORE WRITE GO SEE NUMBER NO WAY COULD PEOPLE MY THAN FIRST WATER BEEN CALL ' +
	'WHO OIL ITS NOW FIND LONG DOWN DAY DID GET COME MADE MAY PART OVER NEW SOUND ' +
	'TAKE ONLY LITTLE WORK KNOW PLACE YEAR LIVE ME BACK GIVE MOST VERY AFTER THING ' +
	'OUR JUST NAME GOOD SENTENCE MAN THINK SAY GREAT WHERE HELP THROUGH MUCH ' +
	'BEFORE LINE RIGHT TOO MEAN OLD ANY SAME TELL BOY FOLLOW CAME WANT SHOW ALSO ' +
	'AROUND FORM THREE SMALL SET PUT END DOES ANOTHER WELL LARGE MUST BIG EVEN ' +
	'SUCH BECAUSE TURN HERE WHY ASK WENT MEN READ NEED LAND DIFFERENT HOME MOVE ' +
	'TRY KIND HAND PICTURE AGAIN CHANGE OFF PLAY SPELL AIR AWAY ANIMAL HOUSE POINT ' +
	'PAGE LETTER MOTHER ANSWER FOUND STUDY STILL LEARN SHOULD AMERICA WORLD RADIO ' +
	'STATION SIGNAL ANTENNA WEATHER SUNNY CLOUDY RAIN COLD WARM').split(' ');
const CHARS = Object.keys(MORSE);
const LETTERS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ';
const DIGITS = '0123456789';

function callsign() {
	const pfx = pick(['JA', 'JH', 'JR', 'JE', 'JF', 'JG', 'JI', 'JJ', 'JK', 'JL', 'JM', 'JN',
		'7K', '7L', '8J', 'W', 'K', 'N', 'AA', 'AI', 'KD', 'VE', 'DL', 'G', 'F', 'VK', 'ZL',
		'BV', 'HL', 'UA', 'EA', 'I', 'PY', 'LU', 'KH6', 'VR2', 'BY']);
	let s = pfx + pick(DIGITS);
	const n = 1 + Math.floor(rnd() * 3);
	for (let i = 0; i < n; i++) s += pick(LETTERS);
	if (rnd() < 0.08) s = pick(['', 'JA1/', 'W6/']) + s + pick(['/P', '/M', '/QRP', '/1']);
	return s;
}

function randomWord() {
	const n = 1 + Math.floor(rnd() * 6);
	let s = '';
	for (let i = 0; i < n; i++) s += pick(CHARS);
	return s;
}

export function makeText() {
	const words = [];
	const style = rnd();
	const n = 4 + Math.floor(rnd() * 10);
	for (let i = 0; i < n; i++) {
		const r = rnd();
		if (style < 0.35) {                     // QSO 風
			words.push(r < 0.35 ? callsign() : r < 0.85 ? pick(QSO) : randomWord());
		} else if (style < 0.6) {               // 英文
			words.push(r < 0.9 ? pick(WORDS) : randomWord());
		} else {                                // ランダム
			words.push(r < 0.75 ? randomWord() : r < 0.9 ? callsign() : pick(QSO));
		}
	}
	// 送信の合間 (局の切り替わり) を '|' で入れる。ここはノイズだけが続く
	// 区間になり、正解は空白 1 つ。ノイズ区間で文字を出さないことを覚えさせる
	if (rnd() < 0.3) {
		const k = 1 + Math.floor(rnd() * (words.length - 1));
		words.splice(k, 0, '|');
	}
	return words.join(' ');
}

//==================================================================
//	キーイング (送り手のばらつき)
//==================================================================
export function makeStyle() {
	const s = {
		wpm: U(10, 42),
		dash: U(2.5, 3.8),              // 長点 / 短点
		weight: rnd() < 0.5 ? U(0.85, 1.15) : U(0.65, 1.9),   // 符号内 / 短点
		charGap: U(2.4, 4.0),
		wordGap: U(4.5, 9.5),
		farns: rnd() < 0.2 ? U(1.2, 2.5) : 1.0,                // 文字間・語間だけ伸ばす
		jitter: rnd() < 0.4 ? U(0, 0.03) : U(0.03, 0.18),     // 手送りの揺れ (σ, 単位長比)
		drift: rnd() < 0.3 ? U(0.02, 0.12) : 0,              // 速度の揺れ
		snr: rnd() < 0.15 ? U(15, 35) : U(-6, 15),
		tone: U(500, 950),
		amp: Math.exp(U(Math.log(0.04), Math.log(0.9))),
		qsb: rnd() < 0.2 ? { depth: U(3, 18), period: U(1.5, 8) } : null,
		qrn: rnd() < 0.2 ? U(0.5, 6) : 0,                     // 毎秒のクラッシュ数
	};
	return s;
}

//	テキスト → [on, 秒] の列
function keying(text, st) {
	let unit = 1.2 / st.wpm;
	const base = unit;
	const keys = [[0, rnd() < 0.3 ? U(2, 10) : U(0.3, 1.2)]];
	let first = true;
	const jit = (d) => Math.max(0.15 * base, d + st.jitter * base * gauss());
	for (const ch of text) {
		if (st.drift) unit = Math.min(base * 1.4, Math.max(base * 0.7,
			unit + base * st.drift * 0.3 * gauss()));
		if (ch === '|') { keys.push([0, U(2, 15)]); first = true; continue; }
		if (ch === ' ') { keys.push([0, jit(st.wordGap * st.farns * unit)]); first = true; continue; }
		const code = MORSE[ch];
		if (!code) continue;
		if (!first) keys.push([0, jit(st.charGap * st.farns * unit)]);
		first = false;
		[...code].forEach((el, i) => {
			if (i) keys.push([0, jit(st.weight * unit)]);
			keys.push([1, jit(el === '.' ? unit : st.dash * unit)]);
		});
	}
	keys.push([0, st.noiseOnly || 1.5]);
	return keys;
}

//	RBJ 定ピークゲイン BPF (ffmpeg の bandpass と同じ)
function bandpass(x, f0, q) {
	const w = 2 * Math.PI * f0 / FS, al = Math.sin(w) / (2 * q), a0 = 1 + al;
	const b0 = al / a0, b2 = -al / a0, a1 = -2 * Math.cos(w) / a0, a2 = (1 - al) / a0;
	const y = new Float64Array(x.length);
	let x1 = 0, x2 = 0, y1 = 0, y2 = 0;
	for (let i = 0; i < x.length; i++) {
		const v = b0 * x[i] + b2 * x2 - a1 * y1 - a2 * y2;
		x2 = x1; x1 = x[i]; y2 = y1; y1 = v; y[i] = v;
	}
	return y;
}
const toCounts = (x) => Uint16Array.from(x, (v) => Math.max(0, Math.min(4095, Math.round(2048 + v * 2048))));

export function synth(text, st) {
	const keys = keying(text, st);
	const n = Math.round(keys.reduce((a, [, d]) => a + d, 0) * FS);
	const env = new Float64Array(n);
	let pos = 0;
	for (const [on, d] of keys) {
		const len = Math.round(d * FS);
		if (on) env.fill(1, pos, Math.min(n, pos + len));
		pos += len;
	}
	const rn = Math.round(U(0.002, 0.008) * FS);      // エッジのなまり 2〜8ms
	const nr = st.noiseOnly ? Math.exp(U(Math.log(0.01), Math.log(0.5))) : st.amp / Math.pow(10, st.snr / 20);
	const x = new Float64Array(n);
	const ph = rnd() * 6.28;
	const ref = st.noiseOnly ? nr * U(1, 5) : st.amp;   // クラッシュ・ブリップの基準振幅
	// トーン状のブリップ (実際のバンドノイズでゲートが単発で開くもの)。
	// 白色ノイズだけではゲートがほとんど開かず、ノイズ区間で文字を出さない
	// ことを学べない (7MHz バンドノイズ 10 分では約 80 回開いていた)
	const blipRate = st.noiseOnly ? (rnd() < 0.85 ? U(0.05, 1.5) : 0)
		: (rnd() < 0.4 ? U(0.02, 0.6) : 0);
	const blips = [];
	for (let t = 0; blipRate && (t += -Math.log(rnd() || 1e-9) / blipRate) < n / FS;) {
		blips.push({ s: Math.round(t * FS), len: Math.round(U(0.008, 0.12) * FS),
			a: ref * U(0.3, 1.5), f: st.tone + U(-150, 150) });
	}
	for (const b of blips) {
		for (let k = 0; k < b.len && b.s + k < n; k++) {
			const w = Math.sin(Math.PI * k / b.len);
			x[b.s + k] += b.a * w * Math.sin(2 * Math.PI * b.f * k / FS);
		}
	}
	let acc = 0, crash = 0;
	for (let i = 0; i < n; i++) {
		acc += env[i] - (i >= rn ? env[i - rn] : 0);
		let a = st.amp * (acc / rn);
		if (st.qsb) a *= Math.pow(10, -st.qsb.depth / 20 * (0.5 + 0.5 * Math.sin(2 * Math.PI * i / FS / st.qsb.period + ph)));
		if (st.qrn && rnd() < st.qrn / FS) crash = ref * U(1, 6);
		crash *= 0.997;
		x[i] += a * Math.sin(2 * Math.PI * st.tone * i / FS) + (nr + crash) * gauss();
	}
	return toCounts(bandpass(x, BPF_F0, BPF_Q));
}

//==================================================================
//	WASM 中核
//==================================================================
export async function createCore() {
	const bytes = readFileSync(join(HERE, '..', '..', 'cwcore.wasm'));
	const { instance } = await WebAssembly.instantiate(bytes, { env: { cos: Math.cos, sin: Math.sin } });
	const e = instance.exports;
	e.__wasm_call_ctors?.();
	e.cw_init();
	const mem = () => e.memory.buffer;
	return {
		e,
		//	1 ブロックずつ流し、ゲートの切り替わり時刻 (ms) と文字を集める。
		//	要素ログの ms ではなく切り替わり時刻の差を使うのは、デコーダが
		//	短すぎるマークでスペースを分断しない (前後をつないで測る) ため。
		//	ML にはゲートの生の並びを渡したい
		run(samples) {
			e.cw_reset();
			e.cw_set_tone(5);                   // AUTO
			e.cw_set_det(4);                    // 実機既定 (NR ON = 方式 E)
			const cap = e.cw_in_cap(), ptr = e.cw_in_ptr();
			const stride = e.cw_elem_stride();
			const trans = [];                   // [mark(切り替わり後の状態), t_ms]
			let text = '';
			for (let off = 0; off < samples.length; off += HOP) {
				const n = Math.min(HOP, samples.length - off);
				new Uint16Array(mem(), ptr, n).set(samples.subarray(off, off + n));
				e.cw_push(n);
				e.cw_run();
				const cnt = e.cw_elem_count();
				if (cnt) {
					const dv = new DataView(mem(), e.cw_elem_ptr(), cnt * stride);
					for (let i = 0; i < cnt; i++) {
						const mark = dv.getUint8(i * stride + 12);
						const t = dv.getUint32(i * stride + 8, true);
						// mark=1 のログはマーク終了 (→ OFF)、0 はスペース終了 (→ ON)
						trans.push([mark ? 0 : 1, t]);
					}
					e.cw_elem_clear();
				}
				if ((off / HOP) % 34 === 0) text += drain();
			}
			text += drain();
			const end = Math.round(samples.length * 1000 / FS);
			// 切り替わり時刻 → [mark, ms] の列 (先頭の無音は捨て、末尾は終端まで)
			const seq = [];
			for (let i = 1; i < trans.length; i++) {
				seq.push([trans[i - 1][0], trans[i][1] - trans[i - 1][1]]);
			}
			if (trans.length) seq.push([trans[trans.length - 1][0], end - trans[trans.length - 1][1]]);
			return { seq, base: text.replace(/\s+/g, ' ').trim() };
		},
	};
	function drain() {
		e.cw_poll(0xFFFFFFFF, 0);
		const st = new Uint32Array(mem(), e.cw_status_ptr(), 18);
		const n = st[4];
		const raw = new Uint8Array(mem(), e.cw_chars_ptr(), n * 8);
		let s = '';
		for (let i = 0; i < n; i++) s += String.fromCharCode(raw[i * 8 + 4]);
		return s;
	}
}

export function ffmpegCounts(file) {
	const raw = execFileSync('ffmpeg', ['-v', 'error', '-i', file, '-map', '0:a',
		'-af', `bandpass=f=${BPF_F0}:width_type=q:w=${BPF_Q},aresample=${FS}`, '-ac', '1', '-f', 's16le', '-'],
		{ maxBuffer: 1 << 30 });
	const s = new Int16Array(raw.buffer, raw.byteOffset, raw.length >> 1);
	return Uint16Array.from(s, (v) => Math.max(0, Math.min(4095, 2048 + Math.round(v / 16))));
}

//==================================================================
//	実行
//==================================================================
if (process.argv[1] === fileURLToPath(import.meta.url)) {
	const [cmd, a1, a2] = process.argv.slice(2);
	const core = await createCore();
	if (cmd === 'synth') {
		const count = parseInt(a1, 10), s0 = parseInt(a2, 10);
		for (let k = 0; k < count; k++) {
			seed(s0 * 100003 + k);
			// 8% はノイズだけ (信号なし、20〜60 秒)。正解は空
			const st = makeStyle();
			let text = makeText();
			if (rnd() < 0.08) { text = ''; st.amp = 0; st.noiseOnly = U(20, 60); }
			const r = core.run(synth(text, st));
			text = text.replace(/ ?\| ?/g, ' ');
			process.stdout.write(JSON.stringify({ text, base: r.base, seq: r.seq, st }) + '\n');
		}
	} else if (cmd === 'audio') {
		const truth = (a2 && a2 !== '-' && existsSync(a2)) ? readFileSync(a2, 'utf8').replace(/\s+/g, ' ').trim() : null;
		const r = core.run(ffmpegCounts(a1));
		process.stdout.write(JSON.stringify({ name: a1, text: truth, base: r.base, seq: r.seq }) + '\n');
	} else {
		console.error('usage: gen.mjs synth <n> <seed> | audio <file> <truth|->');
		process.exit(2);
	}
}
