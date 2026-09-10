# ファーム本体。main.c は sanoTTS-jp/esp32/main/main.c を CoreS3 向け（顔・タッチ）に
# 改変したもの。saan_console.{c,h} / saan_dict.{c,h} / saan_kanji.{c,h} は本家 origin/main
# （d169e91、2026-09-04）の**そのままのコピー**。音声出力は M5.Speaker 版（saan_speaker.cpp）、
# 画面は m5stack-avatar 版（saan_ui.cpp）。本家の saan_i2s.c / saan_pcm.c / saan_audio.h は
# **持ってこない**（PCM 統計は saan_speaker.cpp が持つ。給餌の仕組みは本家 boards/m5unified の
# saan_audio_m5.cpp と同じ: プリロール → start → チャンクごとに playRaw）。
#
# esp_partition は辞書（dict パーティション）を**探す**のに要る。貼るのは esp_mmu_map（esp_mm）:
#    saan_dict.c は CONFIG_SPI_FLASH_ROM_IMPL=y のとき自動で esp_mmu_map に切り替える
#    （ROM 実装の spi_flash_mmap は 128 ページ = 8 MB までしか貼れない。本家 M-90）。
#    重みはヘッダ埋め込み (.rodata) なので、パーティション mmap の経路は持っていない。
# ⚠️ **`driver`（傘）は要らない。** コンソールが使うのは usb_serial_jtag と uart だけ。
#    傘を書くと全 esp_driver_* が IRAM を食う。
# 端末内漢字 G2P（-DSAAN_KANJI=0 で外す。値は components/saanotts_core が決めて CACHE に置く）
set(SAAN_MAIN_K "")
if(SAAN_KANJI)
    set(SAAN_MAIN_K "saan_dict.c" "saan_kanji.c")
endif()

# --- 画面 --------------------------------------------------------------------
#
# 顔（saan_ui_avatar.cpp）と文字（saan_ui_text.cpp）は**両方**入り、saan_ui.cpp が実行時に切り替える
# （長押し / ボタン B / シリアル `/ui`）。-DSAAN_UI=avatar|text は**起動時にどちらを出すか**だけ
# （既定 avatar。画面の無いボードは text 固定）。旧 `atoms3` は text の別名。
# ⚠️ **CMake のキャッシュに残る。** 一度 -D で渡した値は build/ を消すまで有効。
idf_build_get_property(_saan_ui_prop SAAN_UI)
if(_saan_ui_prop)
    set(SAAN_UI "${_saan_ui_prop}")
elseif(NOT DEFINED SAAN_UI)
    set(SAAN_UI "avatar")
endif()
if(SAAN_UI STREQUAL "avatar")
    set(SAAN_UI_DEFAULT_TEXT 0)
    message(STATUS "sanoTTS: 起動時の画面 = **顔**（長押し / ボタン B / `/ui` で文字へ）")
elseif(SAAN_UI STREQUAL "text" OR SAAN_UI STREQUAL "atoms3")
    set(SAAN_UI_DEFAULT_TEXT 1)
    message(STATUS "sanoTTS: 起動時の画面 = **文字**（長押し / ボタン B / `/ui` で顔へ）")
else()
    message(FATAL_ERROR "SAAN_UI=${SAAN_UI} は不明。avatar か text を渡すこと")
endif()

# --- ボードで音声出力と画面の実装を選ぶ ---------------------------------------------------
#
#   stampc5（画面もスピーカーも無い）… saan_speaker_i2s.c（driver/i2s_std 直叩き）+ saan_ui_headless.c。
#       **M5Unified / M5GFX / m5stack-avatar を参照しないのでイメージに入らない**（app 約 1 MB）。
#   それ以外 … saan_speaker.cpp（M5.Speaker）+ 顔と文字の両方（実行時に切り替え）。
idf_build_get_property(_saan_board_early SAAN_BOARD)
if(_saan_board_early STREQUAL "stampc5")
    set(SAAN_IO_SRC "saan_speaker_i2s.c" "saan_ui_headless.c")
    set(SAAN_IO_REQ esp_driver_i2s)
else()
    set(SAAN_IO_SRC "saan_speaker.cpp" "saan_ui.cpp" "saan_ui_avatar.cpp" "saan_ui_text.cpp")
    set(SAAN_IO_REQ m5stack-avatar)
endif()

idf_component_register(
    SRCS "main.c"
         "saan_model.c"
         "saan_console.c"
         ${SAAN_IO_SRC}
         ${SAAN_MAIN_K}
    INCLUDE_DIRS "."
    REQUIRES saanotts_core ${SAAN_IO_REQ} esp_timer esp_driver_usb_serial_jtag esp_driver_uart esp_mm esp_partition
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_UI_DEFAULT_TEXT=${SAAN_UI_DEFAULT_TEXT})

# --- ボード（-DSAAN_BOARD=atoms3 で saan_speaker.cpp が Atomic Voice Base を有効にする）------------
# ⚠️ トップの CMakeLists.txt がビルドプロパティに載せている（SAAN_UI と同じ理由）。
idf_build_get_property(_saan_board_prop SAAN_BOARD)
if(_saan_board_prop MATCHES "^atoms3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_BOARD_ATOMS3=1)
    message(STATUS "sanoTTS: ボード = **${_saan_board_prop} + Atomic Voice Base**（external_speaker.atomic_echo）")
elseif(_saan_board_prop STREQUAL "stampc5")
    message(STATUS "sanoTTS: ボード = **Stamp-C5**（M5Unified 無し。I2S 直叩き BCLK G5 / WS G6 / DOUT G7。-DSAAN_I2S_GPIO_* で変更）")
elseif(_saan_board_prop MATCHES "^(core2|basic)$")
    # ESP32 は静的 DRAM が小さく arena 176 KB が .bss に入らない → 起動時にヒープから取る（main.c。PSRAM 優先）
    # basic は PSRAM が無いので、内部ヒープの大きい塊から最大 4 本に分けて取る（コアの複数ブロック arena）
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_ARENA_HEAP=1)
    message(STATUS "sanoTTS: ボード = **${_saan_board_prop}**（arena はヒープ。PSRAM 優先）")
