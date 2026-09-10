/* 顔 — m5stack-avatar 版。saan_ui_impl_avatar として saan_ui.cpp に渡す。設計は saan_ui.h /
 * saan_ui_impl.h を読むこと。入力（タッチ / ボタン）はここでは扱わない（saan_ui.cpp）。
 *
 * 最初の enter() で avatar.init() が描画タスク（drawLoop 優先度 1 / facialLoop 優先度 2）を core 1 に作る。
 * 文字画面へ切り替えるときは leave() が drawLoop を suspend し（facialLoop と lip_task は回り続けるが
 * 描かない）、戻るときは resume() で顔が画面全体を描き直す。
 * リップシンクの lip_task も core 1（優先度 2）。合成タスクは core 0（main.c）。
 * ⚠️ **合成タスクと同じ core に置かない。** 合成は数秒間 CPU を手放さないので、
 *    同じ core の低優先度タスクは止まる（顔が固まる）。
 */
#include <M5Unified.h>
#include <Avatar.h>

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "saan_speaker.h"
#include "saan_ui_impl.h"

using namespace m5avatar;

static const char *TAG = "saan_ui";

static Avatar s_avatar;
static bool   s_ready;     /* init 済み */
static bool   s_started;   /* 描画タスクを作った（最初の enter） */
static bool   s_small;     /* 128 x 128 */

/* 吹き出しの文。UTF-8 で SAAN_UI_TEXT_CHARS 文字に切り詰める（吹き出しは右下に
 * 固定幅なので、長い行は顔を覆う）。 */
#define SAAN_UI_TEXT_CHARS 14
static char s_text[SAAN_UI_TEXT_CHARS * 4 + 4];

/* lip_task の統計（speaking() でリセット）。「口が動いたか」をログで確認するため。 */
static volatile uint32_t s_lip_frames, s_lip_open;
static volatile float    s_lip_max;

/* リップシンクの更新周期。avatar の drawLoop は 10 ms ごとに描き直す（Avatar.cpp の
 * TaskDelay(10)）ので、それより粗いとここが上限になる。以前は 33 ms（30 Hz）だった。
 * 包絡側（saan_speaker.cpp の SAAN_ENV_BLOCK = 256 sample = 11.6 ms）と同じ粒度。 */
#ifndef SAAN_LIP_PERIOD_MS
#define SAAN_LIP_PERIOD_MS 10
#endif

/* 口の開き = 再生中の音量。saan_speaker が包絡（SAAN_ENV_BLOCK sample ごとの RMS を
 * ブロック間で線形補間）と再生位置を持っているので、ここは読んで渡すだけ。 */
static void lip_task(void *arg) {
    DriveContext *ctx = reinterpret_cast<DriveContext *>(arg);
    Avatar *av = ctx->getAvatar();
    for (;;) {
        float r = saan_speaker_level_now();
        av->setMouthOpenRatio(r);
        ++s_lip_frames;
        if (r > 0.1f) ++s_lip_open;
        if (r > s_lip_max) s_lip_max = r;
        vTaskDelay(pdMS_TO_TICKS(SAAN_LIP_PERIOD_MS));
    }
}

static bool avatar_init(void) {
    if (M5.Display.width() == 0) {
        ESP_LOGE(TAG, "画面が無いボードで顔は出せない");
        return false;
    }
    s_avatar.setSpeechFont(&fonts::lgfxJapanGothic_16);
    /* 小さい画面（ATOMS3 / ATOMS3R の 128 x 128）は顔を縮める。値は m5stack-avatar の AtomS3 例と同じ
     * （scale 0.4、320 x 240 の顔を左上へ寄せる）。顔のスプライトは 1 bit（init の既定）なので
     * PSRAM の無い ATOMS3 でも 128 x 128 / 8 = 2 KB。 */
    s_small = M5.Display.width() <= 128;
    if (s_small) {
        s_avatar.setScale(0.4f);
        s_avatar.setPosition(-56, -96);
    }
    s_ready = true;
    return true;
}

static void avatar_enter(void) {
    if (!s_ready) return;
    if (!s_started) {
        s_avatar.init();   /* drawLoop / facialLoop を core 1 に作る */
        s_avatar.addTask(lip_task, "lipSync", 2048, 2, NULL, APP_CPU_NUM);
        s_started = true;
        ESP_LOGI(TAG, "m5stack-avatar 起動（core %d）/ 画面 %d x %d%s / 吹き出し lgfxJapanGothic_16 / リップシンク %d ms",
                 (int)APP_CPU_NUM, (int)M5.Display.width(), (int)M5.Display.height(),
                 s_small ? "（scale 0.4）" : "", (int)SAAN_LIP_PERIOD_MS);
    } else {
        s_avatar.resume();   /* drawLoop を再開。次のフレームで画面全体を描き直す */
    }
}

static void avatar_leave(void) {
    if (s_started) s_avatar.suspend();   /* drawLoop だけ止める。文字画面が上書きする */
}

static void avatar_set_text(const char *text) {
    if (text == NULL) { s_text[0] = '\0'; return; }
    size_t n = strlen(text), i = 0, chars = 0;
    while (i < n && chars < SAAN_UI_TEXT_CHARS) {
        size_t len = 1;
        unsigned char c = (unsigned char)text[i];
        if (c >= 0xF0) len = 4; else if (c >= 0xE0) len = 3; else if (c >= 0xC0) len = 2;
        if (i + len > n) break;
        i += len; ++chars;
    }
    memcpy(s_text, text, i);
    s_text[i] = '\0';
    if (i < n) strcat(s_text, "…");
}

static void avatar_thinking(void) {
    if (!s_ready) return;
    s_avatar.setExpression(Expression::Doubt);
    s_avatar.setMouthOpenRatio(0.0f);
    s_avatar.setSpeechText("…");
}

static void avatar_speaking(void) {
    if (!s_ready) return;
    s_lip_frames = 0; s_lip_open = 0; s_lip_max = 0.0f;
    s_avatar.setExpression(Expression::Happy);
    s_avatar.setSpeechText(s_text);
}

static void avatar_idle(const char *status) {
    if (!s_ready) return;
    s_avatar.setExpression(Expression::Neutral);
    s_avatar.setMouthOpenRatio(0.0f);
    s_avatar.setSpeechText(status != NULL ? status : "");
}

static void avatar_lip_stats(uint32_t *frames, uint32_t *frames_open, float *max_ratio) {
    if (frames) *frames = s_lip_frames;
    if (frames_open) *frames_open = s_lip_open;
    if (max_ratio) *max_ratio = s_lip_max;
}

extern "C" const saan_ui_impl_t saan_ui_impl_avatar = {
    "avatar", avatar_init, avatar_enter, avatar_leave, avatar_set_text,
    avatar_thinking, avatar_speaking, avatar_idle, avatar_lip_stats,
};
