# 設計メモ

## 構成

```
CMakeLists.txt              PIE 既定 ON、blob のパス、COMPONENTS=main、辞書を dict パーティションへ
partitions.csv              16 MB flash / factory 2 MB / dict 14.6 MB（重みは app の .rodata に入る）
sdkconfig.defaults          CoreS3（Quad PSRAM / USB Serial-JTAG / QIO / D-cache 64 KB・64 B 行）+ sanoTTS 向け設定
idf.sh                      ESP-IDF v5.5.5 をクリーンな環境で有効化して idf.py を呼ぶ
idf_board.sh                ボードを選んで idf.py を呼ぶ（cores3 | atoms3 | atoms3r | core2 | basic | stampc5。build_<ボード>/ に分ける）
sdkconfig.atoms3 / .atoms3r / .core2 / .basic / .stampc5   ボードごとの上書き（defaults に重ねる）。partitions_{atoms3,core2,stampc5}.csv も
scripts/make_images.sh      板 × 辞書の一括イメージを firmware/<日付>_images/ に作る
components/saanotts_core/   sanoTTS-jp csrc のコピー（origin/main d169e91、2026-09-04）+ arena の複数ブロック対応（下）
  saanotts*.c fft.c         C99 推論コア（4 ファイル）。saan_prof.h（段別プロファイラ）、erf_table.h（GELU の表）
  g2p.c line.c              端末側かな G2P（saan_g2p_classify で経路判定も）と行編集
  jdict.c accent.c njd_rules.c label_ids.c   端末内漢字 G2P（旧 k1dict / k4_accent / k4b_njd / k7_label2ids）
  openjtalk/                Open JTalk（無改変、修正 BSD）
  saan_port_esp32.h         配置の注入点（SAAN_HOT_DATA → DRAM_ATTR。erf 表を内部 DRAM に）
  oj_heap_psram.{h,c}       Open JTalk の calloc/strdup/free を PSRAM 優先に（-include で当てる。ソース無改変）
  linker.lf                 コアの .text を IRAM に（-DSAAN_CORE_IRAM=0 で外す）
main/
  main.c                    起動 → セルフテスト → 表示 → 発話 → 入力/タッチのループ（経路判定は speak_auto）
  saan_model.{c,h}          .rodata の重み blob を開く（v2 でないと SAAN_ERR_VERSION）
  saan_speaker.{h,cpp}      M5.Speaker 出力（22.05 kHz 直接。プリロール + 3 枚リング = 本家 M5 実装と同じ。PCM 統計とリップシンク包絡もここ）
  saan_ui.h saan_ui.cpp     画面と入力の API と振り分け（顔 ⇄ 文字を実行時に切り替え。長押し / ボタン B / `/ui`）
  saan_ui_impl.h            2 つの実装が振り分けに見せる内側の API（init / enter / leave / …）
  saan_ui_avatar.cpp        顔（m5stack-avatar）+ 吹き出し + リップシンク。128 x 128 は scale 0.4
  saan_ui_text.cpp          文字だけ（本家 saan_ui_m5.cpp と同じ 3 段。128 x 128 は詰め配置、画面なしは描かない）
  saan_ui_headless.c        画面もボタンも無いボード（Stamp-C5）用のスタブ。M5 系をリンクしない
  saan_speaker_i2s.c        M5Unified を使わない音声出力（driver/i2s_std 直叩き。本家 saan_i2s.c の移植。Stamp-C5）
  saan_console.{c,h}        シリアル `かな> ` 入力（タイムアウト付き poll。本家のコピー）
  saan_dict.{c,h}           辞書パーティションを貼る（本家のコピー。ROM_IMPL=y なら esp_mmu_map）
  saan_kanji.{c,h}          漢字文 → 音素 ID（本家のコピー。作業領域は合成 arena を借りる）
  demo_ids.h                起動セルフテストの錨（かな → ids）
model/student_i8.bin        重み（blob v2、654,032 B。model/README.md）
scripts/blob_to_header.py   blob → const uint8_t[] ヘッダ（ビルド時に自動実行）
scripts/get_dict.sh         辞書 blob を Release から取る
NOTICE.md  LICENSES/        帰属表示とライセンス全文
```

## 流れ

