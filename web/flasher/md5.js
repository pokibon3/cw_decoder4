//
//	MD5 (RFC 1321)。ダウンロードした bin の照合と、esptool-js が書き込み後に
//	本体側のフラッシュと突き合わせるときの計算 (calculateMD5Hash) に使う。
//	Web Crypto は MD5 を持たないので自前で計算する。
//
const S = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
	5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
	4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
	6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21];
const K = new Uint32Array(64);
for (let i = 0; i < 64; i++) K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32) >>> 0;

//	data: Uint8Array → 16 進 32 文字
export function md5hex(data) {
	const n = data.length;
	const padded = new Uint8Array(((n + 8) >> 6) * 64 + 64);
	padded.set(data);
	padded[n] = 0x80;
	const view = new DataView(padded.buffer);
	view.setUint32(padded.length - 8, (n * 8) >>> 0, true);
	view.setUint32(padded.length - 4, Math.floor(n / 0x20000000), true);

	let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
	const M = new Uint32Array(16);
	for (let off = 0; off < padded.length; off += 64) {
		for (let i = 0; i < 16; i++) M[i] = view.getUint32(off + i * 4, true);
		let A = a0, B = b0, C = c0, D = d0;
		for (let i = 0; i < 64; i++) {
			let F, g;
			if (i < 16) { F = (B & C) | (~B & D); g = i; }
			else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
			else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) & 15; }
			else { F = C ^ (B | ~D); g = (7 * i) & 15; }
			F = (F + A + K[i] + M[g]) >>> 0;
			A = D; D = C; C = B;
			B = (B + ((F << S[i]) | (F >>> (32 - S[i])))) >>> 0;
		}
		a0 = (a0 + A) >>> 0; b0 = (b0 + B) >>> 0; c0 = (c0 + C) >>> 0; d0 = (d0 + D) >>> 0;
	}
	let hex = '';
	for (const w of [a0, b0, c0, d0]) {
		for (let i = 0; i < 4; i++) hex += ((w >>> (i * 8)) & 0xff).toString(16).padStart(2, '0');
	}
	return hex;
}
