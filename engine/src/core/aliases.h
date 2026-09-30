// aliases.h — 標準ライブラリの入れ子の名前空間を短く書くための別名(docs/style.md「短い名前」)。
//
// namespace bicameral の中だけに入れる。std 直下の型と関数(std::optional など)は短くしない(std:: を付けて書く)。
// ここに足すのは、あちこちで使うものだけ。1 つのファイルでしか使わない長い名前空間は、そのファイルの中で短くする。
// windows.h も D3D12 も読まない(bicameral_view のように GPU を知らない所からも使う)。ComPtr は gpu/com_ptr.h。
#pragma once

#include <chrono>
#include <filesystem>
#include <ranges>

namespace bicameral {

    namespace fs = std::filesystem;
    namespace rng = std::ranges;
    namespace views = std::views;
    namespace chr = std::chrono;

}  // namespace bicameral
