# CW Decoder for UIAPduino２

UIAPduino Pro Micro で動作する CW Decoder / FFT Analyzer です。  
VS Code + PlatformIO + `ch32v003fun` ベースでビルドできます。  
現在のバージョンは `v1.9` です。

このリポジトリでは、以下の 2 つのモードを切り替えて使えます。

- CW デコーダ
- FFT スペクトラムアナライザ

MODE SW を押すことでモード切り替えができます。

## 対応 MCU / LCD

- CH32V006(UIAPduino Pro Micro CH32V006 V1.1)
- LCD: 1.14inch ST7789 (240x135)

CH32V003 / ST7735 のサポートは終了しました（`v1.7` までは [UIAP_CWDecoder2](https://github.com/pokibon3/UIAP_CWDecoder2) を参照）。

## 機能

### CW デコーダ

モールス信号を MIC 入力し、デコード結果を LCD に表示します。

- SW1: 英文 / 和文切り替え
- SW2: 666Hz / 833Hz / 1000Hz 切り替え

`v1.8` からトーン判定を追加しています。Goertzel フィルタを3ビン化し
（中心 = 目標トーン、サイド = ±341.33Hz）、中心レベルがサイドレベル
（EMA 平滑化後の min(low, high)）の 3 倍を超えた場合のみ信号とみなします。
ホワイトノイズは全ビンがほぼ同レベルになるため棄却され、片側の混信は
反対側のサイドビンで判定が守られます。処理コストは 48 サンプルあたり
約 1.1ms（処理周期 5.86ms の約 19%）です。

### FFT アナライザ

約 2.5kHz までのオーディオ信号スペクトルを表示します。  
ピーク周波数も表示します。

## ハード接続

### CH32V006

| UIAP | CH32V006 | 用途 |
|---|---|---|
| 10 | PC0 | LCD DC |
| 8 | PC6 | LCD MOSI |
| 9 | PC7 | LCD RES |
| 7 | PC5 | LCD SCK |
| 5 | PA4 | LCD CS |
| A1 | PA1 | SW1 |
| A2 | PC4 | SW2 |
| A3 | PD2 | SW3 |
| A0 | PA2 | MIC / ADC_IN0 |
| A6 | PD6 | TEST |
| LED | PC3 | LED |

## ビルド

VS Code でこのフォルダを開き、PlatformIO IDE の機能を使ってビルドします。

1. VS Code でこのプロジェクトを開く
2. PlatformIO IDE が有効になっていることを確認する
3. 画面下部の PlatformIO ツールバーから `Build` を実行する

environment は以下の 2 つです。

- `genericCH32V006F8U6`: CH32V006 版 (デフォルト)
- `esp32dev`: ESP32 版 (`pio run -e esp32dev`)

## ESP32 版 (v2.0)

`src/esp32/` 配下に ESP32 (Arduino + LovyanGFX) への移植版 `v2.0` があります。
air_monitor と同一のボード (ESP32 + ST7789 240x320) で動作します。

### 画面構成 (320x240 横向き)

- 最上段: ステータス行 (欧文/和文モード、WPM、選択トーン、実測ピーク周波数、信号インジケータ)
- 上 2/3: デコード文字エリア 13列 x 6行 (24x24 全角フォント、英数字も全角表示、和文カタカナ対応、濁点/半濁点は自動合成)
- 下 1/3 左: FFT スペクトラム (約300〜1500Hz、1bin=31.25Hz のライン表示、ピークホールド、TONE 選択レンジ 600〜1000Hz のガイド帯 + 選択トーンマーカー)
- 下 1/3 右: オシロスコープ (約3.6秒スパン。生波形 min/max バンド + トーンエンベロープ + キー判定を同一時間軸で色分け重畳)

### CH32V006 版との違い

- トーン検出は v1.9 と同一の 3ビン float Goertzel (8kHz / 48サンプル、サイド±333.33Hz)。
  スペクトラム表示のみ 256pt FFT を使用
- 音声サンプリングは I2S DMA (8kHz) で CPU 負荷ゼロ
- DSP/デコーダは Core 0、描画は Core 1 に分離
- デコード状態機械 (ヒステリシス、ノイズブランカ、ギャップ速度推定) は v1.9 のロジックをそのまま移植

### ハード接続 (ESP32)

| GPIO | 用途 |
|---|---|
| 14 | LCD SCK |
| 13 | LCD MOSI |
| 12 | LCD MISO |
| 2 | LCD DC |
| 15 | LCD CS |
| 21 | LCD BL |
| 35 | MIC / ADC1_CH7 |
| 0 | BOOT ボタン |
| 25 | タッチ SCK (XPT2046) |
| 32 | タッチ MOSI |
| 39 | タッチ MISO |
| 33 | タッチ CS |
| 36 | タッチ INT |

MIC 入力は GPIO35 (ADC1_CH7) を使用します。GPIO27 は ADC2 のため
I2S 内蔵 ADC の DMA サンプリングに使えず、採用していません。

### 操作 (タッチパネル)

- ステータス行の `US`/`JP` バッジをタップ: 欧文 / 和文モード切り替え
- ステータス行の `TONE` 表示をタップ: トーン周波数を順送り (600 / 700 / 800 / 900 / 1000Hz)
- FFT パネル内をタップ: タップ位置の周波数に最も近いトーンを直接選択

BOOT ボタンでも操作できます (短押し: トーン切替 / 長押し 800ms: モード切替)。
タッチ座標がズレる場合は `src/esp32/lgfx_config.h` のキャリブレーション値
(`x_min`/`x_max`/`y_min`/`y_max`) を調整してください。

## ファームウェア更新ツール

更新用パッケージは `tools/006/ST7789` 配下にあります。

- CH32V006 / ST7789 `v1.8` macOS: [tools/006/ST7789/mac/firmwareUpdate1.8](tools/006/ST7789/mac/firmwareUpdate1.8)
- CH32V006 / ST7789 `v1.8` Windows: [tools/006/ST7789/win/firmwareUpdate1.8](tools/006/ST7789/win/firmwareUpdate1.8)
- CH32V006 / ST7789 `v1.7` macOS: [tools/006/ST7789/mac/firmwareUpdate1.7](tools/006/ST7789/mac/firmwareUpdate1.7)
- CH32V006 / ST7789 `v1.7` Windows: [tools/006/ST7789/win/firmwareUpdate1.7](tools/006/ST7789/win/firmwareUpdate1.7)

## 変更履歴

- V1.0
  - 新規リリース
- V1.1
  - オーディオ入力のダイナミックレンジ拡大
  - 欧文 / 和文切換えのバグ修正
  - モールスデコーダのデフォルト周波数を 600Hz に変更
  - FFT アナライザのスプラッシュ表示削除
- V1.2
  - DFT 周波数調整
  - 入力オーディオレベル調整
  - 周波数表示を 600Hz から 700Hz に変更
- V1.3
  - デコードタイミング微調整
  - デコード方式改善
- V1.4
  - 音声サンプリングのダブルバッファ化と表示期間の最適化
  - ノイズブランカの改善
  - LCD に 1.14inch ST7735/ST7789 をサポート
- V1.5
  - ST7735 / ST7789 ドライバを新規作成し最適化
  - 速度検出の安定化
  - ノイズによる誤動作の軽減
- V1.6
  - CH32V006 対応
  - GPIO / LCD 配線差分対応
  - FLASH wait state 設定修正
  - ADC 入力 `PA2 = ADC_IN0` 修正
  - ファームウェア更新ツールを `tools` 配下へ整理
- V1.7
  - CH32V006 の表示フォントを改善
  - ST7735 向け `12x16` ANK フォントを追加
  - ST7789 向け `15x21` ANK フォントを追加
  - 006 では拡大描画ではなくネイティブ字形を使用
  - 未認識符号を `*` で表示してバッファをリセットするよう修正
  - 符号バッファのオーバーフロー防止処理を追加
- V1.8
  - CH32V003 / ST7735 サポートを削除（CH32V006 + ST7789 専用に）
  - Goertzel を3ビン化（中心 + ±341.33Hz サイドビン、1パス処理）
  - 中心/サイド比によるトーン判定を追加し、ホワイトノイズによる誤検出を抑制
  - サイドレベルは平滑化(EMA)後の min(low, high) を使用（片側混信に耐性）
  - 無信号からの立ち上がり時は瞬時サイドレベルも併用し、インパルス性ノイズ（QRN等）の誤検出を阻止
  - 速度推定を改良：長点も推定に利用して収束を高速化、低速側への変化率を制限してノイズバーストによる速度落ち込みを防止、WPM 表示は単位長から直接算出
  - 短点+長点ペア（比率1:3）検出による即時スナップを追加。20〜35wpm 帯ならノイズで倒れた速度推定がペア1組で瞬時に復帰（帯域外は緩やかに追従）
- V1.9
  - トーン判定にヒステリシス（シュミットトリガ）を導入。ON = 0.6×振幅しきい値かつ中心/サイド比3倍、OFF = 0.4×または2.5倍、中間は前状態保持。SNR 0dB でのデコード誤り 30〜43個/40秒 → 1〜2個（シミュレーション）
  - ギャップ（文字内1単位・文字間3単位）も速度推定の情報源に追加し、マーク+ギャップの1:3ペアでもスナップ。速度変化への追従を約2倍高速化（0.5〜0.7秒 → 0.1〜0.3秒）
- V2.0
  - ESP32 (Arduino + LovyanGFX) 移植版を追加（`src/esp32/`、environment: `esp32dev`）
  - トーン検出・デコードロジックは v1.9 と同等（3ビン Goertzel）、スペクトラム表示用に FFT を追加
  - 音声入力を I2S DMA サンプリング化（GPIO35 / ADC1_CH7、8kHz）
  - 320x240 画面: 13列x6行の 24x24 全角フォント表示（和文カタカナ・濁点合成対応）+ FFT / オシロ同時表示
  - タッチパネル対応（XPT2046）: US/JP 切替、TONE 600〜1000Hz の5段階切替、FFT パネルでの直接選択

## 参考

製作方法や使い方は、以下も参照してください。  
[R16 Friendship Radio Scrapbox](https://scrapbox.io/r16fr/UIAPduino%E3%82%92%E4%BD%BF%E3%81%A3%E3%81%9FCW_Decoder%E3%81%AE%E8%A3%BD%E4%BD%9C)

## ライセンス

本ソフトのモールスデコーダ部分は、Hjalmar Skovholm Hansen OZ1JHM 氏の Arduino 用 CW Decoder を一部流用しています。  
オリジナルの GPL ライセンス条件が適用されます。

- Original: [http://oz1jhm.dk/content/very-simpel-cw-decoder-easy-build](http://oz1jhm.dk/content/very-simpel-cw-decoder-easy-build)
- GPL: [http://www.gnu.org/copyleft/gpl.html](http://www.gnu.org/copyleft/gpl.html)
