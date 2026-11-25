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
// 実機の入力 BPF の再現値。R3 交換後 (2026-10-28〜) の実測推定 638Hz/Q3.0。
// 交換前は 760/Q2.2 だった。CW_BPF="760,2.2" のように環境変数で差し替えられる
const [BPF_F0, BPF_Q] = (process.env.CW_BPF || '638,3.0').split(',').map(Number);

export const MORSE = {
	A: '.-', B: '-...', C: '-.-.', D: '-..', E: '.', F: '..-.', G: '--.',
	H: '....', I: '..', J: '.---', K: '-.-', L: '.-..', M: '--', N: '-.',
	O: '---', P: '.--.', Q: '--.-', R: '.-.', S: '...', T: '-', U: '..-',
	V: '...-', W: '.--', X: '-..-', Y: '-.--', Z: '--..',
	0: '-----', 1: '.----', 2: '..---', 3: '...--', 4: '....-',
	5: '.....', 6: '-....', 7: '--...', 8: '---..', 9: '----.',
	'/': '-..-.', '?': '..--..', ',': '--..--', '.': '.-.-.-',
	// CW Tester が送れる残りの記号 (web/tester/index.html の MORSE と同じ)。
	// = + ( は BT / AR / KN と同じ符号なので、正解では 1 つの記号として扱う
	'=': '-...-', ':': '---...', ';': '-.-.-.', '-': '-....-', "'": '.----.',
	'(': '-.--.', ')': '-.--.-', '"': '.-..-.', '@': '.--.-.', '+': '.-.-.', '!': '-.-.--',
};

//==================================================================
//	乱数 (mulberry32)
//==================================================================
let rs = 1;
export const seed = (s) => { rs = s >>> 0; };
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
export const QSO = ('CQ DE K BK TU 73 GM GA GE FB OM OP NAME QTH RST UR 5NN 599 579 559 ' +
	'R ES HR PSE AGN QRZ QSL TNX FER CALL WX RIG ANT PWR W DR SRI QRM QSB QRN ' +
	'HW CPY CUL GL SK KN AR BT 73 TEST POTA SOTA JA JH JR 5W 100W DIPOLE YAGI ' +
	'TKS INFO OK SOLID NR QRP QRO').split(' ');
