#!/usr/bin/env bash
# ESP-IDF v5.5.5 を**クリーンな環境で**有効にして idf.py を呼ぶ。
#
# ⚠️ 5.4 を有効にした端末から呼ぶと IDF_PYTHON_ENV_PATH を引き継いで
#    「5.4 用の venv で 5.5 を動かす」ことになり、install も build も落ちる。
unset IDF_PATH IDF_PYTHON_ENV_PATH ESP_IDF_VERSION IDF_TOOLS_PATH \
      OPENOCD_SCRIPTS ESP_ROM_ELF_DIR
. "$HOME/esp/esp-idf/export.sh" > /dev/null 2>&1 || { echo "export.sh 失敗"; exit 1; }
cd "$(dirname "$0")" || exit 1
exec idf.py "$@"
