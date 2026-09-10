/* sanoTTS-jp on M5Stack CoreS3 — ファーム本体
 *
 * 出所: sanoTTS-jp（https://github.com/ayutaz/sanoTTS-jp）の esp32/main/main.c を
 *       CoreS3 向けに改変したもの。モデルも同リポジトリの v3 int8（NOTICE.md）。
 *
 * 流れ:
 *   .rodata の重み blob を開く（saan_model.c。**blob v2** = 654,032 B。v1 は SAAN_ERR_VERSION）
 *     → スピーカー（M5.Speaker）と顔（m5stack-avatar）を初期化
 *     → **起動セルフテスト**: 組み込みのかな中間表現を saan_g2p() に通し、
 *        demo_ids.h の錨と ids が完全一致するか（**表と実装のずれの検出**）
 *     → 1 回喋る（本家 QEMU の記録値と checksum を突き合わせる基準）
 *     → **ループ**: シリアルの `かな> ` に 1 行入れば合成、画面をタッチすれば直前の列を再合成。
 *        行が「かな中間表現」か「漢字かな交じり文」かは saan_g2p_classify() が決める
 *        （本家 K-B。前置記号は要らない。判定は**かな G2P のトークナイザが行末まで通るか**
 *        そのもので、手書きの文字集合ではない）。`=` でかな、`!` で辞書に**強制**できる（試験用）。
 *
 * 1 発話の中身（synth_once）:
 *   静的 arena で saan_stream_init
 *     → **プリロール**: SAAN_SPK_PREROLL_SAMPLES（4 チャンク）ぶんを pull して int16 に変換し、貯める
 *     → saan_speaker_start() で貯めたぶんを渡して鳴らし始め、以後はチャンクごとに
 *        saan_speaker_write_f32()（M5 のキューが満杯なら**ブロック**する。**計算しながら鳴らす**）
 *     → 統計（xRT / アンダーラン / checksum）をログに出す
 *   この流れは本家 sanoTTS-jp の esp32/main/main.c と同じ（2026-09-10 に合わせた。それ以前は
 *   前の発話の xRT から先読み量を決め、発話バッファ 1 本を 2 区間で渡していた）。
 *
 * ⚠️ **速度の現在地（2026-09-04 に本家 origin/main のコアへ同期）。**
 *    旧コア（2026-09-01 時点）は CoreS3 実機で W8A8+PIE 定常 1.55x RT だった（docs/measurements.md）。
 *    本家はその後 S1〜S5b / T1〜T5 で **CoreS3（顔なし）定常 xRT 0.446**（本家 M-90）まで詰め、
 *    この板（顔あり）でも 2026-09-07 に 0.445 を実測した。xRT < 1 なので固定プリロールで途切れない。
 *    ⚠️ xRT > 1 に戻ったら（コアを重くしたら）この方式では**プリロールを増やしても途切れる**
 *    （合成はキューの 2 枚より先に進めない）。そのときは -DSAAN_BUFFERED=1。
 */
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_mmu_map.h"      /* 起動時の mmap 空き量の診断 */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "saanotts.h"
#include "saanotts_stream.h"
#include "saan_prof.h"

#include "g2p.h"

#include "demo_ids.h"
#include "saan_console.h"
#if SAAN_KANJI
#include "saan_dict.h"
#include "saan_kanji.h"
#endif
#include "saan_model.h"
#include "saan_speaker.h"
#include "saan_ui.h"

static const char *TAG = "saanotts";

/* --- ビルド時の切り替え（main/CMakeLists.txt から -D で渡る）------------------ */

/* 起動時に 1 文喋るか。顔を出してタッチで再生する UI なので**既定で喋る**。
 * ⚠️ 錨との照合（boot_selftest）は喋るかどうかに関係なく必ず走る。 */
#ifndef SAAN_BOOT_SPEAK
#define SAAN_BOOT_SPEAK 1
#endif

/* 端末内漢字 G2P を入れるか（CMake の -DSAAN_KANJI=0 で外す。既定 1）。
 * 外すと入力はかな中間表現だけになり、辞書パーティションも要らない。 */
#ifndef SAAN_KANJI
#define SAAN_KANJI 0
#endif

/* 1 = 全部貯めてから鳴らす（発話開始まで = 合成時間。**途切れない**）。
 * 0 = **プリロール後に計算しながら鳴らす**（既定。xRT とアンダーランを測るのはこちら）。 */
#ifndef SAAN_BUFFERED
#define SAAN_BUFFERED 0
#endif

/* --- プリロール -----------------------------------------------------------
 *
 * 本家と同じ固定量（SAAN_SPK_PREROLL_SAMPLES = 8,192 sample = 4 チャンク = 371 ms）。
 * 最初の pull だけ定常の約 6 倍かかる（この板で 244.65 ms vs 41.47 ms）ので、
 * 鳴らし始める前にそのぶんを含めて数チャンク計算しておく。
 * ⚠️ 途切れない条件は **xRT < 1**。プリロールは初回 pull の遅れを吸収するだけで、
 *    合成が遅いときの貯金にはならない（キューの 2 枚より先には進めない）。 */
#define SAAN_PREROLL_CHUNKS (SAAN_SPK_PREROLL_SAMPLES / (SAAN_CHUNK * SAAN_HOP))

/* --- arena ---------------------------------------------------------------
 *
 * ⚠️ **`saan_stream_arena_needed()` の戻り値を使わないこと。** あれは緩い上限で、
 *    n_ids=350 に対し 302,816 B (296 KB) を返す（T4 後のホスト値）。内部 SRAM に対して大きすぎる。
 * ⚠️ **高水位（`st.peak_used`）もそのまま確保量にしないこと。** init が通る最小 arena は
 *    ALIGN16 の切り上げと確保順の差で高水位をわずかに上回る。
 *
 * 176 KB (180,224 B) の根拠は本家の実測（`make -C csrc arena`、T4 後、2026-09-03）:
 *   - n_ids=350（学習分布の上限に相当）で init も pull も通る最小 arena  160,768 B
 *   - 176 KB 固定で通った最大 n_ids 450、最初に clean fail するのは 496（SAAN_ERR_ARENA。
 *     1000 まで**クラッシュ 0 件**）
 *   - 本家 CoreS3 実測（M-89、53 ids）: used 157,360 B
 *   履歴: 2026-09-04 の同期前は 208 KB（212,992 B。旧コアの CoreS3 実測 used 194,848 B）。
 *   T2（ストリーミングで有効範囲だけ計算）と T4（cdel 6 本 → リング 1 本、iSTFT の re/im/frm を
 *   w_e と共用）で a.used が 177,536 → 160,224 B（350 ids、ホスト）になったので下げた。
 *   CoreS3 では内部 DRAM の空きが 32 KB 増える（顔 + 辞書を積んだこの構成で一番苦しかったところ）。
 *
 * ⚠️ **PSRAM に置かない。** 合成の作業領域なので速度に直結する。 */
#define SAAN_ARENA_BYTES (176 * 1024)