export const WORDS = ('THE OF AND TO IN IS YOU THAT IT HE WAS FOR ON ARE AS WITH HIS THEY ' +
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
//	続けて打つ略符号 (文字間を空けずに 2 文字を 1 符号として送る)。
//	POTA 録音の UR は ..-.-. と符号内の間隔だけで送られていた。テキスト中は
//	制御文字 1 つで持ち、keying では連結符号、正解では 2 文字に戻す
//	3 つ目は正解の表記。AR / BT / KN は + / = / ( と同じ符号なので記号 1 つにする
export const PROSIGN = {
	'\x01': ['UR', '..-.-.', 'UR'], '\x02': ['AR', '.-.-.', '+'], '\x03': ['BT', '-...-', '='],
	'\x04': ['KN', '-.--.', '('], '\x05': ['SK', '...-.-', 'SK'], '\x06': ['BK', '-...-.-', 'BK'],
};
const PROSIGN_OF = Object.fromEntries(Object.entries(PROSIGN).map(([k, [w]]) => [w, k]));
export const expandProsign = (t) => t.replace(/[\x01-\x06]/g, (c) => PROSIGN[c][2]);

//==================================================================
//	和文 (CW Tester の WABUN と同じ符号)
//==================================================================
//	テキスト中は全角カナ (濁点・半濁点は ゛゜ に分けた形) と、ホレ = \x10、
//	ラタ = \x11 で持つ。正解は実機のデコーダが出すのと同じ文字にそろえる:
//	ホレ « (実機の 5 = 「)、ラタ » (6 = 」)、カナは半角カナ (JIS X 0201 の
//	0xA6〜0xDF をそのまま U+FF66〜U+FF9F に)。ー は - 、、は ､、。は ｣
export const WABUN = {
	ア: '--.--', イ: '.-', ウ: '..-', エ: '-.---', オ: '.-...',
	カ: '.-..', キ: '-.-..', ク: '...-', ケ: '-.--', コ: '----',
	サ: '-.-.-', シ: '--.-.', ス: '---.-', セ: '.---.', ソ: '---.',
	タ: '-.', チ: '..-.', ツ: '.--.', テ: '.-.--', ト: '..-..',
	ナ: '.-.', ニ: '-.-.', ヌ: '....', ネ: '--.-', ノ: '..--',
	ハ: '-...', ヒ: '--..-', フ: '--..', ヘ: '.', ホ: '-..',
	マ: '-..-', ミ: '..-.-', ム: '-', メ: '-...-', モ: '-..-.',
	ヤ: '.--', ユ: '-..--', ヨ: '--',
	ラ: '...', リ: '--.', ル: '-.--.', レ: '---', ロ: '.-.-',
	ワ: '-.-', ヲ: '.---', ン: '.-.-.',
	'゛': '..', '゜': '..--.', ー: '.--.-', '、': '.-.-.-', '。': '.-.-..',
	'\x10': '-..---', '\x11': '...-.',
};
const KANA = 'ヲ ア イ ウ エ オ ヤ ユ ヨ ツ ー ア イ ウ エ オ カ キ ク ケ コ サ シ ス セ ソ タ チ ツ テ ト ナ ニ ヌ ネ ノ ハ ヒ フ ヘ ホ マ ミ ム メ モ ヤ ユ ヨ ラ リ ル レ ロ ワ ン ゛ ゜';
// 半角カナの並び (U+FF66 ｦ から)。全角カナ → 半角カナの対応に使う
const HALF = {};
KANA.split(' ').forEach((k, i) => { if (i === 0 || i >= 11) HALF[k] = String.fromCharCode(0xFF66 + i); });  // 1〜10 は小さいカナと ｰ
Object.assign(HALF, { ー: '-', '、': '､', '。': '｣', '\x10': '«', '\x11': '»' });
export const wabunTruth = (t) => t.replace(/[\x10\x11ア-ヲン゛゜ー、。]/g, (c) => HALF[c] || c);

//	和文の語 (QSO でよく使うもの)。濁点・半濁点は分け、小さいカナは大きくして送る
const WABUN_WORDS = ('コンニチハ コンバンハ オハヨウゴザイマス アリガトウ ゴザイマシタ ヨロシク オネガイシマス ' +
	'ハジメマシテ マタ オアイシマショウ サヨウナラ ゴキゲンヨウ ナマエ ハ デス マス ワタシ コチラ ' +
	'ソチラ シグナル リポート ガ ノ ヲ ニ デ ト モ ヘ カラ マデ ヨリ リグ アンテナ ダイポール ' +
	'バーチカル ヤギ テンキ ハレ クモリ アメ ユキ カゼ サムイ アツイ キオン ド ワット ムセン ' +
	'キョク ジムショ ジタク イドウ ヤマ ウミ カワ トウキョウ オオサカ ナゴヤ サッポロ フクオカ ' +
	'センダイ ヒロシマ キョウト コウベ ヨコハマ ケン シ マチ ムラ キンジョ ゲンキ ゲンキデス ' +
	'オツカレサマ ヨク キコエマス ヨワイ ツヨイ ノイズ ガ オオイ スコシ フェージング アリ ' +
	'QSL カード オクリマス ビューロー ケイユ ジカン フン ビョウ キョウ アシタ キノウ ' +
	'コンゴトモ ゴアイサツ マデ ゼロ イチ ニ サン ヨン ゴ ロク ナナ ハチ キュウ ジュウ ' +
	'ソレデハ シツレイシマス オヤスミナサイ タノシイ ウレシイ ホンジツ ハジメテ ニカイメ').split(' ').filter((w) => !/[A-Z]/.test(w));
const SMALL = { ァ: 'ア', ィ: 'イ', ゥ: 'ウ', ェ: 'エ', ォ: 'オ', ッ: 'ツ', ャ: 'ヤ', ュ: 'ユ', ョ: 'ヨ', ヮ: 'ワ' };
//	全角カナを送れる形 (濁点を分け、小さいカナを大きく) にする
const toWabun = (w) => [...w].map((c) => SMALL[c] || c).join('').normalize('NFD')
	.replace(/\u3099/g, '゛').replace(/\u309A/g, '゜');
const WABUN_CHARS = Object.keys(WABUN).filter((c) => !/[\x10\x11]/.test(c));

//	和文の区間: ホレ + 語 (2〜8 個) + ラタ。2 割はランダムなカナ列を混ぜる
function makeWabun() {
	const n = 2 + Math.floor(rnd() * 7);
	const ws = [];
	for (let i = 0; i < n; i++) {
		if (rnd() < 0.2) {
			let w = '';
			for (let j = 1 + Math.floor(rnd() * 5); j > 0; j--) w += pick(WABUN_CHARS);
			ws.push(w);
		} else {
			ws.push(toWabun(pick(WABUN_WORDS)));
		}
	}
	if (rnd() < 0.3) ws[ws.length - 1] += pick(['、', '。']);
	return '\x10' + ws.join(' ') + '\x11';
}
const LETTERS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ';
const DIGITS = '0123456789';

export function callsign() {
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
			let w = r < 0.9 ? pick(WORDS) : randomWord();
			// 英文の句読点 (Alice の "BANK," "DO:" のように語の後ろに付く)
			if (rnd() < 0.1) w += pick([',', ',', '.', '.', ':', ';', '?', '!', "'", '"', ')']);
			if (rnd() < 0.02) w = pick(['"', "'", '(']) + w;
			words.push(w);
		} else {                                // ランダム
			words.push(r < 0.75 ? randomWord() : r < 0.9 ? callsign() : pick(QSO));
		}
		// 略符号の語 (UR/AR/BT/KN/SK/BK) は QSO 語彙に埋もれて出にくいので足す。
		// その 6 割は文字間を空けない続け打ちにする (POTA の UR が ..-.-. だった)
		if (rnd() < 0.06) words[words.length - 1] = pick(Object.keys(PROSIGN_OF));
		const w = words[words.length - 1];
		if (PROSIGN_OF[w] && rnd() < 0.6) words[words.length - 1] = PROSIGN_OF[w];
	}
	// 送信の合間 (局の切り替わり) を '|' で入れる。ここはノイズだけが続く
	// 区間になり、正解は空白 1 つ。ノイズ区間で文字を出さないことを覚えさせる
	// 合間は 4 割の文に 1〜2 か所。合間の後で局が替わることがある (synth の segs)
	const np = rnd() < 0.4 ? (rnd() < 0.3 ? 2 : 1) : 0;
	for (let p = 0; p < np && words.length > 1; p++) {
		const k = 1 + Math.floor(rnd() * (words.length - 1));
		if (words[k] !== '|' && words[k - 1] !== '|') words.splice(k, 0, '|');
	}
	// 和文の区間 (ホレ〜ラタ) を 25% の文に入れる。和文だけの文も混ぜる
	if (rnd() < 0.25) {
		const w = makeWabun();
		if (rnd() < 0.3) return w;
		words.splice(Math.floor(rnd() * (words.length + 1)), 0, w);
	}
	return words.join(' ');
}

