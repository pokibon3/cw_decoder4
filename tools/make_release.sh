#!/bin/sh
#
#	OTA 配布用の firmware.bin を LCD コントローラ別に書き出す。
#
#	出力先は src/version.h の FW_VERSION から決める:
#	  tools/v<版>/firmware_ST7789.bin
#	  tools/v<版>/firmware_ILI9341.bin
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
