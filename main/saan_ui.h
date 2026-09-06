/* 画面とタッチ。実装は 2 つあり、ビルド時に `-DSAAN_UI=avatar|text` で選ぶ（main/CMakeLists.txt）:
 *
 *   saan_ui_avatar.cpp（既定） … m5stack-avatar の顔。画面全体を avatar が描く（自前の描画タスク
 *       2 本、core 1）。文字は吹き出し（右下）に出す。口の開きは saan_speaker の再生位置の音量に
 *       合わせる（リップシンク。lip_task が 10 ms ごとに saan_speaker_level_now() を読む）
 *   saan_ui_text.cpp          … 文字だけ（M5GFX）。上段に文、下段にステータス。リップシンク無し
 *       （saan_ui_lip_stats は 0 を返す）。m5stack-avatar はリンクされない
 *
 * main.c はこのヘッダの API しか使わず、どちらが入っているかを知らない。
 *
 * ⚠️ **M5.begin() の後に呼ぶこと。** M5.begin() は saan_speaker_setup() の中で 1 回だけ呼ぶ。
 * ⚠️ **M5.update()（タッチ）は合成タスクからだけ呼ぶ。** タッチ (FT6336) と
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

/* これから喋る文（吹き出しに出す）。長ければ切り詰めて「…」を付ける。 */
void saan_ui_set_text(const char *text);

/* 合成中: 吹き出し「…」、口を閉じる */
void saan_ui_thinking(void);

/* 再生中: 吹き出しに set_text の文を出す。口は lip_task が動かす */
void saan_ui_speaking(void);

/* 待機: 吹き出しに短いステータス（NULL か "" で吹き出し無し） */
void saan_ui_idle(const char *status);

/* M5.update() を 1 回呼び、**押された瞬間**があれば true（押しっぱなしでは繰り返さない） */
bool saan_ui_poll_touch(void);

/* 直前の speaking 以降に lip_task が口を動かした回数と最大開き（ログ用） */
void saan_ui_lip_stats(uint32_t *frames, uint32_t *frames_open, float *max_ratio);

#ifdef __cplusplus
}
#endif
#endif /* SAAN_UI_H */
