#!/usr/bin/env bash
# ボード × 辞書の一括イメージ（0x0 に焼く 1 本）をまとめて作る。
#   scripts/make_images.sh                       # 全ボード × 入る辞書すべて → firmware/<日付>_images/
#   scripts/make_images.sh atoms3                # 1 ボードだけ
#   scripts/make_images.sh atoms3 44000          # 1 ボード × 1 辞書
#   OUT=firmware/foo scripts/make_images.sh      # 出力先を変える
#
# app はボードごとに 1 回ビルドし（./idf_board.sh <ボード> build）、辞書だけ差し替えて esptool merge_bin で結合する
# （app は辞書に依存しない。dict パーティションに焼く blob が違うだけ）。
# 辞書がそのボードの dict パーティションに入らない組み合わせは飛ばす（表: CMakeLists.txt）。
# ⚠️ model/k1-dict-*.bin が要る（scripts/get_dict.sh all）。
set -euo pipefail
cd "$(dirname "$0")/.."

boards="${1:-cores3 atoms3 atoms3r core2 stampc5}"
dicts="${2:-44000 135000 228000 438750}"
OUT="${OUT:-firmware/$(date +%F)_images}"

dict_file() { case "$1" in
  44000)  echo k1-dict-44000-2mb.bin ;;  135000) echo k1-dict-135000-4mb.bin ;;
  228000) echo k1-dict-228000-8mb.bin ;; 438750) echo k1-dict-438750.bin ;;
  *) echo "不明な辞書: $1" >&2; exit 1 ;; esac; }
# ボードごとの dict パーティション容量（partitions*.csv の dict 行）と flash サイズ、イメージ名の接頭辞
board_info() { case "$1" in
  cores3)  echo "partitions.csv 16MB m5-cores3-avatar" ;;
  core2)   echo "partitions_core2.csv 16MB m5-core2-avatar" ;;
  atoms3)  echo "partitions_atoms3.csv 8MB m5-atoms3-voicebase" ;;
  atoms3r) echo "partitions_atoms3.csv 8MB m5-atoms3r-voicebase" ;;
  stampc5) echo "partitions_stampc5.csv 4MB m5-stampc5-i2sdac" ;;
  *) echo "不明なボード: $1" >&2; exit 1 ;; esac; }

# esptool は ESP-IDF の環境から（idf.sh と同じ手順で有効化）
unset IDF_PATH IDF_PYTHON_ENV_PATH ESP_IDF_VERSION IDF_TOOLS_PATH OPENOCD_SCRIPTS ESP_ROM_ELF_DIR
. "$HOME/esp/esp-idf/export.sh" > /dev/null 2>&1 || { echo "export.sh 失敗" >&2; exit 1; }

mkdir -p "$OUT"
for b in $boards; do
    read -r csv flash prefix <<< "$(board_info "$b")"
    cap=$(grep -E '^dict' "$csv" | awk -F, '{gsub(/ /,"",$5); print $5}'); cap=$((cap))
    off=$(grep -E '^dict' "$csv" | awk -F, '{gsub(/ /,"",$4); print $4}')
    bdir="build"; [ "$b" != cores3 ] && bdir="build_$b"
    chip=esp32s3; [ "$b" = core2 ] && chip=esp32; [ "$b" = stampc5 ] && chip=esp32c5
    echo "=== $b: ビルド（$bdir）"
    ./idf_board.sh "$b" build > "$OUT/build_$b.log" 2>&1 || { tail -30 "$OUT/build_$b.log"; exit 1; }
    mkdir -p "$OUT/$b"
    cp "$bdir/bootloader/bootloader.bin" "$bdir/partition_table/partition-table.bin" "$bdir/saanotts_cores3.bin" "$OUT/$b/"
    for d in $dicts; do
        f="model/$(dict_file "$d")"
        [ -f "$f" ] || { echo "  $d: $f が無い（scripts/get_dict.sh $d）"; continue; }
        sz=$(stat -c %s "$f")
        if [ "$sz" -gt "$cap" ]; then echo "  $d: $sz B > dict $cap B なので飛ばす"; continue; fi
        img="$OUT/${prefix}-kanji-dict${d}.bin"
        esptool.py --chip $chip merge_bin -o "$img" --flash_mode dio --flash_freq 80m --flash_size $flash \
            0x0 "$bdir/bootloader/bootloader.bin" 0x8000 "$bdir/partition_table/partition-table.bin" \
            0x10000 "$bdir/saanotts_cores3.bin" "$off" "$f" > /dev/null
        echo "  $d: $(basename "$img") ($(stat -c %s "$img") B)"
    done
done
(cd "$OUT" && sha256sum *.bin */*.bin > SHA256SUMS.txt)
echo "=== 出力: $OUT"; ls -la "$OUT"
