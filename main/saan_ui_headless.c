/* 画面と入力 — **何も無い版**（Stamp-C5 など画面もボタンも無いボード。-DSAAN_BOARD=stampc5）。
 * M5Unified / M5GFX / m5stack-avatar をリンクしないための saan_ui.h のスタブ。入力はシリアルだけ。 */
#include "saan_ui.h"

bool saan_ui_init(void) { return true; }
void saan_ui_set_text(const char *text) { (void)text; }
void saan_ui_thinking(void) {}
void saan_ui_speaking(void) {}
void saan_ui_idle(const char *status) { (void)status; }
bool saan_ui_poll_touch(void) { return false; }
void saan_ui_toggle(void) {}
const char *saan_ui_mode(void) { return "headless"; }
void saan_ui_lip_stats(uint32_t *frames, uint32_t *frames_open, float *max_ratio) {
    if (frames) *frames = 0;
    if (frames_open) *frames_open = 0;
    if (max_ratio) *max_ratio = 0.0f;
}
