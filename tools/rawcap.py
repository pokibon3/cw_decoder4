#!/usr/bin/env python3
"""
診断: 実機の ADC 生サンプルを取り込んで WAV にする (ファームは RAWCAP=1 でビルド。src/rawcap.h)。
スコープビューアなど、同じポートを開いているものは先に閉じること。

  一括 (32kHz・4 点平均の前、0.13〜0.5 秒):
    python3 tools/rawcap.py -o raw.wav
  連続 (8kHz・DSP に渡す値、指定秒数。Ctrl-C でも止まる):
    python3 tools/rawcap.py --stream 90 -o stream.wav

WAV は ADC 値から 2048 を引いて 16 倍した 16bit (フルスケール ±2048 が ±32768)。
連続のときは、同時に流れるスコープログ (C / E / S などの行) を同じ名前の .log に保存する。
ポートを開くと実機がリセットされることがあるので、起動して H 行が届いてからコマンドを送る。
"""
import argparse
import glob
import sys
import time
import wave

import serial


def find_port():
    # CYD は USB シリアル変換 (CH340 / CP210x) なので usbserial を優先する。
    # usbmodem は ESP32-S3 / P4 などネイティブ USB の別ボードのことがある
    ports = (sorted(glob.glob('/dev/cu.usbserial-*')) + sorted(glob.glob('/dev/cu.wchusbserial*')) +
             sorted(glob.glob('/dev/cu.usbmodem*')))
    if not ports:
        sys.exit('シリアルポートが見つかりません (-p で指定してください)')
    return ports[0]


def write_wav(path, vals, rate):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b''.join(int(max(-32768, min(32767, (v - 2048) * 16))).to_bytes(2, 'little', signed=True)
                               for v in vals))


def hexvals(h):
    return [int(h[i:i + 3], 16) for i in range(0, len(h) - 2, 3)]


def wait_ready(s, timeout=8.0):
    # 表示ループが回り始めると 5 秒ごとに H 行 (スコープログのヘッダ) が出る。
    # 起動直後に開いたときは最初の 1 行がすぐ出る
    deadline = time.time() + timeout
    while time.time() < deadline:
        if s.readline().startswith(b'H rate='):
            return
    sys.exit('実機から応答がありません (RAWCAP=1 のファームか、ポートとボーレートが合っているか確認してください)')


def snapshot(s, args):
    s.write(b'r')
    n = rate = None
    vals = []
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        line = s.readline().decode('ascii', 'replace').strip()
        if line.startswith('R!'):
            sys.exit(f'取り込みに失敗しました: {line}')
        if line.startswith('R0 '):
            kv = dict(f.split('=') for f in line.split()[1:])
            n, rate = int(kv['n']), int(kv['rate'])
            vals = []
        elif line.startswith('R ') and n is not None:
            vals += hexvals(line[2:])
        elif line == 'R1' and n is not None:
            break
    else:
        sys.exit('時間切れ (RAWCAP=1 のファームか、ポートとボーレートが合っているか確認してください)')
    if len(vals) != n:
        print(f'警告: {n} 個のはずが {len(vals)} 個', file=sys.stderr)
    write_wav(args.out, vals, rate)
    print(f'{args.out}: {len(vals)} サンプル / {rate} Hz / 最小 {min(vals)} 最大 {max(vals)}')


def stream(s, args):
    logpath = args.out.rsplit('.', 1)[0] + '.log'
    s.write(b's')
    rate = 8000
    vals = []
    nxt = None
    gaps = 0
    t_end = time.time() + args.stream
    t_giveup = t_end + 15               # 停止の応答が来ないときの打ち切り
    print(f'{args.stream:.0f} 秒取り込みます (Ctrl-C で途中終了)', file=sys.stderr)
    with open(logpath, 'w') as log:
        try:
            while True:
                if time.time() > t_giveup:
                    print('警告: 終了の応答 (Q1) がありませんでした', file=sys.stderr)
                    break
                if nxt is None and time.time() > t_giveup - args.stream - 10:
                    sys.exit('連続取り込みが始まりません (Q0 が届かない)')
                if t_end and time.time() >= t_end:
                    s.write(b'x')
                    t_end = 0
                    t_stop = time.time() + 3
                raw = s.readline()
                if not raw:
                    if not t_end and time.time() > t_stop:
                        break
                    continue
                line = raw.decode('ascii', 'replace').strip()
                if line.startswith('R!'):
                    sys.exit(f'取り込みに失敗しました: {line}')
                if line.startswith('Q0 '):
                    kv = dict(f.split('=') for f in line.split()[1:])
                    rate = int(kv['rate'])
                    log.write(line + '\n')
                elif line.startswith('Q '):
                    f = line.split()
                    if len(f) != 3:
                        continue        # 化けた行
                    seq = int(f[1])
                    if nxt is not None and seq != nxt:
                        gaps += seq - nxt
                        vals += [2048] * max(0, seq - nxt)   # 欠落は無音で埋める
                    vals += hexvals(f[2])
                    nxt = seq + len(f[2]) // 3
                elif line.startswith('Q1 '):
                    log.write(line + '\n')
                    break
                elif line:
                    log.write(line + '\n')
        except KeyboardInterrupt:
            s.write(b'x')
            time.sleep(0.5)
    write_wav(args.out, vals, rate)
    print(f'{args.out}: {len(vals)} サンプル ({len(vals) / rate:.1f} 秒) / 欠落 {gaps} サンプル')
    print(f'{logpath}: スコープログ')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-p', '--port')
    ap.add_argument('-b', '--baud', type=int, default=460800)
    ap.add_argument('-o', '--out', default=time.strftime('rawcap_%Y%m%d_%H%M%S.wav'))
    ap.add_argument('-t', '--timeout', type=float, default=30.0)
    ap.add_argument('--stream', type=float, metavar='SEC', help='8kHz の値を指定秒数流し続けて取り込む')
    args = ap.parse_args()

    s = serial.Serial()
    s.port = args.port or find_port()
    s.baudrate = args.baud
    s.timeout = 0.5
    s.dtr = True
    s.rts = True
    s.open()
    wait_ready(s)
    print(f'{s.port} ({args.baud} bps): 取り込み中...', file=sys.stderr)
    try:
        if args.stream:
            stream(s, args)
        else:
            snapshot(s, args)
    finally:
        s.close()


if __name__ == '__main__':
    main()
