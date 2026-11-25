#
#	B 案: ゲート長の列 → テキスト (単方向 GRU + CTC) の学習と評価
#
#	入力は 1 要素 (マーク or スペース) ごとに [mark, log(ms/60)]。
#	単方向にしているのは実機で逐次に回すため (要素が 1 つ確定するたびに
#	GRU を 1 ステップ進め、CTC の貪欲デコードで文字を出す)。
#
#	使い方:
#	  python train.py train <data_dir> <out.pt> [--hidden 64 --layers 2 --epochs 20]
#	  python train.py eval  <data_dir> <model.pt>
#
import argparse, json, math, random, sys, time
import torch, torch.nn as nn

# CW Tester が送れる全文字 (和文を含む)。= + ( は BT / AR / KN と同じ符号で、記号 1 つとして出す
VOCAB = list("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/?,. =:;-'()\"@+!"
             # 和文: « = ホレ (実機の 5)、» = ラタ (6)、半角カナ (実機の 0xA1〜0xDF)
             "«»ｦｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝﾞﾟ､｣")
C2I = {c: i + 1 for i, c in enumerate(VOCAB)}      # 0 = CTC blank


def feats(seq):
	return [[float(m), math.log(max(1.0, min(4000.0, ms)) / 60.0)] for m, ms in seq]


def load(path, train=False):
	out = []
	for line in open(path):
		d = json.loads(line)
		# 先頭の無音 (最初のマークまで) は捨てる。残すと学習が始まらないことがある
		# (CTC が全部空白を出す段階から抜けない)。頭の短いマーク (前段の立ち上がり)
		# は gen.mjs の温め区間で出なくなったので、無音で区別する必要もない
		while d['seq'] and d['seq'][0][0] == 0:
			d['seq'] = d['seq'][1:]
		if not d['seq']:
			d['seq'] = [[0, 1000]]
		tgt = [C2I[c] for c in d['text'] if c in C2I] if d.get('text') else []
		if train and len(d['seq']) < len(tgt) + 2:
			continue                          # 前段で信号を取れていない: 学習には使えない
		d['x'] = feats(d['seq'])
		d['y'] = tgt
		out.append(d)
	return out


class Net(nn.Module):
	def __init__(self, hidden, layers):
		super().__init__()
		self.rnn = nn.GRU(2, hidden, layers, batch_first=True)
		self.out = nn.Linear(hidden, len(VOCAB) + 1)

	def forward(self, x, h0=None):
		h, hn = self.rnn(x, h0)
		self.hn = hn
		return self.out(h)


def batches(data, bs, shuffle):
	idx = sorted(range(len(data)), key=lambda i: len(data[i]['x']))
	groups = [idx[i:i + bs] for i in range(0, len(idx), bs)]
	if shuffle:
		random.shuffle(groups)
	for g in groups:
		T = max(len(data[i]['x']) for i in g)
		x = torch.zeros(len(g), T, 2)
		for k, i in enumerate(g):
			x[k, :len(data[i]['x'])] = torch.tensor(data[i]['x'])
		yield g, x


def greedy(logits, n):
	ids = logits[:n].argmax(-1).tolist()
	s, prev = [], 0
	for i in ids:
		if i != prev and i != 0:
			s.append(VOCAB[i - 1])
		prev = i
	return ' '.join(''.join(s).split())


def edit(a, b):
	d = list(range(len(b) + 1))
	for i in range(1, len(a) + 1):
		p, d[0] = d[0], i
		for j in range(1, len(b) + 1):
			p, d[j] = d[j], min(d[j] + 1, d[j - 1] + 1, p + (a[i - 1] != b[j - 1]))
	return d[len(b)]


# 現行デコーダは -...- を BT、.-.-. を AR、-.--. を KN と 2 文字で出すので、
# 比べるときは正解・ML の記号をそちらにそろえる
SAME_CODE = str.maketrans({'=': 'BT', '+': 'AR', '(': 'KN'})


def norm(s):
	return ' '.join(s.translate(SAME_CODE).split())


def decode_all(model, data):
	model.eval()
	res = [None] * len(data)
	with torch.no_grad():
		for g, x in batches(data, 256, False):
			lo = model(x)
			for k, i in enumerate(g):
				res[i] = greedy(lo[k], len(data[i]['x']))
	return res


