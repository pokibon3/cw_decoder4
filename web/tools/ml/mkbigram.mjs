//
//	文字 2 連鎖のコスト表 src/bigram.h を作る
//
//	デコーダは、文字間が詰まって 1 つにつながった符号を区切り直すときに、
//	候補の文字列をこの表で採点する (decoder.cpp の split_code)。
//	コストは -log2 P(次の文字 | 前の文字) を 2 倍して整数に丸めたもの。
//	前の文字の 0 番は語の境界 (語頭)。
//
//	コーパスは gen.mjs と同じ語彙の QSO 文と英文。乱数の語は 2 連鎖を
//	平らにするだけなので入れない。
//
//	使い方:  node web/tools/ml/mkbigram.mjs > src/bigram.h
//
import { QSO, WORDS, callsign, seed, PROSIGN } from './gen.mjs';

const SYM = ' ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/?.,';
const N = SYM.length;
const LINES = 200000;
const COST_MAX = 40;

seed(20261125);
let r = 1;
const rnd = () => { r = (r * 1103515245 + 12345) >>> 0; return r / 4294967296; };
const pick = (a) => a[Math.floor(rnd() * a.length)];
const prosigns = Object.values(PROSIGN).map(([w]) => w);

const cnt = Array.from({ length: N }, () => new Float64Array(N));
for (let k = 0; k < LINES; k++) {
	const words = [];
	const qso = rnd() < 0.5;
	const n = 4 + Math.floor(rnd() * 10);
	for (let i = 0; i < n; i++) {
		const x = rnd();
		if (qso) words.push(x < 0.35 ? callsign() : x < 0.4 ? pick(prosigns) : pick(QSO));
		else words.push(pick(WORDS));
	}
	for (const w of words) {
		let prev = 0;
		for (const ch of w) {
			const j = SYM.indexOf(ch);
			if (j <= 0) { prev = 0; continue; }
			cnt[prev][j]++;
			prev = j;
		}
		cnt[prev][0]++;
	}
}

const rows = [];
for (let i = 0; i < N; i++) {
	let tot = 0;
	for (let j = 0; j < N; j++) tot += cnt[i][j] + 0.5;
	const row = [];
	for (let j = 0; j < N; j++) {
		const c = Math.round(-2 * Math.log2((cnt[i][j] + 0.5) / tot));
		row.push(Math.min(COST_MAX, c));
	}
	rows.push(row);
}

const out = [];
out.push('//');
out.push('//	文字 2 連鎖のコスト表 (自動生成: node web/tools/ml/mkbigram.mjs > src/bigram.h)');
out.push('//');
out.push('//	BIGRAM_COST[前][次] = round(-2 log2 P(次 | 前))。添字は BIGRAM_SYM の位置で、');
out.push('//	0 番は語の境界。表に無い文字は BIGRAM_COST_MAX として扱う。');
out.push('//');
out.push('#pragma once');
out.push('#include <stdint.h>');
out.push('');
out.push(`#define BIGRAM_N ${N}`);
out.push(`#define BIGRAM_COST_MAX ${COST_MAX}`);
out.push(`static const char BIGRAM_SYM[] = "${SYM}";`);
out.push('static const uint8_t BIGRAM_COST[BIGRAM_N][BIGRAM_N] = {');
for (let i = 0; i < N; i++) {
	out.push(`\t/* '${SYM[i]}' */ {${rows[i].join(',')}},`);
}
out.push('};');
process.stdout.write(out.join('\n') + '\n');
