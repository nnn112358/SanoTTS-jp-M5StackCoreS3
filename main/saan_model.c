#include "saan_model.h"

#include <inttypes.h>
#include <stdint.h>

#include "esp_log.h"

/* ⚠️ **この include は 1 翻訳単位からだけ。** 配列の定義を持つので、
 *    2 箇所から include するとリンク時に重複定義で落ちる（黙って flash が
 *    2 倍になることはない）。生成元は scripts/blob_to_header.py。 */
#include "saan_model_blob.h"

static const char *TAG = "saan_model";

bool saan_model_open(saan_weights *w) {
    ESP_LOGI(TAG, "重み: %s (%s)", SAAN_MODEL_ORIGIN_NAME, SAAN_MODEL_ORIGIN_URL);
    ESP_LOGI(TAG, "  ヘッダ埋め込み (.rodata = flash) %u B / dtype %s",
             (unsigned)SAAN_MODEL_BLOB_BYTES, SAAN_MODEL_BLOB_DTYPE);
    ESP_LOGI(TAG, "  sha256 %s", SAAN_MODEL_BLOB_SHA256);

    const void *ptr = (const void *)g_saan_model_blob;

    /* ⚠️ **ここで落とす。** 非アラインのまま走らせると、落ちるのはずっと後の
     * conv の中（LoadStoreAlignmentCause）で原因が分からなくなる。
     * 生成ヘッダが `__attribute__((aligned(16)))` を付けているので必ず通るが、
     * **属性を消したときに気づけるようにチェックは残す**。 */
    if (((uintptr_t)ptr & 15u) != 0u) {
        ESP_LOGE(TAG, "blob が 16 バイト境界に無い (ptr=%p)。"
                      "scripts/blob_to_header.py の aligned(16) が消えている", ptr);
        return false;
    }

    saan_status s = saan_weights_open(w, ptr, SAAN_MODEL_BLOB_BYTES);
    if (s != SAAN_OK) {
        if (s == SAAN_ERR_VERSION)
            /* ⚠️ 2026-09-02 の S4 以降のコアは **blob v2**（654,032 B）しか開けない。
             *    v1（643,936 B。Release v0.2.0 以前の saanotts-jp-v3-int8.bin）はここで止まる。 */
            ESP_LOGE(TAG, "saan_weights_open: %s — blob が形式 v1。Release v0.3.0 以降の "
                          "saanotts-jp-v3-int8.bin（v2、654,032 B）を model/student_i8.bin に置くこと",
                     saan_strerror(s));
        else
            ESP_LOGE(TAG, "saan_weights_open: %s (ヘッダの中身が blob でない)", saan_strerror(s));
        return false;
    }
    ESP_LOGI(TAG, "重み OK: %" PRIu32 " tensors / 形式 v%" PRIu32 " / base %p",
             w->n_tensors, w->version, (const void *)w->base);
    return true;
}
