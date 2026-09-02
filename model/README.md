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

## 辞書（端末内漢字 G2P 用、git には入れていない）

| ファイル | 出所 | SHA-256 |
|---|---|---|
| `k1-dict-438750.bin` | sanoTTS-jp Release v0.2.0（13,702,320 B。NAIST-jdic / UniDic を TTS 用に枝刈りした派生物、修正 BSD） | `f162c922074d76817298b34d8a8fd35f7d195f38540303485a76c956b5d84877` |

```sh
./scripts/get_dict.sh        # Release から取って SHA-256 を検証する
```

- ビルド時に `dict` パーティション（0x210000、14.6 MB）へ焼かれ、端末は `esp_partition_mmap` で
  そのまま読む（RAM にコピーしない）。`-DSAAN_KANJI=0` なら不要
- `sanoTTS-jp-v0.2.0.SHA256SUMS.txt` は Release の全資産のハッシュ（照合用）
