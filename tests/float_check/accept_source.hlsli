// 検査のテスト用(コンパイルしない)。コメントや文字列の中の float・0.5、16 進の 0x1e5、asuint は違反ではない。
// ソースの検査が通さなければならない(誤検出のテスト)。
static const uint32_t HEX_WITH_E = 0x1e5u;
static const uint32_t SEPARATED = 1'000'000;
uint32_t Bits(uint32_t value) {
    const char* note = "float 0.5";
    return value + HEX_WITH_E + SEPARATED + (note != 0 ? 1u : 0u); /* float */
}
