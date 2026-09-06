# 設計メモ

## 構成

```
CMakeLists.txt              PIE 既定 ON、blob のパス、COMPONENTS=main、辞書を dict パーティションへ
partitions.csv              16 MB flash / factory 2 MB / dict 14.6 MB（重みは app の .rodata に入る）
sdkconfig.defaults          CoreS3（Quad PSRAM / USB Serial-JTAG / QIO / D-cache 64 KB・64 B 行）+ sanoTTS 向け設定
idf.sh                      ESP-IDF v5.5.5 をクリーンな環境で有効化して idf.py を呼ぶ
components/saanotts_core/   sanoTTS-jp csrc のコピー（origin/main d169e91、2026-09-04）
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
  saan_speaker.{h,cpp}      M5.Speaker 出力（22.05 kHz 直接。先読み自動 / 貯めて再生。PCM 統計もここ）
  saan_ui.{h,cpp}           顔（m5stack-avatar）とタッチ、リップシンク
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
  失敗していないか）→ チャンクを pull して int16 へ → 先読み量まで貯めてから鳴らし始める、または
  貯めて再生（全チャンクを PSRAM に貯めてから 1 回の `playRaw`）

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
| `esp_partition_mmap` が `ESP_ERR_NO_MEM`（3 MB でも 1 MB でも） | **真因は `CONFIG_SPI_FLASH_ROM_IMPL=y`**（IDF の mmap がコンパイルから外れ、ROM の実装 `0x40000bac` に差し替わり、IDF がそれに渡すプールは 128 ページ = 8 MB。`flash_mmap.c:53`）。当時は PSRAM の vaddr と誤診して重みをヘッダ化した（設計としては正しいので残す）。一度 `ROM_IMPL=n` にして 13.7 MB の辞書 mmap を通したが、**2026-09-04 に `y` へ戻した**: 本家の `saan_dict.c` が `ROM_IMPL=y` のとき `esp_mmu_map`（component `esp_mm`。ROM 実装と無関係で上限に当たらない）に自動で切り替えるようになり、本家が CoreS3 の同じ組み合わせで動かしている（M-90）。内部 DRAM が約 9 KB 空く。⚠️ このリポジトリの実機では未確認 |
| S3 のキャッシュ設定と DRAM | `dram0_0_seg` は 341,760 B 固定で、D-cache を 64 KB にしても減らない（64+32 KB と 32+32 KB で overflow が同じ 10,096 B）。無料なので 64 KB |
| M5 の 22.05 → 44.1 kHz リサンプル | `SAAN_SPK_OUT_RATE 22050`。AW88298 は 22.05 kHz 対応で、M5Unified が `rate_tbl` からレジスタ 0x06 (I2SSR) を設定する（M5Unified.cpp 566–581 行。CoreS3 の既定値も 22050） |
| `M5.Speaker.playRaw` はデータをコピーしない | 再生が終わるまでバッファを触らない。ストリーミングは 3 枚回し（キューは 2 枚）、貯める方式は `stop()` で再生完了を待ってから解放 |
| タッチ (FT6336) とアンプ (AW88298) が同じ I2C バス | `M5.update()` と描画は合成タスクからだけ呼ぶ。シリアル入力は 20 ms タイムアウトの poll にして、待ちの間にタッチを見る |
| `CONFIG_SPIRAM_MODE_OCT` だとブートループ | CoreS3 は **Quad**（`octal_psram: PSRAM chip is not connected` → abort） |
| `idf.py monitor` に何も出ない | USB-UART ブリッジ無し → `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` |
| 別バージョンの ESP-IDF を有効化した端末からビルドすると落ちる | `IDF_PYTHON_ENV_PATH` を引き継ぐため。`idf.sh` が unset してから `export.sh` を読む |

## 途切れない再生の条件

ストリーミングで途切れない条件は **`プリロール P ≥ 音声長 T × (1 − 1/xRT)`**（時刻 t での貯まり P + t/xRT が
再生位置 t を常に上回る。最も厳しいのは t = T）。
xRT = 1.55 なら音声の **35.5%** を先に貯めれば以後は追い越されない。
⚠️ かつて「61%（P ≥ xRT·T/(1+xRT)）」と書いたが**誤り**。あれは「残りの合成が先読みぶんの
再生中に終わる」という強すぎる条件で、再生中も合成が進むことを勘定していなかった。
既定はこの先読み量（前の発話の実測 xRT × 1.15 + 2 チャンク）で計算しながら鳴らす。
`-DSAAN_BUFFERED=1` は全部貯めてから鳴らす（待ち ≒ 音声長 × xRT + 初回 pull）。

**2026-09-04 のコア同期後は xRT < 1 の見込み**（本家 CoreS3 顔なしで 0.446）。そのとき式の
(1 − 1/xRT) は負なので 0 に切り、先読みは **2 チャンク（4,096 sample = 186 ms）だけ**になる。
最初の発話の見込み値 `SAAN_XRT_INITIAL` は 1.2（旧 1.8）に下げた。実測して外れていたら直す。

## 顔とリップシンク（m5stack-avatar）

画面は [m5stack-avatar](https://github.com/stack-chan/m5stack-avatar)（`components/m5stack-avatar/`、
MIT、v0.10.0 を vendored）が描く。文は吹き出し（右下）に出す。

```
合成タスク (core 0, 優先度 1)            avatar drawLoop / facialLoop / lip_task (core 1)
  saan_stream_pull → f32                      33 ms ごと:
  → conv_block(): int16 化 + 512 sample ごとの   r = saan_speaker_level_now()
    RMS を包絡 s_env[] に書く（PSRAM）           avatar.setMouthOpenRatio(r)
  → playRaw（開始時刻 s_play_t0 を記録）
