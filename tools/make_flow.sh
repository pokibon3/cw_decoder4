#!/bin/sh
#	docs/decoder.md の「全体の流れ」の図を作る。
#	docs/flow/ のフローチャート (Mermaid) をタスクごとに描き、上端をそろえて横に並べる。
#	Mermaid のまま 1 枚に並べると、列の高さが違うときに上端がそろわないため画像にしている。
#	GitHub の表示モードに合わせて 2 枚作る (配色と字の大きさは config*.json):
#	  docs/images/flow.png       明るい背景 (config.json)
#	  docs/images/flow-dark.png  黒い背景   (config-dark.json)
#
#	要: Node.js (npx)、Google Chrome、ffmpeg、sips (macOS)
#	使い方: sh tools/make_flow.sh
set -e
cd "$(dirname "$0")/.."

CHROME=${CHROME:-"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"}
MMDC=${MMDC:-"npx -y @mermaid-js/mermaid-cli@11.4.2"}
GAP=60          # 列の間 (画素、2 倍で描いた画像での値)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
printf '{"executablePath":"%s","args":["--no-sandbox"]}' "$CHROME" > "$tmp/pp.json"

#	$1 = 設定ファイル、$2 = 背景色、$3 = 出力先
make_one() {
	h=0
	for f in dsp spectrum loop; do
		PUPPETEER_SKIP_DOWNLOAD=1 $MMDC -q -p "$tmp/pp.json" -c "$1" \
			-i "docs/flow/$f.mmd" -o "$tmp/$f.png" -s 2 -b "$2"
		fh=$(sips -g pixelHeight "$tmp/$f.png" | awk '/pixelHeight/ {print $2}')
		[ "$fh" -gt "$h" ] && h=$fh
	done
	# 各列を一番高い列に合わせて下へ背景色で伸ばし (上端をそろえる)、間を空けて横に並べる
	c=$(echo "$2" | sed 's/^#/0x/')
	ffmpeg -loglevel error -y -i "$tmp/dsp.png" -i "$tmp/spectrum.png" -i "$tmp/loop.png" \
		-filter_complex "[0]pad=iw+$GAP:$h:0:0:$c[a];[1]pad=iw+$GAP:$h:0:0:$c[b];[2]pad=iw:$h:0:0:$c[c];[a][b][c]hstack=3" \
		"$3"
	echo "$3"
}

make_one docs/flow/config.json "#ffffff" docs/images/flow.png
make_one docs/flow/config-dark.json "#0d1117" docs/images/flow-dark.png
