/* 重み blob（flash の .rodata → ポインタ 1 本）。
 *
 * モデルは **sanoTTS-jp v3 int8**（https://github.com/ayutaz/sanoTTS-jp、
 * Release の saanotts-jp-v3-int8.bin）。ライセンスは MIT ではなく
 * sanoTTS-jp Model License 1.0 — LICENSES/ と NOTICE.md を読むこと。
 *
 * **SRAM にコピーしない。** 643,936 B は ESP32-S3 の内部 SRAM 512 KB に入らない。
 * ビルド時に scripts/blob_to_header.py が `const uint8_t[]` のヘッダにして
 * app の .rodata（flash）に埋め、そのまま読む（コアは blob を書き換えない）。
 */
#ifndef SAAN_MODEL_H
#define SAAN_MODEL_H

#include <stdbool.h>

#include "saanotts.h"

#define SAAN_MODEL_ORIGIN_NAME "sanoTTS-jp v3 int8"
#define SAAN_MODEL_ORIGIN_URL  "https://github.com/ayutaz/sanoTTS-jp"

/* .rodata の blob で saan_weights を開く。失敗したら false（理由は ESP_LOGE に出る）。
 * ⚠️ **16 バイト境界を assert する。** コアは payload を const float* に直接
 *    キャストし、PIE は 16 B 境界を要求する。Xtensa は非アラインで例外になる。 */
bool saan_model_open(saan_weights *w);

#endif /* SAAN_MODEL_H */
