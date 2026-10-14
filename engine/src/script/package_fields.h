// package_fields.h — パッケージの合わせた表(分類 → 鍵 → 値)の欄を読む道具(T-0398 で reaction_package.cpp から出した)。
//
// 反応表(reaction_package)と、同じ読み込みに足した分類(世界の組み立て world_cells・world_bodies〔T-0398〕・
// 材料のひな形〔T-0399〕・環境の値〔T-0400〕)が同じ約束で読む:
//   - 知らない欄は誤り(綴りの誤りを黙って通さない)
//   - 数は整数だけ(Luau の数は倍精度なので、小数・±2^53 を超える値は誤り。04 R7)
//   - 誤りには「分類.鍵.欄」の道筋を付ける(ReadCategory がパッケージの名前を前に付ける)
// ここは CPU だけ(原則 2)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "script/luau_package.h"
#include "script/script_value.h"

namespace bicameral::script {

    constexpr int64_t PACKAGE_EXACT_INTEGER_LIMIT = int64_t{1} << 53;  // 倍精度で誤差なしに表せる整数の上限

    // 表の鍵が全部 allowed の中の文字列か(知らない欄は綴りの誤りとして落とす)
    [[nodiscard]] std::expected<void, std::string> CheckFields(const ScriptValue& table,
                                                               std::span<const std::string_view> allowed,
                                                               std::string_view path);

    // 欄 field があること
    [[nodiscard]] std::expected<const ScriptValue*, std::string> RequireField(const ScriptValue& table,
                                                                              std::string_view field,
                                                                              std::string_view path);

    // 表で書いてあること
    [[nodiscard]] std::expected<void, std::string> RequireTable(const ScriptValue& value, std::string_view path);

    // 整数で書いた数を読む(小数・±2^53 の外・[minimum, maximum] の外は誤り)
    [[nodiscard]] std::expected<int64_t, std::string> ReadInteger(const ScriptValue& value, std::string_view path,
                                                                  int64_t minimum, int64_t maximum);

    // 欄 field の整数(欄が無ければ誤り)
    [[nodiscard]] std::expected<int64_t, std::string> ReadIntegerField(const ScriptValue& table, std::string_view field,
                                                                       std::string_view path, int64_t minimum,
                                                                       int64_t maximum);

    // { x, y, z } の 3 つの整数(各軸 [minimum, maximum])。セルの場所・大きさ(world_cells・play_metrics。T-0559 で共通に)
    [[nodiscard]] std::expected<std::array<int64_t, 3>, std::string> ReadVector3(const ScriptValue& value,
                                                                                 std::string_view path, int64_t minimum,
                                                                                 int64_t maximum);

    // 欄 field の { x, y, z }(欄が無ければ誤り)
    [[nodiscard]] std::expected<std::array<int64_t, 3>, std::string> ReadVector3Field(const ScriptValue& table,
                                                                                      std::string_view field,
                                                                                      std::string_view path,
                                                                                      int64_t minimum, int64_t maximum);

    // 表であることと、知らない欄が無いことを確かめる
    [[nodiscard]] std::expected<void, std::string> CheckRecord(const ScriptValue& value,
                                                               std::span<const std::string_view> allowed,
                                                               std::string_view path);

    // 分類 category の鍵を全部 read(鍵, 値, 道筋)で読み、out に足す(鍵はバイト順)。分類が無ければ何もしない(空)。
    // 誤りには、その鍵を書いたパッケージの名前を付ける
    template <typename Definition, typename Reader>
    [[nodiscard]] std::expected<void, std::string> ReadCategory(const PackageSetResult& packages,
                                                                std::string_view category, Reader read,
                                                                std::vector<Definition>& out) {
        const auto found = packages.tables.find(std::string(category));
        if (found == packages.tables.end())
            return {};

        for (const auto& [key, entry] : found->second) {
            const std::string path = std::format("{}.{}", category, key);
            auto definition = read(key, entry.value, path);
            if (!definition)
                return std::unexpected(std::format("パッケージ {}: {}", entry.package, definition.error()));

            out.push_back(std::move(*definition));
        }

        return {};
    }

}  // namespace bicameral::script
