// 検査のテスト用(コンパイルしない)。型の名前を書かずに double のリテラルを使う。ソースの検査が拒否しなければならない。
int Half(int value) {
    return static_cast<int>(value * 0.5);
}
