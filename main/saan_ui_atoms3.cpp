/* 画面とボタン — **ATOMS3 版**（128 x 128 LCD、M5GFX だけ。m5stack-avatar を使わない）。
 * 設計は saan_ui.h を読むこと。`-DSAAN_BOARD=atoms3` で選ばれる（main/CMakeLists.txt）。
 *
 * saan_ui_text.cpp（CoreS3 の 320 x 240）を 128 x 128 に詰めたもの。違い:
 *   - フォントは lgfxJapanGothic_16（1 行 8 文字、上段 5 行）
 *   - 「もう一度」は**本体ボタン**（画面を押し込む BtnA、GPIO41）。ATOMS3 にタッチは無い
 *   - 出典は Font0（6 x 8）で 1 行。128 px に収まらないので短縮形
 *
 * ⚠️ **描画も M5.update() も合成タスクからだけ呼ぶ**（saan_ui.h）。
 * ⚠️ リップシンクは無い。saan_ui_lip_stats() は 0 を返す。 */
#include <M5Unified.h>

#include <string.h>

#include "esp_log.h"

#include "saan_console.h"   /* SAAN_CONSOLE_LINE_MAX（文の最大長） */
#include "saan_ui.h"

static const char *TAG = "saan_ui";

/* 出典（128 px = Font0 で 21 文字。モデルの帰属表示の要約） */
#define SAAN_UI_ORIGIN "sanoTTS-jp v3 int8"

static bool s_ready;
static char s_text[SAAN_CONSOLE_LINE_MAX];

/* 下段の開始 y。128 px のうち下 24 px をステータスに使う */
#define UI_STATUS_Y 104
#define UI_MARGIN_X 2

static const lgfx::IFont *ui_font(void) { return &fonts::lgfxJapanGothic_16; }

/* 上段: 文。`dim` なら合成中の色（灰）、そうでなければ白 */
static void draw_text(bool dim) {
    auto &d = M5.Display;
    d.fillRect(0, 0, d.width(), UI_STATUS_Y, TFT_BLACK);
    d.setFont(ui_font());
    d.setTextWrap(true, false);
    d.setTextColor(dim ? TFT_LIGHTGREY : TFT_WHITE, TFT_BLACK);
    d.setCursor(UI_MARGIN_X, 2);
    d.print(s_text);

    d.setFont(&fonts::Font0);
    d.setTextWrap(false, false);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setCursor(UI_MARGIN_X, UI_STATUS_Y - 10);
    d.print(SAAN_UI_ORIGIN);
    d.setFont(ui_font());
}

/* 下段: ステータス 1 行 */
static void draw_status(const char *s, uint32_t color) {
    auto &d = M5.Display;
    d.fillRect(0, UI_STATUS_Y, d.width(), d.height() - UI_STATUS_Y, TFT_BLACK);
    d.drawFastHLine(0, UI_STATUS_Y, d.width(), TFT_DARKGREY);
    d.setFont(ui_font());
    d.setTextWrap(false, false);
    d.setTextColor(color, TFT_BLACK);
    d.setCursor(UI_MARGIN_X, UI_STATUS_Y + 4);
    d.print(s != NULL ? s : "");
}

extern "C" {

bool saan_ui_init(void) {
    if (M5.getBoard() == m5::board_t::board_unknown) {
        ESP_LOGE(TAG, "M5.begin() がまだ。saan_speaker_setup() の後に呼ぶこと");
        return false;
    }
    auto &d = M5.Display;
    if (d.width() == 0) {
        ESP_LOGW(TAG, "画面が無いボード。表示は使わない（ボタンは使う）");
        s_ready = true;
        return true;
    }
    d.setBrightness(96);
    d.fillScreen(TFT_BLACK);
    d.setFont(ui_font());
    d.setTextSize(1);
    d.setTextWrap(true, false);
    s_ready = true;
    s_text[0] = '\0';
    draw_text(false);
    draw_status("起動中…", TFT_YELLOW);
    ESP_LOGI(TAG, "ATOMS3 UI（-DSAAN_BOARD=atoms3）/ 画面 %d x %d / フォント lgfxJapanGothic_16 / ボタン A で再生",
             (int)d.width(), (int)d.height());
    return true;
}

void saan_ui_set_text(const char *text) {
    if (text == NULL) { s_text[0] = '\0'; return; }
    strncpy(s_text, text, sizeof s_text - 1);
    s_text[sizeof s_text - 1] = '\0';
}

void saan_ui_thinking(void) {
    if (!s_ready || M5.Display.width() == 0) return;
    draw_text(true);
    draw_status("合成中…", TFT_YELLOW);
}

void saan_ui_speaking(void) {
    if (!s_ready || M5.Display.width() == 0) return;
    draw_text(false);
    draw_status("再生中", TFT_GREEN);
}

void saan_ui_idle(const char *status) {
    if (!s_ready || M5.Display.width() == 0) return;
    /* CoreS3 の「タッチでもう一度」は ATOMS3 では意味が違うので言い換える */
    if (status != NULL && strcmp(status, "タッチでもう一度") == 0) status = "ボタンでもう一度";
    draw_status(status, TFT_CYAN);
}

/* 本体ボタン（画面の押し込み）。押された瞬間だけ true */
bool saan_ui_poll_touch(void) {
    if (!s_ready) return false;
    M5.update();
    if (M5.BtnA.wasPressed()) {
        ESP_LOGI(TAG, "ボタン A");
        return true;
    }
    return false;
}

/* リップシンクは無い。0 を返す（main.c が「顔なし」と判定する） */
void saan_ui_lip_stats(uint32_t *frames, uint32_t *frames_open, float *max_ratio) {
    if (frames) *frames = 0;
    if (frames_open) *frames_open = 0;
    if (max_ratio) *max_ratio = 0.0f;
}

} /* extern "C" */
