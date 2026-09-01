# sanoTTS-jp を M5Stack CoreS3 で鳴らす（自己完結版）。
#
# モデルと推論コアは **sanoTTS-jp**（https://github.com/ayutaz/sanoTTS-jp）のもの。
#   コード: MIT / 重み (model/student_i8.bin): sanoTTS-jp Model License 1.0
#   → NOTICE.md と LICENSES/ を読むこと。flash イメージは重みを含むので、配布時は
#     NOTICE.md の帰属表示と用途制限をそのまま付けること。
#
# ベース: CoreS3 で実機確認済みの M5Unified サンプル（touch_beep）の設定
# 中身  : components/saanotts_core/  … sanoTTS-jp/csrc からコピーした C99 推論コア
#         main/                      … sanoTTS-jp/esp32/main からコピーしたファーム本体
#                                       + saan_speaker.cpp（M5.Speaker 版の音声出力）
#         model/student_i8.bin       … int8 重み blob（643,936 B）。ビルド時に
#                                       scripts/blob_to_header.py がヘッダにする
#
# ⚠️ **本家 sanoTTS-jp とは独立してビルドできる**（相対パス参照は無い）。
#    その代わり本家を直してもここには反映されない。乖離に注意。
#
#   ./idf.sh -DSAAN_BOOT_SPEAK=1 build          起動時に 1 文喋る（実機測定用）
#   ./idf.sh -DSAAN_ENABLE_PIE=1 build          W8A8 + PIE（整数 SIMD）
#   ./idf.sh -p /dev/ttyACM0 flash monitor

cmake_minimum_required(VERSION 3.16)

# --- 重み blob をヘッダ (`const` 配列) に変換して app に埋める ---------------
#
# ⚠️ **`project()` より前に置くこと。** ESP-IDF は project() の中で
#    コンポーネント（main/ を含む）の CMakeLists を処理する。後ろに置くと
#    main/CMakeLists.txt から見た ${SAAN_MODEL_BLOB} が**空文字列**になり、
#    `--blob ""` でヘッダ生成が落ちる。
#
# ⚠️ **パーティション mmap はしない。** CoreS3 では esp_partition_mmap が
#    ESP_ERR_NO_MEM で落ちた（実機実測。partitions.csv 参照）。
#    ヘッダ埋め込みなら scripts/blob_to_header.py が `__attribute__((aligned(16)))`
#    を付けるのでアライメントも自分で保証できる。saan_model.c が起動時に
#    16 バイト境界を検査して、属性が消えたときに気づけるようにしてある。
#
# ⚠️ **`const` を外さないこと。** const → .rodata → flash（SRAM 消費 0）。
#    非 const → .data → DIRAM 643,936 B で即リンク不能。
#
# **int8 の student_i8.bin (643,936 B) のみ。** fp32 blob は
#    scripts/blob_to_header.py がビルド時に dtype を見て拒否する。
if(NOT DEFINED SAAN_MODEL_BLOB)
    set(SAAN_MODEL_BLOB "${CMAKE_CURRENT_LIST_DIR}/model/student_i8.bin")
endif()

if(NOT EXISTS "${SAAN_MODEL_BLOB}")
    message(FATAL_ERROR
        "重み blob が無い: ${SAAN_MODEL_BLOB}\n"
        "  model/student_i8.bin を置くか -DSAAN_MODEL_BLOB=<絶対パス> を渡すこと\n"
        "  （sanoTTS-jp のリリース saanotts-jp-v3-int8.bin と同じもの。"
        "sha256 c3b89216...）")
endif()

file(SIZE "${SAAN_MODEL_BLOB}" SAAN_MODEL_BLOB_SIZE)
message(STATUS "sanoTTS: model blob ${SAAN_MODEL_BLOB} (${SAAN_MODEL_BLOB_SIZE} B) → .rodata")

# main/CMakeLists.txt から見えるようにする
set(SAAN_MODEL_BLOB "${SAAN_MODEL_BLOB}" CACHE INTERNAL "重み blob の絶対パス")

# --- PIE は**既定で有効** -------------------------------------------------
#
#     ./idf.sh -DSAAN_ENABLE_PIE=0 build   で W8A32 / 移植可能 C に戻る
#
# 実機実測（README）: W8A32 は 4.83x RT、W8A8 + PIE は 1.56x RT。
# 出力は両方とも QEMU と bit 一致しているので、速い方を既定にする。
# ⚠️ **CMake のキャッシュに残る。** 一度 -D で渡した値は build/ を消すまで有効。
if(NOT DEFINED SAAN_ENABLE_PIE)
    set(SAAN_ENABLE_PIE 1)
endif()
set(SAAN_ENABLE_PIE "${SAAN_ENABLE_PIE}" CACHE INTERNAL "W8A8 + PIE を使うか")

# --- ビルドするコンポーネントを絞る ---------------------------------------
#
# ⚠️ **これが無いと IDF は全コンポーネントをビルド対象に含める。**
#    cmock だけで .bss 32,772 B、他に lwip / wpa_supplicant / mqtt など
#    このファームが 1 行も呼ばないものが並ぶ。DIRAM は 341,760 B しか無く、
#    arena 208 KB でほぼ埋まるので、ここを絞らないと苦しい。
#
# `main` とその依存（saanotts_core / M5Unified / M5GFX と、それらが REQUIRES
# するもの）だけが解決される。**依存は IDF が自動で辿る。**
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(saanotts_cores3)
