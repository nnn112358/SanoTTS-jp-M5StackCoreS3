/* 音声出力 — M5Unified の M5.Speaker 版（M5Stack CoreS3 の内蔵アンプ AW88298）。
 *
 * ⚠️ **`playRaw` はデータをコピーしない。** Speaker_Class.cpp の `_play_raw` は
 *    `info.data = data;` とポインタを持つだけ。**再生が終わるまでバッファを
 *    書き換えてはいけない。** チャンクを 1 枚のバッファで使い回すと、
 *    **音は出るが前のチャンクの尾が次の内容で上書きされる**（無音にならないので
 *    「鳴った」で見落とす種類の壊れ方）。→ SAAN_SPK_NBUF 枚で回す。
 *
 * ⚠️ **キューは 1 チャンネルあたり 2 枚**（`wav_info_t wavinfo[2]`）。
 *    `_set_next_wav` は満杯のときセマフォ待ちで**ブロックする**ので、
 *    合成ループの流量制御はこれに任せる。
 *    生きているポインタは最大 2 本なので、**3 枚あれば書き込み先は必ず空き**。
 *
 * ⚠️ **サンプルレートはコアと同じ 22,050 Hz で I2S を回す**（SAAN_SPK_OUT_RATE）。
 *    `playRaw(..., 22050)` と speaker_config_t.sample_rate が一致するので M5 側の
 *    リサンプルは通らない。AW88298 は 22.05 kHz を対応レートとして持つ。
 *    ⚠️ ESP32-S3 に APLL が無い件は変わらない。**実サンプルレートの誤差は未測定。**
 *
 * ⚠️ **checksum は M5 に渡す前の int16 で取る。** `saan_f32_to_i16()` は
 *    本家 saan_i2s.c から**逐語コピー**してある。本家の記録値（M-62）と
 *    突き合わせられるのは「変換の順序・幅・丸めが同じ」ときだけなので、ここを触らないこと。
 */
#include <M5Unified.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "saan_speaker.h"

static const char *TAG = "saan_spk";

/* M5.Speaker が実際に出す I2S のサンプルレート。**コアと同じ 22,050 Hz** にして
 * M5 側のリサンプルを通さない。AW88298 は 22.05 kHz を対応レートとして持ち
 * （レジスタ 0x06 I2SSR）、M5Unified の config() → begin() が I2S と AW88298 の
 * 両方をこの値にそろえる。 */
#ifndef SAAN_SPK_OUT_RATE
#define SAAN_SPK_OUT_RATE 22050
#endif

/* 既定音量（0-255）。⚠️ **聴取で決めること。** 大きすぎると int16 の
 * クリップではなくアンプ側で歪む（クリップカウンタには出ない）。 */
#ifndef SAAN_SPK_VOLUME
#define SAAN_SPK_VOLUME 128
#endif

/* 回すバッファの枚数。**3 未満にしないこと**（上の ⚠️ を読むこと）。 */
#ifndef SAAN_SPK_NBUF
#define SAAN_SPK_NBUF 3
#endif
#if SAAN_SPK_NBUF < 3
#error "SAAN_SPK_NBUF は 3 以上。playRaw はポインタを持つだけで、キューは 2 枚ある"
#endif

#define SAAN_SPK_MAXBUF 2048   /* 1 チャンク = 2,048 sample = 92.88 ms */

/* ⚠️ **ヒープから一度だけ確保し、二度と解放しない。** `playRaw` が持っている
 *    ポインタの生存期間はプログラムと同じでなければならない。
 *    合わせて 28,672 B。PSRAM があればそちら、無ければ内部 DRAM（spk_alloc）。
 * ⚠️ **スタックには置けない**（saan_irfft_1024 の自動変数 4,128 B と衝突する）。 */
static int16_t *s_ring[SAAN_SPK_NBUF];
static size_t   s_ring_idx;
static int16_t *s_preroll;
static size_t   s_preroll_cap;   /* begin_utterance で取った量（sample） */
static size_t   s_preroll_fill;

static uint32_t s_clips;
static bool     s_ready;

/* --- 以下 4 つの統計は本家 saan_i2s.c から逐語コピー --------------------- */
static uint64_t s_pcm_fnv = 1469598103934665603ull;
static uint32_t s_pcm_n;
static int32_t  s_pcm_absmax;
static uint64_t s_pcm_sqsum;

int16_t saan_f32_to_i16(float x) {
    long v = lrintf(x * 32767.0f);
    if (v > 32767) { v = 32767; ++s_clips; }
    else if (v < -32768) { v = -32768; ++s_clips; }
    uint16_t u = (uint16_t)(int16_t)v;
    s_pcm_fnv = (s_pcm_fnv ^ (uint8_t)(u & 0xff)) * 1099511628211ull;
    s_pcm_fnv = (s_pcm_fnv ^ (uint8_t)(u >> 8)) * 1099511628211ull;
    { int32_t av = (int32_t)(v < 0 ? -v : v);
      if (av > s_pcm_absmax) s_pcm_absmax = av;
      s_pcm_sqsum += (uint64_t)((int64_t)v * (int64_t)v); }
    ++s_pcm_n;
    return (int16_t)v;
}

