/* sanoTTS-jp on M5Stack CoreS3 — ファーム本体
 *
 * 出所: sanoTTS-jp（https://github.com/ayutaz/sanoTTS-jp）の esp32/main/main.c を
 *       CoreS3 向けに改変したもの。モデルも同リポジトリの v3 int8（NOTICE.md）。
 *
 * 流れ:
 *   .rodata の重み blob を開く（saan_model.c）
 *     → スピーカー（M5.Speaker）と顔（m5stack-avatar）を初期化
 *     → **起動セルフテスト**: 組み込みのかな中間表現を saan_g2p() に通し、
 *        demo_ids.h の錨と ids が完全一致するか（**表と実装のずれの検出**）
 *     → 1 回喋る（本家 QEMU の記録値と checksum を突き合わせる基準）
 *     → **ループ**: シリアルの `かな> ` に 1 行入れば合成、画面をタッチすれば直前の列を再合成
 *
 * 1 発話の中身（synth_once）:
 *   静的 arena で saan_stream_init
 *     → チャンクを pull して int16 に変換し、PSRAM の発話バッファに**追記**
 *     → 先読み量（下の preroll_target）まで貯まったら鳴らし始め、以後は
 *        M5 のキューに空きがあるたびに続きの区間を渡す（**計算しながら鳴らす**）
 *     → 統計（xRT / 途切れ / checksum）をログに出し、次の発話の先読み量に xRT を反映
 *
 * ⚠️ **実機（CoreS3 / 240 MHz）の実測は W8A8+PIE で定常 1.55x RT**（docs/measurements.md）。
 *    合成は再生より遅いので、**途切れない条件は「音声の (1 − 1/xRT) を先に貯める」**。
 *    xRT 1.55 なら 35.5%。全部貯めるより発話開始が早い（1.2 秒の文で 2.7 s → 約 1.3 s）。
 */
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_mmu_map.h"      /* 起動時の mmap 空き量の診断 */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "saanotts.h"
#include "saanotts_stream.h"

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

/* 1 = 全部貯めてから鳴らす（先読み量の計算を使わない。発話開始まで = 合成時間）。
 * 0 = **xRT から先読み量を決めて、計算しながら鳴らす**（既定）。 */
/* 端末内漢字 G2P を入れるか（CMake の -DSAAN_KANJI=0 で外す。既定 1）。
 * 外すと入力はかな中間表現だけになり、辞書パーティションも要らない。 */
#ifndef SAAN_KANJI
#define SAAN_KANJI 0
#endif

#ifndef SAAN_BUFFERED
#define SAAN_BUFFERED 0
#endif

/* --- 先読み量 -------------------------------------------------------------
 *
 * 音声長 T、合成の実時間比 xRT（> 1 = 再生より遅い）のとき、鳴らし始める前に
 * 貯めておく量 P が **P ≥ T·(1 − 1/xRT)** なら、以後は計算が再生に追い越されない
 * （時刻 t での貯まり P + t/xRT が再生位置 t を常に上回る。最も厳しいのは t = T）。
 *
 * xRT は前の発話で実測した値に余裕（SAAN_XRT_MARGIN）を掛けて使う。最初の発話は
 * SAAN_XRT_INITIAL。さらに 2 チャンクぶん（DMA の先読み + 粒度）を足す。
 * ⚠️ 見込みが甘いと途切れる（`途切れ N 回` に出る）。次の発話で xRT が更新されて直る。 */
#ifndef SAAN_XRT_INITIAL
#define SAAN_XRT_INITIAL 1.8f
#endif
#ifndef SAAN_XRT_MARGIN
#define SAAN_XRT_MARGIN 1.15f
#endif
static float g_xrt_est = SAAN_XRT_INITIAL;

static size_t preroll_target(size_t total) {
#if SAAN_BUFFERED
    return total;
#else
    const float x = g_xrt_est;
    const float ratio = x > 1.0f ? 1.0f - 1.0f / x : 0.0f;
    /* + 2 チャンク: M5.Speaker の DMA 先読み（2,048 sample）と、チャンク単位の粒度のぶん */
    size_t p = (size_t)((float)total * ratio) + (size_t)(2 * SAAN_CHUNK * SAAN_HOP);
    return p > total ? total : p;
#endif
}

