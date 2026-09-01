/* 画面とタッチ（M5Stack CoreS3 / M5GFX）。実装は saan_ui.cpp（C++）。
 *
 * 画面の割り当て（320 x 240 横）:
 *   上段  … 文（漢字。シリアル入力のときは無し）
 *   中段  … かな中間表現（合成に使った列そのもの）+ 出典（model: sanoTTS-jp …）
 *   下段  … ステータス（合成中 / xRT と途切れ回数 / 「タッチで再生」）
 *
 * ⚠️ **M5.begin() の後に呼ぶこと。** M5.begin() は saan_speaker_setup() の中で
 *    1 回だけ呼ぶ。saan_ui_init() は板が判別済みかを見て、前なら落ちる。
 * ⚠️ **描画も M5.update() も合成タスクからだけ呼ぶ。** タッチ (I2C) と
 *    スピーカーの AW88298 (I2C) が同じバスなので、別タスクから触らない。
 */
#ifndef SAAN_UI_H
#define SAAN_UI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool saan_ui_init(void);

/* 文と中間表現を表示する（上段・中段を書き直す。下段はそのまま）。
 * `title` は NULL 可（シリアル入力には漢字が無い）。長い行は折り返す。 */
void saan_ui_show(const char *title, const char *kana);

/* 下段のステータス行を書き直す（printf 書式）。 */
void saan_ui_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* M5.update() を 1 回呼び、**押された瞬間**があれば true。
 * 押しっぱなしでは繰り返さない（wasPressed 判定）。 */
bool saan_ui_poll_touch(void);

#ifdef __cplusplus
}
#endif
#endif /* SAAN_UI_H */
