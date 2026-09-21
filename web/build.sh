#!/bin/sh
#
#	Web 版 CW Decoder の WASM ビルド
#
#	ファームウェアと同一の src/dsp.cpp / decoder.cpp / decode.cpp /
#	lib/float_fft をそのままコンパイルし、web/wasm/ の薄い層
#	(Arduino スタブ + 最小 libc + JS 向け API) と一緒にリンクする。
#	Emscripten は使わない: 中核は STL もヒープも使わないので、
#	clang の wasm32 ターゲット + wasm-ld (Homebrew lld) だけで通る。
#
#	必要なもの:  brew install lld
#	使い方:      sh web/build.sh
#	出力:        web/cwcore.wasm (リポジトリに含める。実行に lld は不要)
#
set -eu

cd "$(dirname "$0")/.."
ROOT=$(pwd)
OUT=$ROOT/web/cwcore.wasm
TMP=${TMPDIR:-/tmp}/cwdec-wasm.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

WASM_LD=$(command -v wasm-ld || echo /opt/homebrew/opt/lld/bin/wasm-ld)
if [ ! -x "$WASM_LD" ]; then
	echo "wasm-ld が見つかりません。'brew install lld' を実行してください。" >&2
	exit 1
fi

CFLAGS="--target=wasm32 -Os -flto \
	-nostdlib -ffreestanding -fno-builtin -fno-math-errno \
	-fvisibility=hidden -fno-exceptions -fno-rtti \
	-Wall -Wno-unused-parameter \
	-I$ROOT/web/wasm/shim -I$ROOT/src -I$ROOT/lib/float_fft"

# ファームウェアと共有する中核 + Web 側の薄い層
SRCS="$ROOT/src/dsp.cpp $ROOT/src/decoder.cpp $ROOT/src/decode.cpp \
	$ROOT/lib/float_fft/float_fft.cpp \
	$ROOT/web/wasm/api.cpp $ROOT/web/wasm/support.c"

OBJS=""
for f in $SRCS; do
	o=$TMP/$(basename "$f").o
	# shellcheck disable=SC2086
	clang $CFLAGS -c "$f" -o "$o"
	OBJS="$OBJS $o"
done

EXPORTS="cw_init cw_push cw_run cw_poll cw_reset \
	cw_in_ptr cw_in_cap cw_status_ptr cw_spec_ptr cw_scope_ptr cw_chars_ptr \
	cw_scope_stride cw_spec_bins cw_sample_rate \
	cw_set_tone cw_set_tone_hz cw_cycle_tone cw_toggle_mode"

EXPORT_FLAGS=""
for e in $EXPORTS; do
	EXPORT_FLAGS="$EXPORT_FLAGS --export=$e"
done

# shellcheck disable=SC2086
"$WASM_LD" --no-entry --lto-O2 \
	-z stack-size=131072 \
	--initial-memory=4194304 \
	--export=__wasm_call_ctors \
	$EXPORT_FLAGS \
	$OBJS -o "$OUT"

# wasm-ld は出力に実行ビットを付けるが、HTTP で配るデータファイルなので落とす
chmod 644 "$OUT"

echo "built $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"

# 単一ファイル版 (file:// で直接開ける) も作り直す
python3 "$ROOT/web/bundle.py"