/* --- arena ---------------------------------------------------------------
 *
 * ⚠️ **`saan_stream_arena_needed()` の戻り値を使わないこと。** あれは緩い上限で、
 *    n_ids=350 に対し 340,016 B (332 KB) を返す。内部 SRAM に対して大きすぎる。
 *
 * 208 KB (212,992 B) の根拠は本家の実測（`make -C csrc arena`）:
 *   - n_ids=350（学習分布の上限に相当）の最小 arena  197,632 B
 *   - 208 KB 固定で n_ids 1〜520 は init も pull も成功、560 以上は
 *     SAAN_ERR_ARENA で**きれいに失敗**（n_ids 1〜1000 の 23 点でクラッシュ 0）
 *   - CoreS3 実測: used 194,848 B（53 ids）
 *
 * ⚠️ **PSRAM に置かない。** 合成の作業領域なので速度に直結する。 */
#define SAAN_ARENA_BYTES (208 * 1024)

/* ⚠️ **黙って確保に失敗したのを検出するための下限。**
 *
 * `saan_alloc` は失敗しても `used` を進めずに NULL を返す。`saan_stream_init` は
 * 25 回の確保のうち各グループの**最後の 1 個しか NULL 検査していない**ので、
 * 途中の大きい確保だけが落ちると **init が SAAN_OK を返したまま壊れた状態**になり、
 * その後 `saan_stream_pull` の中で NULL 書き込み = StoreProhibited で**ログも出ずに再起動**。
 *
 * 本家の実測（n_ids=350）:
 *   正しく init できたときの `a.used`  194,640 B (n_ids=1) 〜 198,768 B (n_ids=520)
 *   黙って失敗したときの `a.used`      最大 191,280 B
 * → 191,280 < 閾値 <= 194,640 なら誤検知も見逃しも無い。中点を採る。
 * ⚠️ **コアの確保順が変わったら再測すること。** */
#define SAAN_ARENA_USED_FLOOR 192960u

/* 受け付ける ids の上限。**arena の限界 (520) ではなく学習分布の上限を採る。**
 * arena は 520 ids まで持つが、生徒が学習したのは max_spec_length=700（= 350 ids 相当）
 * までで、それを超える入力は**分布の外**。**入力を拒否するほうが、分布外の音を
 * 黙って出すより良い。** */
#define SAAN_MAX_IDS 350

/* .bss に静的確保する。**malloc しない**（断片化させない・失敗しない）。
 * 16 バイト境界は PIE（SOC_SIMD_PREFERRED_DATA_ALIGNMENT = 16）のため。 */
static __attribute__((aligned(16))) uint8_t g_arena[SAAN_ARENA_BYTES];

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
static k1_dict_t g_dict;
static bool      g_dict_ok;
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

/* --- 1 発話 ---------------------------------------------------------------
 *
 * ⚠️ **arena も PCM 統計も発話ごとに巻き戻す。** 巻き戻さないと 2 発話目の
 *    checksum が「1 + 2 発話目」になり、しかも値は出るので気づけない。 */
