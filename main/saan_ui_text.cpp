/* 画面とタッチ — **文字表示版**（M5GFX だけ。m5stack-avatar を使わない）。設計は saan_ui.h を読むこと。
 *
 * ビルド時に `-DSAAN_UI=text` で選ぶ（既定は saan_ui_avatar.cpp の顔）。
 * 顔を出さないぶん、m5stack-avatar の描画タスク 2 本 + リップシンクタスクが無く、
 * flash も avatar のコード分だけ小さい。
 *
 * 画面の割り当て（320 x 240 横）:
 *   上段  … 文（シリアルに打った行そのもの。長ければ折り返す）
 *   中段  … 出典（モデルの帰属表示の要約。MODEL_CARD / LICENSE-MODEL）
 *   下段  … ステータス（合成中… / 再生中 / タッチでもう一度 / 途切れた / 入力エラー）
 *
 * フォントは M5GFX 同梱の lgfxJapanGothic_20（IPA ゴシック由来）。
 * ⚠️ **フォントは flash (.rodata) に置かれる。** サイズごとに別の配列なので 1 サイズで済ませる。
 * ⚠️ **描画も M5.update() も合成タスクからだけ呼ぶ。** タッチ (FT6336) とアンプ (AW88298) が
 *    同じ I2C バスなので、別タスクから触らない（avatar 版は SPI しか触らない描画タスクを持つ）。
 * ⚠️ リップシンクは無い。saan_ui_lip_stats() は 0 を返す（main.c はそれを見て「顔なし」と出す）。
 *
 * 出所: 画面の割り当てと描画は sanoTTS-jp の esp32/boards/m5unified/main/saan_ui_m5.cpp
 *       （もとはこのリポジトリの初期版 saan_ui.cpp）を、いまの saan_ui.h に合わせて書き直した。 */
#include <M5Unified.h>

#include <string.h>

#include "esp_log.h"

#include "saan_console.h"   /* SAAN_CONSOLE_LINE_MAX（文の最大長） */
#include "saan_ui.h"

static const char *TAG = "saan_ui";

/* 出典を常時表示する（モデルの帰属表示の要約） */
#define SAAN_UI_ORIGIN "model: sanoTTS-jp v3 int8  github.com/ayutaz/sanoTTS-jp"

static bool s_ready;
static char s_text[SAAN_CONSOLE_LINE_MAX];

/* 下段の開始 y。240 px のうち下 52 px をステータスに使う */
#define UI_STATUS_Y 188
#define UI_MARGIN_X 4

static const lgfx::IFont *ui_font(void) { return &fonts::lgfxJapanGothic_20; }

/* 上段: 文。`dim` なら合成中の色（灰）、そうでなければ白 */
static void draw_text(bool dim) {
    auto &d = M5.Display;
    d.fillRect(0, 0, d.width(), UI_STATUS_Y, TFT_BLACK);
    d.setFont(ui_font());
    d.setTextWrap(true, false);
    d.setTextColor(dim ? TFT_LIGHTGREY : TFT_WHITE, TFT_BLACK);
    d.setCursor(UI_MARGIN_X, 6);
    d.print(s_text);

    /* Font0 は M5GFX 組み込みの 6x8 ASCII で、日本語フォントと違い flash を食わない */
    d.setFont(&fonts::Font0);
    d.setTextWrap(false, false);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setCursor(UI_MARGIN_X, UI_STATUS_Y - 12);
    d.print(SAAN_UI_ORIGIN);
    d.setFont(ui_font());
}

/* 下段: ステータス 1 行 */
static void draw_status(const char *s, uint32_t color) {
    auto &d = M5.Display;
    d.fillRect(0, UI_STATUS_Y, d.width(), d.height() - UI_STATUS_Y, TFT_BLACK);
    d.drawFastHLine(0, UI_STATUS_Y, d.width(), TFT_DARKGREY);
    d.setFont(ui_font());
    d.setTextWrap(true, false);
    d.setTextColor(color, TFT_BLACK);
    d.setCursor(UI_MARGIN_X, UI_STATUS_Y + 5);
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
        ESP_LOGW(TAG, "画面が無い板。表示とタッチは使わない");
        s_ready = false;
        return true;
    }
    d.fillScreen(TFT_BLACK);
    d.setFont(ui_font());
    d.setTextSize(1);
    d.setTextWrap(true, false);
    s_ready = true;
    s_text[0] = '\0';
    draw_text(false);
    draw_status("起動中…", TFT_YELLOW);
    ESP_LOGI(TAG, "文字表示 UI（-DSAAN_UI=text）/ 画面 %d x %d / フォント lgfxJapanGothic_20",
             (int)d.width(), (int)d.height());
    return true;
}

void saan_ui_set_text(const char *text) {
    if (text == NULL) { s_text[0] = '\0'; return; }
    strncpy(s_text, text, sizeof s_text - 1);
    s_text[sizeof s_text - 1] = '\0';
}

void saan_ui_thinking(void) {
    if (!s_ready) return;
    draw_text(true);
    draw_status("合成中…", TFT_YELLOW);
}

void saan_ui_speaking(void) {
    if (!s_ready) return;
    draw_text(false);
    draw_status("再生中", TFT_GREEN);
}

void saan_ui_idle(const char *status) {
    if (!s_ready) return;
    draw_status(status, TFT_CYAN);
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

/* リップシンクは無い。0 を返す（main.c が「顔なし」と判定する） */
void saan_ui_lip_stats(uint32_t *frames, uint32_t *frames_open, float *max_ratio) {
    if (frames) *frames = 0;
    if (frames_open) *frames_open = 0;
    if (max_ratio) *max_ratio = 0.0f;
}

} /* extern "C" */
