// 検査のテスト用(コンパイルしない)。整数を渡しても std::sqrt は double を返す。ソースの検査が拒否しなければならない。
#include <cmath>

int Root(int value) {
    return static_cast<int>(std::sqrt(value));
}