/* ⚠️ **黙って確保に失敗したのを検出する二重防御**（init 後の `a.used` の検査）。
 *
 * `saan_alloc` は失敗しても `used` を進めずに NULL を返す。かつて `saan_stream_init` は
 * 各グループの**最後の 1 個しか NULL 検査していなかった**ので、途中の大きい確保だけが
 * 落ちると **init が SAAN_OK を返したまま壊れた状態**になり、`saan_stream_pull` の中で
 * NULL 書き込み = StoreProhibited で**ログも出ずに再起動**した。
 * 今は saan_arena の粘着フラグ `failed` で「黙って失敗」は起きないが、保険として init 後の
 * `a.used` を**コアが同じ確保一覧から計算する期待値 `saan_stream_arena_used(n_ids)`** と
 * 突き合わせる（synth_once）。
 * ⚠️ 以前はここに定数 SAAN_ARENA_USED_FLOOR 192960u があった（ホストで測った中点）。
 *    `sizeof(struct saan_stream_impl)` がポインタ幅で変わる（ホスト 64 bit / Xtensa 32 bit で
 *    768 B 違う）ので**ホストで測った定数はターゲットの a.used と一致しない**。本家が
 *    各ターゲットで自分の sizeof から計算する関数にしたので、それに追随した。 */

#if SAAN_KANJI
/* 漢字経路（saan_kanji.c）は G2P の間この arena を借りる。**その作業領域（Viterbi 48 KB +
 * 固定長の配列 + T10(a) で .bss から移した label_ids のトークン表 10,240 B）が収まること**を
 * コンパイル時に検査する。C99 には _Static_assert が無いので配列の typedef で潰す
 * （落ちると「負のサイズの配列」でコンパイルが止まる）。 */
typedef char saan_arena_holds_kanji_workbytes[
    (SAAN_ARENA_BYTES >= SAAN_KANJI_WORKBYTES) ? 1 : -1];
#endif

/* 受け付ける ids の上限。**arena の限界 (520) ではなく学習分布の上限を採る。**
 * arena は 520 ids まで持つが、生徒が学習したのは max_spec_length=700（= 350 ids 相当）
 * までで、それを超える入力は**分布の外**。**入力を拒否するほうが、分布外の音を
 * 黙って出すより良い。** */
#define SAAN_MAX_IDS 350

/* 既定は .bss に静的確保する。**malloc しない**（断片化させない・失敗しない）。
 * 16 バイト境界は PIE（SOC_SIMD_PREFERRED_DATA_ALIGNMENT = 16）のため。
 *
 * ⚠️ **ESP32（S3 でない Core2）は dram0_0_seg が小さく、静的 176 KB が入らない**
 *    （本家の Core2 で 65,368 B 溢れた）。SAAN_BOARD=core2 は SAAN_ARENA_HEAP=1 になり
 *    （main/CMakeLists.txt）、tts_task の先頭で heap_caps_aligned_alloc（PSRAM 優先 → 内部 DRAM）
 *    から取る。**PSRAM の arena は遅い**（未測定）。取れなければ起動時に止まる。本家と同じ経路。 */
#ifndef SAAN_ARENA_HEAP
#define SAAN_ARENA_HEAP 0
#endif
/* 複数ブロックで取るとき、各塊に残す余地（Open JTalk のヒープ・音声バッファ 28 KB・画面のため） */
#ifndef SAAN_ARENA_HEAP_RESERVE
#define SAAN_ARENA_HEAP_RESERVE (24 * 1024)
#endif
#if SAAN_ARENA_HEAP
/* tts_task の先頭で確保。16 B 境界は heap_caps_aligned_alloc が保証。
 * 176 KB が 1 本で取れないとき（PSRAM の無い ESP32 = Basic）は、内部ヒープの大きい塊から
 * **複数ブロック**に分けて取り、saan_arena_add() で 1 つの arena にする（コアの複数ブロック対応）。 */
static uint8_t *g_arena;                              /* ブロック 0（漢字 G2P の作業領域もここ） */
static size_t   g_arena0_size;                        /* ブロック 0 の大きさ */
static uint8_t *g_arena_r[SAAN_ARENA_MAX_REGIONS];    /* 全ブロック */
static size_t   g_arena_rn[SAAN_ARENA_MAX_REGIONS];
static int      g_arena_nr;
#else
static __attribute__((aligned(16))) uint8_t g_arena[SAAN_ARENA_BYTES];
#define g_arena0_size ((size_t)SAAN_ARENA_BYTES)
#endif

/* 発話ごとに arena を組み立てる（1 本なら init だけ） */
static void arena_setup(saan_arena *a) {
#if SAAN_ARENA_HEAP
    saan_arena_init(a, g_arena_r[0], g_arena_rn[0]);
    for (int i = 1; i < g_arena_nr; ++i) saan_arena_add(a, g_arena_r[i], g_arena_rn[i]);
#else
    saan_arena_init(a, g_arena, SAAN_ARENA_BYTES);
#endif
}

/* 1 チャンク = 8 frames × 256 = 2,048 sample = 92.88 ms。8,192 B。
 * ⚠️ **スタックに置かない。** saan_irfft_1024 の自動変数だけで 4 KB 使う。 */
static float g_chunk[SAAN_CHUNK * SAAN_HOP];

/* --- 端末側 G2P ----------------------------------------------------------
 *
 * 入力は**かな中間表現**（ひらがな + [ ] # ° と ? ?! ?. ?~）。漢字は端末で扱わない。
 * 表は components/saanotts_core/g2p_table.h。
 *
 * ⚠️ **`saan_g2p_capacity()` と同じ式を使う。** 上限は `2 * バイト数 + 3`。
 *    足りないと SAAN_G2P_ERR_OVERFLOW で**きれいに失敗する**（黙って切り詰めない）。
 *    boot_selftest が実体と突き合わせる。 */
#define SAAN_G2P_IDS_CAP (2 * SAAN_CONSOLE_LINE_MAX + 3)
static int32_t g_ids[SAAN_G2P_IDS_CAP];

/* タッチで「もう一度」喋るために、最後に合成した列を覚えておく。
 * g_ids は speak_line のたびに上書きされるので、個数と元の文字列だけ別に持つ。
 * ⚠️ **G2P が失敗したら 0 にする。** そのとき g_ids は途中まで書かれた壊れた列で、
 *    前の個数のまま再生すると**それらしい音**が出てしまう。 */
#if SAAN_KANJI
/* 端末内漢字 G2P の辞書（flash に mmap したまま使う。RAM には読まない）。
 * ⚠️ **Viterbi は合成用の g_arena を borrow する。** G2P と合成は同時に走らない。 */
static jdict_t g_dict;
static bool    g_dict_ok;
#endif

static int32_t g_last_n_ids;
static char    g_last_text[SAAN_CONSOLE_LINE_MAX];

typedef char saan_g2p_cap_check[(SAAN_G2P_IDS_CAP >= SAAN_DEMO_N_IDS) ? 1 : -1];
typedef char saan_max_ids_check[(SAAN_MAX_IDS <= SAAN_G2P_IDS_CAP) ? 1 : -1];

/* 合成タスクのスタック。saan_irfft_1024 の 4 KB + 呼び出し段 + ログ。
 * CoreS3 実測で 16,384 B 中 11,108 B 残り。 */
#define SAAN_TASK_STACK 16384

/* ⚠️ **優先度は低く、core 0 に固定。** 合成は数秒間 CPU を手放さないので、
 *    高くすると同じ core の顔の描画・リップシンク・スピーカーが止まる。
 *    顔（m5stack-avatar）は core 1、スピーカーは affinity 無し（優先度 2）。 */
