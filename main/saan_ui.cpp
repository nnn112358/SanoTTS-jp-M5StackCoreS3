/* 画面とタッチ — M5GFX 版。設計は saan_ui.h を読むこと。
 *
 * フォントは M5GFX 同梱の lgfxJapanGothic_20（IPA ゴシック由来、U8g2 形式）。
 * ⚠️ **フォントは flash (.rodata) に置かれる。** サイズごとに別の配列なので、
 *    使うサイズを増やすとそのぶん app が太る（1 サイズで数百 KB）。1 サイズで済ませる。
 */
#include <M5Unified.h>

#include <stdarg.h>
#include <stdio.h>

#include "esp_log.h"

#include "saan_model.h"   /* SAAN_MODEL_ORIGIN_* */
#include "saan_ui.h"

static const char *TAG = "saan_ui";

static bool s_ready;

/* 下段の開始 y。240 px のうち下 52 px をステータスに使う（2 行） */
#define UI_STATUS_Y 188   /* 2 行ぶん（20 px フォント × 2 + 余白） */
#define UI_MARGIN_X 4

static const lgfx::IFont *ui_font(void) { return &fonts::lgfxJapanGothic_20; }

bool saan_ui_init(void) {
    if (M5.getBoard() == m5::board_t::board_unknown) {
        ESP_LOGE(TAG, "M5.begin() がまだ。saan_speaker_setup() の後に呼ぶこと");
        return false;
    }
    auto &d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setFont(ui_font());
    d.setTextSize(1);
    d.setTextWrap(true, false);
    s_ready = true;
    ESP_LOGI(TAG, "画面 %d x %d / フォント lgfxJapanGothic_20", (int)d.width(), (int)d.height());
    return true;
}

void saan_ui_show(const char *title, const char *kana) {
    if (!s_ready) return;
    auto &d = M5.Display;
    d.fillRect(0, 0, d.width(), UI_STATUS_Y, TFT_BLACK);
    d.setFont(ui_font());
    d.setTextWrap(true, false);

    d.setCursor(UI_MARGIN_X, 6);
    if (title != NULL && title[0] != '\0') {
        d.setTextColor(TFT_WHITE, TFT_BLACK);
        d.print(title);
        d.print("\n");
        d.setCursor(UI_MARGIN_X, d.getCursorY() + 10);
    }
    d.setTextColor(TFT_CYAN, TFT_BLACK);
    if (kana != NULL) d.print(kana);

    /* 出典を常時表示する（NOTICE.md の帰属表示の要約）。
     * Font0 は M5GFX 組み込みの 6x8 ASCII で、日本語フォントと違い flash を食わない。 */
    d.setFont(&fonts::Font0);
    d.setTextWrap(false, false);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setCursor(UI_MARGIN_X, UI_STATUS_Y - 12);
    d.print("model: " SAAN_MODEL_ORIGIN_NAME "  " SAAN_MODEL_ORIGIN_URL);
    d.setFont(ui_font());
}

void saan_ui_status(const char *fmt, ...) {
    if (!s_ready) return;
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    auto &d = M5.Display;
    d.fillRect(0, UI_STATUS_Y, d.width(), d.height() - UI_STATUS_Y, TFT_BLACK);
    d.drawFastHLine(0, UI_STATUS_Y, d.width(), TFT_DARKGREY);
    d.setFont(ui_font());
    d.setTextWrap(true, false);   /* 2 行目へ折り返す（\n も可） */
    d.setTextColor(TFT_YELLOW, TFT_BLACK);
    d.setCursor(UI_MARGIN_X, UI_STATUS_Y + 5);
    d.print(buf);
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
