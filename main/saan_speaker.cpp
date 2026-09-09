/* 音声出力 — M5Unified の M5.Speaker 版（M5Stack CoreS3 の内蔵アンプ AW88298）。
 *
 * 給餌の仕組みは本家 sanoTTS-jp の esp32/boards/m5unified/main/saan_audio_m5.cpp と同じ
 * （2026-09-10 に合わせた。それ以前は「発話バッファ 1 本を 2 区間で渡し、追い越しを監視する」方式だった）。
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
 *    リサンプルは通らない。AW88298 は 22.05 kHz を対応レートとして持つ（レジスタ 0x06 I2SSR）。
 *    ⚠️ ESP32-S3 に APLL が無い件は変わらない。**実サンプルレートの誤差は未測定。**
 *
 * ⚠️ **checksum は M5 に渡す前の int16 で取る。** `saan_f32_to_i16()` は
 *    本家 saan_pcm.c（旧 saan_i2s.c）から**逐語コピー**してある。本家の記録値と
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

/* M5.Speaker が実際に出す I2S のサンプルレート。**コアと同じ 22,050 Hz** にして
 * M5 側のリサンプルを通さない。 */
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

#define SAAN_SPK_CH 0   /* 使う仮想チャンネル */

/* Stamp-C5 の外付け I2S DAC のピン（SAAN_BOARD_STAMPC5 のときだけ使う） */
#ifndef SAAN_I2S_GPIO_BCLK
#define SAAN_I2S_GPIO_BCLK 5
#endif
#ifndef SAAN_I2S_GPIO_WS
#define SAAN_I2S_GPIO_WS 6
#endif
#ifndef SAAN_I2S_GPIO_DOUT
#define SAAN_I2S_GPIO_DOUT 7
#endif

/* リップシンク包絡の 1 ブロック（sample）。256 = 11.6 ms。lip_task の 10 ms と同じ粒度。
 * ブロック間は saan_speaker_level_now() が線形補間する。
 * 1 発話あたり uint8_t × (sample / 256) なので 10 秒の音声でも 862 B（PSRAM）。 */
#define SAAN_ENV_BLOCK 256

/* playRaw してから実際に鳴るまでの遅れ（DMA バッファぶん）の見込み。
 * ⚠️ **測っていない。** 2,048 sample = 93 ms の DMA バッファの半分を仮置き。 */
#ifndef SAAN_LIP_LATENCY_MS
#define SAAN_LIP_LATENCY_MS 45
#endif

/* --- リング（起動時に確保、解放しない）------------------------------------
 *
 * ⚠️ **ヒープから一度だけ確保し、二度と解放しない。** `playRaw` が持っている
 *    ポインタの生存期間はプログラムと同じでなければならない。
 *    合わせて 12,288 B。PSRAM があればそちら、無ければ内部 DRAM（spk_alloc）。
 * ⚠️ **スタックには置けない**（saan_irfft_1024 の自動変数 4,128 B と衝突する）。 */
static int16_t *s_ring[SAAN_SPK_NBUF];
static size_t   s_ring_idx;

/* プリロール（begin_utterance で取り、stop で解放） */
static int16_t *s_preroll;
static size_t   s_preroll_cap;           /* begin_utterance で取った量（sample） */
static size_t   s_preroll_fill;

static volatile size_t s_fill;           /* この発話で変換した総サンプル数（プリロール + ストリーミング） */
static size_t   s_sent;                  /* キューに渡したサンプル数 */
static bool     s_started;
static bool     s_ready;

static uint32_t s_clips;

/* --- リップシンク --------------------------------------------------------- */
static uint8_t *s_env;                   /* SAAN_ENV_BLOCK ごとの RMS/16（0..255）。PSRAM */
static size_t   s_env_cap;               /* 要素数（伸ばすだけで縮めない） */
static volatile uint8_t s_env_max;       /* この発話の包絡の最大（正規化用） */
static uint64_t s_blk_sum;               /* いまのブロックの Σx² */
static size_t   s_blk_n;
static volatile int64_t s_play_t0_us;    /* 鳴らし始めた時刻 */
static volatile size_t  s_play_base;     /* その時刻に鳴り始めたサンプル位置 */
static volatile bool    s_playing;

/* --- 以下 4 つの統計は本家 saan_pcm.c から逐語コピー --------------------- */
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
    if (p != NULL) {
        ESP_LOGI(TAG, "%s (%u B) を PSRAM に確保", what, (unsigned)nb);
        return p;
    }
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
 * 絶対位置（s_fill）で決まるので、チャンク長が 256 の倍数でなくても正しい。 */
