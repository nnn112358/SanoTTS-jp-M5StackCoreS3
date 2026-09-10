/* 画面の振り分け — 顔（saan_ui_avatar.cpp）と文字（saan_ui_text.cpp）を 1 つのファームに持ち、
 * 実行時に切り替える。main.c から見える API は saan_ui.h。
 *
 * 入力はここで 1 か所だけ読む（M5.update()）:
 *   短いタッチ / ボタン A クリック … もう一度喋る（poll が true を返す）
 *   長押し（タッチ / ボタン A）/ ボタン B クリック … 顔 ⇄ 文字
 * ⚠️ CoreS3 の仮想ボタン A/B/C は画面の下の帯（y ≥ 240）へのタッチ。そこを「タッチ」としては
 *    数えない（y < 画面高さのものだけ）。数えると B で「切り替え + もう一度」が同時に起きる。
 *
 * 切り替えたときは直前の状態（文 / thinking・speaking・idle）を新しい実装に流し直す。 */
#include <M5Unified.h>

#include <string.h>

#include "esp_log.h"

#include "saan_ui.h"
#include "saan_ui_impl.h"

static const char *TAG = "saan_ui";

/* 起動時の画面。-DSAAN_UI=text で 1（main/CMakeLists.txt） */
#ifndef SAAN_UI_DEFAULT_TEXT
#define SAAN_UI_DEFAULT_TEXT 0
#endif

static const saan_ui_impl_t *s_cur;
static bool s_ready;
static bool s_headless;   /* 画面が無い（Stamp-C5）。text 固定で描かない */

/* 切り替え時に流し直す状態 */
enum { ST_IDLE, ST_THINKING, ST_SPEAKING };
static int  s_state = ST_IDLE;
static char s_status[96];

static const char *fix_status(const char *status) {
    /* タッチの無いボード（ATOMS3 / Basic）では言い換える。main.c の文言に合わせてある */
    if (status != NULL && !M5.Touch.isEnabled() && strcmp(status, "タッチでもう一度") == 0)
        return "ボタンでもう一度";
    return status;
}

static void replay(void) {
    switch (s_state) {
    case ST_THINKING: s_cur->thinking(); break;
    case ST_SPEAKING: s_cur->speaking(); break;
    default:          s_cur->idle(s_status[0] ? s_status : NULL); break;
    }
}

static void switch_to(const saan_ui_impl_t *next) {
    if (next == s_cur) return;
    if (s_cur != NULL) s_cur->leave();
    s_cur = next;
    s_cur->enter();
    replay();
    ESP_LOGI(TAG, "画面 → %s", s_cur->name);
}

extern "C" {

bool saan_ui_init(void) {
    if (M5.getBoard() == m5::board_t::board_unknown) {
        ESP_LOGE(TAG, "M5.begin() がまだ。saan_speaker_setup() の後に呼ぶこと");
        return false;
    }
    s_headless = (M5.Display.width() == 0);
    if (!saan_ui_impl_text.init()) return false;
    if (!s_headless && !saan_ui_impl_avatar.init()) return false;
    s_ready = true;
    s_state = ST_IDLE;
    s_status[0] = '\0';
    const saan_ui_impl_t *first = (s_headless || SAAN_UI_DEFAULT_TEXT) ? &saan_ui_impl_text
                                                                        : &saan_ui_impl_avatar;
    s_cur = first;
    s_cur->enter();
    if (s_headless)
        ESP_LOGW(TAG, "画面が無いボード。文字画面（描画なし）に固定。入力はシリアルだけ");
    else
        ESP_LOGI(TAG, "画面 = %s（切り替え: 長押し / ボタン B / シリアル `/ui`。もう一度: 短いタッチ / ボタン A）",
                 s_cur->name);
    return true;
}

void saan_ui_set_text(const char *text) {
    if (!s_ready) return;
    /* どちらに切り替えても同じ文が出るように両方へ */
    saan_ui_impl_text.set_text(text);
    if (!s_headless) saan_ui_impl_avatar.set_text(text);
}

void saan_ui_thinking(void) {
    if (!s_ready) return;
    s_state = ST_THINKING;
    s_cur->thinking();
}

void saan_ui_speaking(void) {
    if (!s_ready) return;
    s_state = ST_SPEAKING;
    s_cur->speaking();
}

void saan_ui_idle(const char *status) {
    if (!s_ready) return;
    s_state = ST_IDLE;
    status = fix_status(status);
    if (status != NULL) { strncpy(s_status, status, sizeof s_status - 1); s_status[sizeof s_status - 1] = '\0'; }
    else s_status[0] = '\0';
    s_cur->idle(status);
}

void saan_ui_toggle(void) {
    if (!s_ready || s_headless) return;
    switch_to(s_cur == &saan_ui_impl_avatar ? &saan_ui_impl_text : &saan_ui_impl_avatar);
}

const char *saan_ui_mode(void) { return s_cur != NULL ? s_cur->name : "none"; }

bool saan_ui_poll_touch(void) {
    if (!s_ready) return false;
    M5.update();
    bool speak = false, toggle = false;
    const int h = M5.Display.height();
    const auto n = M5.Touch.getCount();
    for (size_t i = 0; i < n; ++i) {
        const auto t = M5.Touch.getDetail(i);
        if (t.y >= h) continue;                 /* 画面の下の帯（仮想ボタン）は Btn 側で見る */
        if (t.wasClicked()) { ESP_LOGI(TAG, "タッチ x=%d y=%d", (int)t.x, (int)t.y); speak = true; }
        if (t.wasHold())    { ESP_LOGI(TAG, "長押し x=%d y=%d", (int)t.x, (int)t.y); toggle = true; }
    }
    if (M5.BtnA.wasClicked()) { ESP_LOGI(TAG, "ボタン A"); speak = true; }
    if (M5.BtnA.wasHold())    { ESP_LOGI(TAG, "ボタン A 長押し"); toggle = true; }
    if (M5.BtnB.wasClicked()) { ESP_LOGI(TAG, "ボタン B"); toggle = true; }
    if (toggle) saan_ui_toggle();
    return speak;
}

void saan_ui_lip_stats(uint32_t *frames, uint32_t *frames_open, float *max_ratio) {
    if (!s_ready) { if (frames) *frames = 0; if (frames_open) *frames_open = 0; if (max_ratio) *max_ratio = 0; return; }
    s_cur->lip_stats(frames, frames_open, max_ratio);
}

} /* extern "C" */
