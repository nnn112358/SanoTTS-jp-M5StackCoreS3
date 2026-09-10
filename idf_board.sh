#!/usr/bin/env bash
# ボードを選んで idf.py を呼ぶ。ボードごとに build ディレクトリを分ける（CMake のキャッシュが混ざらない）。
#
#   ./idf_board.sh cores3  build                 # = ./idf.sh build（build/。既定の CoreS3）
#   ./idf_board.sh atoms3  build flash monitor   # build_atoms3/
#   ./idf_board.sh atoms3r build                 # build_atoms3r/
#   ./idf_board.sh core2   build                 # build_core2/（ESP32。W8A32、arena は PSRAM）
#   ./idf_board.sh basic   build                 # build_basic/（ESP32、PSRAM 無し。⚠️ arena が取れない見込み）
#   ./idf_board.sh stampc5 build                 # build_stampc5/（ESP32-C5 RISC-V。W8A32、外付け I2S DAC）
#
# 辞書を選ぶなら -DSAAN_DICT=44000|135000|228000|438750 を足す（既定はボードごと。CMakeLists.txt）。
#   ./idf_board.sh atoms3 -DSAAN_DICT=44000 build
set -e
cd "$(dirname "$0")"
board="${1:?ボードを指定: cores3 | atoms3 | atoms3r | core2 | basic | stampc5}"; shift
case "$board" in
  cores3)  exec ./idf.sh "$@" ;;
  atoms3|atoms3r) extra="" ;;
  core2|basic) extra="-DIDF_TARGET=esp32" ;;
  stampc5) extra="-DIDF_TARGET=esp32c5" ;;
  *) echo "不明なボード: $board（cores3 | atoms3 | atoms3r | core2 | basic | stampc5）" >&2; exit 1 ;;
esac
exec ./idf.sh -B "build_$board" -DSDKCONFIG="build_$board/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.$board" -DSAAN_BOARD="$board" $extra "$@"
