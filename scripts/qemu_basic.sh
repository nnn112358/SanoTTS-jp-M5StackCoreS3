#!/usr/bin/env bash
# Core Basic（ESP32、PSRAM 無し）のメモリ配置を QEMU で確かめる。
# M5Unified / M5GFX は QEMU で動かないので、M5 無しの経路（-DSAAN_HEADLESS=1）+ I2S 書き込み無し
# （-DSAAN_SKIP_I2S=1）で、arena の複数ブロック確保 / 辞書 mmap（ESP32 の 4 MB 窓）/ 合成の checksum /
# 漢字 G2P の作業領域を見る。**M5 込みの実機のヒープはこれより厳しい**（M5GFX・avatar のタスクぶん）。
#   scripts/qemu_basic.sh                 # build_basic_qemu/ を作って起動、ログは logs/qemu_basic.log
#   scripts/qemu_basic.sh -DSAAN_DICT=135000
set -e
cd "$(dirname "$0")/.."
./idf.sh -B build_basic_qemu -DSDKCONFIG=build_basic_qemu/sdkconfig -DIDF_TARGET=esp32 \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.basic" -DSAAN_BOARD=basic \
    -DSAAN_HEADLESS=1 -DSAAN_SKIP_I2S=1 "$@" build > /dev/null
git checkout -q dependencies.lock 2>/dev/null || true
unset IDF_PATH IDF_PYTHON_ENV_PATH ESP_IDF_VERSION IDF_TOOLS_PATH OPENOCD_SCRIPTS ESP_ROM_ELF_DIR
. "$HOME/esp/esp-idf/export.sh" > /dev/null 2>&1
mkdir -p logs
uv run --no-project python scripts/qemu_run.py build_basic_qemu "logs/qemu_basic_$(date +%F).log" --secs 300
