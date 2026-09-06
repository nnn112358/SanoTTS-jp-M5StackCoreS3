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
#         model/student_i8.bin       … int8 重み blob（**v2 形式**、654,032 B。Release v0.3.0）。
#                                       ビルド時に scripts/blob_to_header.py がヘッダにする
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
#    非 const → .data → DIRAM 654,032 B で即リンク不能。
#
# **int8 の student_i8.bin (v2 形式、654,032 B) のみ。** fp32 blob は
#    scripts/blob_to_header.py がビルド時に dtype を見て拒否する。
#    v1 形式（643,936 B。Release v0.2.0 以前）はコアが起動時に SAAN_ERR_VERSION で拒む。
if(NOT DEFINED SAAN_MODEL_BLOB)
    set(SAAN_MODEL_BLOB "${CMAKE_CURRENT_LIST_DIR}/model/student_i8.bin")
endif()

if(NOT EXISTS "${SAAN_MODEL_BLOB}")
    message(FATAL_ERROR
        "重み blob が無い: ${SAAN_MODEL_BLOB}\n"
        "  model/student_i8.bin を置くか -DSAAN_MODEL_BLOB=<絶対パス> を渡すこと\n"
        "  （sanoTTS-jp Release v0.3.0 以降の saanotts-jp-v3-int8.bin = blob v2、"
        "sha256 2d2b8543...）")
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

# --- 画面の実装（-DSAAN_UI=avatar|text）を**早期展開にも見せる**-----------------------
#
# main/CMakeLists.txt はこの値で REQUIRES（m5stack-avatar を要求するか）を変える。
# ⚠️ IDF は各コンポーネントの依存を、project() の中の「早期展開」（別プロセスの cmake -P）で
#    先に集める。そこでは -D のキャッシュ変数が見えないので、普通の変数だと既定の avatar で
#    REQUIRES が固まり、text でも m5stack-avatar がビルド対象に残る（実際に残った。
#    project_description.json の main.reqs で確認）。ビルドプロパティは早期展開に渡るので、
#    ここで載せて main 側は idf_build_get_property で読む。
if(NOT DEFINED SAAN_UI)
    set(SAAN_UI "avatar")
endif()
set(SAAN_UI "${SAAN_UI}" CACHE STRING "画面の実装: avatar（顔）か text（文字だけ）")
idf_build_set_property(SAAN_UI "${SAAN_UI}")

project(saanotts_cores3)

# --- 辞書 blob を flash の `dict` パーティションに焼く ----------------------------
#
# 端末内漢字 G2P の辞書（sanoTTS-jp Release の k1-dict-438750.bin、13,702,320 B。v0.2.0 と v0.3.0 で同一）。
# git には入れていない: scripts/get_dict.sh で取る。
# ⚠️ mmap して読むので app には埋めない（app が 14 MB になる）。
# ⚠️ -DSAAN_KANJI=0 のときは焼かない（辞書も要らない）。値は components/saanotts_core が
#    CACHE に置くが、project() の前に評価されるここでは自分で既定を決める。
if(NOT DEFINED SAAN_KANJI)
    set(SAAN_KANJI 1)
endif()
if(SAAN_KANJI)
if(NOT DEFINED SAAN_DICT_BLOB)
    set(SAAN_DICT_BLOB "${CMAKE_CURRENT_LIST_DIR}/model/k1-dict-438750.bin")
endif()
if(NOT EXISTS "${SAAN_DICT_BLOB}")
    message(FATAL_ERROR
        "辞書 blob が無い: ${SAAN_DICT_BLOB}\n"
        "  scripts/get_dict.sh で取るか -DSAAN_DICT_BLOB=<絶対パス> を渡すこと")
endif()
file(SIZE "${SAAN_DICT_BLOB}" SAAN_DICT_BLOB_SIZE)
message(STATUS "sanoTTS: dict blob ${SAAN_DICT_BLOB} (${SAAN_DICT_BLOB_SIZE} B) → dict パーティション")
esptool_py_flash_to_partition(flash "dict" "${SAAN_DICT_BLOB}")
endif()
