/* 音声出力 — **M5Unified を使わない版**（Stamp-C5 など画面もスピーカーも無いボード）。
 * ESP-IDF の driver/i2s_std を直叩きして外付けの I2S DAC/アンプに流す。
 *
 * 出所: 本家 sanoTTS-jp の esp32/main/saan_i2s.c + saan_pcm.c を、このリポジトリの saan_speaker.h の
 *       名前に合わせて 1 ファイルにした。給餌の考え方は同じ（プリロール → start → チャンクごとに
 *       i2s_channel_write。DMA が満杯なら**ブロック**する）。
 * ⚠️ リップシンクは無い（画面が無い）。saan_speaker_level_now() は 0 を返す。
 * ⚠️ **自分のボードの配線に合わせてピンを変えること。** 既定 BCLK G5 / WS G6 / DOUT G7 は本家と同じ仮置き。
 *    多くの I2S DAC ボード（MAX98357A / PCM5102 など）はこの 3 本で足りる。MCLK は使わない。
 *    -DSAAN_I2S_GPIO_BCLK= / _WS= / _DOUT= で変える（main/CMakeLists.txt が通す）。 */
#include "saan_speaker.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "saan_i2s";

#ifndef SAAN_I2S_GPIO_BCLK
#define SAAN_I2S_GPIO_BCLK 5
#endif
#ifndef SAAN_I2S_GPIO_WS
#define SAAN_I2S_GPIO_WS 6
#endif
#ifndef SAAN_I2S_GPIO_DOUT
#define SAAN_I2S_GPIO_DOUT 7
#endif

/* DMA バッファ。16 bit mono なので 1 バッファ = dma_frame_num * 2 B。
 * 6 × 512 = 3,072 フレーム ≒ 139 ms ぶん（1 バッファ 1,024 B は上限 4,092 B 以内）。本家と同じ。 */
#define SAAN_I2S_DMA_DESC  6
#define SAAN_I2S_DMA_FRAME 512

#define SAAN_I2S_MAXBUF 2048   /* 1 チャンク = 2,048 sample */

static i2s_chan_handle_t s_tx;
static bool s_ready;

/* 変換バッファ。**static にしてスタックから外す**（saan_irfft_1024 の自動変数 4,128 B と衝突する） */
static int16_t s_i16[SAAN_I2S_MAXBUF];
static int16_t s_preroll_static[SAAN_SPK_PREROLL_SAMPLES];

/* begin_utterance で決まる貯め先。静的バッファか、超えるときだけヒープ（-DSAAN_BUFFERED=1）。 */
static int16_t *s_preroll;
static int16_t *s_preroll_heap;
static size_t   s_preroll_cap;
static size_t   s_preroll_fill;
static size_t   s_fill;     /* この発話で変換した総サンプル数 */
static size_t   s_sent;     /* I2S に渡したサンプル数 */
static bool     s_started;

/* --- PCM 統計（本家 saan_pcm.c から逐語コピー。変換の順序・幅・丸めを変えないこと）--- */
#define FNV_OFFSET 1469598103934665603ull
#define FNV_PRIME  1099511628211ull
static uint32_t s_clips;
static uint64_t s_pcm_fnv = FNV_OFFSET;
static uint32_t s_pcm_n;
static int32_t  s_pcm_absmax;
static uint64_t s_pcm_sqsum;

int16_t saan_f32_to_i16(float x) {
    long v = lrintf(x * 32767.0f);
    if (v > 32767) { v = 32767; ++s_clips; }
    else if (v < -32768) { v = -32768; ++s_clips; }
    uint16_t u = (uint16_t)(int16_t)v;
    s_pcm_fnv = (s_pcm_fnv ^ (uint8_t)(u & 0xff)) * FNV_PRIME;
    s_pcm_fnv = (s_pcm_fnv ^ (uint8_t)(u >> 8)) * FNV_PRIME;
    { int32_t av = (int32_t)(v < 0 ? -v : v);
      if (av > s_pcm_absmax) s_pcm_absmax = av;
      s_pcm_sqsum += (uint64_t)((int64_t)v * (int64_t)v); }
    ++s_pcm_n;
    return (int16_t)v;
}

void saan_pcm_reset(void) {
    s_pcm_fnv = FNV_OFFSET;
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
float    saan_speaker_level_now(void)  { return 0.0f; }   /* 画面が無いのでリップシンクは無い */

/* --- I2S ------------------------------------------------------------------ */

bool saan_speaker_setup(uint32_t sample_rate) {
    if (sample_rate != 22050u) {
        ESP_LOGE(TAG, "想定外のサンプルレート %u（コアは 22,050 Hz 固定）", (unsigned)sample_rate);
        return false;
    }
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    cc.dma_desc_num  = SAAN_I2S_DMA_DESC;
    cc.dma_frame_num = SAAN_I2S_DMA_FRAME;
    cc.auto_clear    = true;   /* アンダーラン時に前のデータを繰り返さない */

    esp_err_t err = i2s_new_channel(&cc, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return false;
    }
    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SAAN_I2S_GPIO_BCLK,
            .ws   = SAAN_I2S_GPIO_WS,
            .dout = SAAN_I2S_GPIO_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &sc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "I2S 直叩き（M5Unified 無し）%" PRIu32 " Hz / 16bit / mono / BCLK G%d WS G%d DOUT G%d / "
                  "DMA %d x %d frames / preroll %d sample",
             sample_rate, SAAN_I2S_GPIO_BCLK, SAAN_I2S_GPIO_WS, SAAN_I2S_GPIO_DOUT,
             SAAN_I2S_DMA_DESC, SAAN_I2S_DMA_FRAME, (int)SAAN_SPK_PREROLL_SAMPLES);
    ESP_LOGW(TAG, "⚠️ 実サンプルレートの誤差は**未測定**");
    s_ready = true;
    return true;
}