def decode_warm(model, seq, sil=1000, n=24):
	"""長い無音 (sil ms 以上) の後は、送信の頭 n 要素を一度流して速度をつかませ、
	無音 1.5 秒を挟んでから同じ頭を流し直す (局の交代で速度が変わると先頭の文字を
	読み違えるため)。実機では頭の n 要素ぶん表示が遅れる。
	挟む無音を 3 秒以上にすると、文字間の長い送り手で最初の文字間を語間と読む
	(POTA の AI5DD が A I5DD)。1〜1.5 秒なら直り、局の交代の合成データでも差は無い"""
	model.eval()
	ids, h = [], None
	cuts = [0] + [i + 1 for i, (k, ms) in enumerate(seq) if k == 0 and ms >= sil] + [len(seq)]
	with torch.no_grad():
		for a, b in zip(cuts, cuts[1:]):
			seg = seq[a:b]
			if not seg:
				continue
			warm = seg[:n] + [[0, 1500]]
			model(torch.tensor([feats(warm)]), h)
			lo = model(torch.tensor([feats(seg)]), model.hn)
			h = model.hn
			ids += lo[0].argmax(-1).tolist()
	s, prev = [], 0
	for i in ids:
		if i != prev and i != 0:
			s.append(VOCAB[i - 1])
		prev = i
	return ' '.join(''.join(s).split())


def train(a):
	random.seed(1); torch.manual_seed(1)
	torch.set_num_threads(8)
	tr = load(f'{a.data}/train.jsonl', train=True)
	va = load(f'{a.data}/val.jsonl')
	print(f'train {len(tr)}  val {len(va)}', flush=True)
	model = Net(a.hidden, a.layers)
	print('params', sum(p.numel() for p in model.parameters()), flush=True)
	opt = torch.optim.AdamW(model.parameters(), lr=3e-3, weight_decay=1e-4)
	steps = a.epochs * math.ceil(len(tr) / a.bs)
	sched = torch.optim.lr_scheduler.OneCycleLR(opt, 3e-3, total_steps=steps, pct_start=0.1)
	ctc = nn.CTCLoss(zero_infinity=True)
	for ep in range(a.epochs):
		model.train(); t0 = time.time(); tot = 0; nb = 0
		for g, x in batches(tr, a.bs, True):
			lp = model(x).log_softmax(-1).transpose(0, 1)
			il = torch.tensor([len(tr[i]['x']) for i in g])
			tl = torch.tensor([len(tr[i]['y']) for i in g])
			tg = torch.tensor([c for i in g for c in tr[i]['y']])
			loss = ctc(lp, tg, il, tl)
			opt.zero_grad(); loss.backward()
			nn.utils.clip_grad_norm_(model.parameters(), 1.0)
			opt.step(); sched.step()
			tot += loss.item(); nb += 1
		hyp = decode_all(model, va)
		e = sum(edit(norm(d['text']), h) for d, h in zip(va, hyp))
		n = sum(len(norm(d['text'])) for d in va)
		print(f'ep {ep+1:2d} loss {tot/nb:.3f}  val CER {100*e/n:.2f}%  {time.time()-t0:.0f}s', flush=True)
		torch.save({'hidden': a.hidden, 'layers': a.layers, 'state': model.state_dict()}, a.out)


