/* 音声出力 — M5Unified の M5.Speaker 版（M5Stack CoreS3 の内蔵アンプ AW88298）。
 *
 * ⚠️ **`playRaw` はデータをコピーしない。** Speaker_Class.cpp の `_play_raw` は
 *    `info.data = data;` とポインタを持つだけ。**再生が終わるまでバッファを
 *    書き換えてはいけない。** 1 発話を連続バッファ 1 本に**追記だけ**するので、
 *    渡した区間が後から書き換わることはない。解放は stop() で再生完了を待ってから。
 *
 * ⚠️ **キューは 1 チャンネルあたり 2 枚**（`wav_info_t wavinfo[2]`）。
 *    `isPlaying(ch)` が 0 / 1 / 2（= 空き無し）を返すので、pump() は 2 のときは
 *    渡さずに戻る（ブロックしない）。区間はまとめて渡すので枚数は問題にならない。
 *
 * ⚠️ **サンプルレートはコアと同じ 22,050 Hz で I2S を回す**（SAAN_SPK_OUT_RATE）。
 *    `playRaw(..., 22050)` と speaker_config_t.sample_rate が一致するので M5 側の
 *    リサンプルは通らない。AW88298 は 22.05 kHz を対応レートとして持つ。
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
#include "esp_timer.h"

#include "saan_speaker.h"

static const char *TAG = "saan_spk";

#ifndef SAAN_SPK_OUT_RATE
#define SAAN_SPK_OUT_RATE 22050
#endif

/* 既定音量（0-255）。⚠️ **聴取で決めること。** 大きすぎると int16 の
 * クリップではなくアンプ側で歪む（クリップカウンタには出ない）。 */
#ifndef SAAN_SPK_VOLUME
#define SAAN_SPK_VOLUME 128
#endif

#define SAAN_SPK_CH 0   /* 使う仮想チャンネル */

/* リップシンク包絡の 1 ブロック（sample）。512 = 23.2 ms。lip_task の 33 ms より細かい */
#define SAAN_ENV_BLOCK 512

/* playRaw してから実際に鳴るまでの遅れ（DMA バッファぶん）の見込み。
 * ⚠️ **測っていない。** 2,048 sample = 93 ms の DMA バッファの半分を仮置き。 */
#ifndef SAAN_LIP_LATENCY_MS
#define SAAN_LIP_LATENCY_MS 45
#endif

/* M5.Speaker が DMA へ詰めるために**再生位置より先に読む**量（sample）。
 * dma_buf_len 256 × count 8 = 2,048。合成はこの先まで書いておく必要がある。
 * ⚠️ 実測していない（M5Unified の既定値から）。 */
#define SAAN_SPK_READAHEAD 2048

/* 発話バッファ（begin_utterance で取り、stop で解放） */
static int16_t *s_buf;
static size_t   s_cap;                   /* 総サンプル数 */
static volatile size_t s_fill;           /* 変換済み（= 貯めた）サンプル数 */
static size_t   s_sent;                  /* キューに渡したサンプル数 */
static bool     s_started;

static uint32_t s_clips;
static bool     s_ready;

/* --- リップシンク --------------------------------------------------------- */
static uint8_t *s_env;                   /* SAAN_ENV_BLOCK ごとの RMS/16（0..255）。PSRAM */
static size_t   s_env_cap;               /* 要素数（伸ばすだけで縮めない） */
static volatile uint8_t s_env_max;       /* この発話の包絡の最大（正規化用） */
static uint64_t s_blk_sum;               /* いまのブロックの Σx² */
static size_t   s_blk_n;
static volatile int64_t s_play_t0_us;    /* 鳴らし始めた時刻 */
static volatile size_t  s_play_base;     /* その時刻に鳴り始めたサンプル位置 */
static volatile bool    s_playing;

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
}

