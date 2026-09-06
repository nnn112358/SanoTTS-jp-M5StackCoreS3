# NOTICE

このプロジェクト（sanoTTS-jp on M5Stack CoreS3）は、次の成果物を使用しています。

## 1. sanoTTS-jp — 推論コアとファーム本体（MIT）

- <https://github.com/ayutaz/sanoTTS-jp>
- Copyright (c) 2026 yousan — MIT License（全文: [`LICENSES/sanoTTS-jp.LICENSE.txt`](LICENSES/sanoTTS-jp.LICENSE.txt)）
- 使用箇所: `components/saanotts_core/`（`csrc/` の C99 推論コア・G2P・行編集）、
  `main/main.c` `saan_model.c` `saan_console.{c,h}` `demo_ids.h`（`esp32/main/`）、
  `scripts/blob_to_header.py`。詳細は [`README.md`](README.md) の「ファイルの出所」。

## 2. sanoTTS-jp モデル v3 — 重み（sanoTTS-jp Model License 1.0）

- `model/student_i8.bin` = GitHub Release **v0.3.0** の **`saanotts-jp-v3-int8.bin`**（blob v2 形式、654,032 B、
  SHA-256 `2d2b8543c06b6a749f19c9918de68244409e2bb6ad1d921a90b5c358f96d4d79`）。
  2026-09-04 までは v0.2.0 の同名資産（v1 形式、643,936 B、SHA-256 `c3b89216…`）だった。重みの値は同じで配置だけが違う
- ライセンス: `LicenseRef-sanoTTS-jp-Model-1.0`
  （全文: [`LICENSES/sanoTTS-jp.LICENSE-MODEL.md`](LICENSES/sanoTTS-jp.LICENSE-MODEL.md)）。
  **リポジトリの MIT はモデルの重みには適用されない。**
- ⚠️ このプロジェクトのビルド成果物（flash イメージ）は**重みを含む**ので、
  再配布するときは本 NOTICE ごと配布し、下記 §2.1 の帰属表示と §2.2 の用途制限を
  受け取った側にも課すこと（ライセンス §3.3）。

### 2.1 帰属表示（ライセンス §3.1 により**そのまま**再掲。1 行も削らないこと）

```
This model was distilled from a piper-plus teacher model.
sanoTTS-jp — https://github.com/ayutaz/sanoTTS-jp

つくよみちゃんコーパス
  本ソフトウェアの音声合成には、フリー素材キャラクター「つくよみちゃん」
  （© 夢前黎）が無料公開している音声データを使用しています。
  https://tyc.rei-yumesaki.net/material/corpus/

MOE-Speech (litagin) — https://huggingface.co/spaces/litagin/moe-speech-license
  著作権法 30 条の 4（情報解析のための利用）に基づき学習に使用。

蒸留に使用したテキストコーパス:
  - Common Voice ja (Mozilla) — CC0-1.0
      https://github.com/common-voice/common-voice
  - ROHAN4600 (森勢将雅) — CC0-1.0
      https://github.com/mmorise/rohan4600
  - ITA コーパス — CC0-1.0
      https://github.com/mmorise/ita-corpus
  - JSUT ver1.1 (高道慎之介) — CC-BY-SA-4.0 ほか（subset 別）
      https://sites.google.com/site/shinnosuketakamichi/publication/jsut

教師実装: piper-plus (MIT) — https://github.com/ayutaz/piper-plus
```

### 2.2 生成音声の用途制限（ライセンス §3.2。つくよみちゃんコーパスの条件が伝播したもの）

本モデルが生成した音声は次に使えない:
個人・団体への攻撃・批判 / 政治・宗教上の主張 / アダルト用途 / 音声素材としての再配布。
一次ソース: <https://tyc.rei-yumesaki.net/material/corpus/>（食い違う場合は一次ソースが優先）。

## 3. M5Unified / M5GFX（MIT, M5Stack）

- <https://github.com/m5stack/M5Unified> / <https://github.com/m5stack/M5GFX>
- ESP-IDF Component Registry から取得（`main/idf_component.yml`、バージョンは `dependencies.lock`）。
  ライセンス全文は `managed_components/m5stack__m5unified/LICENSE` と
  `managed_components/m5stack__m5gfx/LICENSE`（初回ビルドで復元される）。
- 画面の日本語フォント `lgfxJapanGothic_20` は M5GFX 同梱の IPA フォント由来
  （IPA Font License Agreement v1.0。`managed_components/m5stack__m5gfx/src/lgfx/Fonts/IPA/`）。

## 4. ESP-IDF（Apache-2.0, Espressif）

- <https://github.com/espressif/esp-idf> v5.5.5

## 5. m5stack-avatar（MIT, Shinya Ishikawa）

- <https://github.com/stack-chan/m5stack-avatar> commit `7a90083`（v0.10.0）
- Copyright (c) 2018 Shinya Ishikawa — MIT License
  （全文: [`components/m5stack-avatar/LICENSE.txt`](components/m5stack-avatar/LICENSE.txt)）
- `components/m5stack-avatar/src/` にコピー。ESP-IDF（Arduino 無し）で通すための差分は
  [`components/m5stack-avatar/README.md`](components/m5stack-avatar/README.md)

## 6. Open JTalk — 端末内の日本語テキスト処理（修正 BSD）

- `components/saanotts_core/openjtalk/`（34 ファイル + COPYING）。sanoTTS-jp が
  pyopenjtalk-plus 0.4.1.post9 の sdist から取り込んだもの（改変は `jpcommon_label.c` の
  `MAXBUFLEN 1024 → 256` の 1 件。詳細は sanoTTS-jp `csrc/openjtalk/PROVENANCE.md`）。
- Copyright (c) 2008-2016 Nagoya Institute of Technology / HTS Working Group — 修正 BSD
  （全文: [`LICENSES/open_jtalk.COPYING.txt`](LICENSES/open_jtalk.COPYING.txt)、
  sanoTTS-jp の帰属表示: [`LICENSES/sanoTTS-jp.NOTICE-openjtalk.txt`](LICENSES/sanoTTS-jp.NOTICE-openjtalk.txt)）

## 7. 辞書 `k1-dict-438750.bin` — 端末内の形態素解析用（修正 BSD）

- sanoTTS-jp Release v0.3.0 の `k1-dict-438750.bin`（13,702,320 B、v0.2.0 と同一のファイル、
  SHA-256 `f162c922074d76817298b34d8a8fd35f7d195f38540303485a76c956b5d84877`）。
  `scripts/get_dict.sh` で取得する（git には入れていない）。
- NAIST Japanese Dictionary（NAIST）と UniDic（The UniDic Consortium）を TTS 用に
  枝刈り・形式変換した派生物。修正 BSD。帰属表示は
  [`LICENSES/sanoTTS-jp.NOTICE-dictionary.txt`](LICENSES/sanoTTS-jp.NOTICE-dictionary.txt)
- ⚠️ 枝刈りしてあるので、フル辞書（ホストの OpenJTalk）と読みが変わる文がある
  （sanoTTS-jp の実測で 17.79% の文。地名・固有名詞で起きやすい）