```

- **口の開き = いま鳴っているサンプル位置の RMS ÷ その発話の最大 RMS**（0..1）。
  再生位置は「鳴らし始めた時刻 + 経過時間 − DMA 遅れ 45 ms（仮置き）」で推定する。
  RMS の床（RMS < 96）は 0 にして息の音で口が震えないようにした
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

## 再生パイプライン（先読み量の自動決定）

1 発話は PSRAM の連続バッファ 1 本に頭から追記する（`saan_speaker_push_f32`）。
先読み量まで貯まったら `saan_speaker_start()` が **2 区間**をキューに渡す:
1 枚目 = 貯めたぶん、2 枚目 = **残り全部（まだ書いていない部分を含む）**。
M5.Speaker は DMA へ詰めるときにその場所のメモリを読むので、合成が再生より先を書き続けている限り
（= 先読み量の条件）2 枚目はそのまま正しく鳴る。以後「渡す」作業は無く、合成は書き続けるだけ。

```
pull → push（int16 化 + 包絡）─┬─ 貯まり < 先読み量 … まだ鳴らさない
                              └─ 貯まり ≥ 先読み量 … start()（貯めたぶん + 残り全部の 2 区間を渡す）
以後 毎チャンク pump() = 「再生位置 + DMA 先読み 2,048 sample > 書き込み位置」なら追い越し
eos → stop()（再生完了を待って解放）
```

- 先読み量 = `T × (1 − 1/xRT_est) + 2 チャンク`（DMA の先読み 93 ms と粒度のぶん）。
  `xRT_est` は前の発話の実測 × 1.15（最初は 1.8）。追い越されたら次は更に × 1.2
- バッファは `begin_utterance` で**ゼロ埋め**する。追い越された区間はゴミではなく無音になる
- `-DSAAN_BUFFERED=1` は先読み量 = 全部（発話開始まで = 合成時間）
- ⚠️ **「キューに空きができたら続きを渡す」方式は途切れる。** チャンクを 1 個合成するたび
  （154 ms ごと）にしか渡せないので、渡した瞬間にキューが 2 枚とも埋まっていると、次の機会までに
  2 枚とも尽きる組み合わせが必ずある（残り 154〜215 ms のとき。実機で 1 発話目に毎回 1 回踏んだ）。
  閾値をいじっても穴が移動するだけなので、渡すタイミングという概念を無くした

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