static void conv_block(const float *pcm, int16_t *dst, size_t n) {
    const size_t base = s_fill;
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
    s_fill = base + n;
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
    /* ブロック内の位置で隣のブロックと線形補間する（段差を無くす）。
     * ⚠️ 次のブロックは**変換が済んでいるときだけ**使う。未変換のところは memset の 0 なので、
     *    合成が再生に近いときに 0 へ向かって口が閉じてしまう。 */
    float e = (float)s_env[idx];
    if (idx + 1 < s_env_cap && (idx + 2) * SAAN_ENV_BLOCK <= s_fill) {
        float t = (float)(pos - idx * SAAN_ENV_BLOCK) / (float)SAAN_ENV_BLOCK;   /* 0..1 */
        e = e + t * ((float)s_env[idx + 1] - e);
    }
    uint32_t m = s_env_max;
    if (m < 32) m = 32;                           /* 無音に近い発話で 0/0 を作らない */
    if (e < 6.0f) return 0.0f;                    /* 床（RMS < 96）。息の音で口が震えない */
    float r = e / (float)m;
    return r > 1.0f ? 1.0f : r;
}

/* --- M5 への送出 ---------------------------------------------------------- */

static bool spk_play(const int16_t *p, size_t n) {
    if (n == 0) return true;
    /* 止まっていたなら、この区間が今から鳴る。リップシンクの再生時計を取り直す */
    if (M5.Speaker.isPlaying(SAAN_SPK_CH) == 0) {
        s_play_base  = s_sent;
        s_play_t0_us = esp_timer_get_time();
    }
    /* repeat=1 / channel=0 / stop_current=false。
     * ⚠️ キューが満杯なら _set_next_wav がブロックして戻ってくる。
     *    false が返るのは「もう片方のスロットが無限ループ再生」のときだけで、
     *    ここでは起こらないが、**握りつぶさずに落とす**。 */
    if (!M5.Speaker.playRaw(p, n, SAAN_SPK_OUT_RATE, false, 1, SAAN_SPK_CH, false)) {
        ESP_LOGE(TAG, "M5.Speaker.playRaw が false を返した (%u sample)", (unsigned)n);
        return false;
    }
    s_sent += n;
    return true;
}

extern "C" {

bool saan_speaker_setup(uint32_t sample_rate) {
    if (sample_rate != 22050u) {
        ESP_LOGE(TAG, "想定外のサンプルレート %u（コアは 22,050 Hz 固定）", (unsigned)sample_rate);
        return false;
    }

    /* ⚠️ **M5.begin() より先に取る。** 失敗したら M5 を初期化しないで戻る。 */
    if (s_ring[0] == NULL) {
        for (int i = 0; i < SAAN_SPK_NBUF; ++i) {
            s_ring[i] = (int16_t *)spk_alloc_bytes(SAAN_SPK_MAXBUF * sizeof(int16_t), "リングバッファ");
            if (s_ring[i] == NULL) return false;
        }
    }

    auto cfg = M5.config();
    /* ⚠️ **ディスプレイは clear しない。** 起動ログを消してしまうと
     *    実機で最初に見たい情報が消える。 */
    cfg.clear_display = false;
    cfg.internal_mic  = false;   /* 使わない。マイクとスピーカーは排他の板もある */
    cfg.internal_spk  = true;
#if SAAN_BOARD_ATOMS3
    /* ATOMS3 + Atomic Voice Base（旧名 Atomic Echo Base: ES8311 + NS4150B）。本体にスピーカーは
     * 無いので、M5Unified の external_speaker.atomic_echo で Base の I2S（BCK G8 / WS G6 / DOUT G5）
     * と ES8311（I2C G38/G39、addr 0x18）を有効にする。-DSAAN_BOARD=atoms3 のときだけ。
     * ⚠️ M5Unified は Base の有無を probe しない（ピンを決め打ちする）。Base を外して起動すると
     *    無音のまま正常終了する。 */
    cfg.internal_spk = false;
    cfg.external_speaker.atomic_echo = true;
#endif
#if SAAN_BOARD_STAMPC5
    cfg.internal_spk = false;    /* 本体にスピーカーは無い。下で外付け I2S DAC のピンを渡す */
#endif
    M5.begin(cfg);

    auto scfg = M5.Speaker.config();
    scfg.sample_rate = SAAN_SPK_OUT_RATE;
    scfg.stereo      = false;
#if SAAN_BOARD_STAMPC5
    /* Stamp-C5: 外付け I2S DAC/アンプ（MAX98357A / PCM5102 など）。既定のピンは本家 DevKit 実装
     * （esp32/main/saan_i2s.c）と同じ BCLK G5 / WS G6 / DOUT G7。-DSAAN_I2S_GPIO_BCLK= などで変える。
     * ⚠️ M5Unified はこのボードにスピーカーの既定を持たないので、ここで渡さないと begin() が false。 */
    scfg.pin_bck      = SAAN_I2S_GPIO_BCLK;
    scfg.pin_ws       = SAAN_I2S_GPIO_WS;
    scfg.pin_data_out = SAAN_I2S_GPIO_DOUT;
    scfg.i2s_port     = I2S_NUM_0;
    scfg.magnification = 16;
#endif
    /* dma_buf_len/count は既定（256 × 8 = 2,048 sample ≒ 93 ms @22.05k）のまま。
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

    ESP_LOGI(TAG, "M5.Speaker: board %d / 出力 %d Hz / 音源 22,050 Hz%s"
                  " / volume %d / バッファ %d 枚 × %d sample",
             (int)M5.getBoard(), (int)SAAN_SPK_OUT_RATE,
             SAAN_SPK_OUT_RATE == 22050 ? "（リサンプル無し）" : "（M5 側でリサンプル）",
             (int)SAAN_SPK_VOLUME, (int)SAAN_SPK_NBUF, (int)SAAN_SPK_MAXBUF);
    ESP_LOGW(TAG, "⚠️ 実サンプルレートの誤差は**未測定**（S3 に APLL は無い）");

    s_ring_idx = 0;
    s_ready = true;
    return true;
}

bool saan_speaker_begin_utterance(size_t preroll_samples, size_t total_samples) {
    if (total_samples == 0) { ESP_LOGE(TAG, "0 sample の発話"); return false; }
    if (preroll_samples > total_samples) preroll_samples = total_samples;
    if (s_preroll != NULL) {
        /* stop() を呼ばずに次の発話に来た。前の再生が終わっているとは限らない。 */
        ESP_LOGW(TAG, "前の発話のバッファが残っている。再生完了を待って解放する");
        saan_speaker_stop();
    }
    s_preroll = (int16_t *)spk_alloc_bytes(preroll_samples * sizeof(int16_t), "プリロール");
    if (s_preroll == NULL) return false;
    s_preroll_cap  = preroll_samples;
    s_preroll_fill = 0;
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
    conv_block(pcm, s_preroll + s_preroll_fill, n_samples);
    s_preroll_fill += n_samples;
    return true;
}

