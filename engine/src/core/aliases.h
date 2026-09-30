// aliases.h — 標準ライブラリの長い名前を短く書くための別名(docs/style.md「短い名前」)。
//
// 名前空間は別名(fs:: など)、std 直下の型と関数は using 宣言(optional・span など)で、namespace bicameral の中だけに入れる。
// ここに足すのは、あちこちで使うものだけ。1 つのファイルでしか使わない長い名前空間は、そのファイルの中で短くする。
// windows.h も D3D12 も読まない(bicameral_view のように GPU を知らない所からも使う)。ComPtr は gpu/com_ptr.h。
#pragma once

#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>

namespace bicameral {

    // --- 名前空間の別名 ---
    namespace fs = std::filesystem;
    namespace rng = std::ranges;
    namespace views = std::views;
    namespace chr = std::chrono;

    // --- std 直下の型と関数 ---
    using std::expected;
    using std::format;
    using std::nullopt;
    using std::optional;
    using std::span;
    using std::string_view;
    using std::unexpected;

}  // namespace bicameral
