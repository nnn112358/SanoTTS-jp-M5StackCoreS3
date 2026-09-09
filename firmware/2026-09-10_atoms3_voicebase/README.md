# 2026-09-10_atoms3_voicebase/ — ATOMS3 + Atomic Voice Base（辞書 135,000 語）

`./idf_atoms3.sh build` の生成物（`-DSAAN_BOARD=atoms3`、sdkconfig.defaults + sdkconfig.atoms3、partitions_atoms3.csv）。
2026-09-10 に ATOMS3 実機で起動・checksum `0xa69a7ebbb5ccb05f`・xRT 0.427・アンダーラン 0・漢字入力
（`今日は良い天気ですね。` → 形態素 7 / 53 ids）を確認した。ログ: `../../logs/2026-09-10_atoms3_voicebase_boot.log`。
⚠️ 音が実際に Voice Base から出たかは人が聴いて確認すること（ログは M5.Speaker が有効になったことしか示さない）。

| ファイル | 焼く先 |
|---|---|
| `m5-atoms3-voicebase-kanji-8mb.bin` | **0x0**（これ 1 つで全部。git には入れない） |
| `bootloader.bin` / `partition-table.bin` / `saanotts_cores3.bin` | 0x0 / 0x8000 / 0x10000 |
| 辞書 `k1-dict-135000-4mb.bin`（本家 firmware/v0.3.1-rc1-smallflash） | 0x210000 |

```sh
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 460800 write_flash 0x0 firmware/2026-09-10_atoms3_voicebase/m5-atoms3-voicebase-kanji-8mb.bin
```
