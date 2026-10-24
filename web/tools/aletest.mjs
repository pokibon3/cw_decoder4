//
//	トーン検出方式の比較 (ALE 評価用)
//
//	  A = 現行 (Goertzel 中心/サイドビン)
//	  B = ALE の出力を Goertzel 判定へ
//	  C = ALE の出力/誤差の電力比だけで判定 (Goertzel 不使用)
//	  D = 振幅は生入力の中心ビン、純音判定 (サイド比) は ALE の中心ビン
//	  E = 振幅は ALE の中心ビン、純音判定は生入力の中心ビン
//
//	どの入力も実機・Web 版と同じく入力 BPF (Q=3) を通してから測る。
//	BPF なしの数値は実機と別物になる (web/README.md 参照)。
//
//	使い方:  node web/tools/aletest.mjs [synth|audio|noise ...] [--modes ABC]
//	         [--ale taps,delay,mu,ron,roff]   例: --ale 128,32,0.01,2,1
//	音源 (web/tools/*.mp3) が無い項目は飛ばす。ffmpeg が要る
//
import { readFileSync, existsSync, readdirSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const FS = 8000;
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
const MODES = opt('--modes', 'ABC').split('');
const ALE = opt('--ale', null)?.split(',').map(Number);
const parts = argv.filter((a, i) => !a.startsWith('--') && !(i > 0 && argv[i - 1].startsWith('--')));
const RUN = parts.length ? parts : ['synth', 'audio', 'noise', 'ref'];  // 'level' は明示したときだけ

//==================================================================
//	中核の駆動
//==================================================================
const wasmBytes = readFileSync(process.env.WASM || join(HERE, '..', 'cwcore.wasm'));
async function createCore(mode) {
	const { instance } = await WebAssembly.instantiate(wasmBytes, { env: { cos: Math.cos, sin: Math.sin } });
	const e = instance.exports;
	e.__wasm_call_ctors?.();
	e.cw_init();
	e.cw_reset();
	e.cw_set_tone(5);
	if (ALE) e.cw_set_ale(ALE[0], ALE[1], Math.round(ALE[2] * 1e5), Math.round(ALE[3] * 100), Math.round(ALE[4] * 100));
	e.cw_set_det('ABCDE'.indexOf(mode));
	return e;
}

let lastToneHz = 0;                       // 直前の decode() で AUTO が掴んだトーン

//	counts: 0..4095 の Uint16Array。0.25 秒ずつ流して文字を回収する
async function decode(mode, counts) {
	const e = await createCore(mode);
	const mem = () => e.memory.buffer;
	const cap = e.cw_in_cap();
	let got = '';
	for (let off = 0; off < counts.length; off += FS / 4) {
		const chunk = counts.subarray(off, Math.min(off + FS / 4, counts.length));
		for (let o = 0; o < chunk.length; o += cap) {
			const n = Math.min(cap, chunk.length - o);
			new Uint16Array(mem(), e.cw_in_ptr(), n).set(chunk.subarray(o, o + n));
			e.cw_push(n);
			e.cw_run();
		}
		e.cw_poll(0xFFFFFFFF, 0);
		const n = new Uint32Array(mem(), e.cw_status_ptr(), 18)[4];
		const raw = new Uint8Array(mem(), e.cw_chars_ptr(), n * 8);
		for (let i = 0; i < n; i++) got += String.fromCharCode(raw[i * 8 + 4]);
	}
	lastToneHz = new Uint32Array(mem(), e.cw_status_ptr(), 18)[6];
	return got.replace(/\s+/g, ' ').trim();
}

//==================================================================
//	入力の用意
//==================================================================
let rng = 12345;
function rnd() {                          // mulberry32 (selftest.mjs と同じ)
	rng = (rng + 0x6D2B79F5) >>> 0;
	let t = rng;
	t = Math.imul(t ^ (t >>> 15), t | 1);
	t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
	return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
}
const gauss = () => Math.sqrt(-2 * Math.log(rnd() || 1e-9)) * Math.cos(2 * Math.PI * rnd());

const MORSE = {
	A: '.-', B: '-...', C: '-.-.', D: '-..', E: '.', F: '..-.', G: '--.',
	H: '....', I: '..', J: '.---', K: '-.-', L: '.-..', M: '--', N: '-.',
	O: '---', P: '.--.', Q: '--.-', R: '.-.', S: '...', T: '-', U: '..-',
	V: '...-', W: '.--', X: '-..-', Y: '-.--', Z: '--..',
	0: '-----', 1: '.----', 2: '..---', 3: '...--', 4: '....-',
	5: '.....', 6: '-....', 7: '--...', 8: '---..', 9: '----.', '/': '-..-.',
};

//	RBJ 定ピークゲイン BPF (WebAudio の bandpass / ffmpeg の bandpass と同じ)
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

//	SNR は素の広帯域ノイズ基準 (selftest.mjs と同じ定義)
function synth(text, { wpm = 20, tone = 700, snr = 20, amp = 0.3 }) {
	const unit = 1.2 / wpm, keys = [[0, 0.5]];
	let first = true;
	for (const ch of text) {
		if (ch === ' ') { keys.push([0, 7 * unit]); first = true; continue; }
		if (!first) keys.push([0, 3 * unit]);
		first = false;
		MORSE[ch].split('').forEach((el, i) => {
			if (i) keys.push([0, unit]);
			keys.push([1, el === '.' ? unit : 3 * unit]);
		});
	}
	keys.push([0, 1.0]);
	const n = Math.round(keys.reduce((a, [, d]) => a + d, 0) * FS);
	const env = new Float64Array(n);
	let pos = 0;
	for (const [on, d] of keys) {
		const len = Math.round(d * FS);
		if (on) env.fill(1, pos, Math.min(n, pos + len));
		pos += len;
	}
	const rn = 40;                    // 5ms の移動平均でエッジをなまらせる
	const x = new Float64Array(n);
	const nr = amp / Math.pow(10, snr / 20);
	let acc = 0;
	for (let i = 0; i < n; i++) {
		acc += env[i] - (i >= rn ? env[i - rn] : 0);
		x[i] = amp * (acc / rn) * Math.sin(2 * Math.PI * tone * i / FS) + nr * gauss();
	}
	return toCounts(bandpass(x, tone, 3));
}

function ffmpegCounts(file, f0) {
	const raw = execFileSync('ffmpeg', ['-v', 'error', '-i', file, '-map', '0:a',
		'-af', `bandpass=f=${f0}:width_type=q:w=3,aresample=${FS}`, '-ac', '1', '-f', 's16le', '-'],
		{ maxBuffer: 1 << 30 });
	const s = new Int16Array(raw.buffer, raw.byteOffset, raw.length >> 1);
	return Uint16Array.from(s, (v) => Math.max(0, Math.min(4095, 2048 + Math.round(v / 16))));
}

function accuracy(a, b) {
	let prev = Array.from({ length: b.length + 1 }, (_, j) => j);
	for (let i = 1; i <= a.length; i++) {
		const cur = [i];
		for (let j = 1; j <= b.length; j++)
			cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
		prev = cur;
	}
	return Math.max(0, 1 - prev[b.length] / a.length);
}
//	録音が原文の途中で終わる場合用: got を原文の「最も合う先頭部分」と比べる
function accuracyPrefix(truth, got) {
	let prev = Array.from({ length: got.length + 1 }, (_, j) => j);
	let best = 0;
	for (let i = 1; i <= truth.length; i++) {
		const cur = [i];
		for (let j = 1; j <= got.length; j++)
			cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (truth[i - 1] === got[j - 1] ? 0 : 1));
		prev = cur;
		best = Math.max(best, 1 - cur[got.length] / Math.max(i, got.length));
	}
	return best;
}
const ALICE = 'ALICE WAS BEGINNING TO GET VERY TIRED OF SITTING BY HER SISTER ON THE BANK, ' +
	'AND OF HAVING NOTHING TO DO: ONCE OR TWICE SHE HAD PEEPED INTO THE BOOK HER SISTER WAS ' +
	'READING, BUT IT HAD NO PICTURES OR CONVERSATIONS IN IT, AND WHAT IS THE USE OF A BOOK, ' +
	'THOUGHT ALICE WITHOUT PICTURES OR CONVERSATIONS?';