//==================================================================
//	キーイング (送り手のばらつき)
//==================================================================
export function makeStyle(profile = 'std') {
	const s = {
		wpm: U(10, 42),
		dash: U(2.5, 3.8),              // 長点 / 短点
		weight: rnd() < 0.5 ? U(0.85, 1.15) : U(0.65, 1.9),   // 符号内 / 短点
		charGap: U(2.4, 4.0),
		wordGap: U(4.5, 9.5),
		farns: rnd() < 0.2 ? U(1.2, 2.5) : 1.0,                // 文字間・語間だけ伸ばす
		jitter: rnd() < 0.4 ? U(0, 0.03) : U(0.03, 0.18),     // 手送りの揺れ (σ, 単位長比)
		drift: rnd() < 0.3 ? U(0.02, 0.12) : 0,              // 速度の揺れ
		// ノイズは CW Tester と同じく帯域を限ったガウス雑音で、S/N はその帯域内の
		// 雑音の標準偏差に対する信号振幅の比 (A/σ)。帯域はトーン中心 600Hz・
		// 受信機の 2.5kHz (300〜2800Hz)・全帯域 (50Hz〜)
		bw: rnd() < 0.6 ? 600 : rnd() < 0.6 ? 2500 : 6000,
		snr: rnd() < 0.15 ? U(20, 35) : U(-4, 20),
		tone: U(500, 950),
		amp: Math.exp(U(Math.log(0.04), Math.log(0.9))),
		qsb: rnd() < 0.2 ? { depth: U(3, 18), period: U(1.5, 8) } : null,
		qrn: rnd() < 0.2 ? U(0.5, 6) : 0,                     // 毎秒のクラッシュ数
	};
	// 手送りプロファイル: 実運用では長点が短点の 5 倍近くまで伸び、文字間・
	// 語間は標準 (3 / 7) より詰まっていることが多い。各間隔は 1 つ下の
	// 間隔より必ず長くする (同じ長さだと人間でも区切れない)
	if (profile === 'hand') {
		s.dash = rnd() < 0.7 ? U(3.5, 5.5) : U(2.5, 3.5);
		s.weight = U(0.6, 1.6);
		s.charGap = U(Math.max(1.6, s.weight + 0.5), 3.5);
		s.wordGap = U(Math.max(3.5, s.charGap + 1.0), 7.0);
		s.farns = 1.0;
		s.jitter = U(0.03, 0.15);
	}
	return s;
}