bool saan_speaker_start(void) {
    if (!s_ready) { ESP_LOGE(TAG, "saan_speaker_setup が済んでいない"); return false; }
    if (s_started) return true;
    s_started = true;
    s_playing = true;
    if (s_preroll_fill > 0) {
        ESP_LOGI(TAG, "貯めた %u sample (%.3f s) を送出 / 包絡 max %u", (unsigned)s_preroll_fill,
                 (double)s_preroll_fill / 22050.0, (unsigned)s_env_max);
        /* ⚠️ s_preroll は stop() が再生完了を待ってから解放する。playRaw が
         *    ポインタを持っている間は生きている。 */
        if (!spk_play(s_preroll, s_preroll_fill)) return false;
    }
    return true;
}

bool saan_speaker_write_f32(const float *pcm, size_t n_samples) {
    if (!s_started) { ESP_LOGE(TAG, "saan_speaker_start が済んでいない"); return false; }
    if (n_samples > (size_t)SAAN_SPK_MAXBUF) {
        ESP_LOGE(TAG, "チャンクが大きすぎる %u > %d sample",
                 (unsigned)n_samples, (int)SAAN_SPK_MAXBUF);
        return false;
    }
    /* ⚠️ **変換してから playRaw する。** 生きているポインタは最大 2 本
     *    （current + next）で、3 枚回しなので今から書く s_ring[s_ring_idx] は
     *    2 回前に queue したもの = 既に再生済み。 */
    int16_t *dst = s_ring[s_ring_idx];
    conv_block(pcm, dst, n_samples);
    s_ring_idx = (s_ring_idx + 1) % SAAN_SPK_NBUF;

    return spk_play(dst, n_samples);
}

void saan_speaker_stop(void) {
    /* ⚠️ **鳴らし終わるまで待つ。** ここで戻ると、次の発話の変換がプリロールを解放し
     *    リングを上書きして**前の発話の尾が化ける**。M5.Speaker.end() は呼ばない。 */
    while (M5.Speaker.isPlaying(SAAN_SPK_CH) != 0) {
        vTaskDelay(1);
    }
    s_playing = false;
    /* ⚠️ **再生が終わってから解放する。** playRaw はポインタを持つだけなので、
     *    先に free すると解放済みメモリを鳴らす（音は出るので気づけない）。 */
    if (s_preroll != NULL) {
        heap_caps_free(s_preroll);
        s_preroll = NULL;
        s_preroll_cap = 0;
        s_preroll_fill = 0;
    }
    s_started = false;
}

} /* extern "C" */
