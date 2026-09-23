#!/bin/sh
#
#	OTA 配布用の firmware.bin を LCD コントローラ別に書き出す。
#
#	出力先は src/version.h の FW_VERSION から決める:
#	  tools/v<版>/firmware_ST7789.bin
#	  tools/v<版>/firmware_ILI9341.bin
#
#	あわせて web/firmware/latest.txt (版数・サイズ・MD5 の目録) を書く。
#	本体の「自動更新」(起動時チェック, src/fwupdate.cpp) は
#	  https://pokibon3.github.io/cw_decoder4/firmware/latest.txt
#	を見て新版を取りに来る。bin は複製せず、公開時に Actions が
#	latest.txt の版数を見て tools/v<版>/ から配る。
#	公開するには tools/v<版>/ と web/firmware/latest.txt を commit して
#	push すること (.github/workflows/pages.yml が拾う)。
#
#	OTA でアップロードするのはこの firmware.bin だけでよい
#	(bootloader.bin / partitions.bin は USB 書き込みでしか更新されない)。
#
#	焼くパネルと一致しない bin を入れると表示の基準面が 180° ずれ、
#	タッチの回転補正も連動して狂う。ファイル名で取り違えないようにするのが
#	このスクリプトの目的。
#
#	使い方:  sh tools/make_release.sh
#
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)

VER=$(sed -n 's/^#define FW_VERSION "\(.*\)"/\1/p' src/version.h)
if [ -z "$VER" ]; then
	echo "src/version.h から FW_VERSION を読めません" >&2
	exit 1
fi
OUT="$ROOT/tools/v$VER"

echo "=== v$VER をビルド ==="
pio run -e esp32dev -e esp32dev_ili9341

mkdir -p "$OUT"
cp "$ROOT/.pio/build/esp32dev/firmware.bin"          "$OUT/firmware_ST7789.bin"
cp "$ROOT/.pio/build/esp32dev_ili9341/firmware.bin"  "$OUT/firmware_ILI9341.bin"
chmod 644 "$OUT"/*.bin

echo
echo "=== $OUT ==="
for f in "$OUT"/firmware_*.bin; do
	# 焼き込まれたパネル指定を確認する (取り違え防止)
	case "$(basename "$f")" in
		*ST7789*)  want=ST7789 ;;
		*ILI9341*) want=ILI9341 ;;
	esac
	if strings "$f" | grep -qx "$want"; then ok="ok"; else ok="!! $want が見つからない"; fi
	printf "  %-24s %8s bytes  %s\n" "$(basename "$f")" "$(wc -c < "$f" | tr -d ' ')" "$ok"
done

# 自動更新 (起動時チェック) 用の目録を web/firmware/latest.txt に書く。
# bin は増やさない: 公開時に Actions が latest.txt の version を読んで
# tools/v<版>/ から同じものを配る (.github/workflows/pages.yml)。
# キー名は src/version.h の FW_PANEL ("ST7789"/"ILI9341") とそろえてあり、
# src/fwupdate.cpp がその名前で size / md5 を引く。
WEB="$ROOT/web/firmware"
mkdir -p "$WEB"

md5_of() {
	# macOS は md5 -q、Linux は md5sum
	if command -v md5 >/dev/null 2>&1; then
		md5 -q "$1"
	else
		md5sum "$1" | cut -d' ' -f1
	fi
}

{
	echo "version=$VER"
	echo "build=$(date '+%Y-%m-%d %H:%M:%S')"
	for p in ST7789 ILI9341; do
		f="$OUT/firmware_$p.bin"
		echo "${p}_size=$(wc -c < "$f" | tr -d ' ')"
		echo "${p}_md5=$(md5_of "$f")"
	done
} > "$WEB/latest.txt"

echo
echo "=== $WEB/latest.txt ==="
sed 's/^/  /' "$WEB/latest.txt"
echo
echo "公開するには tools/v$VER/ と web/firmware/latest.txt を commit して push すること"
