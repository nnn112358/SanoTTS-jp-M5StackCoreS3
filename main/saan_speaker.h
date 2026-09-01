/* 音声出力 — M5Stack CoreS3 の内蔵アンプ (AW88298) を M5Unified の M5.Speaker で鳴らす。
 *
 * 22.05 kHz / 16 bit / mono。実装は saan_speaker.cpp（C++）。main.c は C のままなので
 * ここは extern "C" で公開する。
 *
 * サンプルレートはコアと同じ **22,050 Hz** で I2S を回す（M5 側のリサンプル無し）。
 *    AW88298 は 22.05 kHz を対応レートとして持つ（レジスタ 0x06 I2SSR）。ただし
 *    ESP32-S3 に APLL は無く、**実サンプルレートの誤差は未測定。**
 *
 * 使う順番:
 *   saan_speaker_setup()                    M5 を初期化する（まだ鳴らさない）
 *   saan_speaker_preroll_push() を数回       先に計算だけ済ませる
 *   saan_speaker_start()                    プリロールを吐き出して鳴らし始める
 *   saan_speaker_write_f32() を繰り返す
 *   saan_speaker_stop()                     鳴らし終わるまで待つ
 */
#ifndef SAAN_SPEAKER_H
#define SAAN_SPEAKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* プリロールに貯めるサンプル数。既定 8,192 = 4 チャンク = 371 ms・16 KB。
 *
 * なぜ要るか: **最初の saan_stream_pull だけ定常の約 6 倍かかる**
 * （ホスト実測 12.2 ms vs 2.04 ms。受容野 36 + iSTFT 2 = 38 フレームの
 * warmup で内部の step_chunk が複数回走るため）。鳴らし始めた直後から
 * 合成を始めると、その 1 回ぶんが確実にアンダーランになる。 */
#ifndef SAAN_SPEAKER_PREROLL_SAMPLES
#define SAAN_SPEAKER_PREROLL_SAMPLES 8192
#endif

bool saan_speaker_setup(uint32_t sample_rate);

/* 発話の開始。`n_samples` ぶんの int16 バッファを PSRAM に取る。
 *   貯めてから鳴らす方式 … 発話の総サンプル数（n_frames × SAAN_HOP）を渡す。
 *                          全チャンクを preroll_push で貯め、start で一気に鳴らす
 *   ストリーミング       … プリロール量（SAAN_SPEAKER_PREROLL_SAMPLES）を渡す
 * バッファは saan_speaker_stop() が再生完了を待ってから解放する。
 * ⚠️ 音声 1 秒あたり 44,100 B。350 ids の上限でも数 MB で、8 MB PSRAM に収まる。 */
bool saan_speaker_begin_utterance(size_t n_samples);

/* まだ鳴らさずに変換して貯める。begin_utterance で取った量を超えたら false */
bool saan_speaker_preroll_push(const float *pcm, size_t n_samples);

/* 貯めたぶんを鳴らし始める（貯めてから鳴らす方式ではこれが再生そのもの） */
bool saan_speaker_start(void);

/* float[-1,1] → int16 に変換して送る（キューが満杯なら空くまでブロック）。
 * ⚠️ `n_samples` は**サンプル数**。saan_stream_pull が返すフレーム数ではない
 *    （サンプル数 = フレーム数 × SAAN_HOP）。 */
bool saan_speaker_write_f32(const float *pcm, size_t n_samples);

/* 鳴らし終わるまで待ち、begin_utterance のバッファを解放する */
void saan_speaker_stop(void);

/* float → int16。**正規化しない**（発話ごとに音量が変わると決定性が壊れる）。
 * クリップは数えて出す。 */
int16_t  saan_f32_to_i16(float x);
uint32_t saan_speaker_clip_count(void);

/* --- 出力 PCM のチェックサム（移植の検証用）--------------------------------
 *
 * `saan_f32_to_i16()` を通った **すべての** int16 サンプルの FNV-1a。
 * プリロールも定常ループも同じ関数を通るので、**スピーカーに出た列そのもの**。
 *
 * ⚠️ **「音が鳴った」は移植が正しい証拠にならない。** 本家のホスト stub
 *    （sanoTTS-jp/esp32/host_stub）と同じ値が出れば全経路が bit 一致していると言える。
 * ⚠️ **ホストとターゲットは bit 一致しない。それは正常**（float の丸めが違う）。
 *    そのときは |max| と Σx² の**大きさ**で「丸め差」か「経路が壊れている」かを分ける。
 *    bit 一致を主張してよいのは**同じターゲット上の 2 構成**を比べたときだけ。 */
uint64_t saan_pcm_checksum(void);
uint32_t saan_pcm_samples(void);
int32_t  saan_pcm_absmax(void);
uint64_t saan_pcm_sqsum(void);

/* 統計を発話の頭に戻す。**対話モードで 2 発話目以降を測るのに要る。**
 * ⚠️ これが無いと 2 発話目の checksum が「1 + 2 発話目」になり、しかも値は出るので
 *    突き合わせて「合わない」と悩むまで気づけない。 */
void saan_pcm_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* SAAN_SPEAKER_H */
