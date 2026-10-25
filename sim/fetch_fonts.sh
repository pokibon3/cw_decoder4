#!/bin/sh
#
#	画面写真用 (cwsim-hd) のフォントを取ってくる。本体のビットマップフォントの
#	元になった書体そのものを使うと、字の形が本体と同じまま、なめらかに描ける。
#	どれも自由に使えるライセンス。リポジトリには入れず build/fonts/ に置く。
#
#	  IPA ゴシック / IPA P ゴシック  lgfxJapanGothic / lgfxJapanGothicP の元 (IPA フォントライセンス)
#	  Orbitron Light                 Orbitron_Light_24 / 32 の元 (SIL OFL)
#	  DejaVu Sans                    DejaVu12 / 18 / 24 の元 (Bitstream Vera 系の自由ライセンス)
#	  FreeSans Bold                  FreeSansBold24pt7b の元 (GNU FreeFont、GPL + フォント例外)
#
#	時計の時・分 (Font8) の元は Arial なので Mac の Arial をそのまま使う
#
set -eu
cd "$(dirname "$0")"
OUT=build/fonts
DL=build/fonts-dl
mkdir -p "$OUT" "$DL"

get() {     # URL 保存先
	if [ ! -s "$2" ]; then
		echo "取得: $1"
		curl -fsSL -o "$2.part" "$1"
		mv "$2.part" "$2"
	fi
}

get https://moji.or.jp/wp-content/ipafont/IPAfont/IPAfont00303.zip "$DL/ipa.zip"
get "https://github.com/theleagueof/orbitron/raw/master/Orbitron%20Light.otf" "$OUT/Orbitron-Light.otf"
get https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2 "$DL/dejavu.tar.bz2"
get https://ftp.gnu.org/gnu/freefont/freefont-ttf-20120503.zip "$DL/freefont.zip"

unzip -o -j -q "$DL/ipa.zip" '*/ipag.ttf' '*/ipagp.ttf' -d "$OUT"
tar -xjf "$DL/dejavu.tar.bz2" -C "$DL" dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf
cp "$DL/dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf" "$OUT/"
unzip -o -j -q "$DL/freefont.zip" '*/FreeSansBold.ttf' -d "$OUT"
ls -l "$OUT"
