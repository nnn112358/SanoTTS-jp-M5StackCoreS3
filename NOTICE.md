# NOTICE

このプロジェクト（sanoTTS-jp on M5Stack CoreS3）は、次の成果物を使用しています。

## 1. sanoTTS-jp — 推論コアとファーム本体（MIT）

- <https://github.com/ayutaz/sanoTTS-jp>
- Copyright (c) 2026 yousan — MIT License（全文: [`LICENSES/sanoTTS-jp.LICENSE.txt`](LICENSES/sanoTTS-jp.LICENSE.txt)）
- 使用箇所: `components/saanotts_core/`（`csrc/` の C99 推論コア・G2P・行編集）、
  `main/main.c` `saan_model.c` `saan_console.{c,h}` `demo_ids.h`（`esp32/main/`）、
  `scripts/blob_to_header.py`。詳細は [`README.md`](README.md) の「ファイルの出所」。

## 2. sanoTTS-jp モデル v3 — 重み（sanoTTS-jp Model License 1.0）

- `model/student_i8.bin` = GitHub Release の **`saanotts-jp-v3-int8.bin`**
  （SHA-256 `c3b89216133fa7bee3f61ed9d8e6c7183a5dfd41b70dab194f42c20fce5b4170`）
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
