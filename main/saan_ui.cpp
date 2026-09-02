/* 顔とタッチ — m5stack-avatar 版。設計は saan_ui.h を読むこと。
 *
 * avatar.init() が描画タスク（drawLoop 優先度 1 / facialLoop 優先度 2）を core 1 に作る。
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
#include "saan_ui.h"

using namespace m5avatar;

static const char *TAG = "saan_ui";

static Avatar s_avatar;
static bool   s_ready;

/* 吹き出しの文。UTF-8 で SAAN_UI_TEXT_CHARS 文字に切り詰める（吹き出しは右下に
 * 固定幅なので、長い行は顔を覆う）。 */
#define SAAN_UI_TEXT_CHARS 14
static char s_text[SAAN_UI_TEXT_CHARS * 4 + 4];

/* lip_task の統計（speaking() でリセット）。「口が動いたか」をログで確認するため。 */
static volatile uint32_t s_lip_frames, s_lip_open;
static volatile float    s_lip_max;

/* 口の開き = 再生中の音量。saan_speaker が包絡（512 sample ごとの RMS）と
 * 再生位置を持っているので、ここは読んで渡すだけ。 */
static void lip_task(void *arg) {
    DriveContext *ctx = reinterpret_cast<DriveContext *>(arg);
    Avatar *av = ctx->getAvatar();
    for (;;) {
        float r = saan_speaker_level_now();
        av->setMouthOpenRatio(r);
        ++s_lip_frames;
        if (r > 0.1f) ++s_lip_open;
        if (r > s_lip_max) s_lip_max = r;
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

bool saan_ui_init(void) {
    if (M5.getBoard() == m5::board_t::board_unknown) {
        ESP_LOGE(TAG, "M5.begin() がまだ。saan_speaker_setup() の後に呼ぶこと");
        return false;
    }
    s_avatar.setSpeechFont(&fonts::lgfxJapanGothic_16);
    s_avatar.init();   /* drawLoop / facialLoop を core 1 に作る */
    s_avatar.addTask(lip_task, "lipSync", 2048, 2, NULL, APP_CPU_NUM);
    s_ready = true;
    ESP_LOGI(TAG, "m5stack-avatar 起動（core %d）/ 吹き出し lgfxJapanGothic_16 / リップシンク 33 ms",
             (int)APP_CPU_NUM);
    return true;
}

void saan_ui_set_text(const char *text) {
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

void saan_ui_thinking(void) {
    if (!s_ready) return;
    s_avatar.setExpression(Expression::Doubt);
    s_avatar.setMouthOpenRatio(0.0f);
    s_avatar.setSpeechText("…");
}

void saan_ui_speaking(void) {
    if (!s_ready) return;
    s_lip_frames = 0; s_lip_open = 0; s_lip_max = 0.0f;
    s_avatar.setExpression(Expression::Happy);
    s_avatar.setSpeechText(s_text);
}

void saan_ui_idle(const char *status) {
    if (!s_ready) return;
    s_avatar.setExpression(Expression::Neutral);
    s_avatar.setMouthOpenRatio(0.0f);
    s_avatar.setSpeechText(status != NULL ? status : "");
}

bool saan_ui_poll_touch(void) {
    if (!s_ready) return false;
    M5.update();
    const auto n = M5.Touch.getCount();
    for (size_t i = 0; i < n; ++i) {
        const auto t = M5.Touch.getDetail(i);
        if (t.wasPressed()) {
            ESP_LOGI(TAG, "タッチ x=%d y=%d", (int)t.x, (int)t.y);
            return true;
        }
    }
    return false;
}

void saan_ui_lip_stats(uint32_t *frames, uint32_t *frames_open, float *max_ratio) {
    if (frames) *frames = s_lip_frames;
    if (frames_open) *frames_open = s_lip_open;
    if (max_ratio) *max_ratio = s_lip_max;
}