```
かな中間表現 ─▶ G2P (g2p.c, 表 877 B) ─▶ 音素 ID ─▶ Duration ─▶ Acoustic ─▶ iSTFT Decoder ─▶ PCM 22.05 kHz
   "きょ][おわよ]…"                         53 ids       36 K params  200 K params   331 K params      ─▶ M5.Speaker
```

- 3 つの生徒モデルは 40 次元の潜在 (c-line) で繋がる。合成は 8 frames (2,048 sample) の
  チャンク単位でストリーミング（`saan_stream_pull`）。本家 T2 以降は**出力に効く範囲だけ**計算する
- 重み blob は SAAN **v2** 形式（183 tensors、int8 + per-tensor scale。int8 conv 重みは
  `[cout][k][align16(cin)]` で 0 埋め。PIE が転置コピー無しに直接読む）。ビルド時に
  `scripts/blob_to_header.py` が `const uint8_t[] __attribute__((aligned(16)))` にして
  app の `.rodata` に入れる。ESP32-S3 では `.rodata` は flash（XIP）なので SRAM を食わない。
  v1（旧 Release）はコアが `SAAN_ERR_VERSION` で拒む
- 1 発話の流れ（`main.c` の `synth_once`）: 静的 arena **176 KB**（本家 T4 で 208 KB から下げた）で
  `saan_stream_init` → `a.used` を `saan_stream_arena_used(n_ids)` と突き合わせる（黙って確保に
  失敗していないか）→ 4 チャンクをプリロール → `saan_speaker_start()` → 以後チャンクごとに
  `saan_speaker_write_f32()`（M5 のキューが満杯ならブロック）。`-DSAAN_BUFFERED=1` なら
  全チャンクを PSRAM に貯めてから 1 回の `playRaw`

## 入力仕様

シリアルの 1 行は **`saan_g2p_classify()`（本家 K-B）が 3 値に分ける**。前置記号は要らない。

| 判定 | 条件 | どうなるか |
|---|---|---|
| **かな** | かな G2P の凍結テーブルのトークナイザが行末まで通る | `saan_g2p()` → 合成 |
| **辞書** | 通らず、中間表現のマーク（`[ ] # ° _ ^ $ ? ?! ?. ?~`）が 1 つも無い | 端末内の辞書 + Open JTalk で読む（`-DSAAN_KANJI=0` なら喋らずに理由を出す） |
| **拒否** | 通らないのにマークが混じっている | 喋らない。位置と文字を出す |

```
きょ][おわよ][いて][んきです°ね      ← かな。今日は良い天気ですね。（本家 QEMU との突き合わせ用）
今日は良い天気ですね。                ← 辞書。同じ PCM が出る
こんにちわ                            ← かな（ひらがなだけでも通る）
きょ][おわよ][いて][んきです°ね。     ← 拒否（マーク + 句点。黙って辞書に回すとそれらしい音が出てしまう）
[ 上昇  ] 下降核  # 句境界  ° 無声化  ? ?! ?. ?~ 疑問
```

- 判定は手書きの文字集合ではなく「トークナイザが通るか」そのもの。ホスト側 `scripts/kana_g2p.py` の
  `classify_route()` と同じ規則で、本家の `kb_route_parity.py`（held-out 298 文）が一致を守る
- **強制（試験用）**: `=` 前置で判定を通さずにかな中間表現として、`!` 前置で辞書経路として扱う
  （`=` は同期前のこのリポジトリの仕様、`!` は本家と同じ）
- アクセント記号を省くと平板になる（音は出るが正しい抑揚ではない）
- 上限 350 ids（学習分布の上限。その外は分布外なので拒否する）
- 未知の文字は**黙って無音にせず**、位置を示してエラーにする

## CoreS3 で踏んだこと（sdkconfig.defaults / partitions.csv の根拠）

