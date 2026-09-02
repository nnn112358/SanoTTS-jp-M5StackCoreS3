# ファーム本体。main.c / saan_model.c / saan_console.c は sanoTTS-jp/esp32/main から
# コピーしたもので、音声出力だけ M5.Speaker 版（saan_speaker.cpp）に差し替えてある。
# 本家の saan_i2s.c / saan_i2s.h（driver/i2s_std 直叩き）は**持ってこない**。
#
# esp_partition は辞書（dict パーティション）の mmap に要る。重みはヘッダ埋め込み (.rodata)。
#    パーティション mmap の経路は持っていない（CoreS3 では PSRAM と競合して
#    ESP_ERR_NO_MEM になるため。partitions.csv 参照）。
# ⚠️ **`driver`（傘）は要らない。** コンソールが使うのは usb_serial_jtag と uart だけ。
#    傘を書くと全 esp_driver_* が IRAM を食う。
# 端末内漢字 G2P（-DSAAN_KANJI=0 で外す。値は components/saanotts_core が決めて CACHE に置く）
set(SAAN_MAIN_K "")
if(SAAN_KANJI)
    set(SAAN_MAIN_K "saan_dict.c" "saan_kanji.c")
endif()

idf_component_register(
    SRCS "main.c"
         "saan_model.c"
         "saan_console.c"
         "saan_speaker.cpp"
         "saan_ui.cpp"
         ${SAAN_MAIN_K}
    INCLUDE_DIRS "."
    REQUIRES saanotts_core m5stack-avatar esp_timer esp_driver_usb_serial_jtag esp_driver_uart esp_mm esp_partition
)

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
#    include するので CHARSET_UTF_8 も要る。
if(SAAN_KANJI)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_KANJI=1 CHARSET_UTF_8)
endif()