def evaluate(a):
	ck = torch.load(a.out)
	model = Net(ck['hidden'], ck['layers']); model.load_state_dict(ck['state'])
	va = load(f'{a.data}/val.jsonl')
	hyp = decode_all(model, va)

	def cer(rows, nospace=False):
		f = (lambda s: s.replace(' ', '')) if nospace else (lambda s: s)
		e_b = sum(edit(f(norm(d['text'])), f(norm(d['base']))) for d, _ in rows)
		e_m = sum(edit(f(norm(d['text'])), f(h)) for d, h in rows)
		n = sum(len(f(norm(d['text']))) for d, _ in rows)
		return 100 * e_b / max(1, n), 100 * e_m / max(1, n)

	rows = list(zip(va, hyp))
	print(f'== 合成 検証 {len(rows)} 件  CER (低いほど良い)   現行 → ML')
	b, m = cer(rows); bs, ms = cer(rows, True)
	print(f'  全体            {b:6.2f}% → {m:6.2f}%    (空白を除くと {bs:.2f}% → {ms:.2f}%)')

	def bin_by(name, key, edges):
		print(f'-- {name}')
		for lo, hi in zip(edges, edges[1:]):
			r = [(d, h) for d, h in rows if lo <= key(d['st']) < hi]
			if r:
				b, m = cer(r)
				print(f'  {lo:6.2f}〜{hi:6.2f}  n={len(r):4d}  {b:6.2f}% → {m:6.2f}%')
	bin_by('SNR (dB)', lambda s: s['snr'], [-6, -3, 0, 3, 6, 10, 15, 36])
	bin_by('重み (符号内/短点)', lambda s: s['weight'], [0.6, 0.85, 1.15, 1.4, 1.65, 1.95])
	bin_by('ジッタ σ (単位比)', lambda s: s['jitter'], [0, 0.03, 0.08, 0.13, 0.19])
	bin_by('速度 (WPM)', lambda s: s['wpm'], [10, 15, 20, 25, 30, 35, 43])
	bin_by('Farnsworth 倍率', lambda s: s['farns'], [1.0, 1.01, 2.6])
	bin_by('語間 (単位)', lambda s: s['wordGap'], [4.5, 5.5, 6.5, 8, 9.6])
	r = [(d, h) for d, h in rows if d['st']['snr'] >= 10 and d['st']['jitter'] < 0.03
	     and 0.85 <= d['st']['weight'] < 1.15 and d['st']['farns'] == 1.0 and not d['st']['qsb'] and not d['st']['qrn']]
	b, m = cer(r)
	print(f'-- 素直な条件 (SNR≥10, ジッタ<0.03, 重み≈1, QSB/QRN なし) n={len(r)}  {b:.2f}% → {m:.2f}%')

	print('\n== 実録音')
	real = load(f'{a.data}/real.jsonl')
	for d, h in zip(real, decode_all(model, real)):
		name = d['name'].split('/')[-1]
		if d['text']:
			t = norm(d['text'])
			if name == 'original.mp3':
				t = 'ALICE WAS BEGINNING TO GET VERY TIRED OF SITTING BY HER SISTER'
			hw = decode_warm(model, d['seq'])
			eb, em, ew = edit(t, norm(d['base'])), edit(t, h), edit(t, hw)
			print(f'  {name}: 正解率 {100*(1-eb/len(t)):.1f}% → {100*(1-em/len(t)):.1f}%  (頭の読み直しあり {100*(1-ew/len(t)):.1f}%)')
		else:
			mins = sum(ms for _, ms in d['seq']) / 60000
			print(f'  {name}: 文字数 {len(d["base"].replace(" ", ""))} → {len(h.replace(" ", ""))}  ({mins:.1f} 分)')
		print(f'     現行: {norm(d["base"])[:160]}')
		print(f'     ML  : {h[:160]}')
		if d['text']:
			print(f'     ML読直: {hw[:160]}')


def load_model(path):
	ck = torch.load(path)
	m = Net(ck['hidden'], ck['layers']); m.load_state_dict(ck['state'])
	return m


def grid(a):
	"""格子 (長点比 x 文字間 x 語間) と手送り検証セットで、現行と複数モデルの CER を並べる"""
	models = [(p.split('/')[-1], load_model(p)) for p in a.out.split(',')]
	head = '現行'.rjust(8) + ''.join(n[:10].rjust(12) for n, _ in models)

	def cer(rows, hyps):
		n = sum(len(norm(d['text'])) for d in rows)
		return 100 * sum(edit(norm(d['text']), h) for d, h in zip(rows, hyps)) / max(1, n)

	for name in ('val_hand.jsonl', 'val.jsonl'):
		rows = load(f'{a.data}/{name}')
		hy = [decode_all(m, rows) for _, m in models]
		print(f'== {name} ({len(rows)} 件) CER: 現行 {cer(rows, [d["base"] for d in rows]):.2f}%  ' +
		      '  '.join(f'{n} {cer(rows, h):.2f}%' for (n, _), h in zip(models, hy)))
	rows = load(f'{a.data}/grid.jsonl')
	hy = [decode_all(m, rows) for _, m in models]
	print('\n== 格子 (SNR 15dB, ジッタ 0.05, 重み 1.0, 20/30WPM)  CER %')
	print('長点比 文字間 語間 |' + head)
	keys = []
	for d in rows:
		if d['st']['grid'] not in keys: keys.append(d['st']['grid'])
	for k in keys:
		idx = [i for i, d in enumerate(rows) if d['st']['grid'] == k]
		r = [rows[i] for i in idx]
		dash, cg, wg = k.split('|')
		line = f'{dash:>5} {cg:>6} {wg:>4} |' + f'{cer(r, [d["base"] for d in r]):8.1f}'
		line += ''.join(f'{cer(r, [h[i] for i in idx]):12.1f}' for h in hy)
		print(line)


if __name__ == '__main__':
	p = argparse.ArgumentParser()
	p.add_argument('cmd'); p.add_argument('data'); p.add_argument('out')
	p.add_argument('--hidden', type=int, default=64)
	p.add_argument('--layers', type=int, default=2)
	p.add_argument('--epochs', type=int, default=20)
	p.add_argument('--bs', type=int, default=64)
	a = p.parse_args()
	{'train': train, 'eval': evaluate, 'grid': grid}[a.cmd](a)
