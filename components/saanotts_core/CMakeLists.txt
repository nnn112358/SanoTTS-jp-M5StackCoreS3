# sanoTTS-jp の C99 推論コア（sanoTTS-jp/csrc から**コピー**したもの）。
#
# 出所: sanoTTS-jp origin/main d169e91（2026-09-04 にコピー。全ファイル bit 一致を確認）
#   saanotts.c saanotts_stream.c fft.c saanotts_int8.c  … 推論コア（4 ファイルすべて要る。
#                                                          saanotts_int8.c の saan_conv1d_w 等を
#                                                          saanotts.c と saanotts_stream.c が参照する）
#   saan_prof.h erf_table.h                              … 段別プロファイラ / GELU の erf 表（S3）
#   g2p.c line.c                                         … 端末側 G2P と行編集（コアは参照しない）
#
# ⚠️ **コピーなので本家と乖離しうる。** 本家 csrc/ を直したらここにも持ってくること。
#    golden test（bit 一致）は本家側の `make -C csrc all-test` で回す。
# ⚠️ **blob は v2**（654,032 B、int8 conv 重みが [cout][k][align16(cin)]）。S4 以降のコアは
#    v1 blob（643,936 B）を SAAN_ERR_VERSION で拒む。model/README.md を読むこと。
#
# 2026-09-04 の同期で入った上流の変更（すべて出力 bit 同一 / checksum は S3 で 1 回だけ変わった）:
#   S1 テンソル検索を init に集約 / S2 量子化を逆数乗算 + round.s / S3 GELU を erf 表に /
#   S4 blob v2（転置コピーを消した）/ S5a・S5b PIE の内積を loopnez + weight-stationary に /
#   T2 ストリーミングで有効範囲だけ計算 / T3 token block をパイプに / T4 arena 詰め（208 → 176 KB）/
#   T5 erf 表を内部 DRAM に / K-B 入力 1 経路 / T10 Open JTalk のヒープを PSRAM に
#   上流 CoreS3 実測（M-90、顔なし）: 定常 xRT 0.926 → **0.446**。⚠️ 本リポジトリ（顔あり）では未測定。

# --- 推論コアを IRAM に置く（**既定で有効**。-DSAAN_CORE_IRAM=0 で flash に戻す）------
#     ./idf.sh -DSAAN_CORE_IRAM=1 build
# CoreS3 実測（旧コア）: xRT 1.655 → 1.616（−2.4%）。IRAM を約 10 KB 使う。
# ⚠️ 新コアでの効果は未測定。DIRAM が苦しければ 0 にする。
if(NOT DEFINED SAAN_CORE_IRAM)
    set(SAAN_CORE_IRAM 1)
endif()
set(SAAN_CORE_LDFRAGS "")
if(SAAN_CORE_IRAM)
    set(SAAN_CORE_LDFRAGS "linker.lf")
    message(STATUS "sanoTTS: 推論コアの .text を IRAM に置く（linker.lf）")
endif()

# --- 端末内漢字 G2P（K トラック。sanoTTS-jp origin/main からコピー）--------------
#   jdict.c        辞書 blob（LOUDS）の読み出し + Viterbi（旧 k1dict.c）
#   accent.c       アクセント規則 4 段（旧 k4_accent.c）
#   njd_rules.c    NJD チェーンの適用（旧 k4b_njd.c）
#   label_ids.c    フルコンテキストラベル → 生徒の音素 ID（旧 k7_label2ids.c）
#   dan_table.h / token_table.h  自動生成の表（旧 k7_dan.h / k7_table.h）
#   openjtalk/*.c  Open JTalk（修正 BSD、無改変。-DCHARSET_UTF_8 が要る）
#   ⚠️ 2026-09-03 に上流が工程番号（K1〜K7）から責務ベースの名前に変えた（53f8ef7）。旧名は無い。
# ⚠️ Open JTalk は calloc/strdup を使う（1 文あたりピーク約 105 KB、本家 K-5 実測）。
#    T10(b): `-include oj_heap_psram.h` を openjtalk/*.c **だけ**に当てて calloc/strdup/free を
#    PSRAM 優先（oj_heap_psram.c）に差し替える。ソースは 1 バイトも変えない。
#    CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL（16 KB 以下は内部 DRAM を先に試す）のせいで、
#    素の calloc だと 1 文ぶんがまるごと内部 DRAM に載る。上流 CoreS3 実測（M-90）で
#    1 発話あたりの内部 DRAM の減りが 3 KB になった。-DSAAN_OJ_PSRAM=0 が陽性対照。
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
    set(SAAN_K_SRCS "jdict.c" "accent.c" "njd_rules.c" "label_ids.c" ${SAAN_OJT})
    set(SAAN_K_INC "openjtalk")
    if(NOT DEFINED SAAN_OJ_PSRAM)
        set(SAAN_OJ_PSRAM 1)
    endif()
    if(SAAN_OJ_PSRAM)
        list(APPEND SAAN_K_SRCS "oj_heap_psram.c")
    endif()
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