uint32_t saan_speaker_clip_count(void) { return s_clips; }
uint64_t saan_pcm_checksum(void)       { return s_pcm_fnv; }
uint32_t saan_pcm_samples(void)        { return s_pcm_n; }
int32_t  saan_pcm_absmax(void)         { return s_pcm_absmax; }
uint64_t saan_pcm_sqsum(void)          { return s_pcm_sqsum; }
size_t   saan_speaker_buffered(void)   { return s_fill; }
size_t   saan_speaker_sent(void)       { return s_sent; }

/* --- バッファ確保 ---------------------------------------------------------
 *
 * まず PSRAM、無ければ内部 DRAM から取る。
 * ⚠️ **DMA から読まれるバッファではない。** M5.Speaker はここを **CPU で**読んで
 *    自前の DMA バッファへミックスするので、PSRAM でも動く。
 * ⚠️ **どちらから取れたかを必ずログに出す。** 黙って内部に落ちると、
 *    DRAM が減った理由が分からなくなる。 */
static void *spk_alloc_bytes(size_t nb, const char *what) {
    void *p = heap_caps_malloc(nb, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) return p;
    p = heap_caps_malloc(nb, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p == NULL) {
        ESP_LOGE(TAG, "%s (%u B) を確保できない（PSRAM も内部 DRAM も）", what, (unsigned)nb);
        return NULL;
    }
    ESP_LOGW(TAG, "%s (%u B) を**内部 DRAM**から確保した（PSRAM が無い構成）", what, (unsigned)nb);
    return p;
}

/* --- 変換 + 包絡 ----------------------------------------------------------
 *
 * 変換は必ずここを通す（checksum と包絡を同じ列から取るため）。
 * 包絡: 発話先頭から SAAN_ENV_BLOCK sample ごとの RMS/16。ブロック境界は発話先頭からの
 * 絶対位置で決まるので、チャンク長が 512 の倍数でなくても正しい。 */
static void conv_block(const float *pcm, int16_t *dst, size_t base, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        int16_t v = saan_f32_to_i16(pcm[i]);
        dst[i] = v;
        s_blk_sum += (uint64_t)((int32_t)v * (int32_t)v);
        if (++s_blk_n == SAAN_ENV_BLOCK) {
            uint32_t rms = (uint32_t)sqrtf((float)(s_blk_sum / SAAN_ENV_BLOCK));
            uint32_t e = rms / 16;
            if (e > 255) e = 255;
            size_t idx = (base + i) / SAAN_ENV_BLOCK;
            if (s_env != NULL && idx < s_env_cap) s_env[idx] = (uint8_t)e;
            if (e > s_env_max) s_env_max = (uint8_t)e;
            s_blk_sum = 0;
            s_blk_n = 0;
        }
    }
}

float saan_speaker_level_now(void) {
    if (!s_playing || s_env == NULL) return 0.0f;
    if (M5.Speaker.isPlaying(SAAN_SPK_CH) == 0) return 0.0f;
    int64_t dt = esp_timer_get_time() - s_play_t0_us - (int64_t)SAAN_LIP_LATENCY_MS * 1000;
    if (dt < 0) return 0.0f;
    size_t pos = s_play_base + (size_t)(dt * 22050 / 1000000);
    if (pos >= s_fill) return 0.0f;              /* まだ変換していない / 途切れ */
    size_t idx = pos / SAAN_ENV_BLOCK;
    if (idx >= s_env_cap) return 0.0f;
    uint32_t e = s_env[idx];
    uint32_t m = s_env_max;
    if (m < 32) m = 32;                           /* 無音に近い発話で 0/0 を作らない */
    if (e < 6) return 0.0f;                       /* 床（RMS < 96）。息の音で口が震えない */
    float r = (float)e / (float)m;
    return r > 1.0f ? 1.0f : r;
}

/* --- M5 への送出 ---------------------------------------------------------- */