//	テキスト → [on, 秒] の列
//	音声の頭に足す雑音だけの区間 (秒)。実機は常時動いていて前段 (しきい値・
//	トーン追従) が温まっているが、wasm は毎回冷えた状態から始まり、頭の 100ms
//	ほどで短いマークを出し、最初の文字も崩す。この区間で温めてから、その間の
//	ゲートの動きは捨てる (core.run が samples.warmMs を見る)
const WARM_S = 3;

//	st.segs があれば、合間 ('|') ごとに次の送り手 (局) のキーイングに替える。
//	各キーには何番目の局かを 3 つ目に持たせる (synth がトーンと強さを替える)
function keying(text, st0) {
	const segs = st0.segs || [st0];
	let si = 0, st = segs[0];
	let unit = 1.2 / st.wpm;
	let base = unit;
	const keys = [[0, WARM_S + (rnd() < 0.3 ? U(2, 10) : U(0.3, 1.2)), 0]];
	let first = true;
	const jit = (d) => Math.max(0.15 * base, d + st.jitter * base * gauss());
	for (const ch of text) {
		if (st.drift) unit = Math.min(base * 1.4, Math.max(base * 0.7,
			unit + base * st.drift * 0.3 * gauss()));
		if (ch === '|') {
			keys.push([0, U(2, 15), si]);
			if (si + 1 < segs.length) { st = segs[++si]; unit = base = 1.2 / st.wpm; }
			first = true;
			continue;
		}
		if (ch === ' ') { keys.push([0, jit(st.wordGap * st.farns * unit), si]); first = true; continue; }
		const code = MORSE[ch] || WABUN[ch] || (PROSIGN[ch] && PROSIGN[ch][1]);
		if (!code) continue;
		if (!first) keys.push([0, jit(st.charGap * st.farns * unit), si]);
		first = false;
		[...code].forEach((el, i) => {
			if (i) keys.push([0, jit(st.weight * unit), si]);
			keys.push([1, jit(el === '.' ? unit : st.dash * unit), si]);
		});
	}
	keys.push([0, st0.noiseOnly || 1.5, si]);
	return keys;
}