void saan_pcm_reset(void) {
    s_pcm_fnv = 1469598103934665603ull;   /* FNV-1a 64 bit のオフセット基底 */
    s_pcm_n = 0;
    s_pcm_absmax = 0;
    s_pcm_sqsum = 0;
    s_clips = 0;
    s_preroll_fill = 0;   /* ⚠️ 前の発話の残りを次に混ぜない */
}

uint32_t saan_speaker_clip_count(void) { return s_clips; }
uint64_t saan_pcm_checksum(void)       { return s_pcm_fnv; }
uint32_t saan_pcm_samples(void)        { return s_pcm_n; }
int32_t  saan_pcm_absmax(void)         { return s_pcm_absmax; }
uint64_t saan_pcm_sqsum(void)          { return s_pcm_sqsum; }

/* --- バッファ確保 ---------------------------------------------------------
 *
 * まず PSRAM、無ければ内部 DRAM から取る。
 *
 * ⚠️ **PSRAM を切る構成がある。** CoreS3 で CONFIG_SPIRAM=y にすると
 *    8 MB の PSRAM が flash の data mmap 用 vaddr を食い、重み blob の
 *    `esp_partition_mmap` が **ESP_ERR_NO_MEM** で落ちる（実機実測）。
 *    そのとき PSRAM を切るので、ここは内部 DRAM に落ちられる必要がある。
 * ⚠️ **DMA から読まれるバッファではない。** M5.Speaker はここを **CPU で**読んで
 *    自前の DMA バッファへミックスするので、PSRAM でも動く。
 * ⚠️ **どちらから取れたかを必ずログに出す。** 黙って内部に落ちると、
 *    DRAM が 28,672 B 減った理由が分からなくなる。 */
static int16_t *spk_alloc(size_t n_samples, const char *what) {
    const size_t nb = n_samples * sizeof(int16_t);

    void *p = heap_caps_malloc(nb, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        ESP_LOGI(TAG, "%s (%u B) を PSRAM に確保", what, (unsigned)nb);
        return (int16_t *)p;
    }

    p = heap_caps_malloc(nb, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p == NULL) {
        ESP_LOGE(TAG, "%s (%u B) を確保できない（PSRAM も内部 DRAM も）",
                 what, (unsigned)nb);
        return NULL;
    }
    ESP_LOGW(TAG, "%s (%u B) を**内部 DRAM**から確保した（PSRAM が無い構成）",
             what, (unsigned)nb);
    return (int16_t *)p;
}

/* --- M5 への送出 ---------------------------------------------------------- */

static bool spk_play(const int16_t *p, size_t n) {
    /* repeat=1 / channel=0 / stop_current=false。
     * ⚠️ キューが満杯なら _set_next_wav がブロックして戻ってくる。
     *    false が返るのは「もう片方のスロットが無限ループ再生」のときだけで、
     *    ここでは起こらないが、**握りつぶさずに落とす**。 */
    if (!M5.Speaker.playRaw(p, n, 22050, false, 1, 0, false)) {
        ESP_LOGE(TAG, "M5.Speaker.playRaw が false を返した (%u sample)", (unsigned)n);
        return false;
    }
    return true;
}

bool saan_speaker_begin_utterance(size_t n_samples) {
    if (n_samples == 0) { ESP_LOGE(TAG, "0 sample の発話"); return false; }
    if (s_preroll != NULL) {
        /* stop() を呼ばずに次の発話に来た。前の再生が終わっているとは限らない。 */
        ESP_LOGW(TAG, "前の発話のバッファが残っている。再生完了を待って解放する");
        saan_speaker_stop();
    }
    s_preroll = spk_alloc(n_samples, "発話バッファ");
    if (s_preroll == NULL) return false;
    s_preroll_cap  = n_samples;
    s_preroll_fill = 0;
    return true;
}