endif()

foreach(_pin BCLK WS DOUT)
    if(DEFINED SAAN_I2S_GPIO_${_pin})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_I2S_GPIO_${_pin}=${SAAN_I2S_GPIO_${_pin}})
    endif()
endforeach()

# --- 重み blob → `const` 配列のヘッダ ---------------------------------------
#
# ⚠️ **生成物はビルドディレクトリに置く。** 4 MB のテキストをソースに置かない。
# ⚠️ **blob を差し替えたら自動で作り直る**（DEPENDS に blob とスクリプト）。
#    手作業にすると、古いヘッダのまま焼いて「差し替えたのに変わらない」という
#    気づけない事故になる。
set(SAAN_BLOB_HEADER "${CMAKE_CURRENT_BINARY_DIR}/saan_model_blob.h")
set(SAAN_BLOB_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../scripts/blob_to_header.py")

# uv 経由で呼ぶ（sanoTTS-jp の規約）。--no-project なのはこのディレクトリに
# pyproject.toml が無いため。スクリプトは stdlib だけを使う。
if(NOT DEFINED SAAN_PYTHON)
    set(SAAN_PYTHON uv run --no-project python)
endif()

add_custom_command(
    OUTPUT "${SAAN_BLOB_HEADER}"
    COMMAND ${SAAN_PYTHON} "${SAAN_BLOB_SCRIPT}"
            --blob "${SAAN_MODEL_BLOB}" --out "${SAAN_BLOB_HEADER}"
    DEPENDS "${SAAN_MODEL_BLOB}" "${SAAN_BLOB_SCRIPT}"
    COMMENT "sanoTTS: ${SAAN_MODEL_BLOB} → saan_model_blob.h (.rodata)"
    VERBATIM)

add_custom_target(saan_blob_header DEPENDS "${SAAN_BLOB_HEADER}")
add_dependencies(${COMPONENT_LIB} saan_blob_header)

target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# --- W8A8 + PIE ------------------------------------------------------------
#
# ⚠️ **コア component と同じ条件で main 側にも渡す。** main.c の
#    「fp32 blob なのに W8A8 有効」ガードがこれを見る。片方だけ定義すると
#    ガードが黙って消えて、効いていないのに気づけない構成に戻る。
if(SAAN_ENABLE_PIE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_INT8_ACT=1)
endif()

# --- 起動時に 1 文喋る（**このプロジェクトでは既定で喋る**）-------------------
#
#     ./idf.sh -DSAAN_BOOT_SPEAK=0 build   で止められる
#
# 画面に文を出してタッチで再生する UI なので、起動時に 1 回喋って画面を作る。
# 本家は対話入力があるため既定で喋らないが、ここでは逆にしてある。
if(NOT DEFINED SAAN_BOOT_SPEAK)
    set(SAAN_BOOT_SPEAK 1)
endif()
if(SAAN_BOOT_SPEAK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_BOOT_SPEAK=1)
    message(STATUS "sanoTTS: 起動時に 1 文喋る（突き合わせ用）")
endif()

# --- 再生方式（既定はストリーミング = xRT 測定用）------------------------------
#
#     ./idf.sh -DSAAN_BUFFERED=1 build    1 発話ぶんを PSRAM に貯めてから鳴らす（途切れない）
#     ./idf.sh -DSAAN_BUFFERED=0 build    ストリーミング（既定。xRT と途切れ回数が出る）
#
# ⚠️ **CMake のキャッシュに残る。** 一度 -D で渡した値は build/ を消すまで有効。
if(DEFINED SAAN_BUFFERED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_BUFFERED=${SAAN_BUFFERED})
    if(SAAN_BUFFERED)
        message(STATUS "sanoTTS: 再生 = 貯めてから鳴らす（途切れない）")
    else()
        message(STATUS "sanoTTS: 再生 = ストリーミング（xRT 測定用）")
    endif()
endif()

# --- 端末内漢字 G2P（-DSAAN_KANJI=0 で外す）------------------------------------
# ⚠️ コア component と同じ条件で main 側にも渡す。saan_kanji.c が Open JTalk のヘッダを
#    include するので CHARSET_UTF_8 も要る。LABEL_IDS_EXTERNAL_SCRATCH はコア component が
#    PUBLIC で定義するのでここでは書かない（saan_kanji.h がそれを見て作業領域の寸法を決める）。
if(SAAN_KANJI)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_KANJI=1 CHARSET_UTF_8)
endif()

# --- 段別プロファイラ（-DSAAN_PROFILE=1。既定は無効）--------------------------------
# ⚠️ コア component と同じ条件で main 側にも渡す。main.c が saan_prof_now()（CCOUNT）を
#    定義し、発話のあとに表を出す。片方だけだとリンクエラー（saan_prof_now 未定義）か、
#    表が出ないまま計測コストだけ払うことになる。
if(SAAN_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_PROFILE=1)
    message(STATUS "sanoTTS: main 側も SAAN_PROFILE=1（発話後に段別の表を出す）")
endif()