# --- プラットフォームの注入点（T5-G2）------------------------------------------
# saanotts_internal.h は SAAN_PORT_HEADER が定義されていればそれを include する。
# saan_port_esp32.h が SAAN_HOT_DATA → DRAM_ATTR にし、erf 表 1,032 B（erf_table.h）を
# 内部 DRAM に載せる（flash の .rodata だと重みのストリームと D-cache を争う）。
# 配置だけの変更なので checksum は不変（上流 T5-G2 で確認）。
target_compile_definitions(${COMPONENT_LIB} PRIVATE "SAAN_PORT_HEADER=\"saan_port_esp32.h\"")

if(SAAN_KANJI)
    # CHARSET_UTF_8 は Open JTalk が要求する
    target_compile_definitions(${COMPONENT_LIB} PUBLIC CHARSET_UTF_8 SAAN_KANJI=1)
    message(STATUS "sanoTTS: 端末内漢字 G2P **有効**（辞書 13.7 MB の dict パーティションが要る）")

    # T10(a): label_ids.c のトークン表 10,240 B を .bss ではなく合成 arena から借りる。
    # ⚠️ **saan_kanji.c が label_ids_set_scratch() を必ず呼ぶ前提**（layout() の中で呼ぶ）。
    #    呼ばずに label_ids_convert() を叩くと LABEL_IDS_ERR_ARG で止まる（黙って動かない）。
    target_compile_definitions(${COMPONENT_LIB} PUBLIC LABEL_IDS_EXTERNAL_SCRATCH=1)

    if(SAAN_OJ_PSRAM)
        message(STATUS "sanoTTS: Open JTalk の一時ヒープを **PSRAM 優先**に（-include oj_heap_psram.h）。陽性対照は -DSAAN_OJ_PSRAM=0")
    else()
        message(STATUS "sanoTTS: Open JTalk の一時ヒープは素の calloc（**陽性対照**）")
    endif()
else()
    message(STATUS "sanoTTS: 端末内漢字 G2P 無効（-DSAAN_KANJI=0）。入力はかな中間表現だけ")
endif()

# --- 段別プロファイラ（saan_prof.h）--------------------------------------------
#     ./idf.sh -DSAAN_PROFILE=1 build
# 1 チャンクの時間がどの段・カーネルに行っているかを CCOUNT で出す（main.c が発話後に表を出す）。
# **既定は無効**で、無効ならコアは 1 バイトも変わらない（マクロが ((void)0) に展開される）。
# ⚠️ 計測自体にコストがある。速度を報告するときは SAAN_PROFILE=0 で測ること。
if(SAAN_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_PROFILE=1)
    message(STATUS "sanoTTS: 段別プロファイラ **有効**（速度の報告には使わない）")
endif()

# --- PIE（ESP32-S3 の整数 SIMD）-------------------------------------------
#
#     idf.py -DSAAN_ENABLE_PIE=1 build
#
# ⚠️ **2 つを別々に定義できないようにしてある。** SAAN_PIE=1 だけでは
#    PIE カーネルは W8A8 経路（saan_conv1d_i8a）の中にしか無いので 1 命令も出ない。
# ⚠️ W8A8 は fp32 比で SNR が 25.88 → 23.24 dB に落ちるが、SCOREQ では差が無い（M-55）。
# ⚠️ **PIE は int8 blob v2 でないと効かない**（S4 で転置コピーを消し、blob の配置を直接読む）。
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
#    これらのファイルだけエラーにしない。本家 K トラックの .c も同じ扱い（-Wextra の未使用引数など）。
# ⚠️ **set_source_files_properties は上書き（追記ではない）。** Open JTalk への
#    `-include oj_heap_psram.h`（T10(b)）はここで **同じ 1 回の呼び出し**に入れる。
#    別に呼ぶと後の呼び出しが前の COMPILE_OPTIONS を黙って消す。
#    `-include` は**絶対パス**で当てる（相対だと見つからなくても警告なしで素の calloc のまま
#    = 効いていないのに気づけない）。
if(SAAN_KANJI)
set(SAAN_K_WNO
    "-Wno-error;-Wno-unused-parameter;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-sign-compare;-Wno-missing-field-initializers;-Wno-implicit-fallthrough;-Wno-unused-function")
set(SAAN_OJT_OPTS "${SAAN_K_WNO}")
if(SAAN_OJ_PSRAM)
    list(APPEND SAAN_OJT_OPTS "-include" "${CMAKE_CURRENT_LIST_DIR}/oj_heap_psram.h")
endif()
set_source_files_properties(${SAAN_OJT} PROPERTIES COMPILE_OPTIONS "${SAAN_OJT_OPTS}")
set_source_files_properties(jdict.c accent.c njd_rules.c label_ids.c
    PROPERTIES COMPILE_OPTIONS "${SAAN_K_WNO}")
endif()
