# 公式実装 Ampixa/sanoTTS と sanoTTS-jp（本プロジェクト）の実装の違い

調査日 2026-09-02。対象: <https://github.com/Ampixa/sanoTTS>（master、最終 push 2026-09-02、GPL-3.0）。

## ⚠️ 読んだもの・読んでいないもの

公式実装は **GPL-3.0**、sanoTTS-jp と本プロジェクトは **MIT**。GPL のソースを読んで書き直すと
GPL が伝播して MIT で配布できなくなるので、**ソースコード（`.c` / `.S` / `.py`）は 1 行も読んでいない**
（sanoTTS-jp の決定 D-032 と同じ線引き。hook が `gh api …/contents/*.c` と `git clone` を deny する）。

読んだのは公開ドキュメントと**デバイスのシリアルログ（数値データ）**だけ:
`README.md` / `BOARDS.md` / `docs/mcu-classes-and-porting.md` / `docs/repository-layout.md` /
`mcu/README.md` / `mcu/INT16_CHAIN.md` / `mcu/ports/esp32s3/README.md` /
`mcu/ports/esp32s3/measurements/*.log` / `arduino/README.md`。ファイル名の一覧も見た（中身は見ていない）。
**数値・ハイパーパラメータ・アーキテクチャ構成は著作権の対象外**なので、ここに書いてある事実は使ってよい。
ただし**すべて上流の申告値で、こちらで再現していない**。

## 1. 何が同じで何が違うか

| | 公式 Ampixa/sanoTTS（英語 ほか 5 言語） | sanoTTS-jp → 本プロジェクト（日本語 / CoreS3） |
|---|---|---|
| 論文 | arXiv:2608.21378（同じ著者らの公式実装） | 同じ論文からの **clean-room 再実装**（論文の数値だけから） |
| ライセンス | GPL-3.0 | MIT（重みは sanoTTS-jp Model License 1.0） |
| MCU 向けモデル | en_US kristin **567,008 params**（R7）、int8 ~680 KB。別途 heart-nano 294 k（24 kHz） | 559,008 params（語彙 57）、int8 **643,936 B**（22.05 kHz） |
| 構成 | duration → acoustic → iSTFT decoder（同じ） | 同じ。3 モデルを 40 次元 c-line で接続 |
| G2P | **espeak-ng を同梱**（en_dict だけで 168 KB、SPIFFS 275 KB） | **かな中間表現 → 音素表 877 B**（漢字はホスト側） |
| 重みの置き場 | flash XIP + **SIMD が読む前に SRAM へステージング**（arena 内） | flash（app の .rodata）を **PIE が直接読む** |
| SIMD カーネル | esp-nn（Apache-2.0）の aligned dot + 自前 Xtensa asm 1 本（`sn_matvec_s8_esp32s3.S`、3.5 KB） | 自前の inline asm（`ee.vmulas.s8.accx`、内積のみ。MAC の 99.4%） |
| 活性化の数値 | per-frame symmetric dynamic int8（同じ）。int16 残差チェーンは**設計のみ、無効**（精度再設計待ち） | per-frame int8（W8A8）。層の出口は fp32（float glue） |
| ポート境界 | 4 カーネル + 4 シム（`snt_dot_s8` / `snt_matvec_s8` / int16 版 2 つ、residency / par_run / now / scratch） | 境界の抽象は無し（コアの中に PIE 分岐） |
| 2 コア | **あり**（`snt_par_run`、列分割 + バリア、worker は block） | **無し**（合成は 1 タスク） |
| 作業メモリ | arena 88〜320 KB（大きいほど重みが常駐して速い）、ストリーミングは「planned」 | arena 208 KB 固定、**ストリーミング実装済み**（一括版と bit 一致） |
| 正しさのゲート | 母体 PyTorch との **相関 ≥ 0.98**（SIMD 版はスカラ参照と整数意味論一致） | **QEMU と 27,136 sample すべて bit 一致**（実機実測） |
| 実測 RTF（ESP32-S3 240 MHz） | ランタイム単体 **0.22×**（1 コア）/ **0.146×**（2 コア、E12-nano） | **1.55×**（W8A8 + PIE、1 コア） |
| アプリとしての RTF | espeak + WiFi + PSRAM 込みの standalone firmware は **~1.1×**（「2 秒の先読みでカバー」） | 顔 + 先読み 35% で計算しながら再生 |
| 音声出力 | GPIO PWM → LM386（PDM は雑音、MAX98357 推奨） | M5.Speaker（AW88298, 22.05 kHz 直接） |

「7 倍差」の見出しは**ランタイム単体ベンチ**の数値で、上流の**アプリ**（G2P と WiFi を載せたもの）は 1.1× と
本プロジェクトの 1.55× と同じ桁。ただしランタイム単体で 0.22× まで落とせているのは事実で、その手順が下。

## 2. 上流の最適化ラダー（実機ログ、E12-nano 294 k params / 24 kHz / 4.4 s の文）

`mcu/ports/esp32s3/measurements/` の生ログから。ESP-IDF v6.0.1、S3 rev 2、240 MHz、8 MB Octal PSRAM。
5 回の中央値、相関はチップ自身の PCM から計算。

