#
#	学習済みモデル (train.py の .pt) を C のヘッダ src/mlweights.h に書き出す
#
#	GRU の重み行列と出力層は int8 (行ごとの倍率付き) にする。float32 のまま
#	だと 64x2 で 166KB になり、ESP32 の Flash の空き (約 128KB) に入らない。
#	1 層目の入力側 (入力 2 次元) とバイアスは小さいので float のまま。
#
#	使い方:  python export.py <model.pt> > ../../../src/mlweights.h
#
import sys
import torch
from train import VOCAB


def q8(w):
	"""行ごとに最大絶対値で int8 に量子化する。(int8 の行列, 行ごとの倍率)"""
	s = w.abs().max(dim=1).values.clamp(min=1e-8) / 127.0
	q = torch.round(w / s[:, None]).clamp(-127, 127).to(torch.int8)
	return q, s


def arr(name, ctype, vals, per=16):
	vals = list(vals)
	out = [f'static const {ctype} {name}[{len(vals)}] = {{']
	for i in range(0, len(vals), per):
		chunk = vals[i:i + per]
		if ctype == 'float':
			out.append('\t' + ','.join(f'{v:.8g}f' for v in chunk) + ',')
		else:
			out.append('\t' + ','.join(str(int(v)) for v in chunk) + ',')
	out.append('};')
	return '\n'.join(out)


def main():
	ck = torch.load(sys.argv[1])
	H, L = ck['hidden'], ck['layers']
	sd = ck['state']
	V = len(VOCAB) + 1
	o = []
	o.append('//')
	o.append(f'//	ML デコーダの重み (自動生成: python web/tools/ml/export.py {sys.argv[1].split("/")[-1]})')
	o.append('//')
	o.append(f'//	GRU {H}x{L} + 出力層。入力は要素ごとの [マークなら 1, log(ms/60)]。')
	o.append('//	ゲートの並びは PyTorch と同じ r, z, n。int8 の行列は行ごとの倍率 *_s を掛けて使う')
	o.append('//')
	o.append('#pragma once')
	o.append('#include <stdint.h>')
	o.append('')
	o.append(f'#define ML_H {H}')
	o.append(f'#define ML_LAYERS {L}')
	o.append(f'#define ML_V {V}')
	# 出力ラベルを実機のデコーダが出す文字コードにする: « → 5 (ホレ)、» → 6 (ラタ)、
	# 半角カナ U+FF61〜U+FF9F → 0xA1〜0xDF、ほかは ASCII のまま
	def code(c):
		if c == '«': return 5
		if c == '»': return 6
		if 0xFF61 <= ord(c) <= 0xFF9F: return ord(c) - 0xFEC0
		assert ord(c) < 128, c
		return ord(c)
	o.append('// 出力ラベル (0 は CTC の空白)。ML_VOCAB[i - 1] がラベル i の文字コード')
	o.append('// (5 = ホレ、6 = ラタ、0xA1〜0xDF = 半角カナ。従来のデコーダと同じ)')
	o.append(arr('ML_VOCAB', 'uint8_t', [code(c) for c in VOCAB]))
	o.append('')
	o.append(arr('ml_ih0', 'float', sd['rnn.weight_ih_l0'].flatten().tolist()))
	for l in range(L):
		if l > 0:
			q, s = q8(sd[f'rnn.weight_ih_l{l}'])
			o.append(arr(f'ml_ih{l}_q', 'int8_t', q.flatten().tolist()))
			o.append(arr(f'ml_ih{l}_s', 'float', s.tolist()))
		q, s = q8(sd[f'rnn.weight_hh_l{l}'])
		o.append(arr(f'ml_hh{l}_q', 'int8_t', q.flatten().tolist()))
		o.append(arr(f'ml_hh{l}_s', 'float', s.tolist()))
		o.append(arr(f'ml_bih{l}', 'float', sd[f'rnn.bias_ih_l{l}'].tolist()))
		o.append(arr(f'ml_bhh{l}', 'float', sd[f'rnn.bias_hh_l{l}'].tolist()))
	q, s = q8(sd['out.weight'])
	o.append(arr('ml_out_q', 'int8_t', q.flatten().tolist()))
	o.append(arr('ml_out_s', 'float', s.tolist()))
	o.append(arr('ml_out_b', 'float', sd['out.bias'].tolist()))
	print('\n'.join(o))


if __name__ == '__main__':
	main()
