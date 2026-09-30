// paths.cpp — paths.h の実装。GetModuleFileNameW を使う。
//
// core はプリコンパイルヘッダを持たない小さな静的ライブラリなので、windows.h はここで直接 include する。
#include "core/paths.h"

#include <windows.h>

#include <string>

namespace bicameral {

    fs::path ExecutableDirectory() {
        // MAX_PATH を超えるパスもあるので、足りなければ広げて取り直す
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;) {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
                return {};

            if (length < buffer.size()) {
                buffer.resize(length);
                break;
            }

            buffer.resize(buffer.size() * 2);
        }

        return fs::path(buffer).parent_path();
    }

}  // namespace bicameral
