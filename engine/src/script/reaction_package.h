// reaction_package.h — Luau のパッケージで書いた反応表を、ベイクに渡す定義(sim::ReactionTableDefinition)に読む
// (T-0021、02 §1〜2・13 §2・ADR-0031・ADR-0032)。
//
// データの流れ: パッケージのフォルダ → LoadPackages(合わせた「分類 → 鍵 → 値」)→ ReadReactionTableDefinition
//   → sim::BakeReactionTable(整数の表・元素とエネルギーの検査)→ GPU と CPU リファレンス。
// 分類と書き方(1 つの鍵 = 1 つの元素・物質・規則。Mod は鍵を足して増やす。値は整数で、単位は欄の名前に書く):
//   elements  = { C = { atomic_mass_mg_per_mol = 12011 } }
//   species   = { oxygen = { composition = { O = 2 }, formation_enthalpy_j_per_mol = 0,
//                            heat_capacity_mj_per_mol_k = 29378, thermal_conductivity_mw_per_m_k = 135000 } }
//   reactions = { carbon_combustion = {
//                     reactants = { carbon = 1, oxygen = 1 }, products = { carbon_dioxide = 1 },
//                     rate = { a = "3e8", activation_energy_j_per_mol = 160000 },
//                     orders = { carbon = 1, oxygen = 1 },        -- 省けば反応物は全部次数 1。書いたら、書かない反応物は次数 0
//                     reaction_enthalpy_j_per_mol = -393500 } }   -- 省ける。文献の反応熱(食い違えばベイクが警告)
// 数は整数だけ。Luau の数は倍精度なので、小数・±2^53 を超える値は誤り(丸めで機械ごとに値が変わる余地を作らない)。
// A(頻度因子)は桁が大きいので 10 進の文字列("2.8e19")でも書ける(誤差なしで 仮数 × 10^指数 に読む)。
// 知らない欄は誤り(綴りの誤りを黙って通さない)。この 3 つ以外の分類(UI の文字列など)は読まない。
// 欄を変えたら、型の定義 data/types/bicameral.d.luau も変える(読む前の型検査が同じ形を見る。T-0140・ADR-0046)。
// ここは CPU だけ(原則 2: オーサリングは CPU、ランタイムへはベイクした表で渡す)。
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "script/luau_package.h"
#include "sim/reaction_table.h"

namespace bicameral::script {

    // 反応表の分類の名前(パッケージの entry が返す表の鍵)
    constexpr std::string_view REACTION_ELEMENTS_CATEGORY = "elements";
    constexpr std::string_view REACTION_SPECIES_CATEGORY = "species";
    constexpr std::string_view REACTION_RULES_CATEGORY = "reactions";

    // 10 進の数(仮数 × 10^指数)。仮数の末尾の 0 は指数へ移してある("280e17" → 28, 18)
    struct DecimalNumber {
        uint64_t mantissa = 0;
        int32_t exponent10 = 0;
    };

    // "2.8e19"・"15e8"・"0.5"・"3" を誤差なしで読む(符号なし。有効数字 19 桁まで・指数 ±999 まで)
    [[nodiscard]] std::expected<DecimalNumber, std::string> ParseDecimal(std::string_view text);

    // 合わせた表から反応表の定義を作る。誤りには「どのパッケージの・どの欄か」を付ける。
    // 元素の釣り合いなどの中身の検査はベイク(sim::BakeReactionTable)が行う
    [[nodiscard]] std::expected<sim::ReactionTableDefinition, std::string> ReadReactionTableDefinition(
        const PackageSetResult& packages);

}  // namespace bicameral::script
