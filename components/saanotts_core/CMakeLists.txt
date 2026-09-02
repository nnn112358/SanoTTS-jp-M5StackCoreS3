# sanoTTS-jp の C99 推論コア（sanoTTS-jp/csrc から**コピー**したもの）。
#
# 出所: sanoTTS-jp commit 2c61a8d（2026-09-02 にコピー）
#   saanotts.c saanotts_stream.c fft.c saanotts_int8.c  … 推論コア（4 ファイルすべて要る。
#                                                          saanotts_int8.c の saan_conv1d_w 等を
#                                                          saanotts.c と saanotts_stream.c が参照する）
#   g2p.c line.c                                         … 端末側 G2P と行編集（コアは参照しない）
#
# ⚠️ **コピーなので本家と乖離しうる。** 本家 csrc/ を直したらここにも持ってくること。
#    golden test（bit 一致）は本家側の `make -C csrc all-test` で回す。
# --- 推論コアを IRAM に置く（**既定で有効**。-DSAAN_CORE_IRAM=0 で flash に戻す）------
#     ./idf.sh -DSAAN_CORE_IRAM=1 build
# CoreS3 実測: xRT 1.655 → 1.616（−2.4%）。IRAM を約 10 KB 使う。
if(NOT DEFINED SAAN_CORE_IRAM)
    set(SAAN_CORE_IRAM 1)
endif()
set(SAAN_CORE_LDFRAGS "")
if(SAAN_CORE_IRAM)
    set(SAAN_CORE_LDFRAGS "linker.lf")
    message(STATUS "sanoTTS: 推論コアの .text を IRAM に置く（linker.lf）")
endif()

# --- 端末内漢字 G2P（K-1 〜 K-7。sanoTTS-jp origin/main からコピー）-------------
#   k1dict.c        辞書 blob（LOUDS）の読み出し + Viterbi
#   k4_accent.c     アクセント規則
#   k4b_njd.c       NJD チェーンの適用
#   k7_label2ids.c  フルコンテキストラベル → 生徒の音素 ID
#   openjtalk/*.c   Open JTalk（修正 BSD、無改変。-DCHARSET_UTF_8 が要る）
# ⚠️ Open JTalk は calloc/strdup を使う（1 文あたりピーク約 105 KB、本家 K-5 実測）。
# ⚠️ **外せる。** -DSAAN_KANJI=0 で辞書リーダ・Open JTalk をビルドから外し、辞書も焼かない
#    （かな中間表現の入力だけになる）。既定は 1。CMake のキャッシュに残るので切り替えたら
#    build/ を消すこと。
if(NOT DEFINED SAAN_KANJI)
    set(SAAN_KANJI 1)
endif()
set(SAAN_KANJI "${SAAN_KANJI}" CACHE INTERNAL "端末内漢字 G2P を入れるか")
set(SAAN_K_SRCS "")
set(SAAN_K_INC "")
if(SAAN_KANJI)
    file(GLOB SAAN_OJT "${CMAKE_CURRENT_LIST_DIR}/openjtalk/*.c")
    set(SAAN_K_SRCS "k1dict.c" "k4_accent.c" "k4b_njd.c" "k7_label2ids.c" ${SAAN_OJT})
    set(SAAN_K_INC "openjtalk")
endif()

idf_component_register(
    SRCS "saanotts.c"
         "saanotts_stream.c"
         "fft.c"
         "saanotts_int8.c"
         "g2p.c"
         "line.c"
         ${SAAN_K_SRCS}
    INCLUDE_DIRS "." ${SAAN_K_INC}
    REQUIRES ""
    LDFRAGMENTS ${SAAN_CORE_LDFRAGS}
)

# ⚠️ **SAAN_FFT_DOUBLE と SAAN_USE_NAIVE_DFT は定義しない。**
#    ESP32-S3 の FPU は単精度のみ。定義するとソフト浮動小数に落ちる。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)

if(SAAN_KANJI)
    # CHARSET_UTF_8 は Open JTalk が要求する
    target_compile_definitions(${COMPONENT_LIB} PUBLIC CHARSET_UTF_8 SAAN_KANJI=1)
    message(STATUS "sanoTTS: 端末内漢字 G2P **有効**（辞書 13.7 MB の dict パーティションが要る）")
else()
    message(STATUS "sanoTTS: 端末内漢字 G2P 無効（-DSAAN_KANJI=0）。入力はかな中間表現だけ")
endif()

# --- PIE（ESP32-S3 の整数 SIMD）-------------------------------------------
#
#     idf.py -DSAAN_ENABLE_PIE=1 build
#
# ⚠️ **2 つを別々に定義できないようにしてある。** SAAN_PIE=1 だけでは
#    PIE カーネルは W8A8 経路（saan_conv1d_i8a）の中にしか無いので 1 命令も出ない。
# ⚠️ W8A8 は fp32 比で SNR が 25.88 → 23.24 dB に落ちるが、SCOREQ では差が無い（M-55）。
#    arena ピークは 196.9 → 201.1 KB（main.c の 208 KB には収まる）。
if(SAAN_ENABLE_PIE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_INT8_ACT=1 SAAN_PIE=1)
    message(STATUS "sanoTTS: W8A8 + PIE **有効**")
elseif(SAAN_W8A8_NOPIE)
    # 陰性対照専用（PIE カーネルとスカラ実装の bit 一致を同じターゲットで確かめる）
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_INT8_ACT=1)
    message(STATUS "sanoTTS: W8A8 / PIE 無し（**陰性対照**）")
else()
    message(STATUS "sanoTTS: W8A8 + PIE 無効（既定 = W8A32 / 移植可能 C）— 有効化は -DSAAN_ENABLE_PIE=1")
endif()

# ⚠️ 取り込んだ Open JTalk は無改変（PROVENANCE）。IDF の既定 -Werror=all で止まる警告は
#    これらのファイルだけエラーにしない。本家 k*.c も同じ扱い（-Wextra の未使用引数など）。
if(SAAN_KANJI)
set_source_files_properties(${SAAN_OJT} k1dict.c k4_accent.c k4b_njd.c k7_label2ids.c
    PROPERTIES COMPILE_OPTIONS
    "-Wno-error;-Wno-unused-parameter;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-sign-compare;-Wno-missing-field-initializers;-Wno-implicit-fallthrough;-Wno-unused-function")
endif()
