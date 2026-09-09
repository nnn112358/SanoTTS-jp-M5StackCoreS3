# 2026-09-10_smallflash-dicts/ — 枝刈り辞書 3 種を組み込んだイメージ

本家 sanoTTS-jp の `firmware/v0.3.1-rc1-smallflash/` にある**枝刈り辞書 3 種**を、このリポジトリの
既定ビルド（顔 + 辞書 + PIE、再生の給餌方式を本家 M5 実装に合わせた版）に組み込んだもの。
辞書 1 種につきイメージ 1 本。**app / bootloader / パーティション表は 3 本とも同じ**で、
`dict` パーティション（0x210000）に焼く辞書だけが違う。

⚠️ **実機では未確認**（2026-09-10 時点。焼いて checksum / 読みを確認したら追記すること）。
⚠️ 辞書の形式は 3 種とも `K1D1` v2 で、このリポジトリの `jdict.c`（本家 d169e91）がそのまま読める
   ことは先頭ヘッダで確認した。枝刈りしてあるので、`k1-dict-438750.bin` と読みが変わる文がある。

| イメージ（0x0 に焼く） | 辞書 | 見出し語数 | イメージのサイズ |
|---|---|---|---|
| `m5-cores3-avatar-kanji-dict44000-2mb.bin`  | `k1-dict-44000-2mb.bin`（977,456 B）    | 44,000  | 3,140,144 B |
| `m5-cores3-avatar-kanji-dict135000-4mb.bin` | `k1-dict-135000-4mb.bin`（3,006,656 B） | 135,000 | 5,169,344 B |
| `m5-cores3-avatar-kanji-dict228000-8mb.bin` | `k1-dict-228000-8mb.bin`（7,123,088 B） | 228,000 | 9,285,776 B |

パーティション表は [`../../partitions.csv`](../../partitions.csv)（16 MB / factory 2 MB / dict 0x210000〜、
`k1-dict-438750.bin` 用と同じ）。辞書名の 2mb / 4mb / 8mb は本家が想定する flash 容量だが、
このリポジトリは CoreS3（16 MB）専用なので**表は 16 MB のまま**にし、辞書だけ小さくしてある。
本家の小 flash 向けイメージ（factory 1 MB〜1152 KB）には app（1,388,576 B）が入らないので、
本家の表は使っていない。

| 共通部品 | 焼く先 |
|---|---|
| `bootloader.bin` | 0x0 |
| `partition-table.bin` | 0x8000 |
| `saanotts_cores3.bin` | 0x10000（app 1,388,576 B。重み blob v2 を .rodata に含む） |

```sh
# 一括（辞書込み）。⚠️ --flash_mode qio を渡さないこと（ブートループ。ヘッダは DIO のままでよい）
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 460800 write_flash 0x0 firmware/2026-09-10_smallflash-dicts/m5-cores3-avatar-kanji-dict44000-2mb.bin

# 辞書だけ差し替える（app と表が同じなら十分）
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 460800 write_flash 0x210000 firmware/2026-09-10_smallflash-dicts/k1-dict-135000-4mb.bin
```

ソース: コミット `ea75b9c` + 未コミットの変更（saan_speaker を本家 `saan_audio_m5.cpp` と同じ
プリロール + 3 枚リングに。`git log` で ea75b9c の次のコミットに入る）。ESP-IDF v5.5.5。
辞書の SHA-256 は本家 `firmware/v0.3.1-rc1-smallflash/SHA256SUMS.txt` と一致する。
