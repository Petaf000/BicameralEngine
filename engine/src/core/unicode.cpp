// unicode.cpp — unicode.h の実装。Win32 の WideCharToMultiByte / MultiByteToWideChar を使う。
//
// core はプリコンパイルヘッダを持たない小さな静的ライブラリなので、windows.h はここで直接 include する。
#include "core/unicode.h"

#include <windows.h>

namespace bicameral {

    std::string ToUtf8(std::wstring_view text) {
        if (text.empty())
            return {};

        const int length = static_cast<int>(text.size());
        const int sizeBytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(sizeBytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), sizeBytes, nullptr, nullptr);

        return result;
    }

    std::wstring ToWide(std::string_view text) {
        if (text.empty())
            return {};

        const int length = static_cast<int>(text.size());
        const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
        std::wstring result(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), length, result.data(), count);

        return result;
    }

}  // namespace bicameral
