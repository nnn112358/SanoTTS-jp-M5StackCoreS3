# model/

| ファイル | 出所 | SHA-256 |
|---|---|---|
| `student_i8.bin` | **sanoTTS-jp** <https://github.com/ayutaz/sanoTTS-jp> — GitHub Release の `saanotts-jp-v3-int8.bin`（v3 / int8 / SAAN v1 形式 / 643,936 B） | `c3b89216133fa7bee3f61ed9d8e6c7183a5dfd41b70dab194f42c20fce5b4170` |

- モデル: 559,008 params の蒸留生徒（Duration 32 / Acoustic 48 / Decoder 76 幅、Stage 3 80k step）。
  piper-plus（つくよみちゃん）教師からの蒸留。詳細は本家の `MODEL_CARD.md`。
- **ライセンスは MIT ではない**: `LicenseRef-sanoTTS-jp-Model-1.0`
  （[`../LICENSES/sanoTTS-jp.LICENSE-MODEL.md`](../LICENSES/sanoTTS-jp.LICENSE-MODEL.md)）。
  帰属表示と生成音声の用途制限は [`../NOTICE.md`](../NOTICE.md)。
- ビルド時に `scripts/blob_to_header.py` がこれを `build/esp-idf/main/saan_model_blob.h`
  （`const uint8_t[]`、16 B 境界）へ変換し、app の `.rodata`（flash）に入る。
  差し替えたら自動で作り直る。**fp32 blob は受け付けない**（スクリプトが dtype を見て拒否）。

更新するとき:

```sh
gh release download --repo ayutaz/sanoTTS-jp --pattern 'saanotts-jp-v3-int8.bin' -O model/student_i8.bin
sha256sum model/student_i8.bin      # 上の値と突き合わせる
```
