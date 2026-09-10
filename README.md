# SanoTTS-jp on M5Stack

日本語 TTS **[sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp)**（559 K params の蒸留モデル、
[arXiv:2608.21378](https://arxiv.org/abs/2608.21378) の日本語版）を **M5Stack 単体**で動かす
ESP-IDF プロジェクト。クラウド不要。**CoreS3**（既定）のほか **Core2 / Core Basic / ATOMS3 / ATOMS3R / Stamp-C5** で
ビルドできる（[対応ボード](#対応ボード)。実機で確かめたのは CoreS3 と ATOMS3）。

- 起動すると `今日は良い天気ですね。` を喋る。画面のあるボードでは [m5stack-avatar](https://github.com/stack-chan/m5stack-avatar)
  の顔が出て、吹き出しに文を出しながら**口が音量に合わせて動く**（ATOMS3 / ATOMS3R は 128 x 128 に縮小）。
  Stamp-C5 は画面なし
- **画面を短くタッチ**（ATOMS3 / Basic は本体ボタン A）すると直前の文をもう一度喋る
- **長押し**（またはボタン B、シリアルの `/ui`）で **顔 ⇄ 本家と同じ文字画面** を切り替える（1 つのファームに両方入っている）
- USB シリアルの `かな> ` に**漢字かな交じり文をそのまま**打つと、その文を喋る
  （端末内の辞書 + Open JTalk の NJD 鎖で読みとアクセントを付ける。辞書は flash に合わせて
  13M / 8M / 4M / 2M の 4 種から選ぶ）。
  かな中間表現（`きょ][おわよ][いて][んきです°ね`）を打てば**前置記号なしで**そちらの経路に入る

> **モデルと推論コアは [ayutaz/sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp) のものです。**
> コードは MIT、**重み `model/student_i8.bin` は sanoTTS-jp Model License 1.0**
> （つくよみちゃんコーパス由来の帰属表示と、生成音声の用途制限が伝播します）。
> **ビルドした flash イメージは重みを含む**ので、配布するときは [`NOTICE.md`](NOTICE.md) を付けてください。

## 必要なもの

| | |
|---|---|
| ボード | **M5Stack CoreS3**（既定）/ Core2 / Core Basic / ATOMS3 / ATOMS3R / Stamp-C5（[対応ボード](#対応ボード)）。Tab5 は [別リポジトリ](https://github.com/nnn112358/SanoTTS-jp-Tab5) |
| ESP-IDF | **v5.5.5**（`~/esp/esp-idf` に置く前提。`idf.sh` 参照） |
| Python | `uv`（ビルド時のヘッダ生成に使う。stdlib のみ） |
| ネットワーク | 初回ビルドで M5Unified / M5GFX を Component Registry から取得 |
| 重み | `model/student_i8.bin`（git に入れてある。sanoTTS-jp Release **v0.3.0** の `saanotts-jp-v3-int8.bin` = **blob v2**、654,032 B。[`model/README.md`](model/README.md)） |
| 辞書 | `scripts/get_dict.sh [438750\|228000\|135000\|44000\|all]` で sanoTTS-jp Release の `k1-dict-*.bin`（13M / 8M / 4M / 2M）を `model/` に置く（git には入れていない）。CoreS3 の既定は 13M |

## ビルドと書き込み

```sh
git clone https://github.com/nnn112358/SanoTTS-jp-M5StackCoreS3
cd SanoTTS-jp-M5StackCoreS3
./scripts/get_dict.sh                       # 辞書 blob（13M = 13.7 MB。CoreS3 の既定）を取る
./idf.sh build
./idf.sh -p /dev/ttyACM0 flash monitor      # 終了は Ctrl+]
```

CoreS3 以外は `./idf_board.sh <ボード> …`（[対応ボード](#対応ボード)）。

| フラグ | 既定 | 意味 |
|---|---|---|
| `-DSAAN_UI=avatar/text` | **avatar** | **起動時**の画面。顔と文字は両方ファームに入っていて、**長押し / ボタン B / シリアル `/ui`** で実行時に切り替えられる。avatar = m5stack-avatar の顔 + 吹き出し + リップシンク（128 x 128 では scale 0.4）/ text = 本家と同じ 3 段の文字画面（128 x 128 では詰め配置）。画面の無い stampc5 は text 固定 |
| `-DSAAN_BOARD=cores3/core2/basic/atoms3/atoms3r/stampc5` | **cores3** | ボード（下の「対応ボード」）。`idf_board.sh <ボード>` が sdkconfig.<ボード> と組で渡す |
| `-DSAAN_DICT=438750/228000/135000/44000` | ボードごと | 辞書 13M / 8M / 4M / 2M（本家 Release の `k1-dict-*.bin`。`scripts/get_dict.sh all` で取る）。既定はボードの dict パーティションに入る最大: cores3 438750 / atoms3・atoms3r 135000 / core2・basic・stampc5 44000。入らない組み合わせは CMake が止める |
| `-DSAAN_ENABLE_PIE=0/1` | **1**（S3） | W8A8 + ESP32-S3 の整数 SIMD (PIE)。0 = W8A32 / 移植可能 C。core2 / basic / stampc5 は PIE 命令が無いので 0 に固定。`-DSAAN_W8A8_NOPIE=1` で PIE 無しの W8A8（スカラ実装。checksum は PIE と同じ）にもできる |
| `-DSAAN_BUFFERED=0/1` | **0** | 0 = プリロール（4 チャンク = 371 ms）後に計算しながら鳴らす / 1 = 全部貯めてから鳴らす（途切れない） |
| `-DSAAN_BOOT_SPEAK=0/1` | **1** | 起動時に 1 文喋る |
| `-DSAAN_KANJI=0/1` | **1** | 端末内漢字 G2P（辞書 + Open JTalk）。0 で外すと入力はかな中間表現だけ、辞書も焼かない |
| `-DSAAN_CORE_IRAM=0/1` | **1**（S3） | 推論コアの `.text` を IRAM に置く（約 10 KB。旧コアで −2.4%）。core2 / stampc5 は 0 |
| `-DSAAN_OJ_PSRAM=0/1` | **1** | Open JTalk の一時ヒープを PSRAM に向ける（0 は陽性対照。内部 DRAM が減るのを見る）。PSRAM の無い ATOMS3 / Stamp-C5 では内部 DRAM に落ちる |
| `-DSAAN_PROFILE=0/1` | **0** | 段別プロファイル（CCOUNT）を発話後に出す。**速度の報告には 0 で**（計測にコストがある） |

ビルド環境なしで焼くだけなら、ボード × 辞書の一括イメージと app が [`firmware/`](firmware/README.md) にある
（`esptool.py write_flash 0x0 …` で焼く。辞書込みの一括イメージは大きいので git には入れず、
`scripts/make_images.sh` で作る）。

`-D` の値は `build/` を消すまで CMake キャッシュに残る。顔と文字画面は 1 つのファームに両方入っているので、
焼き直さずに切り替えられる（長押し / ボタン B / `/ui`）。`-DSAAN_UI=text` は起動時に文字画面を出す指定。

### 対応ボード

| ボード | チップ / PSRAM / flash | 画面 | もう一度喋る | スピーカー | 辞書（13M=438750 / 8M=228000 / 4M=135000 / 2M=44000 語） | 実機確認 |
|---|---|---|---|---|---|---|
| **CoreS3**（既定） | S3 / 8 MB Quad / 16 MB | 顔 ⇄ 文字 | タッチ | 内蔵 AW88298 | 13M / 8M / 4M / 2M（dict 14.6 MB） | ✅ 2026-09-07〜10 |
| **ATOMS3** | S3 / 無し / 8 MB | 顔（scale 0.4）⇄ 文字 128 x 128 | 本体ボタン | **Atomic Voice Base**（ES8311 + NS4150B。旧名 Atomic Echo Base） | 4M / 2M（dict 6.2 MB） | ✅ 2026-09-10（文字 UI で確認。顔はビルドのみ。音は人が聴いて確認すること） |
| **ATOMS3R** | S3 / 8 MB **Octal** / 8 MB | 同上 | 本体ボタン | 同上 | 4M / 2M | ⚠️ ビルドのみ |
| **Core Basic**（V2.6 以降） | **ESP32** / **無し** / 16 MB | 顔 ⇄ 文字 | ボタン A | 内蔵 DAC (GPIO25) + アンプ | 4M / 2M（dict 3 MB） | ⚠️ ビルドのみ。**arena 176 KB の置き場が無い見込み**（下） |
| **Core2** | **ESP32** / 8 MB / 16 MB | 顔 ⇄ 文字 | タッチ | 内蔵 NS4168 | 4M / 2M（dict 3 MB。4M は mmap の窓に入らないかもしれない） | ⚠️ ビルドのみ |
| **Stamp-C5** | **ESP32-C5**（RISC-V）/ 無し / 4 MB | 無し | 無し（シリアル入力のみ） | **外付け I2S DAC**（BCLK G5 / WS G6 / DOUT G7。`-DSAAN_I2S_GPIO_*` で変更） | 2M（dict 2.4 MB） | ⚠️ ビルドのみ |

CoreS3 のファイルはそのままで、ボードごとに `sdkconfig.<ボード>` / `partitions_<ボード>.csv` を足し、`-DSAAN_BOARD` で
切り替える（`idf_board.sh` が組で渡す）。ボードごとに build ディレクトリが分かれる（`build/`、`build_atoms3/` …）。

```sh
scripts/get_dict.sh all                                   # 辞書 4 種を model/ に取る
./idf_board.sh atoms3  -p /dev/ttyACM0 flash monitor      # ATOMS3 + Voice Base（辞書 135000）
./idf_board.sh atoms3  -DSAAN_DICT=44000 build            # 辞書を替える
./idf_board.sh atoms3r -p /dev/ttyACM0 flash monitor      # ATOMS3R
./idf_board.sh core2   -p /dev/ttyUSB0 flash monitor      # Core2（UART コンソール）
./idf_board.sh basic   -p /dev/ttyUSB0 flash monitor      # Core Basic（同上）
./idf_board.sh stampc5 -p /dev/ttyACM0 flash monitor      # Stamp-C5（外付け I2S DAC）
scripts/make_images.sh                                    # 全ボード × 入る辞書の一括イメージ → firmware/<日付>_images/
```

- **ATOMS3 / ATOMS3R**: 本体にスピーカーが無いので Atomic Voice Base を M5Unified の
  `external_speaker.atomic_echo` で有効にする（I2S G8/G6/G5、ES8311 は I2C G38/G39）。M5Unified は Base の
  有無を probe しないので、**Base を外すと無音のまま正常終了する**。PSRAM 無しの ATOMS3 では音声バッファ
  28 KB と Open JTalk のヒープが内部 DRAM に落ちる（起動ログの WARN は正常。1 発話後の空き 110 KB）。
- **Core Basic**: Core2 と同じ ESP32 だが PSRAM が無い。arena 176 KB は .bss に入らず、内部ヒープの連続
  ブロックも ESP32 では 110 KB 程度なので、**起動時に「arena を確保できない」で止まる見込み**（実機未確認）。
  コアの arena を分割できるまでは動かない前提で、ビルド設定だけ用意してある。初代 Basic（flash 4 MB）は対象外。
- **Stamp-C5**: 画面・スピーカー・ボタンが無いので、外付けの I2S DAC/アンプ（MAX98357A など）を G5/G6/G7 に繋ぎ、
  シリアルから文を入れる。RISC-V なので W8A32（checksum `0xe4b645c30835d42d`）。速度は**未測定**。
- **Core2**: ESP32 には PIE が無いので W8A32（checksum の期待値は `0xe4b645c30835d42d`）。arena 176 KB は
  .bss に入らず PSRAM から取る（遅い。**xRT は未測定**で、ストリーミングでは途切れる前提。`-DSAAN_BUFFERED=1`
  を勧める）。flash の mmap 窓が 4 MB しかないので辞書は 3 MB まで。

シリアルポート: CoreS3 / ATOMS3 / ATOMS3R / Stamp-C5 は native USB なので `/dev/ttyACM0`（権限が無ければ
`SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666"` を udev に）。Core2 は CP2104 経由の `/dev/ttyUSB0`。

### M5Stack Tab5（ESP32-P4）

Tab5 向けは別リポジトリに分けた: **[SanoTTS-jp-Tab5](https://github.com/nnn112358/SanoTTS-jp-Tab5)**
（Tab5 Keyboard でローマ字入力、横画面、P4 向けの設定）。

## 現状

**2026-09-04 に推論コアを sanoTTS-jp origin/main（d169e91）へ同期した。** 本家はコアの速度を
作り直し（S1〜S5b / T1〜T5。すべて出力を変えない変更）、同じ CoreS3（顔なしの本家 M5 構成）で
**定常 xRT 0.926 → 0.446**、arena 208 → 176 KB、途切れ 0 を実測している（本家 M-89 / M-90）。
入力も本家に合わせて **1 経路**になった: `saan_g2p_classify()` が「かな中間表現 / 漢字かな交じり文 /
拒否」を決めるので、前置記号は要らない（`=` でかな、`!` で辞書に**強制**する試験用の経路は残してある）。

2026-09-07 にこの板で実測した（[`docs/measurements.md`](docs/measurements.md)）: **顔ありの既定ビルドで
定常 xRT 0.445 / 追い越し 0 / checksum `0xa69a7ebbb5ccb05f`（本家 QEMU・本家 M5 実機と一致）**。
先読みは 2 チャンク（186 ms）で足り、**発話開始まで 330 ms**（旧コアは 1.8 s）。
**2026-09-10 に再生の給餌方式を本家の M5 実装（`saan_audio_m5.cpp`）と同じにした**（固定プリロール
4 チャンク + 2,048 sample × 3 枚のリング、チャンクごとに `playRaw`。xRT からの先読み自動決定は外した）。
この方式で CoreS3（顔あり）は **定常 xRT 0.434 / アンダーラン 0 / 発話開始まで 424 ms**、
ATOMS3 + Atomic Voice Base（PSRAM 無し）は **0.427 / 0 / 406 ms**（checksum はどちらも `0xa69a7ebbb5ccb05f`）。
発話開始が 330 → 424 ms に伸びたのはプリロールが 2 → 4 チャンクになったぶん。
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
- [`docs/design-notes.md`](docs/design-notes.md) — 構成、入力仕様、再生パイプライン、CoreS3 で踏んだことと対処
- [`docs/upstream-comparison.md`](docs/upstream-comparison.md) — 公式実装 Ampixa/sanoTTS との違いと、0.22× RT に至った手順（公開文書とログのみ）

## 出所とライセンス

| | 出所 | ライセンス |
|---|---|---|
| 推論コア・ファーム本体・スクリプト | [sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp) origin/main d169e91（`csrc/`, `esp32/main/`, `scripts/`） | MIT |
| M5Stack 向けの変更（M5.Speaker / 顔 / タッチ / ボードと辞書の切り替え） | このリポジトリ | MIT（[`LICENSE`](LICENSE)） |
| **`model/student_i8.bin`** | **sanoTTS-jp Release v0.3.0 `saanotts-jp-v3-int8.bin`**（blob v2、SHA-256 `2d2b8543…`、[`model/README.md`](model/README.md)） | **sanoTTS-jp Model License 1.0** |
| M5Unified / M5GFX | ESP-IDF Component Registry | MIT（日本語フォントは IPA Font License） |
| m5stack-avatar 0.10.0（`components/m5stack-avatar/`） | [stack-chan/m5stack-avatar](https://github.com/stack-chan/m5stack-avatar)（vendored） | MIT |
| Open JTalk（`components/saanotts_core/openjtalk/`、無改変） | sanoTTS-jp 経由 | 修正 BSD |

ライセンス全文は [`LICENSES/`](LICENSES/)、モデルの帰属表示ブロックと生成音声の用途制限は [`NOTICE.md`](NOTICE.md)。
