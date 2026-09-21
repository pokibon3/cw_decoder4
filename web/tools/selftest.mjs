//
//	デコーダ回帰試験 — 合成 CW を WASM 中核に食わせてデコード結果を照合する。
//
//	ブラウザもマイクも無しで、ファームウェアと同一の DSP/デコーダを
//	そのまま走らせる。しきい値やノイズ対策をいじったときの効果を
//	SNR を振って数値で比較できる。
//
//	使い方:  node web/tools/selftest.mjs [--verbose]
//
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const SAMPLE_RATE = 8000;

//	ノイズの乱数は固定シードにする。毎回違うノイズだと結果が数%ぶれて、
//	しきい値をいじった効果なのか運なのか区別がつかなくなる。
//	--seed <n> で系列を変えられるので、複数シードで確かめたいときはそちらで
const SEED = (() => {
	const i = process.argv.indexOf('--seed');
	return i >= 0 ? (parseInt(process.argv[i + 1], 10) >>> 0) : 12345;
})();
let rngState = SEED;
function rnd() {                          // mulberry32
	rngState = (rngState + 0x6D2B79F5) >>> 0;
	let t = rngState;
	t = Math.imul(t ^ (t >>> 15), t | 1);
	t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
	return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
}

const MORSE = {
	A: '.-', B: '-...', C: '-.-.', D: '-..', E: '.', F: '..-.', G: '--.',
	H: '....', I: '..', J: '.---', K: '-.-', L: '.-..', M: '--', N: '-.',
	O: '---', P: '.--.', Q: '--.-', R: '.-.', S: '...', T: '-', U: '..-',
	V: '...-', W: '.--', X: '-..-', Y: '-.--', Z: '--..',
	0: '-----', 1: '.----', 2: '..---', 3: '...--', 4: '....-',
	5: '.....', 6: '-....', 7: '--...', 8: '---..', 9: '----.',
	'/': '-..-.', '?': '..--..', ',': '--..--', '.': '.-.-.-', '=': '-...-',
};

//==================================================================
//	CW 音声の合成
//==================================================================
//	キーイングは立ち上がり/立ち下がりを raised-cosine で 5ms なまらせる
//	(矩形キーイングはクリックで広帯域成分が出て、実際の受信より条件が悪くなる)
function synthesize(text, { wpm = 20, toneHz = 700, snrDb = 20, amp = 0.30 } = {}) {
	const unit = 1.2 / wpm;                       // 短点長 (秒)
	const rise = Math.min(0.005, unit / 4);
	const keys = [];                              // [on(bool), 秒]
	let first = true;
	for (const raw of text.toUpperCase()) {
		// 語間は 7 単位。直前の文字間 3 単位は入れないのでここで 7 を積む
		if (raw === ' ') { keys.push([false, 7 * unit]); first = true; continue; }
		const code = MORSE[raw];
		if (!code) continue;
		if (!first) keys.push([false, 3 * unit]);  // 文字間
		first = false;
		code.split('').forEach((el, i) => {
			if (i > 0) keys.push([false, unit]);   // 符号内
			keys.push([true, el === '.' ? unit : 3 * unit]);
		});
	}
	keys.unshift([false, 0.5]);
	keys.push([false, 1.0]);       // 末尾は最後の文字が確定するまで余裕を取る

	const total = keys.reduce((a, [, d]) => a + d, 0);
	const n = Math.round(total * SAMPLE_RATE);
	const env = new Float32Array(n);
	let pos = 0;
	for (const [on, dur] of keys) {
		const len = Math.round(dur * SAMPLE_RATE);
		if (on) for (let i = 0; i < len && pos + i < n; i++) env[pos + i] = 1;
		pos += len;
	}
	// raised-cosine でエッジをなまらせる
	const rn = Math.max(1, Math.round(rise * SAMPLE_RATE));
	const smooth = new Float32Array(n);
	for (let i = 0; i < n; i++) {
		let s = 0;
		for (let k = 0; k < rn; k++) s += env[Math.min(n - 1, Math.max(0, i - (rn >> 1) + k))];
		smooth[i] = s / rn;
	}

	// SNR: 帯域内ノイズではなく素の広帯域ノイズで与える (実機の耳に近い厳しさ)
	const noiseRms = amp / Math.pow(10, snrDb / 20);
	const out = new Uint16Array(n);
	const w = 2 * Math.PI * toneHz / SAMPLE_RATE;
	for (let i = 0; i < n; i++) {
		// Box-Muller
		const u1 = rnd() || 1e-9, u2 = rnd();
		const g = Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
		const x = amp * smooth[i] * Math.sin(w * i) + noiseRms * g;
		// Float ±1.0 → ADC カウント 0..4095 (中心 2048 = 1.65V バイアス相当)
		let v = Math.round(2048 + x * 2048);
		out[i] = v < 0 ? 0 : v > 4095 ? 4095 : v;
	}
	return out;
}