/* [s_sent, s_fill) を 1 区間として渡す。呼ぶ前にキューの空きを見ること。 */
static bool send_pending(void) {
    size_t n = s_fill - s_sent;
    if (n == 0) return true;
    /* 止まっていたなら、この区間が今から鳴る。再生時計を取り直す */
    if (M5.Speaker.isPlaying(SAAN_SPK_CH) == 0) {
        s_play_base  = s_sent;
        s_play_t0_us = esp_timer_get_time();
    }
    /* repeat=1 / stop_current=false。
     * ⚠️ false が返るのは「もう片方のスロットが無限ループ再生」のときだけで、
     *    ここでは起こらないが、**握りつぶさずに落とす**。 */
    if (!M5.Speaker.playRaw(s_buf + s_sent, n, 22050, false, 1, SAAN_SPK_CH, false)) {
        ESP_LOGE(TAG, "M5.Speaker.playRaw が false を返した (%u sample)", (unsigned)n);
        return false;
    }
    s_sent += n;
    return true;
}

bool saan_speaker_begin_utterance(size_t total_samples) {
    if (total_samples == 0) { ESP_LOGE(TAG, "0 sample の発話"); return false; }
    if (s_buf != NULL) {
        /* stop() を呼ばずに次の発話に来た。前の再生が終わっているとは限らない。 */
        ESP_LOGW(TAG, "前の発話のバッファが残っている。再生完了を待って解放する");
        saan_speaker_stop();
    }
    s_buf = (int16_t *)spk_alloc_bytes(total_samples * sizeof(int16_t), "発話バッファ");
    if (s_buf == NULL) return false;
    /* ⚠️ **ゼロ埋めする。** start() は「まだ書いていない残り全部」もキューに渡す。
     *    合成が追い越されたとき、ゴミではなく無音が鳴るようにするため。 */
    memset(s_buf, 0, total_samples * sizeof(int16_t));
    s_cap  = total_samples;
    s_fill = 0;
    s_sent = 0;
    s_started = false;

    /* 包絡は伸ばすだけ（lip_task が読んでいる最中に free しないため） */
    size_t need = (total_samples + SAAN_ENV_BLOCK - 1) / SAAN_ENV_BLOCK;
    if (need > s_env_cap) {
        uint8_t *e = (uint8_t *)spk_alloc_bytes(need, "リップシンク包絡");
        if (e == NULL) return false;
        s_playing = false;
        uint8_t *old = s_env;
        s_env = e;
        s_env_cap = need;
        if (old != NULL) heap_caps_free(old);
    }
    memset(s_env, 0, s_env_cap);
    s_env_max = 0;
    s_blk_sum = 0;
    s_blk_n   = 0;
    s_playing = false;
    return true;
}

bool saan_speaker_setup(uint32_t sample_rate) {
    if (sample_rate != 22050u) {
        ESP_LOGE(TAG, "想定外のサンプルレート %u（コアは 22,050 Hz 固定）", (unsigned)sample_rate);
        return false;
    }

    auto cfg = M5.config();
    cfg.clear_display = false;
    cfg.internal_mic  = false;   /* 使わない。マイクとスピーカーは排他の板もある */
    cfg.internal_spk  = true;
    M5.begin(cfg);

    auto scfg = M5.Speaker.config();
    scfg.sample_rate = SAAN_SPK_OUT_RATE;
    scfg.stereo      = false;
    /* dma_buf_len/count は既定（256 × 8 = 2,048 sample ≒ 93 ms @22.05k）のまま。 */
    M5.Speaker.config(scfg);

    if (!(M5.Speaker.begin() && M5.Speaker.isEnabled())) {
        /* ⚠️ **スピーカーを持たない板がある**（M5AtomS3 / M5StampS3 など）。
         *    ここで落とさないと「無音だが正常終了」になり原因が分からない。 */
        ESP_LOGE(TAG, "M5.Speaker が使えない（begin/isEnabled が false）。"
                      "スピーカー付きの板か、外付け I2S の設定が要る");
        return false;
    }
    M5.Speaker.setVolume(SAAN_SPK_VOLUME);

    ESP_LOGI(TAG, "M5.Speaker: 出力 %d Hz / 音源 22,050 Hz%s / volume %d",
             (int)SAAN_SPK_OUT_RATE,
             SAAN_SPK_OUT_RATE == 22050 ? "（リサンプル無し）" : "（M5 側でリサンプル）",
             (int)SAAN_SPK_VOLUME);
    ESP_LOGW(TAG, "⚠️ 実サンプルレートの誤差は**未測定**（S3 に APLL は無い）");

    s_ready = true;
    return true;
}