static bool synth_once(const saan_weights *w, const int32_t *ids, int32_t n_ids) {
    saan_pcm_reset();
    saan_ui_thinking();
    const int64_t t_begin = esp_timer_get_time();

    saan_arena a;
    saan_arena_init(&a, g_arena, sizeof g_arena);

    saan_stream st;
    int64_t t_init = esp_timer_get_time();
    saan_status s = saan_stream_init(&st, w, &a, ids, n_ids, SAAN_S_V);
    t_init = esp_timer_get_time() - t_init;
    if (s != SAAN_OK) {
        ESP_LOGE(TAG, "saan_stream_init: %s", saan_strerror(s));
        return false;
    }

    /* 二重防御（SAAN_ARENA_USED_FLOOR の ⚠️）。init が OK でも黙って確保に失敗していることがある */
    if (a.used < SAAN_ARENA_USED_FLOOR) {
        ESP_LOGE(TAG, "saan_stream_init は OK を返したが a.used が %u B しかない "
                      "(下限 %u B)。**確保が黙って失敗している** — "
                      "このまま pull すると NULL 書き込みで再起動する",
                 (unsigned)a.used, (unsigned)SAAN_ARENA_USED_FLOOR);
        return false;
    }

    const size_t total = (size_t)st.n_frames * SAAN_HOP;
    const double audio_s = (double)total / SAAN_SR;
    ESP_LOGI(TAG, "init %.2f ms / %d ids / %d frames / %u sample / 音声 %.3f s",
             (double)t_init / 1000.0, (int)n_ids, (int)st.n_frames, (unsigned)total, audio_s);
    ESP_LOGI(TAG, "arena used %u B / peak %u B / 確保 %u B",
             (unsigned)a.used, (unsigned)a.peak, (unsigned)sizeof g_arena);

    /* 発話の総サンプル数は init の時点で決まる。そのぶん PSRAM に取る */
    if (!saan_speaker_begin_utterance(total)) return false;
    const size_t target = preroll_target(total);
    ESP_LOGI(TAG, "先読み %u / %u sample (%.0f%%) — xRT 見込み %.2f",
             (unsigned)target, (unsigned)total, 100.0 * (double)target / (double)total,
             (double)g_xrt_est);

    int32_t n = 0;
    int chunks = 0, short_pulls = 0, gaps = 0;
    int64_t t_first = 0, t_rest = 0;
    double t_ready_ms = 0.0;   /* 鳴らし始めまでの時間 */
    int32_t total_frames = 0;
    bool started = false;
    bool ok = true;

    /* --- 合成しながら鳴らす -----------------------------------------------
     * ⚠️ 最初の pull だけ定常の約 5 倍かかる（CoreS3 実測 766 ms vs 144 ms。受容野
     *    38 フレームの warmup で内部の step_chunk が複数回走るため）。 */
    for (;;) {
        int64_t t0 = esp_timer_get_time();
        s = saan_stream_pull(&st, g_chunk, &n);
        int64_t dt = esp_timer_get_time() - t0;
        if (s != SAAN_OK) { ESP_LOGE(TAG, "pull: %s", saan_strerror(s)); ok = false; break; }
        if (n <= 0) break;
        if (chunks == 0) t_first = dt; else t_rest += dt;
        if (n < SAAN_CHUNK) ++short_pulls;
        total_frames += n; ++chunks;

        /* ⚠️ `n` は**フレーム数**。サンプル数は n * SAAN_HOP */
        if (!saan_speaker_push_f32(g_chunk, (size_t)n * SAAN_HOP)) { ok = false; break; }

        if (!started) {
            if (saan_speaker_buffered() >= target) {
                t_ready_ms = (double)(esp_timer_get_time() - t_begin) / 1000.0;
                saan_ui_speaking();   /* 吹き出しに文を出す。口は lip_task が動かす */
                if (!saan_speaker_start()) { ok = false; break; }
                started = true;
            }
        } else if (saan_speaker_pump(false)) {   /* 再生に追い越されたか */
            ++gaps;   /* 追い越された = その区間は無音が鳴った */
        }
    }
    if (ok && !started) {
        /* 先読み量が総量に届かないうちに終わった（短い文 / 全部貯める設定） */
        t_ready_ms = (double)(esp_timer_get_time() - t_begin) / 1000.0;
        saan_ui_speaking();
        if (!saan_speaker_start()) ok = false;
        started = true;
    }
    if (ok && saan_speaker_pump(true)) ++gaps;

    saan_speaker_stop();
    {
        const double total_audio = (double)total_frames * SAAN_HOP / SAAN_SR;
        const double mean_rest = chunks > 1 ? (double)t_rest / (chunks - 1) / 1000.0 : 0.0;
        const double chunk_ms = (double)SAAN_CHUNK * SAAN_HOP * 1000.0 / SAAN_SR;
        const double xrt = chunk_ms > 0 ? mean_rest / chunk_ms : 0.0;
        ESP_LOGI(TAG, "----- 結果 -----");
        ESP_LOGI(TAG, "pull %d 回 / %d frames / 音声 %.3f s（端数チャンク %d 回）",
                 chunks, (int)total_frames, total_audio, short_pulls);
        ESP_LOGI(TAG, "初回 pull %.2f ms / 2 回目以降 mean %.2f ms "
                      "(満チャンク 1 個 = %.2f ms の音声)",
                 (double)t_first / 1000.0, mean_rest, chunk_ms);
        ESP_LOGI(TAG, "定常 xRT = %.3f  ← **1.0 を超えたら再生より遅い**", xrt);
        ESP_LOGI(TAG, "発話開始まで %.0f ms（先読み %u sample）/ 追い越し %d 回",
                 t_ready_ms, (unsigned)target, gaps);
        ESP_LOGI(TAG, "int16 クリップ %u sample", (unsigned)saan_speaker_clip_count());
        /* ⚠️ **移植が正しいことの唯一の機械的な証拠。** 「音が鳴った」ではなく、
         *    本家 QEMU の記録値（W8A8: 0x04de91103a0e49f9 / W8A32: 0x78c209af06affc01）と
         *    一致するかで判定する。checksum が違っても |max| と Σx² が合えば丸め差。 */
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
            /* ⚠️ lo == 0 なら口が一度も開いていない = リップシンクが効いていない */
            ESP_LOGI(TAG, "リップシンク: %u フレーム中 %u で口が開いた（最大 %.2f）",
                     (unsigned)lf, (unsigned)lo, (double)lm);
        }

        /* 次の発話の先読み量に反映する（2 チャンク以上で測れたときだけ）。
         * 途切れたなら見込みが甘かったので、実測より更に厚めにする */
        if (chunks > 2 && xrt > 0.0) {
            float est = (float)xrt * SAAN_XRT_MARGIN * (gaps > 0 ? 1.2f : 1.0f);
            if (est < 0.3f) est = 0.3f;
            if (est > 10.0f) est = 10.0f;
            g_xrt_est = est;
        }
        if (gaps > 0)
            ESP_LOGW(TAG, "途切れた。次の発話は xRT 見込み %.2f で先読みを増やす", (double)g_xrt_est);

        saan_ui_idle(gaps > 0 ? "途切れた" : "タッチでもう一度");
    }
    return ok;
}

