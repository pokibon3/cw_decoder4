# CW Decoder for ESP32

ESP32 ボード (2.8inch CYD 系: ST7789 / ILI9341 240x320 + XPT2046 タッチ) で動作する CW Decoder です。  
VS Code + PlatformIO + Arduino + LovyanGFX でビルドできます。  
現在のバージョンは `v2.0` です。

CH32V003 / CH32V006 (UIAPduino) 版は
[UIAP_CWDecoder2](https://github.com/pokibon3/UIAP_CWDecoder2) を参照してください
(こちらも `v2.0` としてノイズ対策を反映済み)。

## 対応ハードウェア

- ESP32 (esp32dev / 240MHz デュアルコア)
- LCD: 2.8inch 240x320 (SPI 40MHz)。コントローラは **ST7789 / ILI9341** の両対応
- タッチ: XPT2046 (抵抗膜、ソフトSPI)
- オーディオ入力: GPIO35 (ADC1_CH7) をADC continuous DMAで32kHz取得し、4点平均で8kHz化

### LCD コントローラの選択

`platformio.ini` の `build_flags` で指定します。

- 無指定 (デフォルト): 起動時に自動判定 (ID4=0xD3 を読んで ILI9341 を識別、応答が無ければ ST7789)
- `-DPANEL_ST7789`: ST7789 に固定
- `-DPANEL_ILI9341`: ILI9341 に固定

ILI9341 は ID 読み出しに応答しない個体があり、その場合は自動判定できないので
`-DPANEL_ILI9341` を明示してください。ILI9341 は ST7789 に対して表示の基準面が
180° 回転しているため、パネル設定側 (`offset_rotation`) で吸収しており、表示コードと
タッチ校正は両コントローラで共通です。起動時にシリアル (115200) へ
`[lcd] panel = ...` と判定結果を出力します。

## 機能

モールス信号 (欧文 / 和文) をオーディオ入力からデコードし、LCD に表示します。

### 画面構成 (320x240 横向き)

- 最上段: ステータス行 (欧文/和文モード、WPM、選択トーン、実測ピーク周波数、信号インジケータ)
- 上 2/3: デコード文字エリア 16列 x 6行 (24x24 全角フォント、英数字も全角表示、和文カタカナ対応、濁点/半濁点は自動合成)
- 下 1/3 左: FFT スペクトラム (約300〜1200Hz、1bin=31.25Hz のライン表示、ピークホールド、TONE 選択レンジ 600〜1000Hz のガイド帯 + 選択トーンの検出帯域(±42Hz)表示)
- 下 1/3 右: オシロスコープ (約3.6秒スパン。生波形 min/max バンド + トーンエンベロープ + キー判定を同一時間軸で色分け重畳)

### 信号処理

- トーン検出は 3ビン float Goertzel (窓長 96 サンプル = 帯域 83.3Hz、判定周期 6ms、
  サイド±333.33Hz は矩形窓 Dirichlet 核のヌル上)
- サイド判定は幾何平均 √(low×high) + 「中心 > max(サイドEMA, サイド瞬時)」条件で、
  低域から裾を引く傾斜ノイズ (バンドノイズ) による偽符号を抑止
- TONE AUTO モード (デフォルト): 550〜1000Hz の最強ピークへゲート中心を自動同調
  (手動 TONE は 600〜1000Hz)。受信中は±25Hz の微修正のみ、信号断で最後の周波数をホールド
- デコード状態機械 (トーン判定ヒステリシス、ノイズブランカ、ギャップ利用の速度推定) は
  CH32 版 v1.9 と同等。タイミングはサンプル数由来のブロッククロック (6ms/ブロック)
- DSP/デコーダは Core 0、描画は Core 1 に分離

### 操作 (タッチパネル)

- ステータス行の `US`/`JP` バッジをタップ: 欧文 / 和文モード切り替え
- ステータス行の `TONE` 表示をタップ: AUTO → 600 → 700 → 800 → 900 → 1000Hz の順送り
  (AUTO は 550〜1000Hz の最強信号へ自動同調、緑色表示でデフォルト)
- FFT パネル内をタップ: タップ位置の周波数に最も近いトーンを手動選択 (AUTO 解除)

BOOT ボタンでも操作できます (短押し: トーン切替 / 長押し 800ms: モード切替)。
タッチ座標がズレる場合は `src/lgfx_config.h` のキャリブレーション値
(`x_min`/`x_max`/`y_min`/`y_max`) を調整してください。

## ハード接続

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
| 36 | タッチ INT (未使用) |

MIC 入力は GPIO35 (ADC1_CH7) を使用します。ADC continuous DMAは
ESP32でADC1のみ対応するため、GPIO27 (ADC2) は採用していません。
入力には約 1.65V の DC バイアスを与えてください。

## ビルド

VS Code でこのフォルダを開き、PlatformIO IDE の機能を使ってビルドします。

1. VS Code でこのプロジェクトを開く
2. PlatformIO IDE が有効になっていることを確認する
3. 画面下部の PlatformIO ツールバーから `Build` を実行する

environment は `esp32dev` のみです。

## 変更履歴

`v1.x` は CH32V003/V006 (UIAPduino) 時代の履歴です。

- V1.0〜V1.7
  - OZ1JHM 版をベースに CH32V003/V006 へ移植、Goertzel 化、ST7735/ST7789 対応、フォント改善など
- V1.8
  - Goertzel を3ビン化（中心 + ±341.33Hz サイドビン、Dirichlet ヌル上）し、中心/サイド比によるトーン判定でホワイトノイズ・インパルスノイズを棄却
  - 速度推定を改良（長点も利用して高速収束、短点+長点1:3ペアで20〜35wpm帯へ即時スナップ）
- V1.9
  - トーン判定にヒステリシス（ON 0.6x/3x, OFF 0.4x/2.5x）を導入し、SNR 0dB のデコード誤りをほぼ解消
  - ギャップ（文字内/文字間）も速度推定に利用し、速度変化追従を約2倍高速化
- V2.0 (ESP32 版)
  - ESP32 (Arduino + LovyanGFX) へ全面移植し、リポジトリを ESP32 専用に再構成
  - 音声入力を I2S DMA 化（GPIO35、8kHz）、デコーダをサンプル数由来のブロッククロック駆動に変更
  - 320x240 画面: 16列x6行の 24x24 全角フォント + FFT / オシロ同時表示
  - タッチパネル対応（XPT2046）: US/JP 切替、TONE 切替、FFT パネルでの直接選択
  - トーン判定を 96 サンプル窓に狭帯域化（83.3Hz、+3dB）
  - TONE AUTO モード（550〜1000Hz の最強ピークへ自動同調）を追加しデフォルト化
  - サイド判定の幾何平均化 + side max 条件で傾斜ノイズによる偽符号を抑止
  - スコープ掃引・トーン判定帯域を WPM 追従化（高速受信のデコード率改善）
  - GUI 刷新（ステータス行のボタン化、波形 ON/OFF トグル、Peak 周波数表示）
  - LCD コントローラ ST7789 / ILI9341 両対応（自動判定 + ビルドフラグ）

## 参考

製作方法や使い方は、以下も参照してください。  
[R16 Friendship Radio Scrapbox](https://scrapbox.io/r16fr/UIAPduino%E3%82%92%E4%BD%BF%E3%81%A3%E3%81%9FCW_Decoder%E3%81%AE%E8%A3%BD%E4%BD%9C)

## ライセンス

本ソフトのモールスデコーダ部分は、Hjalmar Skovholm Hansen OZ1JHM 氏の Arduino 用 CW Decoder を一部流用しています。  
オリジナルの GPL ライセンス条件が適用されます。

- Original: [http://oz1jhm.dk/content/very-simpel-cw-decoder-easy-build](http://oz1jhm.dk/content/very-simpel-cw-decoder-easy-build)
- GPL: [http://www.gnu.org/copyleft/gpl.html](http://www.gnu.org/copyleft/gpl.html)