| 段階 | 追加した手 | RTF 1 コア | RTF 2 コア | 備考 |
|---|---|---:|---:|---|
| 00 baseline | なし（重みは flash XIP） | **1.037** | 1.019 | SIMD が読めたのは MAC の **32.9%**、残り 67.1% は**スカラ**にフォールバック |
| 02 staging | **重みを SRAM にステージング** | **0.242** | 0.224 | **4.3 倍**。arena peak 199,536 B |
| 03 memory | resblock をタイル化、平面を共有 | 0.244 | 0.225 | 速度は同じ、arena peak 128,944 B に減 |
| 04 parallel head/spec | head int8 + スペクトルを 2 コアで並列、FFT twiddle を事前計算 | 0.225 | 0.187 | |
| 05 parallel trunk/fft | trunk を 2 コアでタイル並列、**フレーム 2 枚を 1 回の IFFT** | 0.224 | 0.151 | |
| 06 FINAL | embed / stem-LN をタイル化 | **0.226** | **0.146** | 相関 0.9879 |

FINAL の段別内訳（2 コア、合計 641 ms / 音声 4.416 s）:

| 段 | 割合 |
|---|---:|
| head int8 + スペクトル（並列） | 30.4% |
| **iFFT** | **23.0%** |
| acoustic | 15.2% |
| trunk pointwise int8 | 9.2% |
| embed + stem LN | 7.0% |
| trunk depthwise + LN | 6.5% |
| OLA + 出力 | 6.1% |
| LayerNorm（float glue、上に含む） | 2.7% |

参考: arena を **PSRAM に置く**と SIMD が使えずスカラに落ちて **0.778×**（SRAM の 3.4 倍遅い）。

上流が「実測で得た移植ルール」として書いているもの（`mcu/README.md`）:
1. SIMD の読み出しは常駐オペランドが要る — **flash-XIP のベクタロードは S3 で黙ってゴミを返した**（corr 0.011）
2. 常駐は「バイトがどこにあるか」の問題 — 36 k params の段が、flash にあるだけで 74 KB の行列の 3 倍かかった
3. libm はただではない — `lroundf` ~50 cycle、float 除算 ~40、double はソフト
4. idle の busy-wait はメモリバスを ~10% 汚す — worker は block させる
5. 95% 使用の malloc は賭け — arena だけが生き残る

## 3. 本プロジェクトへの含意（xRT 1.55 をどう縮めるか）

本プロジェクトのホスト側（sanoTTS-jp `reports/d3b_latency_fft.json`、fp32、M4 Max）の段別は
decoder 60% / acoustic 25% / token block 13% / iSTFT < 1%。実機（int8 + PIE）では積和が縮んで
iSTFT（float FFT）の割合が上がるはずだが、**実機の内訳は未測定**。

| 優先 | 手 | 上流での効き | 本プロジェクトでの見込み | 注意 |
|---|---|---|---|---|
| 0 | **実機プロファイル**（段別 + カーネル別） | — | 方針が決まる | `-DSAAN_PROFILE=1` でだけ有効化、checksum は変えない |
| 1 | **重みの SRAM 常駐**（decoder の大きい層から、空き ~100 KB ぶん） | **4.3 倍**（ただし上流の baseline は 67% がスカラ落ちだった） | うちは PIE が flash から直接読めているので 4 倍は出ない。**flash 待ちの分だけ**（要プロファイル） | 上流はここで「flash-XIP のベクタロードがゴミ」を踏んだ。うちは実機で QEMU と bit 一致しているので現状は問題ないが、配置を変えたら再検証 |
| 2 | **2 コア化** | 0.226 → 0.146（**1.55 倍**） | 1.55 → **約 1.0** に届く可能性。ただし上流は列分割 + バリアで**モデルを知らないポート層**に閉じている。うちは `saan_stream_pull` の中を段に分ける必要がある | worker は block（busy-wait 禁止） |
| 3 | iSTFT: **フレーム 2 枚を 1 回の複素 IFFT** | 05 段で寄与（iFFT は最終 23%） | うちの `fft.c` は twiddle 事前計算済み、実 IFFT 1 枚 = 複素 512 点。2 枚ペア化で FFT 部分が約半分 | 出力は bit で変わりうる（丸め順）。|max| と Σx² で確認 |
| 4 | int16 残差チェーン（float glue 削減） | **上流も無効**（精度再設計待ち） | やらない | — |
| 5 | esp-nn（Apache-2.0）の aligned dot に置き換え | 上流はこれ + 自前 asm | うちの PIE 内積と同等のはず。**効かない見込み** | ライセンスは問題ない（Apache-2.0） |

まとめ: 上流が 0.22× に達した本質は **(a) SIMD に食わせる重みを SRAM に置いた**、**(b) 2 コア**、
**(c) iSTFT の並列化とペア化** の 3 つで、**カーネルの内積そのものではない**。本プロジェクトは (a) の
半分（PIE が flash を直接読める）は済んでいて、(b)(c) が未着手。先に 0（プロファイル）で
「flash 待ち」「iSTFT」「float glue」の比率を取ってから 1 → 2 → 3 の順で入れる。
