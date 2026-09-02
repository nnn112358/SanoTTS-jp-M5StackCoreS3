# 設計メモ

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

## 流れ

```
かな中間表現 ─▶ G2P (g2p.c, 表 877 B) ─▶ 音素 ID ─▶ Duration ─▶ Acoustic ─▶ iSTFT Decoder ─▶ PCM 22.05 kHz
   "きょ][おわよ]…"                         53 ids       36 K params  200 K params   331 K params      ─▶ M5.Speaker
```

- 3 つの生徒モデルは 40 次元の潜在 (c-line) で繋がる。合成は 8 frames (2,048 sample) の
  チャンク単位でストリーミング（`saan_stream_pull`）
- 重み blob は SAAN v1 形式（183 tensors、int8 + per-tensor scale）。ビルド時に
  `scripts/blob_to_header.py` が `const uint8_t[] __attribute__((aligned(16)))` にして
  app の `.rodata` に入れる。ESP32-S3 では `.rodata` は flash（XIP）なので SRAM を食わない
- 1 発話の流れ（`main.c` の `synth_once`）: 静的 arena 208 KB で `saan_stream_init` →
  チャンクを pull して int16 へ → ストリーミング（4 チャンク先読みしてから鳴らす）または
  貯めて再生（全チャンクを PSRAM に貯めてから 1 回の `playRaw`）

## 入力仕様

シリアル入力は**かな中間表現**のみ。漢字・カタカナ・句読点は受け付けない（端末に辞書が無い）。

```
きょ][おわよ][いて][んきです°ね      ← 今日は良い天気ですね。
[ 上昇  ] 下降核  # 句境界  ° 無声化  ? ?! ?. ?~ 疑問
```

- 漢字混じり文からの変換は sanoTTS-jp リポジトリの `uv run python scripts/to_intermediate.py "文"`
- アクセント記号を省くと平板になる（音は出るが正しい抑揚ではない）
- 上限 350 ids（学習分布の上限。arena は 520 ids まで持つが、その外は分布外なので拒否する）
- 未知の文字は**黙って無音にせず**、位置を示してエラーにする

## CoreS3 で踏んだこと（sdkconfig.defaults / partitions.csv の根拠）

| 事象 | 対処 |
|---|---|
| `.dram0.bss` が 10,096 B 溢れてリンクできない | 音声バッファ 28,672 B を static からヒープ確保（PSRAM 優先）に。IRAM のコードを flash へ（`FREERTOS/HEAP/RINGBUF_PLACE_*_INTO_FLASH`, `SPI_FLASH_ROM_IMPL`, `SPI_MASTER_ISR_IN_IRAM=n`）。`set(COMPONENTS main)` で不要なコンポーネントを外す |
| `esp_partition_mmap` が `ESP_ERR_NO_MEM`（3 MB でも 1 MB でも） | PSRAM 8 MB が data 用 vaddr を占有する。**重みはヘッダ化して app の `.rodata` に**（起動時に DROM としてマップされるので競合しない） |
| S3 のキャッシュ設定と DRAM | `dram0_0_seg` は 341,760 B 固定で、D-cache を 64 KB にしても減らない（64+32 KB と 32+32 KB で overflow が同じ 10,096 B）。無料なので 64 KB |
| M5 の 22.05 → 44.1 kHz リサンプル | `SAAN_SPK_OUT_RATE 22050`。AW88298 は 22.05 kHz 対応で、M5Unified が `rate_tbl` からレジスタ 0x06 (I2SSR) を設定する（M5Unified.cpp 566–581 行。CoreS3 の既定値も 22050） |
| `M5.Speaker.playRaw` はデータをコピーしない | 再生が終わるまでバッファを触らない。ストリーミングは 3 枚回し（キューは 2 枚）、貯める方式は `stop()` で再生完了を待ってから解放 |
| タッチ (FT6336) とアンプ (AW88298) が同じ I2C バス | `M5.update()` と描画は合成タスクからだけ呼ぶ。シリアル入力は 20 ms タイムアウトの poll にして、待ちの間にタッチを見る |
| `CONFIG_SPIRAM_MODE_OCT` だとブートループ | CoreS3 は **Quad**（`octal_psram: PSRAM chip is not connected` → abort） |
| `idf.py monitor` に何も出ない | USB-UART ブリッジ無し → `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` |
| 別バージョンの ESP-IDF を有効化した端末からビルドすると落ちる | `IDF_PYTHON_ENV_PATH` を引き継ぐため。`idf.sh` が unset してから `export.sh` を読む |

## 途切れない再生の条件

ストリーミングで途切れない条件は `プリロール ≥ xRT × 音声長 / (1 + xRT)`。
xRT = 1.55 なら音声の 61% を先に貯める必要があり、待ちは 1.6 s 程度にしかならない。
そこで `-DSAAN_BUFFERED=1` は全部貯めてから鳴らす（待ち ≒ 音声長 × 1.55 + 0.77 s）。
ストリーミングのまま途切れなくするには合成をあと 1.56 倍速くする必要がある。
