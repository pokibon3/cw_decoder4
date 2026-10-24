#!/usr/bin/env python3
#
#	USB 書き込みツール (web/flasher/) が使う配布物と目録を組み立てる。
#
#	  <出力先>/firmware/v<版>/firmware_<パネル>.bin   … tools/v<版>/ の複製
#	  <出力先>/firmware/common/*.bin                   … tools/common/ の複製
#	  <出力先>/firmware/versions.json                  … 版の目録
#
#	USB 書き込みにはアプリ (firmware_*.bin) のほかにブートローダー・
#	パーティション表・boot_app0 が要る。v3.4 までの tools/v<版>/ には
#	アプリしか残していないので、それらは tools/common/ (現行ビルドの物) を使う。
#	パーティション表は v2.1 から min_spiffs.csv のまま変わっておらず、
#	v2.4 のアプリが現行のブートローダーで起動することは実機で確認した。
#	tools/v<版>/ に bootloader.bin などがあればそちらを優先する
#	(tools/make_release.sh は今後の版でそれも書き出す)。
#
#	版ごとの日付は tools/v<版>/ の bin を追加したコミットの日付 (git が要る)、
#	変更の要約は README.md の変更履歴 (「- V3.4」の項) から取る。配布しなかった版
#	(v3.3 など) の変更は、その次に配布した版の要約に含める。
#
#	使い方:
#	  python3 tools/make_versions.py _site    Pages の組み立て (pages.yml)
#	  python3 tools/make_versions.py web      手元で試すとき (sh web/serve.sh)。
#	                                          生成物は .gitignore 済み
#
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PANELS = ('ST7789', 'ILI9341')
BOOT_FILES = ('bootloader', 'partitions', 'boot_app0')


def vkey(v):
	return tuple(int(x) for x in v.split('.'))


def file_entry(src, rel, out_fw):
	dst = os.path.join(out_fw, rel)
	os.makedirs(os.path.dirname(dst), exist_ok=True)
	shutil.copyfile(src, dst)
	with open(src, 'rb') as f:
		data = f.read()
	return {'path': rel, 'size': len(data), 'md5': hashlib.md5(data).hexdigest()}


def git_date(path):
	try:
		out = subprocess.run(
			['git', 'log', '--diff-filter=A', '--format=%ad', '--date=short', '--', path],
			cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
		return out[-1] if out else None
	except (OSError, subprocess.CalledProcessError):
		return None


#	README の変更履歴を [(下限, 上限, [要約…])] で返す。「- V2.6〜V2.9 (…)」の
#	ような範囲の項は下限と上限が異なる。各項目は最初の一文だけ
def readme_notes():
	notes = {}
	try:
		with open(os.path.join(ROOT, 'README.md'), encoding='utf-8') as f:
			lines = f.read().split('\n')
	except OSError:
		return []
	head = re.compile(r'^- V(\d+(?:\.\d+)+)(?:〜V(\d+(?:\.\d+)+))?')
	cur = None
	items = []

	def flush():
		if cur is None:
			return
		lo, hi = cur
		texts = []
		for it in items:
			t = re.sub(r'`([^`]*)`', r'\1', it)
			t = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', t)
			t = re.sub(r'\*\*([^*]*)\*\*', r'\1', t)
			t = t.split('。')[0].strip()
			if t:
				texts.append(t)
		notes[(lo, hi)] = texts

	for line in lines:
		m = head.match(line)
		if m:
			flush()
			cur = (m.group(1), m.group(2) or m.group(1))
			items = []
			continue
		if cur is None:
			continue
		if line.startswith('- ') or line.startswith('#'):
			flush()
			cur = None
			continue
		if line.startswith('  - '):
			items.append(line[4:])
		elif line.startswith('    ') and items:
			items[-1] += line.strip()
	flush()

	return [(lo, hi, texts) for (lo, hi), texts in notes.items()]


#	ver の要約: 直前に配布した版 prev より新しく ver 以下の項をすべて (新しい順)。
#	範囲の項は ver を含むときも付ける (v2.6〜v2.9 は同じ項をそれぞれに出す)
def notes_for(entries, ver, prev):
	out = []
	for lo, hi, texts in sorted(entries, key=lambda e: vkey(e[1]), reverse=True):
		inside = vkey(lo) <= vkey(ver) <= vkey(hi)
		# 最古の版 (prev が無い) は自分の項だけ。それより前の履歴は配布物と関係ない
		between = prev is not None and vkey(prev) < vkey(hi) <= vkey(ver)
		if inside or between:
			out += texts
	return out


def main():
	if len(sys.argv) != 2:
		print('使い方: python3 tools/make_versions.py <出力先>', file=sys.stderr)
		sys.exit(1)
	out_fw = os.path.join(sys.argv[1], 'firmware')
	tools = os.path.join(ROOT, 'tools')
	entries = readme_notes()

	common = {}
	for name in BOOT_FILES:
		common[name] = file_entry(os.path.join(tools, 'common', name + '.bin'),
		                          'common/%s.bin' % name, out_fw)

	versions = []
	for d in sorted(os.listdir(tools)):
		m = re.match(r'^v(\d+(?:\.\d+)+)$', d)
		if not m or not os.path.isdir(os.path.join(tools, d)):
			continue
		ver = m.group(1)
		src = os.path.join(tools, d)
		panels = {}
		for p in PANELS:
			f = os.path.join(src, 'firmware_%s.bin' % p)
			if os.path.exists(f):
				panels[p] = file_entry(f, '%s/firmware_%s.bin' % (d, p), out_fw)
		if not panels:
			continue
		boot = {}
		for name in BOOT_FILES:
			f = os.path.join(src, name + '.bin')
			boot[name] = file_entry(f, '%s/%s.bin' % (d, name), out_fw) if os.path.exists(f) \
				else common[name]
		versions.append({
			'version': ver,
			'date': git_date('tools/%s/firmware_%s.bin' % (d, PANELS[0])),
			'notes': [],
			'panels': panels,
			'boot': boot,
		})

	versions.sort(key=lambda v: vkey(v['version']), reverse=True)
	for i, v in enumerate(versions):
		prev = versions[i + 1]['version'] if i + 1 < len(versions) else None
		v['notes'] = notes_for(entries, v['version'], prev)
	index = {
		'latest': versions[0]['version'] if versions else None,
		# 書き込み先アドレス (min_spiffs.csv)。nvs は「設定を初期化」で消す範囲
		'layout': {'bootloader': 0x1000, 'partitions': 0x8000, 'nvs': [0x9000, 0x5000],
		           'boot_app0': 0xE000, 'app': 0x10000},
		'versions': versions,
	}
	with open(os.path.join(out_fw, 'versions.json'), 'w', encoding='utf-8') as f:
		json.dump(index, f, ensure_ascii=False, indent=1)
	print('versions.json: %s' % ', '.join(v['version'] for v in versions))


if __name__ == '__main__':
	main()