//	帯域を限ったガウス雑音 (実効値 1)。CW Tester の bandNoiseBuf と同じく、
//	周波数領域で帯域内の成分だけに乱数を置いて逆 FFT する
function bandNoise(n, bw, tone) {
	const [lo, hi] = bw >= 6000 ? [50, FS / 2] : bw === 2500 ? [300, 2800]
		: [Math.max(20, tone - bw / 2), tone + bw / 2];
	let N = 1;
	while (N < n) N <<= 1;
	const re = new Float64Array(N), im = new Float64Array(N);
	const k0 = Math.max(1, Math.ceil(lo * N / FS)), k1 = Math.min(N / 2 - 1, Math.floor(hi * N / FS));
	for (let k = k0; k <= k1; k++) {
		const g = Math.sqrt(-2 * Math.log(rnd() || 1e-12)), p = 2 * Math.PI * rnd();
		re[k] = re[N - k] = g * Math.cos(p);
		im[k] = g * Math.sin(p);
		im[N - k] = -im[k];
	}
	fft(re, im);
	let ss = 0;
	for (let i = 0; i < N; i++) ss += re[i] * re[i];
	const sc = 1 / Math.sqrt(ss / N);
	return Float64Array.from(re.subarray(0, n), (v) => v * sc);
}

function fft(re, im) {
	const n = re.length;
	for (let i = 1, j = 0; i < n; i++) {
		let bit = n >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) {
			let t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for (let len = 2; len <= n; len <<= 1) {
		const h = len >> 1;
		const wr0 = Math.cos(Math.PI / h), wi0 = Math.sin(Math.PI / h);
		for (let a0 = 0; a0 < n; a0 += len) {
			let wr = 1, wi = 0;
			for (let k = 0; k < h; k++) {
				const a = a0 + k, b = a + h;
				const tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
				re[b] = re[a] - tr; im[b] = im[a] - ti;
				re[a] += tr; im[a] += ti;
				const t = wr * wr0 - wi * wi0; wi = wr * wi0 + wi * wr0; wr = t;
			}
		}
	}
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
	const segOf = new Uint8Array(n);
	const segs = st.segs || [st];
	let pos = 0;
	for (const [on, d, si] of keys) {
		const len = Math.round(d * FS);
		if (on) env.fill(1, pos, Math.min(n, pos + len));
		if (si) segOf.fill(si, pos, Math.min(n, pos + len));
		pos += len;
	}
	const rn = Math.round(U(0.002, 0.008) * FS);      // エッジのなまり 2〜8ms
	const nr = st.noiseOnly ? Math.exp(U(Math.log(0.01), Math.log(0.5))) : st.amp / Math.pow(10, st.snr / 20);
	const noise = bandNoise(n, st.bw || 600, st.tone);
	const x = new Float64Array(n);
	const ph = rnd() * 6.28;
	const ref = st.noiseOnly ? nr * U(1, 5) : st.amp;   // クラッシュ・ブリップの基準振幅
	// トーン状のブリップ (実際のバンドノイズでゲートが単発で開くもの)。
	// 白色ノイズだけではゲートがほとんど開かず、ノイズ区間で文字を出さない
	// ことを学べない (7MHz バンドノイズ 10 分では約 80 回開いていた)。
	// 長さは 5〜40ms: ゲート長が 18〜45ms (中央 23ms) になり、実測の
	// 18〜49ms (中央 19ms) に合う。以前の 8〜120ms は短点と重なり、
	// 孤立した E を消すことを覚えさせていた (TEST が TST になる)
	const blipRate = st.noBlip ? 0 : st.noiseOnly ? (rnd() < 0.85 ? U(0.05, 1.5) : 0)
		: (rnd() < 0.4 ? U(0.02, 0.6) : 0);
	const blips = [];
	for (let t = 0; blipRate && (t += -Math.log(rnd() || 1e-9) / blipRate) < n / FS;) {
		blips.push({ s: Math.round(t * FS), len: Math.round(U(0.005, 0.04) * FS),
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
		const sg = segs[segOf[i]];
		let a = sg.amp * (acc / rn);
		if (st.qsb) a *= Math.pow(10, -st.qsb.depth / 20 * (0.5 + 0.5 * Math.sin(2 * Math.PI * i / FS / st.qsb.period + ph)));
		if (st.qrn && rnd() < st.qrn / FS) crash = ref * U(1, 6);
		crash *= 0.997;
		x[i] += a * Math.sin(2 * Math.PI * sg.tone * i / FS) + nr * noise[i] + crash * gauss();
	}
	const out = toCounts(bandpass(x, BPF_F0, BPF_Q));
	out.warmMs = WARM_S * 1000;
	return out;
}

//==================================================================
//	WASM 中核
//==================================================================
export async function createCore() {
	const bytes = readFileSync(process.env.CW_WASM || join(HERE, '..', '..', 'cwcore.wasm'));
	const { instance } = await WebAssembly.instantiate(bytes, { env: { cos: Math.cos, sin: Math.sin } });
	const e = instance.exports;
	e.__wasm_call_ctors?.();
	e.cw_init();
	const mem = () => e.memory.buffer;
	let pushed = 0;                         // 流した通算サンプル数 (wasm の時刻は cw_reset で戻らない)
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
			// 文字の出し方 (0=従来, 1=DL)。CW_ENGINE=1 で DL 方式の出力を base に入れる
			e.cw_set_engine?.(parseInt(process.env.CW_ENGINE || '0', 10));
			const cap = e.cw_in_cap(), ptr = e.cw_in_ptr();
			const stride = e.cw_elem_stride();
			const trans = [];                   // [mark(切り替わり後の状態), t_ms]
			const t0 = Math.floor(pushed * 1000 / FS);
			let text = '';
			for (let off = 0; off < samples.length; off += HOP) {
				const n = Math.min(HOP, samples.length - off);
				new Uint16Array(mem(), ptr, n).set(samples.subarray(off, off + n));
				e.cw_push(n);
				pushed += n;
				e.cw_run();
				const cnt = e.cw_elem_count();
				if (cnt) {
					const dv = new DataView(mem(), e.cw_elem_ptr(), cnt * stride);
					for (let i = 0; i < cnt; i++) {
						const mark = dv.getUint8(i * stride + 12);
						const t = dv.getUint32(i * stride + 8, true);
						// mark=1 のログはマーク終了 (→ OFF)、0 はスペース終了 (→ ON)
						trans.push([mark ? 0 : 1, t - t0]);
					}
					e.cw_elem_clear();
				}
				if ((off / HOP) % 34 === 0) text += drain();
			}
			text += drain();
			const end = Math.floor(pushed * 1000 / FS) - t0;
			// 切り替わり時刻 → [mark, ms] の列。先頭の無音 (0 からの最初の ON まで) も
			// 入れ、末尾は終端まで。以前は先頭の無音を捨てていたため、学習データの
			// 列が必ずマークで始まり、録音の頭で前段が出す短いマークと区別できなかった
			const seq = [];
			const warm = samples.warmMs || 0;
			while (trans.length && (trans[0][1] < warm || trans[0][0] !== 1)) trans.shift();
			if (trans.length) seq.push([0, trans[0][1] - warm]);
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
		for (let i = 0; i < n; i++) {
			// 和文の文字は正解と同じ表記に: 5 (ホレ) → «、6 (ラタ) → »、
			// 0xA1〜0xDF (半角カナ) → U+FF61〜
			const b = raw[i * 8 + 4];
			s += b === 5 ? '«' : b === 6 ? '»' : (b >= 0xA1 && b <= 0xDF) ? String.fromCharCode(b + 0xFEC0) : String.fromCharCode(b);
		}
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
		const profile = process.argv[5] || 'std';
		for (let k = 0; k < count; k++) {
			seed(s0 * 100003 + k);
			// 8% はノイズだけ (信号なし、20〜60 秒)。正解は空
			const st = makeStyle(profile);
			let text = makeText();
			if (rnd() < 0.08) { text = ''; st.amp = 0; st.noiseOnly = U(20, 60); }
			// 局の交代: 合間の後は 6 割で別の送り手 (速度・癖・トーン・強さ) にする。
			// 交代直後の最初の文字は速度の手がかりが無く、読み違えやすい (A→W)
			const sender = (o) => ({ wpm: o.wpm, dash: o.dash, weight: o.weight, charGap: o.charGap,
				wordGap: o.wordGap, farns: o.farns, jitter: o.jitter, drift: o.drift, tone: o.tone, amp: o.amp });
			st.segs = [sender(st)];
			for (let p = (text.match(/\|/g) || []).length; p > 0; p--) {
				const prev = st.segs[st.segs.length - 1];
				if (rnd() < 0.6) {
					const o = makeStyle(rnd() < 0.5 ? profile : (profile === 'hand' ? 'std' : 'hand'));
					o.tone = Math.max(450, Math.min(1000, st.tone + U(-150, 150)));
					o.amp = st.amp * Math.pow(10, U(-10, 10) / 20);
					st.segs.push(sender(o));
				} else {
					st.segs.push(prev);
				}
			}
			if (st.segs.length === 1) delete st.segs;
			const r = core.run(synth(text, st));
			// 合間で区切った各部分 (評価で「交代直後の先頭文字」を数えるため)
			const parts = text.split('|').map((t) => wabunTruth(expandProsign(t.trim())));
			text = wabunTruth(expandProsign(text.replace(/ ?\| ?/g, ' ')));
			process.stdout.write(JSON.stringify({ text, base: r.base, seq: r.seq, st, parts }) + '\n');
		}
	} else if (cmd === 'grid') {
		// 評価用の格子: 長点比 x 文字間 x 語間 を固定値で振る。タイミング以外の
		// 劣化要因 (QSB/QRN/速度の揺れ) は外し、SNR 15dB・ジッタ 0.05 で揃える
		const per = parseInt(a1, 10), s0 = parseInt(a2, 10);
		let k = 0;
		for (const dash of [3, 4, 5])
			for (const charGap of [3.0, 2.5, 2.0])
				for (const wordGap of [7, 5, 4])
					for (const wpm of [20, 30])
						for (let r = 0; r < per; r++) {
							seed(s0 * 100003 + k++);
							const text = makeText().replace(/ ?\| ?/g, ' ');
							const st = { wpm, dash, weight: 1.0, charGap, wordGap, farns: 1.0,
								jitter: 0.05, drift: 0, snr: 20, bw: 600, tone: 700, amp: 0.3, qsb: null, qrn: 0,
								grid: `${dash}|${charGap.toFixed(1)}|${wordGap}` };
							const res = core.run(synth(text, st));
							process.stdout.write(JSON.stringify({ text: wabunTruth(expandProsign(text)), base: res.base, seq: res.seq, st }) + '\n');
						}
	} else if (cmd === 'snr') {
		// ノイズ耐性の評価: CW Tester の既定 (トーン 600Hz、ノイズ帯域 600Hz、
		// 理想的なキーイング) で S/N だけを振る。文は全文字を含む makeText
		const per = parseInt(a1, 10), s0 = parseInt(a2, 10);
		const bw = parseInt(process.argv[5] || '600', 10);
		let k = 0;
		for (let snr = -6; snr <= 20; snr += 2)
			for (const wpm of [20, 30])
				for (let r = 0; r < per; r++) {
					seed(s0 * 100003 + k++);
					const text = makeText().replace(/ ?\| ?/g, ' ');
					const st = { wpm, dash: 3, weight: 1.0, charGap: 3, wordGap: 7, farns: 1.0,
						jitter: 0.02, drift: 0, snr, bw, tone: 600, amp: 0.3, qsb: null, qrn: 0,
						noBlip: true, snrpt: snr };
					const res = core.run(synth(text, st));
					process.stdout.write(JSON.stringify({ text: wabunTruth(expandProsign(text)), base: res.base, seq: res.seq, st }) + '\n');
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