| 事象 | 対処 |
|---|---|
| `.dram0.bss` が 10,096 B 溢れてリンクできない | 音声バッファ 28,672 B を static からヒープ確保（PSRAM 優先）に。IRAM のコードを flash へ（`FREERTOS/HEAP/RINGBUF_PLACE_*_INTO_FLASH`, `SPI_FLASH_ROM_IMPL`, `SPI_MASTER_ISR_IN_IRAM=n`）。`set(COMPONENTS main)` で不要なコンポーネントを外す |
| `esp_partition_mmap` が `ESP_ERR_NO_MEM`（3 MB でも 1 MB でも） | **真因は `CONFIG_SPI_FLASH_ROM_IMPL=y`**（IDF の mmap がコンパイルから外れ、ROM の実装 `0x40000bac` に差し替わり、IDF がそれに渡すプールは 128 ページ = 8 MB。`flash_mmap.c:53`）。当時は PSRAM の vaddr と誤診して重みをヘッダ化した（設計としては正しいので残す）。一度 `ROM_IMPL=n` にして 13.7 MB の辞書 mmap を通したが、**2026-09-04 に `y` へ戻した**: 本家の `saan_dict.c` が `ROM_IMPL=y` のとき `esp_mmu_map`（component `esp_mm`。ROM 実装と無関係で上限に当たらない）に自動で切り替えるようになり、本家が CoreS3 の同じ組み合わせで動かしている（M-90）。内部 DRAM が約 9 KB 空く。✅ 2026-09-07 に本家 v0.3.0 の M5 イメージをこの板で焼き、`esp_mmu_map OK` / `辞書 OK` を確認（docs/measurements.md） |
| S3 のキャッシュ設定と DRAM | `dram0_0_seg` は 341,760 B 固定で、D-cache を 64 KB にしても減らない（64+32 KB と 32+32 KB で overflow が同じ 10,096 B）。無料なので 64 KB |
| M5 の 22.05 → 44.1 kHz リサンプル | `SAAN_SPK_OUT_RATE 22050`。AW88298 は 22.05 kHz 対応で、M5Unified が `rate_tbl` からレジスタ 0x06 (I2SSR) を設定する（M5Unified.cpp 566–581 行。CoreS3 の既定値も 22050） |
| `M5.Speaker.playRaw` はデータをコピーしない | 再生が終わるまでバッファを触らない。ストリーミングは 3 枚回し（キューは 2 枚）、貯める方式は `stop()` で再生完了を待ってから解放 |
| タッチ (FT6336) とアンプ (AW88298) が同じ I2C バス | `M5.update()` と描画は合成タスクからだけ呼ぶ。シリアル入力は 20 ms タイムアウトの poll にして、待ちの間にタッチを見る |
| `CONFIG_SPIRAM_MODE_OCT` だとブートループ | CoreS3 は **Quad**（`octal_psram: PSRAM chip is not connected` → abort） |
| `idf.py monitor` に何も出ない | USB-UART ブリッジ無し → `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` |
| 別バージョンの ESP-IDF を有効化した端末からビルドすると落ちる | `IDF_PYTHON_ENV_PATH` を引き継ぐため。`idf.sh` が unset してから `export.sh` を読む |

## 途切れない再生の条件

**2026-09-10 に給餌方式を本家の M5 実装と同じにした**（次の「再生パイプライン」）。この方式では
合成は M5 のキュー（2 枚）より先に進めないので、**途切れない条件は xRT < 1**（1 チャンクの合成が
そのチャンクの音声長より短い）。プリロール（4 チャンク = 371 ms）は初回 pull の遅れを吸収するだけで、
合成が遅いときの貯金にはならない。xRT > 1 なら `-DSAAN_BUFFERED=1`（全部貯めてから鳴らす）。

2026-09-07 の実測で顔ありの既定ビルドは定常 xRT 0.445 なので、条件は十分に満たしている。

履歴: 2026-09-04 の同期前は xRT 1.55 で再生に追いつかず、「プリロール P ≥ 音声長 T × (1 − 1/xRT)」
（時刻 t での貯まり P + t/xRT が再生位置 t を常に上回る）から前の発話の実測 xRT で先読み量を決め、
発話バッファ 1 本を 2 区間で渡す方式にしていた。xRT < 1 になってその仕組みは要らなくなったので、
本家に合わせて外した（⚠️ かつて「61%（P ≥ xRT·T/(1+xRT)）」と書いたのは**誤り**だった）。

## 顔とリップシンク（m5stack-avatar）

