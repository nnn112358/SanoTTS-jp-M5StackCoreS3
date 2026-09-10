/* 画面の実装 2 つ（顔 / 文字）が saan_ui.cpp（振り分け）に見せる内側の API。main.c は見ない。
 *
 *   saan_ui_avatar.cpp … saan_ui_impl_avatar（m5stack-avatar の顔 + 吹き出し + リップシンク）
 *   saan_ui_text.cpp   … saan_ui_impl_text（文字だけ。320 x 240 は本家 saan_ui_m5.cpp と同じ 3 段、
 *                        128 x 128 は詰めた配置、画面が無ければ何も描かない）
 *
 * 入力（M5.update() / タッチ / ボタン）は実装側では扱わない。saan_ui.cpp が 1 か所で読む。
 * ⚠️ enter() / leave() は**実行時の切り替え**のためにある。avatar は描画タスクを suspend / resume し、
 *    text は enter() で画面を描き直す。leave() の後、画面は相手の実装が上書きする。 */
#ifndef SAAN_UI_IMPL_H
#define SAAN_UI_IMPL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *name;                       /* ログと吹き出し用（"avatar" / "text"） */
    bool (*init)(void);                     /* M5.begin() の後に 1 回。まだ描かなくてよい */
    void (*enter)(void);                    /* この実装に切り替える（描画開始 / 描き直し） */
    void (*leave)(void);                    /* 別の実装へ（描画を止める） */
    void (*set_text)(const char *text);     /* これから喋る文 */
    void (*thinking)(void);
    void (*speaking)(void);
    void (*idle)(const char *status);
    void (*lip_stats)(uint32_t *frames, uint32_t *frames_open, float *max_ratio);
} saan_ui_impl_t;

#ifdef __cplusplus
extern "C" {
#endif
extern const saan_ui_impl_t saan_ui_impl_avatar;
extern const saan_ui_impl_t saan_ui_impl_text;
#ifdef __cplusplus
}
#endif
#endif /* SAAN_UI_IMPL_H */
