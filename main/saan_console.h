/* シリアルコンソールからの 1 行入力（かな中間表現）。
 *
 * 行編集そのものは components/saanotts_core/line.c のステートマシン（本家 sanoTTS-jp で
 * ゲート済み）。ここがやるのは **バイトの取り込みとエコーだけ**。
 *
 * CoreS3 は USB-UART ブリッジを持たないので、コンソールは native USB
 * （CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y、sdkconfig.defaults）。UART に切り替えた
 * ときのために UART の経路も残してある。
 */
#ifndef SAAN_CONSOLE_H
#define SAAN_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 入力バッファ。**上限を超えたら切り詰めずに行ごと拒否する**（line.c の overflow）。
 * 512 B は、かな 1 文字 3 B で約 170 文字。ids の上限 (SAAN_MAX_IDS) の方が
 * 先に効くので、ここは「異常に長い貼り付けを止める」ための枠。
 * main.c が ids バッファの大きさをこれから決める。 */
#define SAAN_CONSOLE_LINE_MAX 512

bool saan_console_init(void);

/* プロンプト `かな> ` を出す。行を受け付ける前に 1 回呼ぶ。 */
void saan_console_prompt(void);

/* 1 行を**待たずに**読む。`timeout_ms` のあいだにバイトが来なければ PENDING。
 *   戻り値 >= 0 : 行のバイト数。`*out` に NUL 終端の行が入る
 *   戻り値 -3   : まだ行が完成していない（PENDING。呼び出し側はこの間にタッチを見る）
 *   戻り値 -2   : 入力が長すぎた（`*out` は使ってはいけない）
 *   戻り値 -1   : 読み取りエラー
 * ⚠️ 空行 (0) は「何も打たずに Enter」。呼び出し側で弾くこと。
 *
 * ⚠️ **バッファは呼び出し側から渡さない。** 行編集の状態（特に CRLF の
 *    「次の LF を吸う」フラグ）は**行をまたいで持ち越す必要がある**ので、
 *    バッファと状態はこのモジュールが 1 組だけ持つ。呼び出しごとに
 *    `saan_line_reset()` すると、CRLF を送る端末で**発話のたびに空行が 1 回**入る
 *    （本家が QEMU で実際に踏んだ）。`*out` は次に行が完成するまで有効。 */
int saan_console_poll(const char **out, uint32_t timeout_ms);

#define SAAN_CONSOLE_ERROR    (-1)
#define SAAN_CONSOLE_TOO_LONG (-2)
#define SAAN_CONSOLE_PENDING  (-3)

#endif /* SAAN_CONSOLE_H */
