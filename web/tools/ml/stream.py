#
#	ML デコーダの逐次化 (実機の動きを再現する試験)
#
#	モデルは文字を「その文字の後のスペース要素」で出す。スペースの長さは次の
#	マークが来るまで決まらないので、そのままでは送信の最後の文字が出ない。
#	そこで無音が続いている間、経過時間を仮のスペース長としてモデルに 1 歩だけ
#	通し (隠れ状態は確定しない)、出力が文字になった時点でその文字を出す。
#	無音が終わったら確定した長さで正式に 1 歩進める。仮に出した文字は二重に
#	出さない。
#
#	使い方:
#	  python stream.py <model.pt> <data.jsonl> [--step 10] [--limit N]
#	一括デコード (greedy) と逐次デコードの出力・正解との一致と、文字を出す
#	までの遅れ (スペースの始まりから、短点の何個分か) を表示する
#
import argparse, json, sys, statistics
import torch
from train import VOCAB, feats, load, load_model, norm, edit


def step(model, h, mark, ms):
	"""1 要素ぶん進め、(出力ラベル, その確率, 新しい隠れ状態) を返す"""
	lo = model(torch.tensor([feats([[mark, ms]])]), h)[0, 0].softmax(-1)
	p, l = lo.max(-1)
	return int(l), float(p), model.hn


def greedy_ids(ids):
	s, prev = [], 0
	for i in ids:
		if i != prev and i != 0:
			s.append(VOCAB[i - 1])
		prev = i
	return ''.join(s)


def stream_decode(model, seq, dt=10, max_sil=3000, pmin=0.0, hold=0, kmin=2.5):
	"""逐次デコード。(文字列, [(文字, スペース開始から出すまでの ms)], 食い違い数) を返す。
	仮の 1 歩の出力が確率 pmin 以上の同じ文字で hold ms 続いたら出す。
	経過時間が短点 kmin 個分に達するまでは仮の出力を見ない: 符号内スペースの
	長さで止まった無音は学習データに無く、出力が当てにならない (F の途中で U)。
	短点の長さは直近 12 個のマークの短い方 1/4 の値で推定する"""
	marks = []
	out, delays, conflicts = [], [], 0
	h, prev = None, 0
	with torch.no_grad():
		for mark, ms in seq:
			if mark:
				l, _, h = step(model, h, 1, ms)
				marks = (marks + [ms])[-12:]
				if l != 0 and l != prev:
					out.append(VOCAB[l - 1])
				prev = l
				continue
			# 無音: 経過時間ごとに仮の 1 歩を試す
			emitted = 0
			unit = sorted(marks)[len(marks) // 4] if marks else 60
			e = max(dt, int(kmin * unit))
			cand, since = 0, 0
			while e < min(ms, max_sil):
				l, p, _ = step(model, h, 0, e)
				if l == 0 or l == prev or p < pmin:
					cand = 0
				elif l != cand:
					cand, since = l, e
				if cand and e - since >= hold:
					emitted = l
					out.append(VOCAB[l - 1])
					delays.append((VOCAB[l - 1], e))
					break
				e += dt
			l, _, h = step(model, h, 0, ms)
			if emitted:
				if l != emitted:
					conflicts += 1
				prev = emitted
			else:
				if l != 0 and l != prev:     # 仮の 1 歩では出ず、確定して初めて出た
					out.append(VOCAB[l - 1])
					delays.append((VOCAB[l - 1], ms))
				prev = l
	return ' '.join(''.join(out).split()), delays, conflicts


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument('model')
	ap.add_argument('data')
	ap.add_argument('--step', type=int, default=10)
	ap.add_argument('--limit', type=int, default=0)
	ap.add_argument('--minsnr', type=float, default=-99)
	ap.add_argument('--pmin', type=float, default=0.0)
	ap.add_argument('--hold', type=int, default=0)
	ap.add_argument('--kmin', type=float, default=2.5)
	ap.add_argument('--quiet', action='store_true')
	a = ap.parse_args()
	m = load_model(a.model)
	m.eval()
	data = load(a.data)
	data = [d for d in data if d.get('text') and d.get('st', {}).get('snr', 99) >= a.minsnr]
	if a.limit:
		data = data[:a.limit]
	T = Eb = Eg = Es = Egs = conf = 0
	dl = []
	for d in data:
		t = norm(d['text'])
		if d.get('name', '').startswith('original'):
			t = 'ALICE WAS BEGINNING TO GET VERY TIRED OF SITTING BY HER SISTER'
		with torch.no_grad():
			g = norm(greedy_ids(m(torch.tensor([feats(d['seq'])]))[0].argmax(-1).tolist()))
		s, delays, c = stream_decode(m, d['seq'], a.step, pmin=a.pmin, hold=a.hold, kmin=a.kmin)
		s = norm(s)
		unit = 1200 / d['st']['wpm'] if 'st' in d else None
		if unit:
			dl += [ms / unit for _, ms in delays]
		T += len(t); Eb += edit(norm(d.get('base', '')), t); Eg += edit(g, t); Es += edit(s, t)
		Egs += edit(s, g); conf += c
		if 'name' in d and not a.quiet:
			print(f"{d['name'].split('/')[-1]}: 現行 {100*(1-edit(norm(d['base']), t)/len(t)):.1f}%  "
			      f"一括 {100*(1-edit(g, t)/len(t)):.1f}%  逐次 {100*(1-edit(s, t)/len(t)):.1f}%  食い違い {c}")
			print('   一括:', g[-60:])
			print('   逐次:', s[-60:])
	print(f'{len(data)} 件  CER 現行 {100*Eb/T:.1f}%  一括 {100*Eg/T:.1f}%  逐次 {100*Es/T:.1f}%  '
	      f'一括と逐次の差 {100*Egs/T:.2f}%  食い違い {conf}')
	if dl:
		q = statistics.quantiles(dl, n=10)
		print(f'文字を出すまでの遅れ (スペース開始から、短点の個数): 中央 {statistics.median(dl):.2f}  '
		      f'10% {q[0]:.2f}  90% {q[-1]:.2f}')


if __name__ == '__main__':
	main()