const pct = (v) => `${(v * 100).toFixed(1).padStart(5)}%`;
const head = (t) => console.log(`\n== ${t}\n   ${MODES.map((m) => `     ${m}`).join(' ')}`);

//==================================================================
//	合成: SNR / 速度スイープ (3 シード平均)
//==================================================================
const TEXT = 'CQ CQ DE JA1ABC JA1ABC PSE K THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 599 TU';
if (RUN.includes('synth')) {
	head(`合成 (BPF Q=3、文字正解率、3 シード平均)  ALE=${ALE ? ALE.join(',') : '既定'}`);
	const cases = [];
	for (const snr of [10, 0, -3, -6, -8, -10]) cases.push({ wpm: 20, tone: 700, snr });
	for (const wpm of [12, 30, 40]) cases.push({ wpm, tone: 700, snr: -3 });
	cases.push({ wpm: 20, tone: 900, snr: -3 });
	for (const c of cases) {
		const row = [];
		for (const m of MODES) {
			let sum = 0;
			for (const seed of [1, 2, 3]) {
				rng = seed * 7919;
				const got = await decode(m, synth('VVV ' + TEXT, c));
				sum += accuracy(TEXT, got.split(' ').slice(1).join(' '));
			}
			row.push(pct(sum / 3));
		}
		console.log(`   ${row.join(' ')}   ${c.wpm}WPM ${c.tone}Hz ${c.snr}dB`);
	}
}

