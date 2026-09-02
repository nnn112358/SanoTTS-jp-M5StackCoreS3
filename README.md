# SanoTTS-jp on M5Stack CoreS3

日本語 TTS **[sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp)**（559 K params の蒸留モデル、
[arXiv:2608.21378](https://arxiv.org/abs/2608.21378) の日本語版）を **M5Stack CoreS3 単体**で動かす
ESP-IDF プロジェクト。クラウドも辞書も不要。

- 起動すると画面に `今日は良い天気ですね。` を出して内蔵スピーカーで喋る
- **画面をタッチ**すると直前の文をもう一度喋る
- USB シリアルの `かな> ` にかな中間表現（例: `きょ][おわよ][いて][んきです°ね`）を打つと、その文を喋る

> **モデルと推論コアは [ayutaz/sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp) のものです。**
> コードは MIT、**重み `model/student_i8.bin` は sanoTTS-jp Model License 1.0**
> （つくよみちゃんコーパス由来の帰属表示と、生成音声の用途制限が伝播します）。
> **ビルドした flash イメージは重みを含む**ので、配布するときは [`NOTICE.md`](NOTICE.md) を付けてください。

## 必要なもの

| | |
|---|---|
| ボード | **M5Stack CoreS3**（ESP32-S3 / 16 MB flash / 8 MB Quad PSRAM） |
| ESP-IDF | **v5.5.5**（`~/esp/esp-idf` に置く前提。`idf.sh` 参照） |
| Python | `uv`（ビルド時のヘッダ生成に使う。stdlib のみ） |
| ネットワーク | 初回ビルドで M5Unified / M5GFX を Component Registry から取得 |

## ビルドと書き込み

```sh
git clone https://github.com/nnn112358/SanoTTS-jp-M5StackCoreS3
cd SanoTTS-jp-M5StackCoreS3
./idf.sh build
./idf.sh -p /dev/ttyACM0 flash monitor      # 終了は Ctrl+]
```

| フラグ | 既定 | 意味 |
|---|---|---|
| `-DSAAN_ENABLE_PIE=0/1` | **1** | W8A8 + ESP32-S3 の整数 SIMD (PIE)。0 = W8A32 / 移植可能 C |
| `-DSAAN_BUFFERED=0/1` | **0** | 1 = 1 発話ぶんを貯めてから鳴らす（**途切れない**。待ち ≒ 音声長 × 1.55 + 0.8 s） |
| `-DSAAN_BOOT_SPEAK=0/1` | **1** | 起動時に 1 文喋る |

`-D` の値は `build/` を消すまで CMake キャッシュに残る。
CoreS3 は native USB なので `/dev/ttyACM0`（権限が無ければ
`SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666"` を udev に）。

## 現状

実機（CoreS3 / 240 MHz）で **W8A8 + PIE は定常 1.55× RT**。ストリーミングでは途切れるので、
途切れない再生が要るなら `-DSAAN_BUFFERED=1`。出力 PCM は sanoTTS-jp の QEMU 記録と
27,136 sample すべて bit 一致（移植は正しい）。聴取での品質確認はしていない。

詳細:
- [`docs/measurements.md`](docs/measurements.md) — 速度・正しさ・メモリの実測値
- [`docs/design-notes.md`](docs/design-notes.md) — 構成、入力仕様、CoreS3 で踏んだことと対処

## 出所とライセンス

| | 出所 | ライセンス |
|---|---|---|
| 推論コア・ファーム本体・スクリプト | [sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp)（`csrc/`, `esp32/main/`, `scripts/`） | MIT |
| CoreS3 向けの変更（M5.Speaker / 画面 / タッチ） | このリポジトリ | MIT（[`LICENSE`](LICENSE)） |
| **`model/student_i8.bin`** | **sanoTTS-jp Release `saanotts-jp-v3-int8.bin`**（SHA-256 `c3b89216…`、[`model/README.md`](model/README.md)） | **sanoTTS-jp Model License 1.0** |
| M5Unified / M5GFX | ESP-IDF Component Registry | MIT（日本語フォントは IPA Font License） |

ライセンス全文は [`LICENSES/`](LICENSES/)、モデルの帰属表示ブロックと生成音声の用途制限は [`NOTICE.md`](NOTICE.md)。
