/* 音声出力 — M5Stack CoreS3 の内蔵アンプ (AW88298) を M5Unified の M5.Speaker で鳴らす。
 *
 * 22.05 kHz / 16 bit / mono。実装は saan_speaker.cpp（C++）。main.c は C のままなので
 * ここは extern "C" で公開する。
 *
 * サンプルレートはコアと同じ **22,050 Hz** で I2S を回す（M5 側のリサンプル無し）。
 *    AW88298 は 22.05 kHz を対応レートとして持つ（レジスタ 0x06 I2SSR）。ただし
 *    ESP32-S3 に APLL は無く、**実サンプルレートの誤差は未測定。**
 *
 * 1 発話は **PSRAM の連続バッファ 1 本**に頭から書く。鳴らし始めるときに「貯めたぶん」と
 * 「残り全部（まだ書いていない部分を含む）」の 2 区間をキューに渡し、以後は書き続けるだけ。
 * 先に何サンプル貯めてから鳴らし始めるかは呼び出し側（main.c）が xRT から決める。
 *
 *   saan_speaker_setup()                    M5 を初期化する（まだ鳴らさない）
 *   saan_speaker_begin_utterance(total)     発話ぶんのバッファと包絡を取る
 *   saan_speaker_push_f32() を繰り返す       変換して貯める（まだ鳴らさない）
 *   saan_speaker_start()                    貯めたぶんを鳴らし始める（呼び出し側が時機を決める）
 *   saan_speaker_pump(false) を繰り返す      再生が書き込みを追い越していないか見る（渡す作業は無い）
 *   saan_speaker_pump(true)                 同上（互換のため残してある）
 *   saan_speaker_stop()                     鳴らし終わるまで待ち、バッファを解放
 *
 * リップシンク: 変換のたびに 512 sample（23 ms）ごとの RMS を包絡として貯め、
 *   鳴らし始めた時刻から「いま鳴っているサンプル位置」を推定して
 *   saan_speaker_level_now() で 0..1 を返す。saan_ui.cpp の lip_task が読む。
 */
#ifndef SAAN_SPEAKER_H
#define SAAN_SPEAKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool saan_speaker_setup(uint32_t sample_rate);

/* 発話の開始。`total_samples`（n_frames × SAAN_HOP）ぶんの int16 バッファと
 * リップシンク包絡を PSRAM に取る。saan_speaker_stop() が再生完了を待ってから解放する。
 * ⚠️ 音声 1 秒あたり 44,100 B（+ 包絡 43 B）。350 ids の上限でも 8 MB PSRAM に収まる。 */
bool saan_speaker_begin_utterance(size_t total_samples);

/* float[-1,1] → int16 に変換してバッファに貯める。まだ鳴らさない。
 * ⚠️ `n_samples` は**サンプル数**（フレーム数 × SAAN_HOP）。総量を超えたら false */
bool saan_speaker_push_f32(const float *pcm, size_t n_samples);

/* 貯めたぶんを鳴らし始める。begin 以降に 1 回だけ */
bool saan_speaker_start(void);

/* 再生が書き込みを追い越していないかを見る（渡す作業は start() で済んでいる）。
 *   合成ループから毎チャンク呼ぶ。final は互換のための引数で意味は無い。
 *   ⚠️ 追い越された区間は begin_utterance のゼロ埋め = 無音が鳴る（ゴミではない）。
 * 戻り値: 再生位置（DMA の先読みを含む）が書き込み位置を越えていたら true。 */
bool saan_speaker_pump(bool final);

/* 鳴らし終わるまで待ち、バッファを解放する */
void saan_speaker_stop(void);

/* 貯めた / 渡した サンプル数（ログ用） */
size_t saan_speaker_buffered(void);
size_t saan_speaker_sent(void);

/* いま鳴っている位置の音量 0..1（発話内の最大 RMS で正規化）。鳴っていなければ 0。
 * ⚠️ 再生位置は「鳴らし始めた時刻 + 経過時間」から推定する。途切れている間は
 *    M5.Speaker.isPlaying() が false になるので 0 を返し、次の区間を渡すときに
 *    時刻を取り直す。 */
float saan_speaker_level_now(void);

/* float → int16。**正規化しない**（発話ごとに音量が変わると決定性が壊れる）。
 * クリップは数えて出す。 */
int16_t  saan_f32_to_i16(float x);
uint32_t saan_speaker_clip_count(void);

/* --- 出力 PCM のチェックサム（移植の検証用）--------------------------------
 *
 * `saan_f32_to_i16()` を通った **すべての** int16 サンプルの FNV-1a。
 * **スピーカーに出た列そのもの**。
 *
 * ⚠️ **「音が鳴った」は移植が正しい証拠にならない。** 本家 sanoTTS-jp の QEMU 記録
 *    （blob v2 / S3 以降のコア: W8A8+PIE 0xa69a7ebbb5ccb05f / W8A32 0xe4b645c30835d42d。
 *    旧コアは 0x04de91103a0e49f9 / 0x78c209af06affc01）と同じ値が出れば
 *    全経路が bit 一致していると言える。
 * ⚠️ **ホストとターゲットは bit 一致しない。それは正常**（float の丸めが違う）。
 *    そのときは |max| と Σx² の**大きさ**で「丸め差」か「経路が壊れている」かを分ける。 */
uint64_t saan_pcm_checksum(void);
uint32_t saan_pcm_samples(void);
int32_t  saan_pcm_absmax(void);
uint64_t saan_pcm_sqsum(void);

/* 統計を発話の頭に戻す。**2 発話目以降を測るのに要る。**
 * ⚠️ これが無いと 2 発話目の checksum が「1 + 2 発話目」になり、しかも値は出るので
 *    突き合わせて「合わない」と悩むまで気づけない。 */
void saan_pcm_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* SAAN_SPEAKER_H */
