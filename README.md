# CW Decoder for UIAPduino２

UIAPduino Pro Micro で動作する CW Decoder / FFT Analyzer です。  
VS Code + PlatformIO + `ch32v003fun` ベースでビルドできます。

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

environment は `genericCH32V006F8U6` のみです。

## ファームウェア更新ツール

更新用パッケージは `tools/006/ST7789` 配下にあります。

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

## 参考

製作方法や使い方は、以下も参照してください。  
[R16 Friendship Radio Scrapbox](https://scrapbox.io/r16fr/UIAPduino%E3%82%92%E4%BD%BF%E3%81%A3%E3%81%9FCW_Decoder%E3%81%AE%E8%A3%BD%E4%BD%9C)

## ライセンス

本ソフトのモールスデコーダ部分は、Hjalmar Skovholm Hansen OZ1JHM 氏の Arduino 用 CW Decoder を一部流用しています。  
オリジナルの GPL ライセンス条件が適用されます。

- Original: [http://oz1jhm.dk/content/very-simpel-cw-decoder-easy-build](http://oz1jhm.dk/content/very-simpel-cw-decoder-easy-build)
- GPL: [http://www.gnu.org/copyleft/gpl.html](http://www.gnu.org/copyleft/gpl.html)