/* --- 入力 1 行 → 合成 -----------------------------------------------------
 *
 * ⚠️ **拒否する理由を必ず「どの文字か」まで出す。** 未知語や記号は
 *    「黙って無音になる」のがこの入力仕様の一番危ない壊れ方なので、
 *    端末側では**必ずエラーにして位置を示す**（`err_byte`）。 */
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
        ESP_LOGE(TAG, "G2P 失敗: %s（%d バイト目）", saan_g2p_strerror(gs), (int)gi.err_byte);
        if (gs == SAAN_G2P_ERR_UNKNOWN && gi.err_byte >= 0
            && (size_t)gi.err_byte < nbytes) {
            /* err_byte から先の 1 文字（最大 4 B）を見せる。**何を消せばよいか分かるように。** */
            char ch[8] = {0};
            size_t k = 0;
            for (size_t i = (size_t)gi.err_byte; i < nbytes && k < 4; ++i, ++k) {
                ch[k] = text[i];
                if (k > 0 && ((unsigned char)text[i] & 0xC0u) != 0x80u) { ch[k] = '\0'; break; }
            }
            ESP_LOGE(TAG, "  受け付けない文字: \"%s\"", ch);
            ESP_LOGE(TAG, "  使えるのは **ひらがな** と [ ] # ° ー っ ん と ? ?! ?. ?~ だけ。"
                          "漢字・カタカナ・句読点 (。、) は端末では扱わない");
            ESP_LOGE(TAG, "  漢字混じり文からの変換は**ホスト側**（sanoTTS-jp リポジトリ）で: "
                          "uv run python scripts/to_intermediate.py \"文\"");
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
 * 文 → k1_analyze（辞書 + Viterbi）→ mecab2njd → NJD 8 段 → jpcommon → ラベル → ids。
 * ⚠️ **ホスト（フル辞書）とは一致しない。** 枝刈りの分だけ読みが変わる文がある
 *    （sanoTTS-jp 実測 17.79% の文。地名・固有名詞）。**既知の代償**であって欠陥ではない。
 * ⚠️ 未知語は「無音で消える」のではなく k1_unk_guess が 1 文字ずつ読みを推測する（平板）。 */
static bool speak_kanji(const saan_weights *w, const char *text, size_t nbytes) {
    g_last_n_ids = 0;
    if (nbytes == 0) {
        ESP_LOGW(TAG, "空行。文を入力すること（例: 今日は良い天気ですね。）");
        return false;
    }
    if (!g_dict_ok) {
        ESP_LOGE(TAG, "辞書が開けていない。`=` 前置のかな中間表現だけ使える");
        saan_ui_idle("辞書なし");
        return false;
    }
    int32_t n_ids = 0;
    int n_tok = 0;
    int64_t t0 = esp_timer_get_time();
    saan_kanji_status ks = saan_kanji_to_ids(&g_dict, text, nbytes,
                                            g_arena, sizeof g_arena,
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

static void print_usage_kana(void);

static void print_usage(void) {
    ESP_LOGI(TAG, "==================== 対話モード ====================");
#if SAAN_KANJI
    ESP_LOGI(TAG, "**文をそのまま 1 行入力して Enter で喋る**（漢字かな交じり文。端末内の辞書で読む）。");
    ESP_LOGI(TAG, "  例:  今日は良い天気ですね。");
    ESP_LOGI(TAG, "  ⚠️ 辞書は枝刈りしてあるので、ホストと読みが変わる文がある（地名・固有名詞）");
    ESP_LOGI(TAG, "`=` で始めるとかな中間表現として扱う（突き合わせ用）:");
#else
    ESP_LOGI(TAG, "かな中間表現を 1 行入力して Enter で喋る（`=` 前置も可）:");
#endif
    print_usage_kana();
    ESP_LOGI(TAG, "画面をタッチすると直前の文をもう一度喋る。");
    ESP_LOGI(TAG, "====================================================");
}

static void print_usage_kana(void) {
    ESP_LOGI(TAG, "  例:  =きょ][おわよ][いて][んきです°ね     （今日は良い天気ですね。）");
    ESP_LOGI(TAG, "記号:  [ 上昇 / ] 下降核 / # 句境界 / ° 無声化 / ? ?! ?. ?~ 疑問");
    ESP_LOGI(TAG, "⚠️ **漢字・カタカナ・句読点は受け付けない**（端末に辞書が無い）。");
    ESP_LOGI(TAG, "   漢字混じり文からは**ホスト側**（sanoTTS-jp リポジトリ）で作る:");
    ESP_LOGI(TAG, "     uv run python scripts/to_intermediate.py \"今日は良い天気ですね。\"");
    ESP_LOGI(TAG, "⚠️ アクセント記号を省くと平板になる。**音は出るが正しい抑揚ではない。**");
    ESP_LOGI(TAG, "編集: BS/DEL 1 文字消す / Ctrl-U 行を消す / 上限 %d ids", (int)SAAN_MAX_IDS);
}

static void tts_task(void *arg) {
    (void)arg;
    log_heap("起動直後");
    log_mmap_room();
    ESP_LOGI(TAG, "arena %d B を .bss に静的確保 (%p) / G2P の ids %d B",
             (int)SAAN_ARENA_BYTES, (void *)g_arena, (int)sizeof g_ids);

    static saan_weights w;
    if (!saan_model_open(&w)) { vTaskDelete(NULL); return; }

#if SAAN_KANJI
    /* 辞書は重みの後に開く（MMU の窓は flash と PSRAM で共有。起動ログに空き量が出る）。
     * 開けなくても `=` のかな入力だけで続ける。 */
    g_dict_ok = saan_dict_open(&g_dict) && (saan_kanji_init() != 0);
    if (!g_dict_ok)
        ESP_LOGW(TAG, "辞書を開けなかった（または作業領域を取れなかった）。**かな入力だけ**で続ける");
    else
        ESP_LOGI(TAG, "漢字経路の作業領域 %u B", (unsigned)saan_kanji_workbytes());
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
        ESP_LOGI(TAG, "W8A8 + PIE 有効 / int8 blob を確認");
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
                ESP_LOGI(TAG, "タッチ → もう一度: \"%s\" (%d ids)", g_last_text, (int)g_last_n_ids);
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
        /* `=` 前置はかな中間表現（本家 QEMU と突き合わせる用）。
         * 漢字対応ビルドではそれ以外を文そのものとして辞書で読む。 */
        const char *body = (n > 0 && line[0] == '=') ? line + 1 : line;
        size_t body_n = (n > 0 && line[0] == '=') ? (size_t)n - 1 : (size_t)n;
#if SAAN_KANJI
        if (body != line) (void)speak_line(&w, body, body_n);
        else              (void)speak_kanji(&w, body, body_n);
#else
        (void)speak_line(&w, body, body_n);
#endif
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
