#!/usr/bin/env bash
# 互換用。./idf_board.sh atoms3 と同じ
exec "$(dirname "$0")/idf_board.sh" atoms3 "$@"
