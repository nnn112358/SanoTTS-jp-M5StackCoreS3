/* 画面と入力。実装は 2 つあり、**1 つのファームに両方入っていて実行時に切り替えられる**:
 *
 *   顔（avatar） … m5stack-avatar の顔。画面全体を avatar が描く（自前の描画タスク 2 本、core 1）。
 *       文字は吹き出し（右下）に出す。口の開きは saan_speaker の再生位置の音量に合わせる
 *       （リップシンク。lip_task が 10 ms ごとに saan_speaker_level_now() を読む）。
 *       128 x 128（ATOMS3 / ATOMS3R）では scale 0.4 に縮める
 *   文字（text）  … 本家 boards/m5unified/saan_ui_m5.cpp と同じ 3 段（文 / 出典 / ステータス）。
 *       128 x 128 では詰めた配置。リップシンク無し（saan_ui_lip_stats は 0 を返す）
 *
 * 起動時の画面は `-DSAAN_UI=avatar|text`（既定 avatar。画面が無いボードは text 固定）。
 * 切り替え: **画面（またはボタン A）を長押し**、ボタン B、シリアルの `/ui`。
 * 「もう一度喋る」: 画面（またはボタン A）を**短く**タッチ / 押す。
 *
 * main.c はこのヘッダの API しか使わず、どちらが出ているかを知らない（振り分けは saan_ui.cpp）。
 *
 * ⚠️ **M5.begin() の後に呼ぶこと。** M5.begin() は saan_speaker_setup() の中で 1 回だけ呼ぶ。
 * ⚠️ **M5.update()（タッチ / ボタン）は合成タスクからだけ呼ぶ。** タッチ (FT6336) と
 *    スピーカーの AW88298 が同じ I2C バスなので、別タスクから触らない。
 *    avatar の描画タスクは SPI（ディスプレイ）しか触らないので同居できる。
 */
#ifndef SAAN_UI_H
#define SAAN_UI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool saan_ui_init(void);

/* これから喋る文（吹き出し / 上段に出す）。長ければ実装側で切り詰める。 */
void saan_ui_set_text(const char *text);

/* 合成中: 吹き出し「…」、口を閉じる / ステータス「合成中…」 */
void saan_ui_thinking(void);

/* 再生中: 文を出す。口は lip_task が動かす / ステータス「再生中」 */
void saan_ui_speaking(void);

/* 待機: 短いステータス（NULL か "" で無し）。タッチの無いボードでは「タッチで…」を「ボタンで…」に言い換える */
void saan_ui_idle(const char *status);

/* M5.update() を 1 回呼び、「もう一度喋る」操作（短いタッチ / ボタン A のクリック）があれば true。
 * 長押し / ボタン B は画面の切り替えとしてここで処理する（true は返さない）。 */
bool saan_ui_poll_touch(void);

/* 顔 ⇄ 文字を切り替える（シリアルの `/ui` から）。画面が無いボードでは何もしない */
void saan_ui_toggle(void);

/* いま出ている画面の名前（"avatar" / "text"） */
const char *saan_ui_mode(void);

/* 直前の speaking 以降に lip_task が口を動かした回数と最大開き（ログ用。文字画面では 0） */
void saan_ui_lip_stats(uint32_t *frames, uint32_t *frames_open, float *max_ratio);

#ifdef __cplusplus
}
#endif
#endif /* SAAN_UI_H */