bool saan_speaker_setup(uint32_t sample_rate) {
    if (sample_rate != 22050u) {
        ESP_LOGE(TAG, "想定外のサンプルレート %u（コアは 22,050 Hz 固定）",
                 (unsigned)sample_rate);
        return false;
    }

    /* ⚠️ **M5.begin() より先に取る。** 失敗したら M5 を初期化しないで戻る。 */
    if (s_ring[0] == NULL) {
        for (int i = 0; i < SAAN_SPK_NBUF; ++i) {
            s_ring[i] = spk_alloc(SAAN_SPK_MAXBUF, "リングバッファ");
            if (s_ring[i] == NULL) return false;
        }
    }

    auto cfg = M5.config();
    /* ⚠️ **ディスプレイは clear しない。** 起動ログを消してしまうと
     *    実機で最初に見たい情報が消える。 */
    cfg.clear_display = false;
    cfg.internal_mic  = false;   /* 使わない。マイクとスピーカーは排他の板もある */
    cfg.internal_spk  = true;
    M5.begin(cfg);

    auto scfg = M5.Speaker.config();
    scfg.sample_rate = SAAN_SPK_OUT_RATE;
    scfg.stereo      = false;
    /* dma_buf_len/count は既定（256 × 8 = 2,048 sample ≒ 46 ms @44.1k）のまま。
     * ⚠️ 減らすとアンダーランしやすくなる。**実機で測るまで触らない。** */
    M5.Speaker.config(scfg);

    if (!(M5.Speaker.begin() && M5.Speaker.isEnabled())) {
        /* ⚠️ **スピーカーを持たない板がある**（M5AtomS3 / M5StampS3 など）。
         *    ここで落とさないと「無音だが正常終了」になり原因が分からない。 */
        ESP_LOGE(TAG, "M5.Speaker が使えない（begin/isEnabled が false）。"
                      "スピーカー付きの板か、外付け I2S の設定が要る");
        return false;
    }
    M5.Speaker.setVolume(SAAN_SPK_VOLUME);

    ESP_LOGI(TAG, "M5.Speaker: 出力 %d Hz / 音源 22,050 Hz%s"
                  " / volume %d / バッファ %d 枚 × %d sample",
             (int)SAAN_SPK_OUT_RATE,
             SAAN_SPK_OUT_RATE == 22050 ? "（リサンプル無し）" : "（M5 側でリサンプル）",
             (int)SAAN_SPK_VOLUME, (int)SAAN_SPK_NBUF, (int)SAAN_SPK_MAXBUF);
    ESP_LOGW(TAG, "⚠️ 実サンプルレートの誤差は**未測定**（S3 に APLL は無い）");

    s_ring_idx = 0;
    s_ready = true;
    return true;
}

bool saan_speaker_preroll_push(const float *pcm, size_t n_samples) {
    if (s_preroll == NULL) {
        ESP_LOGE(TAG, "saan_speaker_begin_utterance が済んでいない");
        return false;
    }
    if (s_preroll_fill + n_samples > s_preroll_cap) return false;
    for (size_t i = 0; i < n_samples; ++i)
        s_preroll[s_preroll_fill + i] = saan_f32_to_i16(pcm[i]);
    s_preroll_fill += n_samples;
    return true;
}

bool saan_speaker_start(void) {
    if (!s_ready) { ESP_LOGE(TAG, "saan_speaker_setup が済んでいない"); return false; }
    if (s_preroll_fill > 0) {
        ESP_LOGI(TAG, "貯めた %u sample (%.3f s) を送出", (unsigned)s_preroll_fill,
                 (double)s_preroll_fill / 22050.0);
        /* ⚠️ s_preroll は stop() が再生完了を待ってから解放する。playRaw が
         *    ポインタを持っている間は生きている。 */
        if (!spk_play(s_preroll, s_preroll_fill)) return false;
        s_preroll_fill = 0;
    }
    return true;
}

bool saan_speaker_write_f32(const float *pcm, size_t n_samples) {
    if (n_samples > (size_t)SAAN_SPK_MAXBUF) {
        ESP_LOGE(TAG, "チャンクが大きすぎる %u > %d sample",
                 (unsigned)n_samples, (int)SAAN_SPK_MAXBUF);
        return false;
    }
    /* ⚠️ **変換してから playRaw する。** 生きているポインタは最大 2 本
     *    （current + next）で、3 枚回しなので今から書く s_ring[s_ring_idx] は
     *    2 回前に queue したもの = 既に再生済み。 */
    int16_t *dst = s_ring[s_ring_idx];
    for (size_t i = 0; i < n_samples; ++i) dst[i] = saan_f32_to_i16(pcm[i]);
    s_ring_idx = (s_ring_idx + 1) % SAAN_SPK_NBUF;

    return spk_play(dst, n_samples);
}

void saan_speaker_stop(void) {
    /* ⚠️ **鳴らし終わるまで待つ。** ここで戻ると、対話モードで次の発話の
     *    変換がバッファを上書きして**前の発話の尾が化ける**。
     *    M5.Speaker.end() は呼ばない（次の発話でまた begin する意味が無い）。 */
    while (M5.Speaker.isPlaying()) {
        vTaskDelay(1);
    }
    /* ⚠️ **再生が終わってから解放する。** playRaw はポインタを持つだけなので、
     *    先に free すると解放済みメモリを鳴らす（音は出るので気づけない）。 */
    if (s_preroll != NULL) {
        heap_caps_free(s_preroll);
        s_preroll = NULL;
        s_preroll_cap = 0;
        s_preroll_fill = 0;
    }
}
