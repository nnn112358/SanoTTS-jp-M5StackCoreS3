# SanoTTS-jp on M5Stack CoreS3

日本語 TTS **[sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp)**（559 K params の蒸留モデル、
[arXiv:2608.21378](https://arxiv.org/abs/2608.21378) の日本語版）を **M5Stack CoreS3 単体**で動かす
ESP-IDF プロジェクト。クラウドも辞書も不要。

- 起動すると [m5stack-avatar](https://github.com/stack-chan/m5stack-avatar) の顔が出て、
  `今日は良い天気ですね。` を吹き出しに出しながら内蔵スピーカーで喋る（**口は音量に合わせて動く**）
- **画面をタッチ**すると直前の文をもう一度喋る
- USB シリアルの `かな> ` に**漢字かな交じり文をそのまま**打つと、その文を喋る
  （端末内の辞書 13.7 MB + Open JTalk の NJD 鎖で読みとアクセントを付ける。クラウド不要）。
  かな中間表現（`きょ][おわよ][いて][んきです°ね`）を打てば**前置記号なしで**そちらの経路に入る

> **モデルと推論コアは [ayutaz/sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp) のものです。**
> コードは MIT、**重み `model/student_i8.bin` は sanoTTS-jp Model License 1.0**
> （つくよみちゃんコーパス由来の帰属表示と、生成音声の用途制限が伝播します）。
> **ビルドした flash イメージは重みを含む**ので、配布するときは [`NOTICE.md`](NOTICE.md) を付けてください。

## 必要なもの

| | |
|---|---|
| ボード | **M5Stack CoreS3**（ESP32-S3 / 16 MB flash / 8 MB Quad PSRAM）。Tab5 は [別リポジトリ](https://github.com/nnn112358/SanoTTS-jp-Tab5) |
| ESP-IDF | **v5.5.5**（`~/esp/esp-idf` に置く前提。`idf.sh` 参照） |
| Python | `uv`（ビルド時のヘッダ生成に使う。stdlib のみ） |
| ネットワーク | 初回ビルドで M5Unified / M5GFX を Component Registry から取得 |
| 重み | `model/student_i8.bin`（git に入れてある。sanoTTS-jp Release **v0.3.0** の `saanotts-jp-v3-int8.bin` = **blob v2**、654,032 B。[`model/README.md`](model/README.md)） |
| 辞書 | `scripts/get_dict.sh` で sanoTTS-jp Release の `k1-dict-438750.bin`（13.7 MB）を `model/` に置く（git には入れていない） |

## ビルドと書き込み

```sh
git clone https://github.com/nnn112358/SanoTTS-jp-M5StackCoreS3
cd SanoTTS-jp-M5StackCoreS3
./scripts/get_dict.sh                       # 辞書 blob（13.7 MB）を取る
./idf.sh build
./idf.sh -p /dev/ttyACM0 flash monitor      # 終了は Ctrl+]
```

| フラグ | 既定 | 意味 |
|---|---|---|
| `-DSAAN_UI=avatar/text` | **avatar** | 画面。avatar = m5stack-avatar の顔 + 吹き出し + リップシンク / text = 文字だけ（文・出典・ステータスの 3 段。avatar はリンクしない） |
| `-DSAAN_ENABLE_PIE=0/1` | **1** | W8A8 + ESP32-S3 の整数 SIMD (PIE)。0 = W8A32 / 移植可能 C |
| `-DSAAN_BUFFERED=0/1` | **0** | 0 = xRT から先読み量を決めて計算しながら鳴らす / 1 = 全部貯めてから鳴らす |
| `-DSAAN_BOOT_SPEAK=0/1` | **1** | 起動時に 1 文喋る |
| `-DSAAN_KANJI=0/1` | **1** | 端末内漢字 G2P（辞書 13.7 MB + Open JTalk）。0 で外すと入力はかな中間表現だけ、辞書も焼かない |
| `-DSAAN_CORE_IRAM=0/1` | **1** | 推論コアの `.text` を IRAM に置く（約 10 KB。旧コアで −2.4%） |
| `-DSAAN_OJ_PSRAM=0/1` | **1** | Open JTalk の一時ヒープを PSRAM に向ける（0 は陽性対照。内部 DRAM が減るのを見る） |
| `-DSAAN_PROFILE=0/1` | **0** | 段別プロファイル（CCOUNT）を発話後に出す。**速度の報告には 0 で**（計測にコストがある） |

ビルド環境なしで焼くだけなら、実機確認済みのイメージが [`firmware/`](firmware/README.md) にある
（一括 16 MB イメージを `esptool.py write_flash 0x0 …` で焼く）。

`-D` の値は `build/` を消すまで CMake キャッシュに残る。顔と文字表示を行き来するなら build ディレクトリを分ける:

```sh
./idf.sh -p /dev/ttyACM0 flash monitor                                                # 顔（既定、build/）
./idf.sh -B build_text -DSDKCONFIG=build_text/sdkconfig -DSAAN_UI=text -p /dev/ttyACM0 flash monitor   # 文字だけ
```

### M5Stack Tab5（ESP32-P4）

Tab5 向けは別リポジトリに分けた: **[SanoTTS-jp-Tab5](https://github.com/nnn112358/SanoTTS-jp-Tab5)**
（Tab5 Keyboard でローマ字入力、横画面、P4 向けの設定）。このリポジトリは CoreS3 専用。
CoreS3 は native USB なので `/dev/ttyACM0`（権限が無ければ
`SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666"` を udev に）。

## 現状

**2026-09-04 に推論コアを sanoTTS-jp origin/main（d169e91）へ同期した。** 本家はコアの速度を
作り直し（S1〜S5b / T1〜T5。すべて出力を変えない変更）、同じ CoreS3（顔なしの本家 M5 構成）で
**定常 xRT 0.926 → 0.446**、arena 208 → 176 KB、途切れ 0 を実測している（本家 M-89 / M-90）。
入力も本家に合わせて **1 経路**になった: `saan_g2p_classify()` が「かな中間表現 / 漢字かな交じり文 /
拒否」を決めるので、前置記号は要らない（`=` でかな、`!` で辞書に**強制**する試験用の経路は残してある）。

2026-09-07 にこの板で実測した（[`docs/measurements.md`](docs/measurements.md)）: **顔ありの既定ビルドで
定常 xRT 0.445 / 追い越し 0 / checksum `0xa69a7ebbb5ccb05f`（本家 QEMU・本家 M5 実機と一致）**。
先読みは 2 チャンク（186 ms）で足り、**発話開始まで 330 ms**（旧コアは 1.8 s）。
本家 Release v0.3.0 の CoreS3 イメージ（顔なし）も同じ板で 0.448 を再現している。
⚠️ **checksum の期待値が変わった**（S3 = GELU の erf 近似）: W8A8+PIE **`0xa69a7ebbb5ccb05f`** /
W8A32 `0xe4b645c30835d42d`。旧コアの `0x04de91103a0e49f9` とは一致しない。
⚠️ **blob は v2**（654,032 B）。v0.2.0 以前の `saanotts-jp-v3-int8.bin`（v1、643,936 B）はコアが
`SAAN_ERR_VERSION` で拒む。重みの値は同じで配置だけが違う。

辞書は枝刈りしてあるので、ホストの OpenJTalk と読みが変わる文がある（sanoTTS-jp の実測で
17.79% の文。地名・固有名詞で起きやすい）。

同期前（旧コア、2026-09-02 実測）: W8A8+PIE 定常 1.55× RT で再生に追いつかず、音声の
(1 − 1/xRT) + 2 チャンク ≒ 60% を先に貯めてから鳴らし始めていた（1.2 秒の文で発話開始まで 1.8 s、
途切れ 0）。出力 PCM は sanoTTS-jp の QEMU 記録と 27,136 sample すべて bit 一致（移植は正しい）。

詳細:
- [`docs/measurements.md`](docs/measurements.md) — 速度・正しさ・メモリの実測値
- [`docs/design-notes.md`](docs/design-notes.md) — 構成、入力仕様、CoreS3 で踏んだことと対処
- [`docs/upstream-comparison.md`](docs/upstream-comparison.md) — 公式実装 Ampixa/sanoTTS との違いと、0.22× RT に至った手順（公開文書とログのみ）

## 出所とライセンス

| | 出所 | ライセンス |
|---|---|---|
| 推論コア・ファーム本体・スクリプト | [sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp) origin/main d169e91（`csrc/`, `esp32/main/`, `scripts/`） | MIT |
| CoreS3 向けの変更（M5.Speaker / 顔 / タッチ / 先読み自動） | このリポジトリ | MIT（[`LICENSE`](LICENSE)） |
| **`model/student_i8.bin`** | **sanoTTS-jp Release v0.3.0 `saanotts-jp-v3-int8.bin`**（blob v2、SHA-256 `2d2b8543…`、[`model/README.md`](model/README.md)） | **sanoTTS-jp Model License 1.0** |
| M5Unified / M5GFX | ESP-IDF Component Registry | MIT（日本語フォントは IPA Font License） |
| m5stack-avatar 0.10.0（`components/m5stack-avatar/`） | [stack-chan/m5stack-avatar](https://github.com/stack-chan/m5stack-avatar)（vendored） | MIT |
| Open JTalk（`components/saanotts_core/openjtalk/`、無改変） | sanoTTS-jp 経由 | 修正 BSD |

ライセンス全文は [`LICENSES/`](LICENSES/)、モデルの帰属表示ブロックと生成音声の用途制限は [`NOTICE.md`](NOTICE.md)。
