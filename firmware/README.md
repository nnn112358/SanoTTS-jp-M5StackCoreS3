# firmware/ — 焼いて使えるバイナリ

ボード × 辞書の一括イメージ（0x0 に焼く）と、その部品（bootloader / パーティション表 / app）を置く。
辞書込みの一括イメージは大きいので git には入れず（`SHA256SUMS.txt` で照合）、`scripts/make_images.sh` で作る。
⚠️ **flash イメージは重み（sanoTTS-jp Model License 1.0）と辞書（修正 BSD）を含む。** 配布するときは
[`../NOTICE.md`](../NOTICE.md) を付けること。

古いイメージ（2026-09-07 の CoreS3 版、2026-09-10 の辞書別 / ATOMS3 単体）は 2026-09-12 に消した。
必要なら git の履歴（`0f946a5` 以前）にある。

## release_v0.3.0/ — 本家 sanoTTS-jp Release v0.3.0 の CoreS3 イメージ（顔なし）

<https://github.com/ayutaz/sanoTTS-jp/releases/download/v0.3.0/m5-cores3-firmware-kanji-16mb.bin>
（SHA-256 `48140d7c…`、`SHA256SUMS.release.txt` は Release の全資産のハッシュ）。本家の M5 ファーム
（文字表示、辞書パーティションは 0x2D0000）で、2026-09-07 にこの板で checksum `0xa69a7ebbb5ccb05f` /
xRT 0.448 を確認した（`../logs/2026-09-07_release_v0.3.0_boot.log`）。

```sh
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 firmware/release_v0.3.0/m5-cores3-firmware-kanji-16mb.bin
```

⚠️ このリポジトリのビルドと**辞書の offset が違う**（0x210000 と 0x2D0000）ので、片方から片方へ戻すときは
app だけでなく表と辞書も焼き直すこと（一括イメージならそれで済む）。

⚠️ 16 MB を**圧縮ありで一括**で焼くと、辞書領域（高エントロピー 13.7 MB）で `chip stopped responding` に
なることがあった（2026-09-07 に 2 回）。止まったら `--no-compress` で分けて焼く。焼く前に `fuser /dev/ttyACM0` で
ポートが空いているか見ること。

## 2026-09-10_images/ — 6 ボード × 辞書の一括イメージ（`scripts/make_images.sh` の出力）

辞書は本家の呼び名で 13M / 8M / 4M / 2M（想定 flash 容量）。ファイル名は語数:

| 呼び名 | ファイル | 語数 | サイズ |
|---|---|---|---|
| 13M | `k1-dict-438750.bin` | 438,750 | 13,702,320 B |
| 8M | `k1-dict-228000-8mb.bin` | 228,000 | 7,123,088 B |
| 4M | `k1-dict-135000-4mb.bin` | 135,000 | 3,006,656 B |
| 2M | `k1-dict-44000-2mb.bin` | 44,000 | 977,456 B |

| ボード | チップ | PSRAM | Flash | イメージ（0x0 に焼く） | 辞書 13M | 辞書 8M | 辞書 4M | 辞書 2M |
|---|---|---|---|---|---|---|---|---|
| CoreS3 | ESP32-S3 | 8 MB Quad | 16 MB | `m5-cores3-avatar-kanji-dict<語数>.bin` | ✅ 確認済み（2026-09-07 の配置と同じ） | 生成（未確認） | ✅ 起動確認 | ✅ 起動確認 |
| ATOMS3 + Voice Base | ESP32-S3 | 無し | 8 MB | `m5-atoms3-voicebase-kanji-dict<語数>.bin` | 入らない | 入らない | ✅ 起動確認（文字 UI。顔版は未確認） | ✅ 起動確認（同左） |
| ATOMS3R + Voice Base | ESP32-S3 | 8 MB Octal | 8 MB | `m5-atoms3r-voicebase-kanji-dict<語数>.bin` | 入らない | 入らない | ⚠️ ビルドのみ | ⚠️ ビルドのみ |
| Core2（W8A32） | ESP32 | 8 MB | 16 MB | `m5-core2-avatar-kanji-dict<語数>.bin` | 入らない | 入らない | ⚠️ ビルドのみ（mmap 窓に入らないかも） | ⚠️ ビルドのみ |
| Core Basic（W8A32） | ESP32 | 無し | 16 MB | `m5-basic-avatar-kanji-dict<語数>.bin` | 入らない | 入らない | ⚠️ ビルドのみ（arena は複数ブロック） | ⚠️ ビルドのみ（同左） |
| Stamp-C5（W8A32、外付け I2S DAC、M5 無し） | ESP32-C5 | 無し | 4 MB | `m5-stampc5-i2sdac-kanji-dict<語数>.bin` | 入らない | 入らない | ⚠️ ビルドのみ | ⚠️ ビルドのみ |

「入らない」= そのボードの dict パーティション（ATOMS3/ATOMS3R 6.2 MB、Core2 / Basic 3 MB、Stamp-C5 2.9 MB）に収まらないので生成していない。
起動確認は 2026-09-10（checksum `0xa69a7ebbb5ccb05f`）。

app（`<ボード>/saanotts_cores3.bin`）はボードごとに 1 つで、辞書だけが違う。一括イメージは git に入れない
（`SHA256SUMS.txt` で照合）。ソースは 2026-09-10 の作業ツリー（上流合わせのスピーカー + 4 ボード対応）。