bool saan_speaker_push_f32(const float *pcm, size_t n_samples) {
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "saan_speaker_begin_utterance が済んでいない");
        return false;
    }
    if (s_fill + n_samples > s_cap) {
        ESP_LOGE(TAG, "発話バッファを超えた（%u + %u > %u sample）。"
                      "n_frames × SAAN_HOP と pull の合計が合っていない",
                 (unsigned)s_fill, (unsigned)n_samples, (unsigned)s_cap);
        return false;
    }
    conv_block(pcm, s_buf + s_fill, s_fill, n_samples);
    s_fill += n_samples;
    return true;
}

bool saan_speaker_start(void) {
    if (!s_ready) { ESP_LOGE(TAG, "saan_speaker_setup が済んでいない"); return false; }
    if (s_started) return true;
    s_started = true;
    s_playing = true;
    ESP_LOGI(TAG, "鳴らし始め: 貯めた %u / %u sample (%.0f%%) / 包絡 max %u",
             (unsigned)s_fill, (unsigned)s_cap,
             s_cap ? 100.0 * (double)s_fill / (double)s_cap : 0.0, (unsigned)s_env_max);
    /* 1 枚目 = 貯めたぶん、2 枚目 = **残り全部（まだ書いていない部分を含む）**。
     * バッファは追記しかしないので、合成が再生より先を書き続けている限り
     * （= 先読み量の条件）2 枚目はそのまま正しく鳴る。以後、渡す作業は無い。
     * ⚠️ 2 枚目を先に渡してしまうと 1 枚目より前に鳴るので順番を守る。 */
    if (!send_pending()) return false;
    if (s_sent < s_cap) {
        if (!M5.Speaker.playRaw(s_buf + s_sent, s_cap - s_sent, 22050, false, 1, SAAN_SPK_CH, false)) {
            ESP_LOGE(TAG, "M5.Speaker.playRaw（残り区間）が false を返した");
            return false;
        }
        s_sent = s_cap;
    }
    return true;
}

bool saan_speaker_pump(bool final) {
    (void)final;
    if (!s_started) return false;
    /* 渡す作業は start() で済んでいる。ここでは**再生が書き込みを追い越していないか**だけ見る。
     * 追い越されると、その区間は begin_utterance のゼロ埋め = 無音が鳴る（途切れ）。 */
    if (M5.Speaker.isPlaying(SAAN_SPK_CH) == 0) return false;
    int64_t dt = esp_timer_get_time() - s_play_t0_us;
    if (dt < 0) return false;
    size_t pos = s_play_base + (size_t)(dt * 22050 / 1000000) + SAAN_SPK_READAHEAD;
    return pos > s_fill;
}

void saan_speaker_stop(void) {
    /* ⚠️ **鳴らし終わるまで待つ。** ここで戻ると、次の発話が begin で解放して
     *    **前の発話の尾が化ける**。M5.Speaker.end() は呼ばない。 */
    while (M5.Speaker.isPlaying(SAAN_SPK_CH) != 0) {
        vTaskDelay(1);
    }
    s_playing = false;
    /* ⚠️ **再生が終わってから解放する。** playRaw はポインタを持つだけなので、
     *    先に free すると解放済みメモリを鳴らす（音は出るので気づけない）。 */
    if (s_buf != NULL) {
        heap_caps_free(s_buf);
        s_buf = NULL;
        s_cap = 0;
        s_fill = 0;
        s_sent = 0;
    }
    s_started = false;
}
