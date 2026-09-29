// unicode.h — UTF-8(エンジンの中の文字列)と UTF-16(Windows の W 系 API)の変換。
//
// エンジンの中の文字列はすべて UTF-8 で持つ(MSVC は /utf-8 でビルドしている)。
// OutputDebugStringW・FormatMessageW・ファイルパスなど、Windows と話す境界でだけ変換する。
#pragma once

#include <string>
#include <string_view>

namespace bicameral {

    // UTF-16 → UTF-8。不正なサロゲートは U+FFFD に置き換わる
    std::string ToUtf8(std::wstring_view text);

    // UTF-8 → UTF-16。不正なバイト列は U+FFFD に置き換わる
    std::wstring ToWide(std::string_view text);

}  // namespace bicameral