#define SAAN_TASK_PRIO  1
#define SAAN_TASK_CORE  0

/* シリアル入力を待つ単位。この間隔でタッチも見る（合成中は見ない）。 */
#define SAAN_POLL_MS 20

/* flash を mmap できる vaddr がどれだけ残っているか。
 * ⚠️ CoreS3 では PSRAM 8 MB が同じ 32 MB の MMU 窓を使う。以前 model パーティションの
 *    mmap（3 MB / 1 MB）が ESP_ERR_NO_MEM で落ちたので、辞書（13.7 MB）を載せる前に
 *    実測しておく。 */
static void log_mmap_room(void) {
    size_t room = 0;
    esp_err_t e = esp_mmu_map_get_max_consecutive_free_block_size(
        MMU_MEM_CAP_READ | MMU_MEM_CAP_8BIT, MMU_TARGET_FLASH0, &room);
    ESP_LOGI(TAG, "flash mmap の最大連続空き: %u B (%.1f MB) [%s]",
             (unsigned)room, (double)room / 1048576.0, esp_err_to_name(e));
    esp_mmu_map_dump_mapped_blocks(stdout);
}

static void log_heap(const char *when) {
    ESP_LOGI(TAG, "%s: 内部 DRAM free %u B / 最大ブロック %u B", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

/* --- 段別プロファイラ（-DSAAN_PROFILE=1 のときだけ。components/saanotts_core/saan_prof.h）---
 *
 * 時計は CCOUNT（CPU サイクル。240 MHz なら 1 サイクル = 4.17 ns）。コアは saan_prof_now() を
 * 呼ぶだけで、時計の実装はプラットフォーム側（ここ）が出す。
 * ⚠️ **計測自体のコスト**: 区間の出入りで CCOUNT を読む関数を呼ぶ。`hout` は出力チャネル
 *    1,539 本なので WCOPY / MAC の区間は 1 チャンクに約 3,000 回入る。細かい区間ほど過大に出る。
 *    速度の報告には SAAN_PROFILE=0 のビルドを使うこと。 */
#if SAAN_PROFILE
#include "esp_cpu.h"
uint32_t saan_prof_now(void) { return esp_cpu_get_cycle_count(); }

static void prof_report(void) {
    const uint32_t steps = saan_prof_cnt[SAAN_PROF_STEP];
    if (steps == 0) return;
    const double step = (double)saan_prof_acc[SAAN_PROF_STEP] / (double)steps;
    ESP_LOGI(TAG, "----- 段別プロファイル（step_chunk %u 回の平均。単位 = CCOUNT）-----", (unsigned)steps);
    ESP_LOGI(TAG, "%-8s %10s %12s %7s %12s %10s", "区間", "回数/step", "cyc/step", "%%STEP", "要素/step", "cyc/要素");
    for (int id = 0; id < SAAN_PROF_N; ++id) {
        if (id == SAAN_PROF_INIT || id == SAAN_PROF_LOOKUP) continue;   /* 発話側に出す（下） */
        const double cnt = (double)saan_prof_cnt[id] / steps;
        const double acc = (double)saan_prof_acc[id] / steps;
        const double n   = (double)saan_prof_n[id] / steps;
        ESP_LOGI(TAG, "%-8s %10.2f %12.0f %6.1f%% %12.0f %10.2f", saan_prof_name(id), cnt, acc,
                 step > 0 ? 100.0 * acc / step : 0.0, n,
                 saan_prof_n[id] ? (double)saan_prof_acc[id] / (double)saan_prof_n[id] : 0.0);
    }
    /* ⚠️ DW 行には入れ子の QUANT（dw 入力の量子化）が含まれる。カーネル行の合算は二重計上 */
    ESP_LOGI(TAG, "  ⚠️ DW の cyc/step は入れ子の QUANT を含む。カーネル行の合算は二重計上");
    /* ⚠️ LOOKUP は S1 で init 側に移った（pull の中では 0 回が期待値） */
    ESP_LOGI(TAG, "----- INIT 側（発話あたり。step で割らない）-----");
    ESP_LOGI(TAG, "INIT  : %.0f cyc / 回 (%u 回)", saan_prof_cnt[SAAN_PROF_INIT]
             ? (double)saan_prof_acc[SAAN_PROF_INIT] / saan_prof_cnt[SAAN_PROF_INIT] : 0.0,
             (unsigned)saan_prof_cnt[SAAN_PROF_INIT]);
    ESP_LOGI(TAG, "LOOKUP: %u 回 / %llu cyc（init の resolve_weights。pull の中では 0 回が期待値）",
             (unsigned)saan_prof_cnt[SAAN_PROF_LOOKUP],
             (unsigned long long)saan_prof_acc[SAAN_PROF_LOOKUP]);
    ESP_LOGI(TAG, "1 step = %.0f cyc（240 MHz なら %.2f ms）", step, step / 240000.0);
}
#endif

/* --- 1 発話 ---------------------------------------------------------------
 *
 * ⚠️ **arena も PCM 統計も発話ごとに巻き戻す。** 巻き戻さないと 2 発話目の
 *    checksum が「1 + 2 発話目」になり、しかも値は出るので気づけない。 */
static bool synth_once(const saan_weights *w, const int32_t *ids, int32_t n_ids) {
    saan_pcm_reset();
#if SAAN_PROFILE
    saan_prof_reset();
#endif
    saan_ui_thinking();
    const int64_t t_begin = esp_timer_get_time();

    saan_arena a;
    arena_setup(&a);

    saan_stream st;
    int64_t t_init = esp_timer_get_time();
    saan_status s = saan_stream_init(&st, w, &a, ids, n_ids, SAAN_S_V);
    t_init = esp_timer_get_time() - t_init;
    if (s != SAAN_OK) {
        ESP_LOGE(TAG, "saan_stream_init: %s", saan_strerror(s));
        return false;
    }

    /* 二重防御（上の ⚠️）。init が OK でも黙って確保に失敗していることがある。
     * 期待値はコアが同じ確保一覧から計算する（ポインタ幅の差もターゲット側の sizeof で吸収） */
    {   /* 複数ブロックでも used は「生きている確保の合計」なので同じ検査が効く */
        const size_t used_expect = saan_stream_arena_used(n_ids);
        if (a.used != used_expect) {
            ESP_LOGE(TAG, "saan_stream_init は OK を返したが a.used が %u B（期待 %u B）。"
                          "**確保が黙って失敗しているか、コアの確保一覧と "
                          "saan_stream_arena_used() がずれている** — "
                          "このまま pull すると NULL 書き込みで再起動しうる",
                     (unsigned)a.used, (unsigned)used_expect);
            return false;
        }
    }

    const size_t total = (size_t)st.n_frames * SAAN_HOP;
    const double audio_s = (double)total / SAAN_SR;
    ESP_LOGI(TAG, "init %.2f ms / %d ids / %d frames / %u sample / 音声 %.3f s",
             (double)t_init / 1000.0, (int)n_ids, (int)st.n_frames, (unsigned)total, audio_s);
    ESP_LOGI(TAG, "arena used %u B / peak %u B / 確保 %u B（%d ブロック）",
             (unsigned)a.used, (unsigned)a.peak, (unsigned)a.size, a.n_regions);

    /* --- プリロール ------------------------------------------------------
     * ⚠️ **鳴らし始める前に数チャンク計算しておく。** 最初の pull だけ定常の約 6 倍かかる
     *    （旧コアの CoreS3 実測 766 ms vs 144 ms、新コアの実測 244.65 ms vs 41.47 ms。
     *    受容野 38 フレームの warmup で内部の step_chunk が複数回走るため）。 */
#if SAAN_BUFFERED
    /* 発話の総サンプル数は init の時点で決まる。そのぶん貯めて全部計算してから鳴らす */
    const size_t preroll = total;
    const int preroll_chunks = INT_MAX;   /* = 最後まで */
#else
    const size_t preroll = total < (size_t)SAAN_SPK_PREROLL_SAMPLES ? total : (size_t)SAAN_SPK_PREROLL_SAMPLES;
    const int preroll_chunks = SAAN_PREROLL_CHUNKS;
#endif
    /* プリロールバッファ（preroll）とリップシンク包絡（total ぶん）を PSRAM に取る */
    if (!saan_speaker_begin_utterance(preroll, total)) return false;

    int32_t n = 0;
    int chunks = 0, short_pulls = 0, underruns = 0;
    int64_t t_first = 0, t_rest = 0;
    int32_t total_frames = 0;
    double t_ready_ms = 0.0;   /* 鳴らし始めまでの時間（goto done で飛ぶ経路があるのでここで初期化） */
    bool eos = false;
    bool ok = true;

    for (int i = 0; i < preroll_chunks && !eos; ++i) {
        int64_t t0 = esp_timer_get_time();
        s = saan_stream_pull(&st, g_chunk, &n);
        int64_t dt = esp_timer_get_time() - t0;
        if (s != SAAN_OK) { ESP_LOGE(TAG, "pull: %s", saan_strerror(s)); ok = false; goto done; }
        if (n <= 0) { eos = true; break; }
        if (chunks == 0) t_first = dt; else t_rest += dt;
        if (n < SAAN_CHUNK) ++short_pulls;
        total_frames += n; ++chunks;
        /* ⚠️ `n` は**フレーム数**。サンプル数は n * SAAN_HOP */
        if (!saan_speaker_preroll_push(g_chunk, (size_t)n * SAAN_HOP)) {
            ESP_LOGE(TAG, "プリロール容量の計算が合っていない。SAAN_SPK_PREROLL_SAMPLES を見直すこと");
            ok = false; goto done;
        }
    }
    t_ready_ms = (double)(esp_timer_get_time() - t_begin) / 1000.0;
#if SAAN_BUFFERED
    ESP_LOGI(TAG, "全 %d チャンクを貯めた。発話開始まで %.0f ms（音声 %.3f s）",
             chunks, t_ready_ms, audio_s);
#else
    ESP_LOGI(TAG, "プリロール %d チャンク完了（初回 pull %.2f ms / 鳴らし始めまで %.0f ms）",
             chunks, (double)t_first / 1000.0, t_ready_ms);
#endif

    saan_ui_speaking();   /* 吹き出しに文を出す。口は lip_task が動かす */
    if (!saan_speaker_start()) { ok = false; goto done; }

    /* --- 定常ループ（ストリーミングのみ。貯める方式では eos 済みで入らない）------
     * write_f32 は M5 のキュー（2 枚）が満杯なら空くまでブロックする。渡すタイミングを
     * 見る必要は無い。 */
    while (!eos) {
        int64_t t0 = esp_timer_get_time();
        s = saan_stream_pull(&st, g_chunk, &n);
        int64_t dt = esp_timer_get_time() - t0;
        if (s != SAAN_OK) { ESP_LOGE(TAG, "pull: %s", saan_strerror(s)); ok = false; break; }
        if (n <= 0) break;

        /* ⚠️ **これが実機で最初に見るべき数値。** 1 チャンクの計算に、
         *    そのチャンクが表す音声より長くかかったらアンダーラン（本家と同じ定義）。 */
        int64_t budget_us = (int64_t)n * SAAN_HOP * 1000000 / SAAN_SR;
        if (dt > budget_us) ++underruns;
        if (chunks == 0) t_first = dt; else t_rest += dt;
        if (n < SAAN_CHUNK) ++short_pulls;
        total_frames += n; ++chunks;

        if (!saan_speaker_write_f32(g_chunk, (size_t)n * SAAN_HOP)) { ok = false; break; }
    }

done:
    saan_speaker_stop();
    {
        const double total_audio = (double)total_frames * SAAN_HOP / SAAN_SR;
        const double mean_rest = chunks > 1 ? (double)t_rest / (chunks - 1) / 1000.0 : 0.0;
        const double chunk_ms = (double)SAAN_CHUNK * SAAN_HOP * 1000.0 / SAAN_SR;
        const double xrt = chunk_ms > 0 ? mean_rest / chunk_ms : 0.0;
        const double total_ms = (double)(t_first + t_rest) / 1000.0;
        ESP_LOGI(TAG, "----- 結果 -----");
        ESP_LOGI(TAG, "pull %d 回 / %d frames / 音声 %.3f s（端数チャンク %d 回）",
                 chunks, (int)total_frames, total_audio, short_pulls);
        ESP_LOGI(TAG, "初回 pull %.2f ms / 2 回目以降 mean %.2f ms "
                      "(満チャンク 1 個 = %.2f ms の音声)",
                 (double)t_first / 1000.0, mean_rest, chunk_ms);
        /* ⚠️ この「定常 xRT」は 2 回目以降の全 pull の平均（末尾の端数 pull 込み）。本家 T1 以降の
         *    定義（満チャンク pull の中央値）とは違い、少し大きめに出る。
         *    **版どうしを比べるなら次の「合成合計 / 音声」**
         *    （定義に依らない量）を見ること。 */
        ESP_LOGI(TAG, "定常 xRT = %.3f  ← **1.0 を超えたら再生より遅い**", xrt);
        ESP_LOGI(TAG, "合成合計 %.2f ms（全 pull の dt の和）/ 音声 %.3f s → 合成/音声 %.3f",
                 total_ms, total_audio, total_audio > 0 ? total_ms / 1000.0 / total_audio : 0.0);
        ESP_LOGI(TAG, "発話開始まで %.0f ms（プリロール %u sample）/ アンダーラン %d 回",
                 t_ready_ms, (unsigned)preroll, underruns);
        ESP_LOGI(TAG, "int16 クリップ %u sample", (unsigned)saan_speaker_clip_count());
        /* ⚠️ **移植が正しいことの唯一の機械的な証拠。** 「音が鳴った」ではなく、
         *    本家 QEMU の記録値と一致するかで判定する。checksum が違っても |max| と Σx² が合えば丸め差。
         *    期待値（S3 = GELU の erf 近似以降のコア。本家 M-81 / M-90、blob v2）:
         *      W8A8+PIE 0xa69a7ebbb5ccb05f（|max| 9627 / Σx² 74,264,237,672）
         *      W8A32    0xe4b645c30835d42d（|max| 9529 / Σx² 74,155,591,505）
         *    旧コア（2026-09-01 以前）は 0x04de91103a0e49f9 / 0x78c209af06affc01 だった。 */
        ESP_LOGI(TAG, "出力 PCM: %u sample / FNV-1a 0x%016llx",
                 (unsigned)saan_pcm_samples(), (unsigned long long)saan_pcm_checksum());
        ESP_LOGI(TAG, "        |max| %d / Σx² %llu",
                 (int)saan_pcm_absmax(), (unsigned long long)saan_pcm_sqsum());
        ESP_LOGI(TAG, "タスクスタック残り %u B（%d B 中）",
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
                 (int)SAAN_TASK_STACK);
        {
            uint32_t lf = 0, lo = 0; float lm = 0.0f;
            saan_ui_lip_stats(&lf, &lo, &lm);
            /* ⚠️ 顔あり（-DSAAN_UI=avatar）で lo == 0 なら口が一度も開いていない = リップシンクが
             *    効いていない。文字表示 UI（-DSAAN_UI=text）は常に 0 を返す。 */
            if (lf == 0)
                ESP_LOGI(TAG, "リップシンク: なし（文字表示 UI か、lip_task が回っていない）");
            else
                ESP_LOGI(TAG, "リップシンク: %u フレーム中 %u で口が開いた（最大 %.2f）",
                         (unsigned)lf, (unsigned)lo, (double)lm);
        }

        if (underruns > 0)
            ESP_LOGW(TAG, "アンダーラン %d 回（pull の計算時間 > そのチャンクの音声長）。"
                          "途切れない再生が要るなら -DSAAN_BUFFERED=1", underruns);
#if SAAN_PROFILE
        prof_report();
#endif

        saan_ui_idle(underruns > 0 ? "途切れた" : "タッチでもう一度");
    }
    return ok;
}

/* --- 拒否の理由を「どの文字か」まで出す -----------------------------------
 *
 * ⚠️ 未知語や記号は「黙って無音になる」のがこの入力仕様の一番危ない壊れ方なので、
 *    端末側では**必ずエラーにして位置を示す**（`err_byte`）。 */
static void log_reject(const char *text, size_t nbytes, int32_t err_byte) {
    if (err_byte < 0 || (size_t)err_byte >= nbytes) return;
    /* err_byte から先の 1 文字（最大 4 B）を見せる。**何を消せばよいか分かるように。** */
    char ch[8] = {0};
    size_t k = 0;
    for (size_t i = (size_t)err_byte; i < nbytes && k < 4; ++i, ++k) {
        ch[k] = text[i];
        if (k > 0 && ((unsigned char)text[i] & 0xC0u) != 0x80u) { ch[k] = '\0'; break; }
    }
    ESP_LOGE(TAG, "  受け付けない文字: \"%s\"（%d バイト目）", ch, (int)err_byte);
}

/* --- 入力 1 行 → 合成（かな経路）------------------------------------------
 *
 * 普段は speak_auto() が saan_g2p_classify() で「かな」と判定した行だけがここに来る
 * （`=` 前置の強制も来る）。 */
static bool speak_line(const saan_weights *w, const char *text, size_t nbytes) {
    g_last_n_ids = 0;   /* 失敗したら「もう一度」も無効にする（g_last_n_ids の ⚠️） */
    if (nbytes == 0) {
        ESP_LOGW(TAG, "空行。かな中間表現を入力すること（例: きょ][おわよ][いて][んきです°ね）");
        return false;
    }
    if (saan_g2p_capacity(nbytes) > SAAN_G2P_IDS_CAP) {
        ESP_LOGE(TAG, "入力 %u B は長すぎる（ids バッファ %d 分）",
                 (unsigned)nbytes, (int)SAAN_G2P_IDS_CAP);
        return false;
    }

    int32_t n_ids = 0;
    saan_g2p_info gi;
    int64_t t_g2p = esp_timer_get_time();
    saan_g2p_status gs = saan_g2p(text, nbytes, g_ids, SAAN_G2P_IDS_CAP, &n_ids, &gi);
    t_g2p = esp_timer_get_time() - t_g2p;

    if (gs != SAAN_G2P_OK) {
        /* ⚠️ speak_auto 経由なら**ここには来ないはず** — 呼ぶ前に saan_g2p_classify() が同じ
         *    トークナイザで「かな経路」と判定している。来たら 2 つがずれた印（`=` の強制なら来うる）。 */
        ESP_LOGE(TAG, "G2P 失敗: %s（%d バイト目）", saan_g2p_strerror(gs), (int)gi.err_byte);
        if (gs == SAAN_G2P_ERR_UNKNOWN) {
            log_reject(text, nbytes, gi.err_byte);
            ESP_LOGE(TAG, "  かな中間表現で使えるのは **ひらがな** と [ ] # ° ー っ ん と ? ?! ?. ?~ だけ");
        }
        saan_ui_idle("入力エラー");
        return false;
    }

    ESP_LOGI(TAG, "G2P: %u B -> %d ids / %.3f ms（音素 %d 個・うち PAD %d）",
             (unsigned)nbytes, (int)n_ids, (double)t_g2p / 1000.0,
             (int)gi.n_phonemes, (int)gi.n_pad_phonemes);

    /* ⚠️ **黙って落ちたものを必ず出す。** `ー`（直前に平母音が無い）と
     *    `°`（直前が平母音でない）は**例外を出さずに捨てられる**規約なので、
     *    件数を見せないと「打ったのに反映されない」に気づけない。 */
    if (gi.n_dropped_long > 0 || gi.n_dropped_devoice > 0)
        ESP_LOGW(TAG, "  ⚠️ 黙って落ちた: ー %d 個 / ° %d 個"
                      "（直前が平母音でないと効かない規約）",
                 (int)gi.n_dropped_long, (int)gi.n_dropped_devoice);

    if (n_ids > SAAN_MAX_IDS) {
        ESP_LOGE(TAG, "%d ids は上限 %d を超える。**短く区切って入力すること**"
                      "（arena は 520 ids まで持つが、生徒が学習したのは %d ids 相当まで。"
                      "その外は分布外で品質を保証できない）",
                 (int)n_ids, (int)SAAN_MAX_IDS, (int)SAAN_MAX_IDS);
        saan_ui_idle("長すぎる");
        return false;
    }

    /* ここまで来れば g_ids は完成した列。タッチで再生できるようにしてから喋る。 */
    g_last_n_ids = n_ids;
    memcpy(g_last_text, text, nbytes);
    g_last_text[nbytes] = '\0';
    saan_ui_set_text(g_last_text);
    return synth_once(w, g_ids, n_ids);
}

#if SAAN_KANJI
/* --- 漢字かな交じり文 1 行 → 合成 ----------------------------------------
 *
 * 文 → jdict_analyze（辞書 + Viterbi）→ mecab2njd → NJD 8 段 → jpcommon → ラベル → ids。
 * ⚠️ **ホスト（フル辞書）とは一致しない。** 枝刈りの分だけ読みが変わる文がある
 *    （sanoTTS-jp 実測 17.79% の文。地名・固有名詞）。**既知の代償**であって欠陥ではない。
 * ⚠️ 未知語は「無音で消える」のではなく jdict_unk_guess が 1 文字ずつ読みを推測する（平板）。 */
static bool speak_kanji(const saan_weights *w, const char *text, size_t nbytes) {
    g_last_n_ids = 0;
    if (nbytes == 0) {
        ESP_LOGW(TAG, "空行。文を入力すること（例: 今日は良い天気ですね。）");
        return false;
    }
    if (!g_dict_ok) {
        ESP_LOGE(TAG, "辞書が開けていない。かな中間表現だけ使える（例: きょ][おわよ][いて][んきです°ね）");
        saan_ui_idle("辞書なし");
        return false;
    }
    int32_t n_ids = 0;
    int n_tok = 0;
    int64_t t0 = esp_timer_get_time();
    /* ⚠️ Viterbi の作業領域は**連続**が要る。複数ブロックの arena ではブロック 0 だけを貸す */
    saan_kanji_status ks = saan_kanji_to_ids(&g_dict, text, nbytes,
                                            g_arena, g_arena0_size,
                                            g_ids, SAAN_G2P_IDS_CAP, &n_ids, &n_tok);
    int64_t dt = esp_timer_get_time() - t0;
    if (ks != SAAN_KANJI_OK) {
        ESP_LOGE(TAG, "漢字 G2P 失敗: %s", saan_kanji_strerror(ks));
        saan_ui_idle("読めない");
        return false;
    }
    ESP_LOGI(TAG, "漢字 G2P: %u B → 形態素 %d 個 → ids %d 個 / %.1f ms",
             (unsigned)nbytes, n_tok, (int)n_ids, (double)dt / 1000.0);
    if (n_ids > SAAN_MAX_IDS) {
        ESP_LOGE(TAG, "%d ids は上限 %d を超える。**短く区切って入力すること**", (int)n_ids, (int)SAAN_MAX_IDS);
        saan_ui_idle("長すぎる");
        return false;
    }
    g_last_n_ids = n_ids;
    memcpy(g_last_text, text, nbytes);
    g_last_text[nbytes] = '\0';
    saan_ui_set_text(g_last_text);
    return synth_once(w, g_ids, n_ids);
}
#endif /* SAAN_KANJI */

/* --- 入力 1 行 → 経路を選んで合成（本家 K-B / T11）-------------------------
 *
 * **前置記号は要らない。** 1 本のプロンプトで「かな中間表現」と「漢字かな交じり文」の
 * 両方を受け、`saan_g2p_classify()`（components/saanotts_core/g2p.c）が経路を決める:
 *   かな   … トークン化が行末まで通った            → speak_line
 *   辞書   … 通らず、行に中間表現のマークが 1 つも無い → speak_kanji（-DSAAN_KANJI=0 なら喋らずに理由を出す）
 *   拒否   … 通らないのにマークが混じっている        → 喋らない。位置を見せる
 *
 * ⚠️ **経路を必ずログに出す。** どちらで読まれたかが分からないと、読み違いを見ても
 *    「辞書が悪いのか判定が悪いのか」を切り分けられない。
 * ⚠️ **拒否をそのまま残す。** 「中間表現 + `。`」を黙って辞書経路に回すと `[` `]` `#` が
 *    記号として読まれたり落とされたりして**それらしい音が出てしまう**（気づけない壊れ方）。
 * ⚠️ 判定は手書きの文字集合ではなく、凍結テーブルのトークナイザが行末まで通るかそのもの。
 *    ホスト側 `scripts/kana_g2p.py` の classify_route() と同じ規則（本家 kb_route_parity.py が守る）。 */
static bool speak_auto(const saan_weights *w, const char *text, size_t nbytes) {
    if (nbytes == 0) {
        ESP_LOGW(TAG, "空行。かな中間表現か漢字かな交じり文を入力すること"
                      "（例: きょ][おわよ][いて][んきです°ね / 今日は良い天気ですね。）");
        g_last_n_ids = 0;
        return false;
    }

    saan_g2p_status why = SAAN_G2P_OK;
    int32_t err_byte = -1;
    const saan_g2p_route route = saan_g2p_classify(text, nbytes, &why, &err_byte);
    ESP_LOGI(TAG, "経路: %s", saan_g2p_route_name(route));

    if (route == SAAN_G2P_ROUTE_KANA) return speak_line(w, text, nbytes);

    if (route == SAAN_G2P_ROUTE_DICT) {
#if SAAN_KANJI
        return speak_kanji(w, text, nbytes);
#else
        /* ⚠️ **喋らずに理由を出す。** 辞書を持たないビルドでこの行をかな経路に
         *    無理やり通すと、読めない文字が黙って落ちる。 */
        g_last_n_ids = 0;
        ESP_LOGE(TAG, "この構成は辞書を持たない（-DSAAN_KANJI=0）ので、漢字・カタカナ・句読点は扱えない");
        log_reject(text, nbytes, err_byte);
        ESP_LOGE(TAG, "  漢字混じり文からの変換は**ホスト側**（sanoTTS-jp リポジトリ）で: "
                      "uv run python scripts/to_intermediate.py \"文\"");
        saan_ui_idle("辞書なし");
        return false;
#endif
    }

    /* 拒否 */
    g_last_n_ids = 0;
    if (why == SAAN_G2P_ERR_UTF8) {
        ESP_LOGE(TAG, "不正な UTF-8（%d バイト目）。端末は UTF-8 しか受けない", (int)err_byte);
        saan_ui_idle("入力エラー");
        return false;
    }
    ESP_LOGE(TAG, "かな中間表現として読めないのに、中間表現の記号"
                  "（[ ] # ° _ ^ $ ? ?! ?. ?~）が混じっている。**喋らない**");
    log_reject(text, nbytes, err_byte);
    ESP_LOGE(TAG, "  かな中間表現なら: **ひらがな** と [ ] # ° ー っ ん と ? ?! ?. ?~ だけ（句読点 。、 は入れない）");
    ESP_LOGE(TAG, "  漢字かな交じり文なら: 中間表現の記号を消してから入力すること");
    saan_ui_idle("入力エラー");
    return false;
}

/* --- 起動セルフテスト -----------------------------------------------------
 *
 * ⚠️ **kSaanDemoIds は入力ではなく答え合わせの錨。** 合成に使うのは
 *    saan_g2p() が今その場で作った g_ids の方。錨と食い違ったら**走らせない** —
 *    ずれたまま**それらしい音**を出すのが一番悪い（未知語が無音で消えるのと同じ壊れ方）。 */
static bool boot_selftest(int32_t *n_ids_out) {
    /* SAAN_G2P_IDS_CAP は saan_g2p_capacity() の式を写したもの（配列サイズには
     * 関数を書けない）。**2 か所にある式は必ずずれる**ので、実体と突き合わせる。 */
    if (SAAN_G2P_IDS_CAP < saan_g2p_capacity(SAAN_DEMO_INTERMEDIATE_BYTES)) {
        ESP_LOGE(TAG, "SAAN_G2P_IDS_CAP (%d) が saan_g2p_capacity() (%d) より小さい。"
                      "main.c の式が g2p.c とずれている",
                 (int)SAAN_G2P_IDS_CAP, (int)saan_g2p_capacity(SAAN_DEMO_INTERMEDIATE_BYTES));
        return false;
    }

    int32_t n_ids = 0;
    saan_g2p_info gi;
    int64_t t_g2p = esp_timer_get_time();
    saan_g2p_status gs = saan_g2p(SAAN_DEMO_INTERMEDIATE, SAAN_DEMO_INTERMEDIATE_BYTES,
                                  g_ids, SAAN_G2P_IDS_CAP, &n_ids, &gi);
    t_g2p = esp_timer_get_time() - t_g2p;
    if (gs != SAAN_G2P_OK) {
        ESP_LOGE(TAG, "saan_g2p: %s (err_byte=%d)", saan_g2p_strerror(gs), (int)gi.err_byte);
        return false;
    }
    ESP_LOGI(TAG, "G2P セルフテスト: \"%s\" (%d B) -> %d ids / %.3f ms",
             SAAN_DEMO_INTERMEDIATE, (int)SAAN_DEMO_INTERMEDIATE_BYTES,
             (int)n_ids, (double)t_g2p / 1000.0);
    if (n_ids != SAAN_DEMO_N_IDS
        || memcmp(g_ids, kSaanDemoIds, sizeof kSaanDemoIds) != 0) {
        ESP_LOGE(TAG, "G2P の出力が demo_ids.h の錨と一致しない（%d ids / 期待 %d）。"
                      "**テーブルか実装がずれている**", (int)n_ids, (int)SAAN_DEMO_N_IDS);
        return false;
    }
    ESP_LOGI(TAG, "     OK  %d ids が demo_ids.h の錨と完全一致", (int)n_ids);
    /* g_ids には「今その場で G2P した、錨と一致することを確認済みの列」が入っている。
     * 起動時の 1 発話はこれをそのまま使う。 */
    *n_ids_out = n_ids;
    return true;
}

static void print_usage(void) {
    ESP_LOGI(TAG, "==================== 対話モード ====================");
    ESP_LOGI(TAG, "1 行入力して Enter で喋る。**経路は自動で決まる**（前置記号は要らない）。");
    ESP_LOGI(TAG, "  かな中間表現:  きょ][おわよ][いて][んきです°ね     （今日は良い天気ですね。）");
    ESP_LOGI(TAG, "  ひらがなだけ:  こんにちわ");
    ESP_LOGI(TAG, "記号:  [ 上昇 / ] 下降核 / # 句境界 / ° 無声化 / ? ?! ?. ?~ 疑問");
#if SAAN_KANJI
    ESP_LOGI(TAG, "**漢字かな交じり文はそのまま入力する**（端末内の辞書 + Open JTalk で読む）。");
    ESP_LOGI(TAG, "  例:  今日は良い天気ですね。");
    ESP_LOGI(TAG, "  ⚠️ 辞書は枝刈りしてあるので、ホストと読みが変わる文がある（地名・固有名詞）");
    ESP_LOGI(TAG, "強制（試験用）: `=` 前置でかな中間表現として、`!` 前置で辞書経路として扱う。");
#else
    ESP_LOGI(TAG, "⚠️ **この構成は辞書を持たない**（-DSAAN_KANJI=0）ので、漢字・カタカナ・句読点は喋れない。");
    ESP_LOGI(TAG, "   漢字混じり文からは**ホスト側**（sanoTTS-jp リポジトリ）で作る:");
    ESP_LOGI(TAG, "     uv run python scripts/to_intermediate.py \"今日は良い天気ですね。\"");
    ESP_LOGI(TAG, "強制（試験用）: `=` 前置でかな中間表現として扱う。");
#endif
    ESP_LOGI(TAG, "⚠️ 中間表現の記号が混じったまま読めない行は拒否する（例: きょ][おわ…です°ね。）。");
    ESP_LOGI(TAG, "⚠️ アクセント記号を省くと平板になる。**音は出るが正しい抑揚ではない。**");
    ESP_LOGI(TAG, "編集: BS/DEL 1 文字消す / Ctrl-U 行を消す / 上限 %d ids", (int)SAAN_MAX_IDS);
    ESP_LOGI(TAG, "画面（かボタン A）を短く押すと直前の文をもう一度喋る。長押し / ボタン B / `/ui` で顔 ⇄ 文字。");
    ESP_LOGI(TAG, "====================================================");
}

static void tts_task(void *arg) {
    (void)arg;
#if SAAN_ARENA_HEAP
    /* 1) PSRAM に 1 本（Core2）。2) 内部 DRAM に 1 本。3) 内部 DRAM の大きい塊から複数ブロック（Basic）。
     * ⚠️ 3) は「他の確保（Open JTalk のヒープ、音声バッファ 28 KB、画面）の余地」を SAAN_ARENA_HEAP_RESERVE
     *    だけ残す。足りなければ起動時にここで止まる（黙って小さくしない）。 */
    g_arena = (uint8_t *)heap_caps_aligned_alloc(16, SAAN_ARENA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (g_arena) {
        ESP_LOGW(TAG, "arena %d B を **PSRAM** に確保 (%p)。合成は遅くなる（速度の測定には使わない）",
                 (int)SAAN_ARENA_BYTES, (void *)g_arena);
        g_arena_r[0] = g_arena; g_arena_rn[0] = SAAN_ARENA_BYTES; g_arena_nr = 1;
    } else {
        g_arena = (uint8_t *)heap_caps_aligned_alloc(16, SAAN_ARENA_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (g_arena) {
            ESP_LOGI(TAG, "arena %d B を内部 DRAM のヒープに 1 本で確保 (%p)", (int)SAAN_ARENA_BYTES, (void *)g_arena);
            g_arena_r[0] = g_arena; g_arena_rn[0] = SAAN_ARENA_BYTES; g_arena_nr = 1;
        } else {
            size_t got = 0;
            while (got < (size_t)SAAN_ARENA_BYTES && g_arena_nr < SAAN_ARENA_MAX_REGIONS) {
                size_t blk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (blk <= SAAN_ARENA_HEAP_RESERVE) break;
                blk -= SAAN_ARENA_HEAP_RESERVE;                   /* 他の確保の余地 */
                blk &= ~(size_t)15u;
                if (blk > (size_t)SAAN_ARENA_BYTES - got) blk = ((size_t)SAAN_ARENA_BYTES - got + 15u) & ~(size_t)15u;
                if (blk < 4096) break;
                uint8_t *p = (uint8_t *)heap_caps_aligned_alloc(16, blk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (!p) break;
                g_arena_r[g_arena_nr] = p; g_arena_rn[g_arena_nr] = blk; ++g_arena_nr;
                got += blk;
                ESP_LOGI(TAG, "arena ブロック %d: %u B (%p)", g_arena_nr, (unsigned)blk, (void *)p);
            }
            if (got < (size_t)SAAN_ARENA_BYTES) {
                ESP_LOGE(TAG, "arena %d B を確保できない（PSRAM 無し。内部 DRAM から %u B / %d ブロックしか取れない）",
                         (int)SAAN_ARENA_BYTES, (unsigned)got, g_arena_nr);
                vTaskDelete(NULL); return;
            }
            g_arena = g_arena_r[0];
            ESP_LOGW(TAG, "arena %u B を内部 DRAM の **%d ブロック**に分けて確保した（複数ブロック arena）",
                     (unsigned)got, g_arena_nr);
        }
    }
    g_arena0_size = g_arena_rn[0];
#if SAAN_KANJI
    if (g_arena0_size < SAAN_KANJI_WORKBYTES)
        ESP_LOGE(TAG, "arena のブロック 0（%u B）が漢字 G2P の作業領域（%u B）より小さい。漢字経路は失敗する",
                 (unsigned)g_arena0_size, (unsigned)SAAN_KANJI_WORKBYTES);
#endif
#endif
    log_heap("起動直後");
    log_mmap_room();
#if !SAAN_ARENA_HEAP
    ESP_LOGI(TAG, "arena %d B を .bss に静的確保 (%p) / G2P の ids %d B",
             (int)SAAN_ARENA_BYTES, (void *)g_arena, (int)sizeof g_ids);
#endif

    static saan_weights w;
    if (!saan_model_open(&w)) { vTaskDelete(NULL); return; }

#if SAAN_KANJI
    /* 辞書は重みの後に開く（MMU の窓は flash と PSRAM で共有。起動ログに空き量が出る）。
     * 開けなくてもかな入力だけで続ける。 */
    g_dict_ok = saan_dict_open(&g_dict) && (saan_kanji_init() != 0);
    if (!g_dict_ok)
        ESP_LOGW(TAG, "辞書を開けなかった（または作業領域を取れなかった）。**かな入力だけ**で続ける");
    else
        /* ⚠️ **2 つとも出す。** workbytes は「最低限これだけ要る」、Viterbi バイト数は「実際に渡る」。
         *    T10(a) で固定長の配列を arena へ移したぶん後者が減るので、減りすぎ
         *    （16 KB 未満で SAAN_KANJI_ERR_TOO_LONG）に気づけるようにしておく。 */
        ESP_LOGI(TAG, "漢字経路の作業領域 %u B（最低限）/ Viterbi に渡る %u B（arena %d B のうち）",
                 (unsigned)saan_kanji_workbytes(),
                 (unsigned)saan_kanji_vitbytes(SAAN_ARENA_BYTES), (int)SAAN_ARENA_BYTES);
    log_heap("辞書 mmap 後");
#endif

#if SAAN_INT8_ACT
    /* ⚠️ **W8A8/PIE を有効にしても、blob が fp32 なら 1 命令も効かない。**
     *    `saan_conv1d_w` は `W.f32` があればそこで return するので、
     *    **速度が変わらないのに理由が分からない**という最悪の壊れ方をする。
     *    int8 blob だけが `<name>.scale` を持つ（fp32 blob は 0 個）ので、それで判る。 */
    {
        uint32_t dt = 0, d[4] = {0};
        uint64_t nb = 0;
        if (!saan_tensor(&w, "duration.blocks.0.c1.weight.scale", &dt, d, &nb)) {
            ESP_LOGE(TAG, "W8A8/PIE 有効でビルドしたのに **fp32 blob** が埋まっている。"
                          "この構成では PIE は 1 命令も効かない。int8 blob を使うこと");
            vTaskDelete(NULL); return;
        }
        ESP_LOGI(TAG, "W8A8 + PIE 有効 / int8 blob（形式 v%" PRIu32 "）を確認", w.version);
    }
#endif

    /* ⚠️ この順番。saan_speaker_setup() が M5.begin() を呼び、saan_ui_init() はその後 */
    if (!saan_speaker_setup(SAAN_SR)) { vTaskDelete(NULL); return; }
    if (!saan_ui_init()) { vTaskDelete(NULL); return; }

    int32_t demo_n_ids = 0;
    if (!boot_selftest(&demo_n_ids)) { vTaskDelete(NULL); return; }

    saan_ui_set_text(SAAN_DEMO_TEXT);
#if SAAN_BOOT_SPEAK
    /* ⚠️ **本家 QEMU / 実機を突き合わせる基準はこの 1 文。** 対話入力は毎回違う列なので
     *    突き合わせに使えない（同じ中間表現を打てば同じ列になることは確認済み）。 */
    g_last_n_ids = demo_n_ids;
    strncpy(g_last_text, SAAN_DEMO_INTERMEDIATE, sizeof g_last_text - 1);
    ESP_LOGI(TAG, "起動時の 1 発話: \"%s\"", SAAN_DEMO_TEXT);
    (void)synth_once(&w, g_ids, demo_n_ids);
    log_heap("1 発話後");
#else
    (void)demo_n_ids;   /* 錨との照合だけして喋らない */
    saan_ui_idle("かな> に入力");
    ESP_LOGI(TAG, "起動時は喋らない（-DSAAN_BOOT_SPEAK=0）");
#endif

    if (!saan_console_init()) {
        ESP_LOGE(TAG, "コンソールを開けなかった。対話入力は使えない");
        vTaskDelete(NULL); return;
    }
    print_usage();
    saan_console_prompt();
    for (;;) {
        const char *line = NULL;
        int n = saan_console_poll(&line, SAAN_POLL_MS);
        if (n == SAAN_CONSOLE_PENDING) {
            /* 行が完成していない間はタッチを見る。**合成中はここに来ない**ので、
             * 合成中に何度触っても、戻ってきたときの 1 回ぶんにまとまる。 */
            if (saan_ui_poll_touch() && g_last_n_ids > 0) {
                ESP_LOGI(TAG, "もう一度: \"%s\" (%d ids)", g_last_text, (int)g_last_n_ids);
                (void)synth_once(&w, g_ids, g_last_n_ids);
            }
            continue;
        }
        if (n == SAAN_CONSOLE_ERROR) {
            ESP_LOGE(TAG, "コンソールの読み取りに失敗した");
            break;
        }
        if (n == SAAN_CONSOLE_TOO_LONG) {
            /* ⚠️ **切り詰めて喋らない。** 先頭だけ喋ると「端末とホストで同じ列」が崩れる */
            ESP_LOGE(TAG, "入力が %d B を超えた。**行ごと捨てた**（切り詰めていない）。"
                          "短く区切ること", (int)SAAN_CONSOLE_LINE_MAX - 1);
            saan_console_prompt();
            continue;
        }
        /* 既定は speak_auto() が経路を決める（前置記号は要らない）。
         * ⚠️ **前置記号は試験用の強制だけに残してある**: `=` でかな中間表現（判定を通さずに
         *    saan_g2p に渡す。旧仕様との互換）、`!` で辞書経路（本家と同じ。「同じ行を無理やり
         *    辞書経路に流したらどうなるか」を測るのに要る）。 */
        if (n == 3 && strcmp(line, "/ui") == 0) {
            saan_ui_toggle();
            ESP_LOGI(TAG, "画面: %s", saan_ui_mode());
            saan_console_prompt();
            continue;
        }
        if (n > 0 && line[0] == '=') {
            ESP_LOGI(TAG, "経路: かな（`=` による強制）");
            (void)speak_line(&w, line + 1, (size_t)n - 1);
            saan_console_prompt();
            continue;
        }
#if SAAN_KANJI
        if (n > 0 && line[0] == '!') {
            ESP_LOGI(TAG, "経路: 辞書（`!` による強制）");
            (void)speak_kanji(&w, line + 1, (size_t)n - 1);
            saan_console_prompt();
            continue;
        }
#endif
        (void)speak_auto(&w, line, (size_t)n);
        saan_console_prompt();
    }

    log_heap("終了時");
    vTaskDelete(NULL);
}

void app_main(void) {
    ESP_LOGI(TAG, "sanoTTS-jp on M5Stack CoreS3 / model: %s (%s)",
             SAAN_MODEL_ORIGIN_NAME, SAAN_MODEL_ORIGIN_URL);
    xTaskCreatePinnedToCore(tts_task, "saan_tts", SAAN_TASK_STACK, NULL,
                            SAAN_TASK_PRIO, NULL, SAAN_TASK_CORE);
}
