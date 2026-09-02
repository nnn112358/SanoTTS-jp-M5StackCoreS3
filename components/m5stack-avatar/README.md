# m5stack-avatar（vendored）

- 出所: <https://github.com/stack-chan/m5stack-avatar> commit `7a90083`（v0.10.0、2024-09-13）
- ライセンス: MIT（Copyright (c) 2018 Shinya Ishikawa。[`LICENSE.txt`](LICENSE.txt)）
- ESP-IDF Component Registry には無いので `src/` をコピーしている

## 本家との差分（ESP-IDF / Arduino 無しで通すため）

`src/Avatar.cpp` の先頭に、`ARDUINO` が定義されていないときだけ効く 3 行を足した:

| 本家が使うもの | ESP-IDF では | 対処 |
|---|---|---|
| `xTaskCreateUniversal` | arduino-esp32 の関数で無い | `xTaskCreatePinnedToCore` に `#define`（引数は同じ） |
| `random(long)` | newlib の `random()` は引数無し | `namespace m5avatar` 内に `static long random(long)` を足す（SDL 向けと同じ実装） |
| `String` | 無い | 本家が `Avatar.h` で `typedef std::string String` 済み（変更なし） |

`src/tasks/LipSync.h`（AquesTalk 用）は持ってこない。それ以外は無変更。
