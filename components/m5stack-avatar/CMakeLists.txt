# m5stack-avatar（stack-chan/m5stack-avatar, MIT, Copyright (c) 2018 Shinya Ishikawa）
#
# 出所: https://github.com/stack-chan/m5stack-avatar  commit 7a90083 (v0.10.0, 2024-09-13)
#       src/ をそのままコピー。README.md に ESP-IDF 向けの差分を書いてある。
#
# ⚠️ **src/tasks/LipSync.h は持ってこない。** AquesTalk 前提で、Arduino.h も要る。
#    リップシンクは main/saan_ui.cpp が saan_speaker の包絡から自前でやる。
# ⚠️ **src/faces/ はヘッダだけ。** eye_small.h が <pgmspace.h>（Arduino）を include するが、
#    どこからも include されないのでコンパイルされない。
file(GLOB AVATAR_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")

idf_component_register(
    SRCS ${AVATAR_SRCS}
    INCLUDE_DIRS "src"
)

# ⚠️ 本家のコードは -Wreorder / -Wswitch の警告を出し、ESP-IDF の既定 -Werror=all で
#    止まる（Arduino では警告のまま通る）。第三者のコードなので**このコンポーネントだけ**
#    エラーにしない。本家の src/ は無変更のまま置くため、ここで吸収する。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=all -Wno-error -Wno-reorder -Wno-switch -Wno-unused-variable
    -Wno-unused-but-set-variable -Wno-missing-field-initializers)