画面の実装は 2 つあり、**両方がファームに入っていて実行時に切り替える**（`saan_ui.cpp` が振り分け。長押し /
ボタン B / シリアル `/ui`。`-DSAAN_UI=avatar|text` は起動時にどちらを出すか）。
既定の `avatar` は [m5stack-avatar](https://github.com/stack-chan/m5stack-avatar)（`components/m5stack-avatar/`、
MIT、v0.10.0 を vendored）が描く。文は吹き出し（右下）に出す。
`text` は `saan_ui_text.cpp`（M5GFX だけ。上段に文、下段にステータス、中段に出典 = 本家と同じ）で、合成タスクからだけ
描く。文字画面の間は avatar の drawLoop を suspend しておき（facialLoop と lip_task は回るが描かない）、顔に戻すと
resume で画面全体を描き直す。以下は `avatar` の話。

```
合成タスク (core 0, 優先度 1)            avatar drawLoop (10 ms) / facialLoop (33 ms) / lip_task (core 1)
  saan_stream_pull → f32                      10 ms ごと:
  → conv_block(): int16 化 + 256 sample ごとの   r = saan_speaker_level_now()
    RMS を包絡 s_env[] に書く（PSRAM）           avatar.setMouthOpenRatio(r)
  → playRaw（開始時刻 s_play_t0 を記録）
```

- **口の開き = いま鳴っているサンプル位置の RMS ÷ その発話の最大 RMS**（0..1）。
  再生位置は「鳴らし始めた時刻 + 経過時間 − DMA 遅れ 45 ms（仮置き）」で推定する。
  RMS の床（RMS < 96）は 0 にして息の音で口が震えないようにした
- 更新頻度（2026-09-07 に上げた）: lip_task 33 → **10 ms**（avatar の drawLoop と同じ）、包絡 512 →
  **256 sample**（11.6 ms）、隣のブロックとの**線形補間**で段差を消した。次のブロックは変換済みの
  ときだけ使う（未変換は 0 なので、合成が再生に近いと口が閉じてしまう）。
  CPU は core 1 で sqrtf が 256 sample に 1 回増えるだけ。`SAAN_LIP_PERIOD_MS` で変えられる
- 包絡は変換のたびに書くので、貯めてから鳴らす方式（既定）では**再生前に発話全体の包絡が
  揃っている**。ストリーミングでは途切れた瞬間に `isPlaying()` が false → 口を閉じ、
  次のチャンクを送るときに時刻を取り直す（正確なのは貯める方式）
- **core の割り当てが要点。** 合成は数秒間 CPU を手放さないので、同じ core に顔の描画や
  リップシンクを置くと固まる。合成タスクを core 0 / 優先度 1 に固定し、avatar の
  タスク（drawLoop 1 / facialLoop 2 / lip_task 2）は core 1。M5.Speaker のタスク
  （優先度 2、affinity 無し）はどちらでも動ける
- 吹き出しは 14 文字で切って「…」。合成中は「…」+ Doubt、再生中は文 + Happy、
  待機は「タッチでもう一度」+ Neutral
- タッチ (I2C) は引き続き合成タスクの `saan_ui_poll_touch()` からだけ読む。avatar は
  SPI（ディスプレイ）しか触らない
- ESP-IDF（Arduino 無し）で通すための差分は
  [`components/m5stack-avatar/README.md`](../components/m5stack-avatar/README.md)
  （`xTaskCreateUniversal` → `xTaskCreatePinnedToCore`、`random(long)` の補い、
  本家の `-Wreorder/-Wswitch` 警告をそのコンポーネントだけエラーにしない）

## 再生パイプライン（本家 boards/m5unified の saan_audio_m5.cpp と同じ）

- **プリロール**: 発話ごとに `SAAN_SPK_PREROLL_SAMPLES`（8,192 sample = 4 チャンク）ぶんのバッファを
  PSRAM に取り（`saan_speaker_begin_utterance`）、変換して貯める（`saan_speaker_preroll_push`）。
  `saan_speaker_start()` がそれを 1 回の `playRaw` で渡す
- **定常**: 2,048 sample × 3 枚のリング（起動時に確保、解放しない）。チャンクごとに変換して
  `playRaw`（`saan_speaker_write_f32`）。M5 のキュー（1 ch あたり `wav_info_t wavinfo[2]`）が満杯なら
  `_set_next_wav` がセマフォ待ちで**ブロック**するので、合成ループの流量制御はこれに任せる。
  生きているポインタは最大 2 本（current + next）なので 3 枚あれば書き込み先は必ず再生済み
- **停止**: `saan_speaker_stop()` が `isPlaying()` が 0 になるまで待ってからプリロールを解放する
  （`playRaw` はポインタを持つだけなので、先に free すると解放済みメモリを鳴らす）

```
pull × 4 → preroll_push（int16 化 + 包絡）→ start()（貯めたぶんを 1 回で渡す）
以後 pull → write_f32（リングに変換 → playRaw。満杯ならブロック）
eos → stop()（再生完了を待って解放）
```

- アンダーランの定義は本家と同じ「pull の計算時間 > そのチャンクの音声長」（ログの `アンダーラン N 回`）
- `-DSAAN_BUFFERED=1` はプリロール = 発話全体（発話開始まで = 合成時間、途切れない）
- 以前の方式（発話バッファ 1 本を「貯めたぶん + 残り全部」の 2 区間で渡し、`pump()` で追い越しを
  監視する）は 2026-09-10 に外した。**「キューに空きができたら続きを渡す」ポーリング方式は
  途切れる**（渡した瞬間に 2 枚とも埋まっていると次の機会までに尽きる）が、本家の方式は playRaw の
  **ブロック**で空いた瞬間に渡すので、その穴は無い

## arena の複数ブロック対応（コアへの独自変更）

ESP32（Core2 / Basic）は静的 .bss に 176 KB が入らず、PSRAM の無い Basic では内部ヒープの連続ブロックも
100〜127 KB 程度しか無い。そこで `saan_arena` を複数ブロックから取れるようにした（本家 csrc には無い。
`saanotts.h` / `saanotts.c` の `saan_arena_add()`）。

- 確保は **first-fit**（入る最初のブロックへ）。`used` は「生きている確保の合計」のままなので、1 本のときと
  同じ値になり、`saan_stream_arena_used()` との突き合わせ（main.c の二重防御）も複数ブロックで効く
- コアの mark / rollback（`mark = a->used; … a->used = mark;`、`a->used -= n`）は触っていない。次の `saan_alloc`
  が **LIFO の履歴**（SAAN_ARENA_HIST = 128 件）を巻き戻して各ブロックのカーソルに反映する。境界に合わない
  戻し方は粘着失敗（コアの確保は LIFO なので起きない）
- main.c（SAAN_ARENA_HEAP）は PSRAM 1 本 → 内部 1 本 → 内部の大きい塊から最大 4 本、の順に試す。各塊には
  SAAN_ARENA_HEAP_RESERVE（24 KB）を残す。漢字 G2P の Viterbi は連続領域が要るのでブロック 0 だけを貸す
- 漢字 G2P（`saan_kanji_to_ids_arena()`）も同じ arena から切り出す。固定長の配列は saan_alloc で個別に、
  Viterbi には残りが最大のブロックの残り全部。1 ブロックなら従来の `saan_kanji_to_ids()` と同じ配置
- ホストテスト `scripts/host/arena_regions_test.c`: 1 本 / 100+76 KB / 64 KB × 3 で PCM の FNV-1a が bit 一致、
  used も同じ。40+40 KB は `SAAN_ERR_ARENA` で止まる
- **QEMU（ESP32）で確認**（`scripts/qemu_basic.sh` → `scripts/qemu_run.py`。M5Unified は QEMU で動かないので
  M5 無しの経路 `-DSAAN_HEADLESS=1 -DSAAN_SKIP_I2S=1`）: ESP32 の内部 DRAM は 147 KB + 111 KB + 14 KB + 6 KB の
  塊で、arena は 94,208 + 86,016 B の 2 ブロックになる。辞書 mmap（3 MB。4 MB 窓の空き 3.2 MB）OK、
  合成 checksum `0xe4b645c30835d42d`、漢字入力（形態素 7 / 53 ids）OK、1 発話後の内部 DRAM 空き 70 KB /
  最大ブロック 26 KB。**M5 込みの実機はこれより厳しい**（M5GFX・avatar・M5.Speaker のタスクとバッファ）

```sh
cc -std=c99 -O2 -Icomponents/saanotts_core -Imain scripts/host/arena_regions_test.c \
   components/saanotts_core/{saanotts,saanotts_stream,fft,saanotts_int8}.c -lm -o /tmp/arena_test && /tmp/arena_test
```

## 端末内漢字 G2P（sanoTTS-jp K トラックの取り込み）

漢字かな交じり文を端末だけで音素 ID にする。本家 sanoTTS-jp origin/main（d169e91、2026-09-04）の
コードをそのまま `components/saanotts_core/`（`jdict.c` / `accent.c` / `njd_rules.c` /
`label_ids.c` / `openjtalk/`）と `main/`（`saan_dict.c` / `saan_kanji.c`）に持ってきた。
⚠️ 2026-09-03 に本家がファイル名を工程番号（`k1dict` / `k4_accent` / `k4b_njd` / `k7_label2ids`）から
責務ベースに変えた（53f8ef7）。API の接頭辞も `k1_` → `jdict_`、`k4_` → `accent_`、
`k4b_` → `njd_rules_`、`k7_` → `label_ids_` に変わっている。

```
文 ─▶ jdict_encode_key ─▶ jdict_analyze（LOUDS 辞書 + Viterbi、arena を借りる）─▶ jdict_entry_feature
   ─▶ mecab2njd ─▶ NJD 8 段（pronunciation / digit / accent_phrase / accent_type / unvoiced / long_vowel …）
   ─▶ njd2jpcommon ─▶ フルコンテキストラベル ─▶ label_ids_convert ─▶ 生徒の音素 ID（57 トークン）─▶ 合成
```

- 辞書 `k1-dict-438750.bin`（13,702,320 B、NAIST-jdic / UniDic を TTS 用に枝刈りした派生物、
  修正 BSD）は `dict` パーティション（0x210000〜、14.6 MB）を貼ってそのまま読む。
  貼り方は `saan_dict.c` が `CONFIG_SPI_FLASH_ROM_IMPL` で選ぶ: `y`（いまの既定）なら `esp_mmu_map`
  （ROM 実装の 128 ページ = 8 MB 上限に当たらない）、`n` なら `esp_partition_mmap`。
  PSRAM 8 MB と同じ MMU 窓（32 MB）を使うが、実測で flash mmap の空きは 22.8 MB あった
  （起動ログ `flash mmap の最大連続空き`）
- 作業領域は合成用の `g_arena`（176 KB）を借りる（G2P と合成は同時に走らない）。本家 T10(a) で
  固定長の配列（鍵・トークン・ラベル表・`label_ids` のトークン表 10,240 B）も全部 arena に移り、
  `.bss` に残るのはポインタ表 384 B だけ。`SAAN_ARENA_BYTES ≥ SAAN_KANJI_WORKBYTES` は `main.c` が
  コンパイル時に検査する
- Open JTalk の NJD / JPCommon は calloc を使う（本家 K-5 実測: 1 文ピーク約 105 KB）。本家 T10(b) の
  `oj_heap_psram.h` を `-include` で Open JTalk の .c だけに当て、calloc / strdup / free を PSRAM 優先に
  差し替える（ソースは無改変）。`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` のせいで素の calloc だと
  1 文ぶんがまるごと内部 DRAM に載るため。本家 CoreS3 実測で 1 発話あたりの内部 DRAM の減りが 3 KB になった
- 入力の経路は上の「入力仕様」（`saan_g2p_classify()`。前置記号は要らない）。
  **`-DSAAN_KANJI=0` で丸ごと外せる**（辞書リーダ・Open JTalk をビルドから外し、辞書も焼かない。
  入力はかな中間表現だけ。CMake キャッシュに残るので切り替えたら build/ を消す）
- 未知語は jdict_unk_guess が 1 文字ずつ読みを推測して平板で読む（無音で消えない）
- ⚠️ 枝刈りの代償: フル辞書と読みが変わる文が 17.79%（本家 M-74）。SCOREQ は変わらない
- 本家 3e5cf8e（2026-09-03）: 経路判定から半角 `?` を外した（普通の疑問文 42 行が拒否されていた。
  held-out の拒否 1.93% → 0%）
