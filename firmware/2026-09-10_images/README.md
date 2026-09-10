# 2026-09-10_images/ — `scripts/make_images.sh` の出力（6 ボード × 入る辞書）

0x0 に焼く一括イメージ。`<ボード>/` に部品（bootloader / partition-table / app）。詳細は `../README.md`。

```sh
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 460800 write_flash 0x0 firmware/2026-09-10_images/m5-atoms3-voicebase-kanji-dict44000.bin
esptool.py --chip esp32   -p /dev/ttyUSB0 -b 460800 write_flash 0x0 firmware/2026-09-10_images/m5-core2-avatar-kanji-dict44000.bin
```

辞書の呼び名（本家）: 13M = 438750 語 / 8M = 228000 語 / 4M = 135000 語 / 2M = 44000 語。

| ボード | チップ | PSRAM | Flash | 辞書 13M | 辞書 8M | 辞書 4M | 辞書 2M | 備考 |
|---|---|---|---|---|---|---|---|---|
| cores3 | ESP32-S3 | 8 MB Quad | 16 MB | ✅ 確認済み（2026-09-07 の配置と同じ） | 生成（未確認） | ✅ 起動確認 | ✅ 起動確認 | checksum `0xa69a7ebbb5ccb05f` |
| atoms3 | ESP32-S3 | 無し | 8 MB | 入らない | 入らない | ✅ 起動確認（文字 UI。顔版は未確認） | ✅ 起動確認（同左） | 音は人が聴いて確認すること |
| atoms3r | ESP32-S3 | 8 MB Octal | 8 MB | 入らない | 入らない | ⚠️ ビルドのみ | ⚠️ ビルドのみ | Octal PSRAM。ブートループなら sdkconfig.atoms3r を QUAD に |
| core2 | ESP32 | 8 MB | 16 MB | 入らない | 入らない | ⚠️ ビルドのみ（mmap 窓に入らないかも） | ⚠️ ビルドのみ | W8A32、期待 checksum `0xe4b645c30835d42d` |
| basic | ESP32 | 無し | 16 MB | 入らない | 入らない | ⚠️ ビルドのみ | ⚠️ ビルドのみ | W8A32。**arena 176 KB の連続ヒープが取れず起動時に止まる見込み** |
| stampc5 | ESP32-C5 | 無し | 4 MB | 入らない | 入らない | 入らない | ⚠️ ビルドのみ | ESP32-C5（RISC-V）、W8A32、外付け I2S DAC（G5/G6/G7） |
