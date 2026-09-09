/* 音声出力 — M5Stack CoreS3 の内蔵アンプ (AW88298) を M5Unified の M5.Speaker で鳴らす。
 *
 * 22.05 kHz / 16 bit / mono。実装は saan_speaker.cpp（C++）。main.c は C のままなので
 * ここは extern "C" で公開する。
 *
 * 給餌の仕組みは本家 sanoTTS-jp の esp32/boards/m5unified/main/saan_audio_m5.cpp と同じ
 * （2026-09-10 に合わせた）:
 *   - プリロール（鳴らし始める前に貯めるぶん）は発話ごとに 1 本のバッファ（PSRAM 優先）
 *   - 鳴らし始めた後は **2,048 sample × 3 枚のリング**に変換して、チャンクごとに playRaw
 *   - M5 のキュー（1 ch あたり 2 枚）が満杯なら playRaw が**ブロック**する。合成ループの
 *     流量制御はこれに任せる（「空きを見て渡す」タイミングという概念は無い）
 *
 * サンプルレートはコアと同じ **22,050 Hz** で I2S を回す（M5 側のリサンプル無し）。
 *    AW88298 は 22.05 kHz を対応レートとして持つ（レジスタ 0x06 I2SSR）。ただし
 *    ESP32-S3 に APLL は無く、**実サンプルレートの誤差は未測定。**
 *
 *   saan_speaker_setup()                          M5 を初期化し、リングを確保する（まだ鳴らさない）
 *   saan_speaker_begin_utterance(preroll, total)  プリロールバッファとリップシンク包絡を取る
 *   saan_speaker_preroll_push() を数回             変換して貯める（まだ鳴らさない）
 *   saan_speaker_start()                          鳴らし始め、貯めたぶんを 1 回の playRaw で渡す
 *   saan_speaker_write_f32() を繰り返す            変換してチャンクごとに渡す（満杯ならブロック）
 *   saan_speaker_stop()                           鳴らし終わるまで待ち、プリロールを解放
 *
 * リップシンク: 変換のたびに 256 sample（11.6 ms）ごとの RMS を包絡として貯め、
 *   鳴らし始めた時刻から「いま鳴っているサンプル位置」を推定して
 *   saan_speaker_level_now() で 0..1 を返す（隣のブロックと線形補間）。
 *   saan_ui_avatar.cpp の lip_task が 10 ms ごとに読む。
 */
#ifndef SAAN_SPEAKER_H
#define SAAN_SPEAKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ストリーミング時にプリロールするサンプル数。本家と同じ既定 8,192 = 4 チャンク = 371 ms・16 KB。
 *
 * なぜ要るか: **最初の saan_stream_pull だけ定常の約 5〜6 倍かかる**
 * （この板の実測 244.65 ms vs 41.47 ms。受容野 36 + iSTFT 2 = 38 フレームの warmup で
 * 内部の step_chunk が複数回走るため）。鳴らし始めた直後から合成を始めると、
 * その 1 回ぶんが確実にアンダーランになる。 */
#ifndef SAAN_SPK_PREROLL_SAMPLES
#define SAAN_SPK_PREROLL_SAMPLES 8192
#endif

bool saan_speaker_setup(uint32_t sample_rate);

/* 発話の開始。`preroll_samples` ぶんの int16 を貯める場所と、`total_samples`
 * （n_frames × SAAN_HOP）ぶんのリップシンク包絡を PSRAM に取る。
 *   ストリーミング   … preroll = SAAN_SPK_PREROLL_SAMPLES（total 以下に切る）
 *   貯めてから鳴らす … preroll = total
 * 取れなければ false（**黙って切り詰めない**）。saan_speaker_stop() が再生完了を待ってから解放する。 */
bool saan_speaker_begin_utterance(size_t preroll_samples, size_t total_samples);

/* まだ鳴らさずに変換して貯める。begin_utterance の量を超えたら false。
 * ⚠️ `n_samples` は**サンプル数**（フレーム数 × SAAN_HOP）。 */
bool saan_speaker_preroll_push(const float *pcm, size_t n_samples);

/* 鳴らし始めて、貯めたぶんを 1 回の playRaw で渡す */
bool saan_speaker_start(void);

/* float[-1,1] → int16 に変換してリングに書き、playRaw する（キューが満杯なら空くまで**ブロック**）。
 * ⚠️ 1 回に渡せるのは 2,048 sample（= 1 チャンク）まで。 */
bool saan_speaker_write_f32(const float *pcm, size_t n_samples);

/* 鳴らし終わるまで待ち、プリロールバッファを解放する */
void saan_speaker_stop(void);

/* 変換した / キューに渡した サンプル数（ログ用。発話ごとに 0 に戻る） */
size_t saan_speaker_buffered(void);
size_t saan_speaker_sent(void);

/* いま鳴っている位置の音量 0..1（発話内の最大 RMS で正規化）。鳴っていなければ 0。
 * ⚠️ 再生位置は「鳴らし始めた時刻 + 経過時間」から推定する。アンダーランで止まった間は
 *    M5.Speaker.isPlaying() が 0 になるので 0 を返し、次のチャンクを渡すときに時刻を取り直す。 */
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