bool saan_speaker_begin_utterance(size_t preroll_samples, size_t total_samples) {
    if (total_samples == 0) { ESP_LOGE(TAG, "0 sample の発話"); return false; }
    if (preroll_samples > total_samples) preroll_samples = total_samples;
    if (s_preroll != NULL) {
        ESP_LOGW(TAG, "前の発話のバッファが残っている。stop してから続ける");
        saan_speaker_stop();
    }
    if (preroll_samples <= (size_t)SAAN_SPK_PREROLL_SAMPLES) {
        s_preroll = s_preroll_static;
    } else {
        /* 貯めてから鳴らす方式（-DSAAN_BUFFERED=1）。1 秒 44,100 B。**取れなければ false で止める** */
        const size_t nb = preroll_samples * sizeof(int16_t);
        void *p = heap_caps_malloc(nb, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p == NULL) {
            p = heap_caps_malloc(nb, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (p == NULL) {
                ESP_LOGE(TAG, "発話バッファ %u B を確保できない。-DSAAN_BUFFERED=0 にするか文を短くすること",
                         (unsigned)nb);
                return false;
            }
            ESP_LOGW(TAG, "発話バッファ %u B を**内部 DRAM**から確保した（PSRAM が無い構成）", (unsigned)nb);
        }
        s_preroll_heap = (int16_t *)p;
        s_preroll = s_preroll_heap;
    }
    s_preroll_cap  = preroll_samples;
    s_preroll_fill = 0;
    s_fill = 0;
    s_sent = 0;
    s_started = false;
    return true;
}

bool saan_speaker_preroll_push(const float *pcm, size_t n_samples) {
    if (s_preroll == NULL) {
        ESP_LOGE(TAG, "saan_speaker_begin_utterance が済んでいない");
        return false;
    }
    if (s_preroll_fill + n_samples > s_preroll_cap) {
        ESP_LOGE(TAG, "プリロールを超えた（%u + %u > %u sample）",
                 (unsigned)s_preroll_fill, (unsigned)n_samples, (unsigned)s_preroll_cap);
        return false;
    }
    for (size_t i = 0; i < n_samples; ++i)
        s_preroll[s_preroll_fill + i] = saan_f32_to_i16(pcm[i]);
    s_preroll_fill += n_samples;
    s_fill += n_samples;
    return true;
}

static bool write_i16(const int16_t *p, size_t n) {
    size_t wrote = 0;
    esp_err_t err = i2s_channel_write(s_tx, p, n * sizeof(int16_t), &wrote, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_write: %s", esp_err_to_name(err));
        return false;
    }
    if (wrote != n * sizeof(int16_t)) {
        ESP_LOGE(TAG, "i2s_channel_write が %u/%u B しか書かなかった",
                 (unsigned)wrote, (unsigned)(n * sizeof(int16_t)));
        return false;
    }
    s_sent += n;
    return true;
}

bool saan_speaker_start(void) {
    if (!s_ready) { ESP_LOGE(TAG, "saan_speaker_setup が済んでいない"); return false; }
    if (s_started) return true;
    esp_err_t err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable: %s", esp_err_to_name(err));
        return false;
    }
    s_started = true;
    if (s_preroll_fill > 0)
        ESP_LOGI(TAG, "貯めた %u sample (%.3f s) を送出", (unsigned)s_preroll_fill,
                 (double)s_preroll_fill / 22050.0);
    /* I2S の 1 回の書き込みは SAAN_I2S_MAXBUF ずつに割る（貯めてから鳴らす方式では数十万 sample になる） */
    size_t off = 0;
    while (off < s_preroll_fill) {
        size_t n = s_preroll_fill - off;
        if (n > (size_t)SAAN_I2S_MAXBUF) n = SAAN_I2S_MAXBUF;
        if (!write_i16(s_preroll + off, n)) return false;
        off += n;
    }
    s_preroll_fill = 0;
    return true;
}

bool saan_speaker_write_f32(const float *pcm, size_t n_samples) {
    if (!s_started) { ESP_LOGE(TAG, "saan_speaker_start が済んでいない"); return false; }
    while (n_samples > 0) {
        size_t n = n_samples > SAAN_I2S_MAXBUF ? SAAN_I2S_MAXBUF : n_samples;
        for (size_t i = 0; i < n; ++i) s_i16[i] = saan_f32_to_i16(pcm[i]);
        s_fill += n;
        if (!write_i16(s_i16, n)) return false;
        pcm += n; n_samples -= n;
    }
    return true;
}

void saan_speaker_stop(void) {
    /* ⚠️ i2s_channel_write は DMA に渡し終えた時点で返る。最後の DMA バッファ
     *    （139 ms ぶん）が鳴り切るまで待ってから disable する。待たないと語尾が切れる */
    if (s_tx != NULL && s_started) {
        vTaskDelay(pdMS_TO_TICKS(SAAN_I2S_DMA_DESC * SAAN_I2S_DMA_FRAME * 1000 / 22050 + 10));
        i2s_channel_disable(s_tx);
    }
    if (s_preroll_heap != NULL) {
        heap_caps_free(s_preroll_heap);
        s_preroll_heap = NULL;
    }
    s_preroll = NULL;
    s_preroll_cap = 0;
    s_preroll_fill = 0;
    s_started = false;
}
