# firmware/ — 実機に書き込んだバイナリの保存

実際に CoreS3 に焼いて動作を確認したイメージをそのまま置く（ビルドし直すと bit が変わりうるので保存する）。
各ディレクトリの `SHA256SUMS.txt` で照合できる。⚠️ **flash イメージは重み（sanoTTS-jp Model License 1.0）と
辞書（修正 BSD）を含む。** 配布するときは [`../NOTICE.md`](../NOTICE.md) を付けること。

## 2026-09-07_avatar_newcore/ — このリポジトリの既定ビルド（顔 + 辞書 + PIE、新コア）

ソース: コミット `69984c4` + 未コミットの変更（`-DSAAN_UI` の切り替え、リップシンク 10 ms、
`SAAN_XRT_INITIAL` 0.6。`git log` で 69984c4 の次のコミットに入る）。ESP-IDF v5.5.5。
実機ログ: [`../logs/2026-09-07_avatar_lip10ms_boot.log`](../logs/2026-09-07_avatar_lip10ms_boot.log)
（checksum `0xa69a7ebbb5ccb05f` / 定常 xRT 0.445 / 追い越し 0 / 発話開始まで 333 ms / リップシンク 114 フレーム/発話）。

| ファイル | 焼く先 | 中身 |
|---|---|---|
| `m5-cores3-avatar-kanji-16mb.bin` | **0x0**（これ 1 つで全部） | bootloader + パーティション表 + app + 辞書を `esptool merge_bin` で結合（15,865,008 B） |
| `bootloader.bin` | 0x0 | |
| `partition-table.bin` | 0x8000 | `../partitions.csv`（factory 2 MB / dict 0x210000） |
| `saanotts_cores3.bin` | 0x10000 | app（1,388,304 B。重み blob v2 を .rodata に含む） |
| `flash_args` | — | `idf.py flash` が使う引き数（辞書は `../model/k1-dict-438750.bin`） |

```sh
# 一括（辞書込み）。⚠️ --flash_mode qio を渡さないこと（ブートループ。ヘッダは DIO のままでよい）
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 460800 write_flash 0x0 firmware/2026-09-07_avatar_newcore/m5-cores3-avatar-kanji-16mb.bin

# app だけ差し替える（辞書と表が同じなら十分。数十秒）
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 460800 write_flash 0x10000 firmware/2026-09-07_avatar_newcore/saanotts_cores3.bin
```

⚠️ 16 MB を**圧縮ありで一括**で焼くと、辞書領域（高エントロピー 13.7 MB）で `chip stopped responding` に
なることがあった（2026-09-07 に 2 回。別プロセスの同時アクセスも重なっていた）。止まったら
`--no-compress` で辞書を 3.4 MB × 4 に分けて焼く（`docs/measurements.md`）。焼く前に `fuser /dev/ttyACM0` で
ポートが空いているか見ること。

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

## 2026-09-10_smallflash-dicts/ — 枝刈り辞書 3 種（44,000 / 135,000 / 228,000 語）を組み込んだイメージ

本家 `firmware/v0.3.1-rc1-smallflash/` の辞書を、このリポジトリの既定ビルド（顔 + 辞書 + PIE。再生の給餌方式を
本家 M5 実装に合わせた版）に組み込んだもの。辞書 1 種につき 0x0 に焼く一括イメージ 1 本。app・表は共通
（表は 16 MB のまま）。⚠️ **実機では未確認。** 詳細は [`2026-09-10_smallflash-dicts/README.md`](2026-09-10_smallflash-dicts/README.md)。

## 2026-09-10_images/ — 5 ボード × 辞書の一括イメージ（`scripts/make_images.sh` の出力）

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
| ATOMS3 + Voice Base | ESP32-S3 | 無し | 8 MB | `m5-atoms3-voicebase-kanji-dict<語数>.bin` | 入らない | 入らない | ✅ 起動確認 | ✅ 起動確認 |
| ATOMS3R + Voice Base | ESP32-S3 | 8 MB Octal | 8 MB | `m5-atoms3r-voicebase-kanji-dict<語数>.bin` | 入らない | 入らない | ⚠️ ビルドのみ | ⚠️ ビルドのみ |
| Core2（W8A32） | ESP32 | 8 MB | 16 MB | `m5-core2-avatar-kanji-dict<語数>.bin` | 入らない | 入らない | ⚠️ ビルドのみ（mmap 窓に入らないかも） | ⚠️ ビルドのみ |
| Stamp-C5（W8A32、外付け I2S DAC） | ESP32-C5 | 無し | 4 MB | `m5-stampc5-i2sdac-kanji-dict<語数>.bin` | 入らない | 入らない | 入らない | ⚠️ ビルドのみ |

「入らない」= そのボードの dict パーティション（ATOMS3/ATOMS3R 6.2 MB、Core2 3 MB、Stamp-C5 2.4 MB）に収まらないので生成していない。
起動確認は 2026-09-10（checksum `0xa69a7ebbb5ccb05f`）。

app（`<ボード>/saanotts_cores3.bin`）はボードごとに 1 つで、辞書だけが違う。一括イメージは git に入れない
（`SHA256SUMS.txt` で照合）。ソースは 2026-09-10 の作業ツリー（上流合わせのスピーカー + 4 ボード対応）。