//==================================================================
//	録音
//==================================================================
if (RUN.includes('audio')) {
	head('録音 (BPF Q=3)');
	// Alice in Wonderland の冒頭。録音は原文の途中で終わるので先頭部分と比べる
	for (const f of ['original.mp3', 'Vol20_Sig50_Noise100.mp3']) {
		const p = join(HERE, f);
		if (!existsSync(p)) continue;
		const counts = ffmpegCounts(p, 600);
		const row = [];
		for (const m of MODES) row.push(pct(accuracyPrefix(ALICE, await decode(m, counts))));
		console.log(`   ${row.join(' ')}   ${f} (Alice 冒頭との正解率)`);
	}
	// 正解文の無い音源は「1 文字語の割合」(ゴミの目安) と文字数
	for (const [f, hz] of [['FS_sample.mp3', 586], ['qsb_sample.mp3', 984], ['qrm_sample.mp3', 523], ['pota_qso.mp3', 700]]) {
		const p = join(HERE, f);
		if (!existsSync(p)) continue;
		const counts = ffmpegCounts(p, hz);
		const row = [];
		for (const m of MODES) {
			const w = (await decode(m, counts)).split(' ').filter(Boolean);
			const junk = w.filter((x) => x.length === 1).length / Math.max(1, w.length);
			row.push(`${pct(junk)}/${String(w.join('').length).padStart(4)}`);
		}
		console.log(`   ${row.join(' ')}   ${f} ${hz}Hz (1文字語の割合/文字数)`);
	}
}

//==================================================================
//	純ノイズの偽文字
//==================================================================
if (RUN.includes('noise')) {
	head('純ノイズの偽文字/分');
	rng = 99;
	const x = new Float64Array(60 * FS).map(() => 0.1 * gauss());
	const counts = toCounts(bandpass(x, 700, 3));
	const row = [];
	for (const m of MODES) row.push(String((await decode(m, counts)).replace(/ /g, '').length).padStart(6));
	console.log(`   ${row.join(' ')}   合成白色ノイズ 60 秒`);
	const p = join(HERE, 'ノイズ7MHz(約10分).m4a');
	if (existsSync(p)) {
		const c2 = ffmpegCounts(p, 700);
		const min = c2.length / FS / 60;
		const r2 = [];
		for (const m of MODES) r2.push(((await decode(m, c2)).replace(/ /g, '').length / min).toFixed(1).padStart(6));
		console.log(`   ${r2.join(' ')}   7MHz 実ノイズ ${min.toFixed(1)} 分`);
	}
}

//==================================================================
//	正解文のある録音: web/tools/<名前>.txt を <名前>.mp3 の正解として全文で採点
//==================================================================
//	BPF の中心は、一度 700Hz / Q=1 (ほぼ素通し) で流して AUTO が掴んだトーンに
//	合わせる (Web 版の既定「トーン追従」に相当)。
//	「文字のみ」は空白を除いて比べた値。語間の誤り (語の連結・分断) と
//	文字そのものの誤りを分けて見るため
if (RUN.includes('ref')) {
	head('正解文あり (全文の文字正解率 / 空白を除いた正解率)');
	for (const t of readdirSync(HERE).filter((f) => f.endsWith('.txt')).sort()) {
		const base = t.slice(0, -4);
		const media = ['.mp3', '.m4a', '.wav'].map((x) => join(HERE, base + x)).find(existsSync);
		if (!media) continue;
		const truth = readFileSync(join(HERE, t), 'utf8').toUpperCase().replace(/\s+/g, ' ').trim();
		const wide = execFileSync('ffmpeg', ['-v', 'error', '-i', media, '-map', '0:a',
			'-af', `bandpass=f=700:width_type=q:w=1,aresample=${FS}`, '-ac', '1', '-f', 's16le', '-'],
			{ maxBuffer: 1 << 30 });
		const ws = new Int16Array(wide.buffer, wide.byteOffset, wide.length >> 1);
		await decode('A', Uint16Array.from(ws, (v) => Math.max(0, Math.min(4095, 2048 + Math.round(v / 16)))));
		const hz = lastToneHz;
		const counts = ffmpegCounts(media, hz);
		const row = [];
		const outs = [];
		for (const m of MODES) {
			const got = await decode(m, counts);
			outs.push(`   ${m}: ${got}`);
			row.push(`${pct(accuracy(truth, got))}/${pct(accuracy(truth.replace(/ /g, ''), got.replace(/ /g, '')))}`);
		}
		console.log(`   ${row.join('  ')}   ${base} ${hz}Hz (${truth.length} 文字)`);
		if (argv.includes('--show')) console.log(`   正: ${truth}\n${outs.join('\n')}`);
	}
}

