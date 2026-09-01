# SanoTTS-jp on M5Stack CoreS3

日本語 TTS **[sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp)**（559 K params の蒸留生徒モデル、
論文 [arXiv:2608.21378](https://arxiv.org/abs/2608.21378) の日本語版）を **M5Stack CoreS3 単体**で動かす
ESP-IDF プロジェクト。クラウドも辞書も不要で、画面に文を出して内蔵スピーカーから喋る。

- 起動すると `今日は良い天気ですね。` を表示して喋る
- **画面をタッチ**すると直前の文をもう一度喋る
- USB シリアルの `かな> ` にかな中間表現を打つと、その文を喋る

> **モデルと推論コアは [ayutaz/sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp) のものです。**
> コードは MIT ですが、**重み `model/student_i8.bin` は sanoTTS-jp Model License 1.0**
> （つくよみちゃんコーパス由来の帰属表示と、生成音声の用途制限が伝播します）。
> 全文は [`LICENSES/`](LICENSES/)、帰属表示は [`NOTICE.md`](NOTICE.md)。
> **ビルドした flash イメージは重みを含む**ので、配布するときは `NOTICE.md` を付けてください。

## 必要なもの

| | |
|---|---|
| ボード | **M5Stack CoreS3**（ESP32-S3 / 16 MB flash / 8 MB Quad PSRAM / ILI9342C 320×240 / AW88298 アンプ） |
| ESP-IDF | **v5.5.5**（動作確認版。`~/esp/esp-idf` に置く前提。`idf.sh` 参照） |
| Python | `uv`（ビルド時のヘッダ生成に `uv run --no-project python` を使う。stdlib のみ） |
| ネットワーク | 初回ビルドで M5Unified / M5GFX を ESP-IDF Component Registry から取得 |

## ビルドと書き込み

```sh
git clone https://github.com/nnn112358/SanoTTS-jp-M5StackCoreS3
cd SanoTTS-jp-M5StackCoreS3
./idf.sh build                              # 既定: W8A8 + PIE / 起動時に喋る / ストリーミング
./idf.sh -p /dev/ttyACM0 flash monitor      # 終了は Ctrl+]
```

| フラグ | 既定 | 意味 |
|---|---|---|
| `-DSAAN_ENABLE_PIE=0/1` | **1** | W8A8 + ESP32-S3 の整数 SIMD (PIE)。0 = W8A32 / 移植可能 C |
| `-DSAAN_BUFFERED=0/1` | **0** | 1 = 1 発話ぶんを PSRAM に貯めてから鳴らす（**途切れない**。待ちは合成時間） |
| `-DSAAN_BOOT_SPEAK=0/1` | **1** | 起動時に 1 文喋る |

- `-D` の値は `build/` を消すまで CMake キャッシュに残る
- `idf.sh` は ESP-IDF v5.5.5 を**クリーンな環境で**有効化してから `idf.py` を呼ぶ
  （別バージョンを `export.sh` した端末から素の `idf.py` を打つと、その venv を掴んで落ちる）
- CoreS3 は native USB なので `/dev/ttyACM0`。権限が無ければ
  `SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666"` を `/etc/udev/rules.d/` に
- 書き込みモードに入らないときは電源ボタン（左下）を約 2 秒長押ししてから再実行

## 使い方

画面は 3 段: 上段に文（漢字）、中段にかな中間表現と出典、下段にステータス
（`合成中…` → `xRT 1.55  途切れ 10/14   タッチで再生`）。

シリアル入力は**かな中間表現**のみ（端末に辞書が無いので漢字・カタカナ・句読点は受け付けない）:

```
きょ][おわよ][いて][んきです°ね      ← 今日は良い天気ですね。
[ 上昇  ] 下降核  # 句境界  ° 無声化  ? ?! ?. ?~ 疑問
```

漢字混じり文からの変換は sanoTTS-jp リポジトリの `uv run python scripts/to_intermediate.py "文"`。
アクセント記号を省くと平板になる（音は出るが正しい抑揚ではない）。上限 350 ids（約 8 秒の音声）。

## 実機の数値

CoreS3 / ESP-IDF v5.5.5 / -O2 / 240 MHz / PSRAM ON / 22.05 kHz、
同じ文（53 ids / 106 frames / 27,136 sample / 音声 1.231 s）。2026-09-02 実測。

| | W8A32 / PIE 無効 | **W8A8 + PIE（既定）** |
|---|---:|---:|
| `saan_stream_init` | 68.95 ms | **23.19 ms** |
| 初回 pull（warmup） | 2,546.98 ms | **766 ms** |
| 2 回目以降の pull（1 チャンク = 92.88 ms の音声） | 448.95 ms | **144 ms** |
| **定常 xRT** | 4.834 | **1.554** |
| ストリーミングでの途切れ | 10 / 14 チャンク | 10 / 14 チャンク |
| `-DSAAN_BUFFERED=1` の発話開始までの待ち | — | **2.68 s** |
| 出力 PCM（FNV-1a / \|max\| / Σx²） | `0x78c209af06affc01` / 9529 / 74,155,592,149 | `0x04de91103a0e49f9` / 9744 / 74,374,063,946 |

- **両構成とも sanoTTS-jp の QEMU 記録（M-62）と 27,136 sample すべて bit 一致。**
  移植の正しさは「音が鳴った」ではなくこれで確認している
- タッチ再生を含む連続 6 発話が同じ checksum（発話ごとの統計リセットが効いている）
- メモリ: イメージ 1,325,568 B（factory 3 MB の 42%）、静的 DIRAM 290,227 / 341,760 B、
  起動直後の内部 DRAM free 102,895 B、arena used 194,848 B、タスクスタック残り 11,108 B
- ⚠️ **PIE でも 1.55× RT で実時間に届かない**（ストリーミングでは途切れる）。
  途切れない再生は `-DSAAN_BUFFERED=1`（待ち ≒ 音声長 × 1.55 + 0.77 s）。
  論文の 0.22× RT（英語 / ESP32-S3）とは 7 倍離れており、原因は未調査
- ⚠️ 聴取での品質確認はしていない。実サンプルレートの誤差も未測定

## 構成

```
CMakeLists.txt              PIE 既定 ON、blob のパス、COMPONENTS=main
partitions.csv              16 MB flash / factory 3 MB（重みは app の .rodata に入る）
sdkconfig.defaults          CoreS3（Quad PSRAM / USB Serial-JTAG）+ sanoTTS 向け設定
idf.sh                      ESP-IDF v5.5.5 をクリーンな環境で有効化して idf.py を呼ぶ
components/saanotts_core/   C99 推論コア・G2P・行編集（sanoTTS-jp csrc のコピー）
main/
  main.c                    起動 → セルフテスト → 表示 → 発話 → 入力/タッチのループ
  saan_model.{c,h}          .rodata の重み blob を開く
  saan_speaker.{h,cpp}      M5.Speaker 出力（22.05 kHz 直接。ストリーミング / 貯めて再生）
  saan_ui.{h,cpp}           画面とタッチ（M5GFX）
  saan_console.{c,h}        シリアル `かな> ` 入力（タイムアウト付き poll）
  demo_ids.h                起動セルフテストの錨（かな → ids）
model/student_i8.bin        重み（643,936 B。model/README.md）
scripts/blob_to_header.py   blob → const uint8_t[] ヘッダ（ビルド時に自動実行）
NOTICE.md  LICENSES/        帰属表示とライセンス全文
```

### 設計メモ（CoreS3 で踏んだこと）

| 事象 | 対処 |
|---|---|
| `.dram0.bss` が 10 KB 溢れてリンクできない | 音声バッファ 28 KB をヒープ確保（PSRAM 優先）に。IRAM のコードを flash へ（`FREERTOS/HEAP/RINGBUF_PLACE_*_INTO_FLASH`, `SPI_FLASH_ROM_IMPL`）。`set(COMPONENTS main)` |
| `esp_partition_mmap` が `ESP_ERR_NO_MEM` | PSRAM 8 MB が data 用 vaddr を占有する。**重みはヘッダ化して app の `.rodata` に**（起動時に DROM としてマップされる） |
| S3 のキャッシュ設定と DRAM | `dram0_0_seg` は 341,760 B 固定で、D-cache 64 KB にしても減らない（無料なので 64 KB） |
| M5 の 22.05 → 44.1 kHz リサンプル | `SAAN_SPK_OUT_RATE 22050`。AW88298 は 22.05 kHz 対応（M5Unified が `rate_tbl` から I2SSR を設定） |
| `M5.Speaker.playRaw` はコピーしない | 再生が終わるまでバッファを触らない。ストリーミングは 3 枚回し、貯める方式は `stop()` で再生完了を待ってから解放 |
| タッチとスピーカーが同じ I2C バス | `M5.update()` と描画は合成タスクからだけ呼ぶ（別タスクにしない） |
| `CONFIG_SPIRAM_MODE_OCT` だとブートループ | CoreS3 は **Quad** |
| `idf.py monitor` に何も出ない | USB-UART ブリッジ無し → `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` |

## 出所とライセンス

| ここ | 出所 | ライセンス |
|---|---|---|
| `components/saanotts_core/` | sanoTTS-jp `csrc/`（commit `2c61a8d`） | MIT |
| `main/main.c` `saan_console.{c,h}` `demo_ids.h` | sanoTTS-jp `esp32/main/`（CoreS3 向けに改変） | MIT |
| `main/saan_model.c` `saan_speaker.cpp` の元 | sanoTTS-jp ブランチ `wip/esp32-m5unified` | MIT |
| `main/saan_speaker.h` `saan_ui.{h,cpp}` ほか本プロジェクトの変更 | このリポジトリ | MIT（[`LICENSE`](LICENSE)） |
| `scripts/blob_to_header.py` | sanoTTS-jp `scripts/` | MIT |
| **`model/student_i8.bin`** | **sanoTTS-jp Release `saanotts-jp-v3-int8.bin`**<br>SHA-256 `c3b89216133fa7bee3f61ed9d8e6c7183a5dfd41b70dab194f42c20fce5b4170` | **sanoTTS-jp Model License 1.0** |
| M5Unified 0.2.21 / M5GFX 0.2.28 | ESP-IDF Component Registry | MIT（日本語フォントは IPA Font License） |

sanoTTS-jp（Copyright (c) 2026 yousan, MIT）と本プロジェクトの変更（MIT）の全文は
[`LICENSES/`](LICENSES/) と [`LICENSE`](LICENSE)。モデルの帰属表示ブロックと生成音声の用途制限は
[`NOTICE.md`](NOTICE.md)。
