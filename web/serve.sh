#!/bin/sh
#
#	開発用サーバーを立ててブラウザで開く。
#
#	AudioWorklet はサーバー経由でしか読み込めない (file:// はオリジンが
#	null になり cross-origin として拒否される) ので、ローカルで試すときは
#	これを使う。マイク入力の許可も localhost なら通常どおり効く。
#
#	使い方:  sh web/serve.sh [ポート]
#
set -eu
cd "$(dirname "$0")"

PORT=${1:-8777}
URL="http://localhost:$PORT/"

# 既に誰かが使っていればそのまま開くだけ
if curl -s -o /dev/null -m 1 "$URL"; then
	echo "既に $URL で動いています"
else
	python3 -m http.server "$PORT" --bind 127.0.0.1 &
	SERVER=$!
	trap 'kill $SERVER 2>/dev/null || true' EXIT INT TERM
	sleep 1
	echo "サーバーを起動しました (pid $SERVER)"
fi

echo "$URL"
case "$(uname -s)" in
	Darwin) open "$URL" ;;
	Linux)  xdg-open "$URL" >/dev/null 2>&1 || true ;;
esac

# フォアグラウンドで待つ (Ctrl-C で停止)
if [ -n "${SERVER:-}" ]; then
	echo "停止するには Ctrl-C"
	wait $SERVER
fi
