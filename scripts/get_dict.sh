#!/usr/bin/env bash
# 端末内漢字 G2P 用の辞書 blob を sanoTTS-jp の Release から取る（13.7 MB。git には入れていない）。
#   scripts/get_dict.sh            → model/k1-dict-438750.bin
set -euo pipefail
cd "$(dirname "$0")/.."
gh release download v0.2.0 --repo ayutaz/sanoTTS-jp --pattern 'k1-dict-438750.bin' --dir model --clobber
echo "f162c922074d76817298b34d8a8fd35f7d195f38540303485a76c956b5d84877  model/k1-dict-438750.bin" | sha256sum -c -