//==================================================================
//	入力レベル: ピーク (フルスケール ±2048 比) を振って正解率を見る
//==================================================================
//	実機の入力レベル表示 (%) はピークのフルスケール比。2 つの入力モデルで比べる:
//	  理想   … フルスケールまで線形、超えた分は切り捨て (Web 版の入力と同じ)
//	  ADC    … ESP32 ADC (12dB 減衰、1.65V バイアス) の模擬。README の実測どおり
//	           +0.55 (−5 dBFS ≈ 0.85V) から上は直線範囲外とし、そこから上を tanh で
//	           レールまで圧縮する。下側は 0V 付近の不感帯にあたる −0.9 から圧縮する。
//	           曲線の形は仮定 (実機の ADC 特性は測っていない)
function adcModel(v) {
	const up = 0.55, lo = -0.9;
	if (v > up) return up + (1 - up) * Math.tanh((v - up) / (1 - up));
	if (v < lo) return lo - (1 + lo) * Math.tanh((lo - v) / (1 + lo));
	return v;
}
function scaleTo(x, peak, model) {
	let m = 0;
	for (const v of x) if (Math.abs(v) > m) m = Math.abs(v);
	const k = peak / m;
	return Uint16Array.from(x, (v) => {
		let y = v * k;
		if (model === 'adc') y = adcModel(y);
		if (model === 'clip55') y = Math.min(y, 0.55);   // 最悪ケース: 直線範囲の上限で正側を切る
		return Math.max(0, Math.min(4095, Math.round(2048 + y * 2048)));
	});
}
function ffmpegFloat(file, f0) {
	const raw = execFileSync('ffmpeg', ['-v', 'error', '-i', file, '-map', '0:a',
		'-af', `bandpass=f=${f0}:width_type=q:w=3,aresample=${FS}`, '-ac', '1', '-f', 's16le', '-'],
		{ maxBuffer: 1 << 30 });
	const s = new Int16Array(raw.buffer, raw.byteOffset, raw.length >> 1);
	return Float64Array.from(s, (v) => v / 32768);
}
if (RUN.includes('level')) {
	const LEVELS = [0.05, 0.1, 0.2, 0.3, 0.5, 0.6, 0.7, 0.8, 1.0, 1.4];
	const srcs = [];
	for (const [base, hz] of [['Vol20_Sig50_Noise100', 596], ['pota_qso', 616]]) {
		const media = join(HERE, base + '.mp3'), t = join(HERE, base + '.txt');
		if (!existsSync(media) || !existsSync(t)) continue;
		const truth = readFileSync(t, 'utf8').toUpperCase().replace(/\s+/g, ' ').trim();
		srcs.push({ name: base, x: ffmpegFloat(media, hz), truth, prefix: false });
	}
	// 合成: BPF 後の浮動小数で作ってから振る (synth() と同じ作り方)
	for (const snr of [0, -5]) {
		rng = 7919;
		const c = synth('VVV ' + TEXT, { wpm: 20, tone: 700, snr });
		srcs.push({ name: `合成 20WPM ${snr}dB`, x: Float64Array.from(c, (v) => (v - 2048) / 2048), truth: TEXT, synth: true });
	}
	for (const model of (opt('--model', 'ideal,adc')).split(',')) {
		head(`入力レベル (ピーク %、モデル: ${{ ideal: '理想', adc: 'ESP32 ADC 模擬', clip55: '正側を +55% で切る (最悪ケース)' }[model]})`);
		console.log('   ' + ' '.repeat(26) + LEVELS.map((l) => `${Math.round(l * 100)}%`.padStart(13)).join(''));
		for (const src of srcs) {
			for (const m of MODES) {
				const row = [];
				for (const lv of LEVELS) {
					let got = await decode(m, scaleTo(src.x, lv, model));
					if (src.synth) got = got.split(' ').slice(1).join(' ');
					row.push(pct(accuracy(src.truth, got)).padStart(13));
				}
				console.log(`   ${(src.name + ' ' + m).padEnd(26)}${row.join('')}`);
			}
		}
	}
}