//==================================================================
//	WASM 中核の駆動
//==================================================================
export async function createCore(wasmPath = join(HERE, '..', 'cwcore.wasm')) {
	const bytes = readFileSync(wasmPath);
	const { instance } = await WebAssembly.instantiate(bytes, {
		env: { cos: Math.cos, sin: Math.sin },
	});
	const e = instance.exports;
	e.__wasm_call_ctors?.();
	e.cw_init();
	const mem = () => e.memory.buffer;
	return {
		exports: e,
		status: () => new Uint32Array(mem(), e.cw_status_ptr(), 18),
		statusI: () => new Int32Array(mem(), e.cw_status_ptr(), 18),
		feed(samples) {
			const cap = e.cw_in_cap();
			const ptr = e.cw_in_ptr();
			for (let off = 0; off < samples.length; off += cap) {
				const n = Math.min(cap, samples.length - off);
				new Uint16Array(mem(), ptr, n).set(samples.subarray(off, off + n));
				e.cw_push(n);
				e.cw_run();
			}
		},
		drainChars() {
			e.cw_poll(0xFFFFFFFF, 0);        // 列は要らないので取らない
			const st = new Uint32Array(mem(), e.cw_status_ptr(), 18);
			const n = st[4];
			const raw = new Uint8Array(mem(), e.cw_chars_ptr(), n * 8);
			let s = '';
			for (let i = 0; i < n; i++) s += String.fromCharCode(raw[i * 8 + 4]);
			return s;
		},
	};
}

//	編集距離ベースの文字正解率
function accuracy(exp, got) {
	const a = exp, b = got;
	const d = Array.from({ length: a.length + 1 }, (_, i) =>
		Array.from({ length: b.length + 1 }, (_, j) => (i === 0 ? j : j === 0 ? i : 0)));
	for (let i = 1; i <= a.length; i++)
		for (let j = 1; j <= b.length; j++)
			d[i][j] = Math.min(d[i - 1][j] + 1, d[i][j - 1] + 1,
			                   d[i - 1][j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
	return Math.max(0, 1 - d[a.length][b.length] / a.length);
}

//==================================================================
//	実行
//==================================================================
//	先頭に捨て打ち (ウォームアップ) を置く。デコーダは振幅しきい値の EMA と
//	速度推定が収束するまで最初の 1〜2 文字を落とすので、実運用と同じく
//	前置きのある状態で測る。採点は WARMUP を除いた部分だけで行う
const WARMUP = 'VVV ';
const TEXT = 'CQ CQ DE JA1ABC JA1ABC K';
const CASES = [
	{ wpm: 15, toneHz: 700, snrDb: 30 },
	{ wpm: 20, toneHz: 700, snrDb: 30 },
	{ wpm: 20, toneHz: 700, snrDb: 10 },
	{ wpm: 20, toneHz: 700, snrDb: 3 },
	{ wpm: 20, toneHz: 900, snrDb: 20 },
	{ wpm: 25, toneHz: 600, snrDb: 20 },
	{ wpm: 30, toneHz: 800, snrDb: 20 },
	{ wpm: 35, toneHz: 700, snrDb: 20 },
];

const verbose = process.argv.includes('--verbose');
let worst = 1;
console.log(`text: "${TEXT}"  (noise seed=${SEED})\n`);
console.log('  wpm  tone   snr | 推定wpm  正解率  デコード結果');
console.log('  ---------------- + ------------------------------------------');
for (const c of CASES) {
	const core = await createCore();
	core.exports.cw_reset();
	core.exports.cw_set_tone(5);                // AUTO (既定)
	const audio = synthesize(WARMUP + TEXT, c);
	let got = '';
	// 0.25 秒ずつ流し、そのつど文字を回収する (実機の poll と同じ粒度)
	const step = SAMPLE_RATE / 4;
	for (let off = 0; off < audio.length; off += step) {
		core.feed(audio.subarray(off, Math.min(off + step, audio.length)));
		got += core.drainChars();
	}
	got = got.replace(/\s+/g, ' ').trim();
	// ウォームアップの 1 語ぶんを落としてから採点する (先頭文字自体が
	// 化けることがあるので、文字列一致ではなく語数で切る)
	got = got.split(' ').slice(1).join(' ');
	const acc = accuracy(TEXT, got);
	if (acc < worst) worst = acc;
	const st = core.status();
	console.log(`  ${String(c.wpm).padStart(3)}  ${String(c.toneHz).padStart(4)}  ` +
		`${String(c.snrDb).padStart(3)}dB | ${String(st[5]).padStart(6)}  ` +
		`${(acc * 100).toFixed(0).padStart(5)}%  ${got}`);
	if (verbose) console.log(`        tone=${st[6]}Hz auto=${st[8]} peak=${st[9]} bw=${st[10]}`);
}
console.log(`\n最低正解率: ${(worst * 100).toFixed(0)}%`);
process.exit(worst >= 0.8 ? 0 : 1);
