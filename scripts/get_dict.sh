#!/usr/bin/env bash
# 端末内漢字 G2P 用の辞書 blob を sanoTTS-jp の Release から取る（git には入れていない）。
#   scripts/get_dict.sh              → model/k1-dict-438750.bin（13.7 MB。CoreS3 の既定）
#   scripts/get_dict.sh 44000        → model/k1-dict-44000-2mb.bin（0.98 MB。Core2 の既定）
#   scripts/get_dict.sh 135000       → model/k1-dict-135000-4mb.bin（3.0 MB。ATOMS3 / ATOMS3R の既定）
#   scripts/get_dict.sh 228000       → model/k1-dict-228000-8mb.bin（7.1 MB。CoreS3 のみ入る）
#   scripts/get_dict.sh all          → 4 つとも
# ビルドでは -DSAAN_DICT=44000|135000|228000|438750 で選ぶ（CMakeLists.txt）。
set -euo pipefail
cd "$(dirname "$0")/.."

get() {  # <tag> <file> <sha256>
    gh release download "$1" --repo ayutaz/sanoTTS-jp --pattern "$2" --dir model --clobber
    echo "$3  model/$2" | sha256sum -c -
}
one() {
    case "$1" in
      438750) get v0.3.0 k1-dict-438750.bin f162c922074d76817298b34d8a8fd35f7d195f38540303485a76c956b5d84877 ;;
      44000)  get v0.3.1-rc1-smallflash k1-dict-44000-2mb.bin  cd1ed65241600b29ced9fde627b1543d93f5f5c0cd5297a4dba42d3f01d8806a ;;
      135000) get v0.3.1-rc1-smallflash k1-dict-135000-4mb.bin 5010ed9fd100914333cb1f3b973f0fb2a5c746f5c14841f42beb08f3fb0dbc2c ;;
      228000) get v0.3.1-rc1-smallflash k1-dict-228000-8mb.bin 5776982417fb8435cd9509ba2ffa6fe62d62a0e47e1aaa3eb838ced6ed1fe399 ;;
      *) echo "不明: $1（438750 | 44000 | 135000 | 228000 | all）" >&2; exit 1 ;;
    esac
}
case "${1:-438750}" in
  all) for n in 438750 44000 135000 228000; do one "$n"; done ;;
  *)   one "${1:-438750}" ;;
esac
