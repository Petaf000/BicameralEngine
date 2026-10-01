// reaction_table.h — 反応表の定義(人が書く値)と、GPU と CPU リファレンスが読む整数の表へのベイク(02 §1〜2。T-0014)。
//
// データの流れ:
//   ReactionTableDefinition(元素・物質・規則。SI の値。今は C++ に手で書く: sim/reaction_test_table、M2 から Luau)
//   → BakeReactionTable(整数の数学ライブラリだけで計算する。R7)→ BakedReactionTable(RxSpecies・RxRule・索引・速度の表)
//   → ReactionTableView(CPU の RxEvaluateCell 用)/ そのままバッファに載せて GPU へ(shaders/sim/reaction_cells.hlsl)
//
// ベイクの検査(02 §2): 元素の釣り合いが崩れた規則・知らない元素と物質・係数や次数の誤り・名前の重なりはエラーにする。
// 反応熱は生成エンタルピーの差から自動で決まる(手で書かない)ので、エネルギーの釣り合いは構造的に守られる。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/reaction.hlsli"

namespace bicameral::sim {

    // --- 定義(人が書く値)---

    struct ElementDefinition {
        std::string name;
        uint32_t atomicMass = 0;  // mg/mol(C = 12,011)
    };

    struct ElementCount {
        std::string element;
        uint32_t count = 0;
    };

    struct SpeciesDefinition {
        std::string name;
        std::vector<ElementCount> composition;  // 物質 1 mol あたりの元素の数
        int64_t formationEnthalpy = 0;          // J/mol(298.15 K)
        uint32_t heatCapacity = 0;              // mJ/(mol·K)(一定と置く)
        uint32_t thermalConductivity = 0;       // mW/(m·K)(一定と置く。T-0089)
    };

    struct RuleTerm {
        std::string species;
        uint32_t coefficient = 1;
        bool firstOrder = false;  // 反応物だけ: 速度式で次数 1(false なら次数 0)
    };

    // Arrhenius: k = A · exp(−Ea / RT)。A は SI(次数 1 の反応物が n 個なら (m³/mol)^(n−1) / s)で、仮数 × 10^指数 で書く
    struct ArrheniusRate {
        uint64_t preExponentialMantissa = 0;
        int32_t preExponentialExponent10 = 0;
        int64_t activationEnergy = 0;  // J/mol
    };

    struct RuleDefinition {
        std::string name;
        std::vector<RuleTerm> reactants;
        std::vector<RuleTerm> products;
        ArrheniusRate rate;
    };

    struct ReactionTableDefinition {
        std::vector<ElementDefinition> elements;
        std::vector<SpeciesDefinition> species;
        std::vector<RuleDefinition> rules;
    };

    // --- ベイクした表 ---

    static_assert(sizeof(reaction::RxSpecies) == 24);
    static_assert(sizeof(reaction::RxRule) == 80);
    static_assert(sizeof(reaction::RxCell) == 112);

    // CPU の RxEvaluateCell に渡す表の読み方(reaction.hlsli の Table の約束)
    struct ReactionTableView {
        std::span<const reaction::RxSpecies> species;
        std::span<const reaction::RxRule> rules;
        std::span<const uint32_t> ruleIndex;
        std::span<const uint64_t> rates;

        [[nodiscard]] reaction::RxSpecies Species(uint32_t id) const { return species[id]; }
        [[nodiscard]] reaction::RxRule Rule(uint32_t id) const { return rules[id]; }
        [[nodiscard]] uint32_t RuleIndex(uint32_t position) const { return ruleIndex[position]; }
        [[nodiscard]] uint64_t Rate(uint32_t rule, uint32_t kelvin) const {
            return rates[(size_t{rule} * reaction::RX_RATE_TABLE_KELVINS) + kelvin];
        }
    };

    struct BakedReactionTable {
        // --- GPU に載せる(添字 = ID。物質の 0 は使わない)---
        std::vector<reaction::RxSpecies> species;
        std::vector<reaction::RxRule> rules;
        std::vector<uint32_t> ruleIndex;  // 物質ごとの規則の一覧をつないだもの(RxSpecies::ruleBegin から)
        std::vector<uint64_t> rates;      // 規則 × RX_RATE_TABLE_KELVINS(仮数 << 32 | 指数)

        // --- CPU だけ(検査と表示)---
        std::vector<std::string> elementNames;
        std::vector<std::string> speciesNames;  // 添字 = 物質 ID
        std::vector<std::string> ruleNames;     // 添字 = 規則 ID
        std::vector<uint32_t> molarMasses;      // mg/mol。添字 = 物質 ID
        std::vector<uint32_t> speciesElements;  // 物質 ID × 元素の数 + 元素

        [[nodiscard]] ReactionTableView View() const {
            return {.species = species, .rules = rules, .ruleIndex = ruleIndex, .rates = rates};
        }

        // 名前から物質 ID(無ければ 0)
        [[nodiscard]] uint32_t SpeciesId(std::string_view name) const;
    };

    [[nodiscard]] std::expected<BakedReactionTable, std::string> BakeReactionTable(
        const ReactionTableDefinition& definition);

    // --- セルを作る・調べる(テストと、T-0089 の初期状態)---

    struct SpeciesAmount {
        uint32_t species = 0;
        uint64_t amount = 0;  // µmol
    };

    // 成分と温度(mK)からセルを作る。エネルギー = 化学 + 熱容量 × 温度 を mJ に切り上げる(熱が負にならないように)
    [[nodiscard]] reaction::RxCell MakeReactionCell(const BakedReactionTable& table,
                                                    std::span<const SpeciesAmount> amounts,
                                                    int32_t temperatureMilliKelvin);

    // 元素ごとの数(µmol ぶんの原子の数。添字 = 元素)
    [[nodiscard]] std::vector<uint64_t> CountElements(const BakedReactionTable& table, const reaction::RxCell& cell);

    // セルの中身のハッシュ(エネルギーと成分。比べるため)
    [[nodiscard]] uint64_t HashReactionCell(const reaction::RxCell& cell);

    // CPU リファレンス: 1 セルの 1 刻み(GPU と同じ関数)
    [[nodiscard]] reaction::RxCell EvaluateReactionCell(const BakedReactionTable& table, const reaction::RxCell& cell,
                                                        uint64_t worldSeed, uint64_t tick, uint64_t cellId);

    // 同じく、熱と「進める規則があったか」も返す(眠れるかの判定。T-0089)
    [[nodiscard]] reaction::RxCellStep StepReactionCell(const BakedReactionTable& table, const reaction::RxCell& cell,
                                                        uint64_t worldSeed, uint64_t tick, uint64_t cellId);

}  // namespace bicameral::sim
