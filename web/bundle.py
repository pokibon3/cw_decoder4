#!/usr/bin/env python3
"""
単一ファイル版 web/cw-decoder.html を作る。

index.html / app.js / cw-worklet.js / cwcore.wasm を 1 枚の HTML にまとめる。
file:// で直接開けるようにするのが目的:
  - ES モジュールの外部読み込みは file:// では CORS で拒否される
    → app.js をインラインの module として埋め込む (インラインは許可される)
  - fetch('cwcore.wasm') も拒否される
    → base64 で埋め込み、WebAssembly.instantiate に直接渡す
  - audioWorklet.addModule('cw-worklet.js') も同じく拒否される
    → ソースを埋め込み、Blob URL 経由で addModule する (同一オリジン扱い)

build.sh から呼ばれる。単体でも実行できる。
"""
import base64
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent


def js_string(text: str) -> str:
    """JS の文字列リテラルにする。'</script>' で HTML が切れないよう '<\\/' に逃がす。"""
    return json.dumps(text, ensure_ascii=False).replace("</", "<\\/")


def main() -> int:
    html = (HERE / "index.html").read_text(encoding="utf-8")
    app = (HERE / "app.js").read_text(encoding="utf-8")
    worklet = (HERE / "cw-worklet.js").read_text(encoding="utf-8")
    wasm = base64.b64encode((HERE / "cwcore.wasm").read_bytes()).decode("ascii")

    # index.html の末尾にある app.js ローダー (script ブロック) を差し替える
    marker = "<script>\n// app.js を読み込む。"
    start = html.find(marker)
    if start < 0:
        print("index.html のローダー script が見つかりません", file=sys.stderr)
        return 1
    end = html.find("</script>", start)
    if end < 0:
        print("ローダー script の終端が見つかりません", file=sys.stderr)
        return 1
    end += len("</script>")

    inlined = (
        "<script>\n"
        "// --- 単一ファイル版: ワークレットと wasm をここに埋め込んである ---\n"
        "window.__CW_BUNDLE = {\n"
        f"  worklet: {js_string(worklet)},\n"
        f"  wasm: {js_string(wasm)}\n"
        "};\n"
        "</script>\n"
        '<script type="module">\n'
        + app.replace("</script", "<\\/script")
        + "\n</script>"
    )

    body = html[:start] + inlined + html[end:]
    # 単一ファイル版は 1 枚で完結させるので、別ファイルであるマニュアルへの
    # リンクは外す (リンク先が無い状態で残すと切れたリンクになる)
    body = body.replace('<a class="manual" href="manual.html">使い方</a>', "")

    out = HERE / "cw-decoder.html"
    out.write_text(body, encoding="utf-8")
    out.chmod(0o644)
    size = out.stat().st_size
    print(f"built {out} ({size} bytes, {size / 1024:.0f}KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
