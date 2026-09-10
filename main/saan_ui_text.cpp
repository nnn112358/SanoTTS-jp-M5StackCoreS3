/* 画面 — **文字表示**（M5GFX だけ。m5stack-avatar を使わない）。saan_ui_impl_text として
 * saan_ui.cpp に渡す。設計は saan_ui.h / saan_ui_impl.h を読むこと。
 *
 * 画面の割り当て（320 x 240 横。本家 boards/m5unified/saan_ui_m5.cpp と同じ）:
 *   上段  … 文（シリアルに打った行そのもの。長ければ折り返す）
 *   中段  … 出典（モデルの帰属表示の要約。MODEL_CARD / LICENSE-MODEL）
 *   下段  … ステータス（合成中… / 再生中 / タッチでもう一度 / 途切れた / 入力エラー）
 * 128 x 128（ATOMS3 / ATOMS3R）は lgfxJapanGothic_16 で詰める（1 行 8 文字、上段 5 行、出典は短縮形）。
 * 画面が無いボード（Stamp-C5）は何も描かない（init は成功する）。
 *
 * フォントは M5GFX 同梱の lgfxJapanGothic_20 / _16（IPA ゴシック由来）。
 * ⚠️ **フォントは flash (.rodata) に置かれる。** 使うサイズぶんだけ入る。
 * ⚠️ **描画は合成タスクからだけ呼ぶ**（saan_ui.h）。
 *
 * 出所: 画面の割り当てと描画は sanoTTS-jp の esp32/boards/m5unified/main/saan_ui_m5.cpp
 *       （もとはこのリポジトリの初期版 saan_ui.cpp）を、いまの API に合わせて書き直した。 */
#include <M5Unified.h>

#include <string.h>

#include "esp_log.h"

#include "saan_console.h"   /* SAAN_CONSOLE_LINE_MAX（文の最大長） */
#include "saan_ui_impl.h"

static const char *TAG = "saan_ui";

static bool s_have_display;
static bool s_small;          /* 128 x 128 */
static char s_text[SAAN_CONSOLE_LINE_MAX];
static bool s_dim;            /* 上段の色（合成中は灰） */
static char s_status[96];
static uint32_t s_status_color = TFT_YELLOW;

/* 配置（幅で決める） */
static int status_y(void)  { return s_small ? 104 : 188; }   /* 下段の開始 y */
static int margin_x(void)  { return s_small ? 2 : 4; }
static const lgfx::IFont *ui_font(void) {
    return s_small ? (const lgfx::IFont *)&fonts::lgfxJapanGothic_16 : (const lgfx::IFont *)&fonts::lgfxJapanGothic_20;
}
static const char *origin(void) {
    /* 出典（モデルの帰属表示の要約）。128 px では Font0 で 21 文字まで */
    return s_small ? "sanoTTS-jp v3 int8" : "model: sanoTTS-jp v3 int8  github.com/ayutaz/sanoTTS-jp";
}

/* 上段: 文。`dim` なら合成中の色（灰）、そうでなければ白 */
static void draw_text(void) {
    auto &d = M5.Display;
    d.fillRect(0, 0, d.width(), status_y(), TFT_BLACK);
    d.setFont(ui_font());
    d.setTextWrap(true, false);
    d.setTextColor(s_dim ? TFT_LIGHTGREY : TFT_WHITE, TFT_BLACK);
    d.setCursor(margin_x(), s_small ? 2 : 6);
    d.print(s_text);

    /* Font0 は M5GFX 組み込みの 6x8 ASCII で、日本語フォントと違い flash を食わない */
    d.setFont(&fonts::Font0);
    d.setTextWrap(false, false);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setCursor(margin_x(), status_y() - (s_small ? 10 : 12));
    d.print(origin());
    d.setFont(ui_font());
}

/* 下段: ステータス 1 行 */
static void draw_status(void) {
    auto &d = M5.Display;
    d.fillRect(0, status_y(), d.width(), d.height() - status_y(), TFT_BLACK);
    d.drawFastHLine(0, status_y(), d.width(), TFT_DARKGREY);
    d.setFont(ui_font());
    d.setTextWrap(!s_small, false);
    d.setTextColor(s_status_color, TFT_BLACK);
    d.setCursor(margin_x(), status_y() + (s_small ? 4 : 5));
    d.print(s_status);
}

static bool text_init(void) {
    auto &d = M5.Display;
    s_have_display = (d.width() > 0);
    s_small = s_have_display && d.width() <= 128;
    s_text[0] = '\0';
    strcpy(s_status, "起動中…");
    s_status_color = TFT_YELLOW;
    if (!s_have_display) {
        ESP_LOGW(TAG, "画面が無いボード。文字画面は描かない");
        return true;
    }
    ESP_LOGI(TAG, "文字画面: %d x %d / フォント %s", (int)d.width(), (int)d.height(),
             s_small ? "lgfxJapanGothic_16（詰め配置）" : "lgfxJapanGothic_20（本家と同じ 3 段）");
    return true;
}

static void text_enter(void) {
    if (!s_have_display) return;
    auto &d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setFont(ui_font());
    d.setTextSize(1);
    draw_text();
    draw_status();
}

static void text_leave(void) { /* 相手の実装が画面を上書きする */ }

static void text_set_text(const char *text) {
    if (text == NULL) { s_text[0] = '\0'; return; }
    strncpy(s_text, text, sizeof s_text - 1);
    s_text[sizeof s_text - 1] = '\0';
}

static void set_status(const char *s, uint32_t color) {
    strncpy(s_status, s != NULL ? s : "", sizeof s_status - 1);
    s_status[sizeof s_status - 1] = '\0';
    s_status_color = color;
}

static void text_thinking(void) {
    s_dim = true; set_status("合成中…", TFT_YELLOW);
    if (s_have_display) { draw_text(); draw_status(); }
}

static void text_speaking(void) {
    s_dim = false; set_status("再生中", TFT_GREEN);
    if (s_have_display) { draw_text(); draw_status(); }
}

static void text_idle(const char *status) {
    set_status(status, TFT_CYAN);
    if (s_have_display) draw_status();
}

/* リップシンクは無い。0 を返す（main.c が「顔なし」と判定する） */
static void text_lip_stats(uint32_t *frames, uint32_t *frames_open, float *max_ratio) {
    if (frames) *frames = 0;
    if (frames_open) *frames_open = 0;
    if (max_ratio) *max_ratio = 0.0f;
}

extern "C" const saan_ui_impl_t saan_ui_impl_text = {
    "text", text_init, text_enter, text_leave, text_set_text,
    text_thinking, text_speaking, text_idle, text_lip_stats,
};
