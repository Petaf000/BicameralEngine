// reaction_slope.cpp — 反応の坂(ΔG°)のベイクの検査(reaction_slope.h。T-0492・D-487 ①②)。
// 整数の数学ライブラリ(shaders/common/fixed.hlsli)だけで数える。どの機械でベイクしても同じ判定・同じ警告になる(04 R7)。
// 試験 slope_check(tests/slope_check_test.cpp。倍精度)の静的な一覧と、坂を上る温度の範囲がビット単位で同じことを確かめている。
#include "sim/reaction_slope.h"

#include <algorithm>
#include <format>
#include <span>

#include "sim/reaction_table.h"

namespace bicameral::sim {

    using namespace bicameral::fx;
    using namespace bicameral::reaction;

    namespace {

        // 標準の温度 298.15 K(1/100 K。ln の基準)
        constexpr uint64_t STANDARD_CENTIKELVIN = 29815;
        constexpr uint64_t CENTIKELVIN_PER_KELVIN = 100;
        constexpr int64_t MILLIJOULES_PER_JOULE = 1000;
        constexpr int64_t NANOJOULES_PER_MILLIJOULE = 1000000;
        constexpr uint32_t Q32_SHIFT = 32;
        constexpr uint32_t GAS_CONSTANT_MILLIJOULES_PER_MOLE_KELVIN = 8314;  // R(ベイクが c_v = c_p − R に使う値と同じ)

        // 値の上限。|G| ≤ 2^49 mJ/mol・正味の係数 ≤ 2^8・項 ≤ 6 で、和が 64bit に収まる(現実の値は 4 桁以上小さい)
        constexpr int64_t ENTHALPY_LIMIT_JOULES = int64_t{1} << 36;
        constexpr int64_t ENTROPY_LIMIT_MILLIJOULES = int64_t{1} << 36;
        constexpr int64_t COEFFICIENT_LIMIT = int64_t{1} << 8;

        // 「釣り合いに近い」の幅: |ΔG°| < RT ln 10^6(成分の比が 10^6 倍で向きが変わる。T-0491 の 13.8 RT)
        constexpr uint64_t NEAR_BALANCE_RATIO = 1000000;

        // --- 規則の正味の係数(生成物 − 反応物。触媒は両辺で消える)---

        using NetTerm = RuleNetTerm;

        std::vector<NetTerm> NetTerms(const RxRule& rule) {
            std::vector<NetTerm> terms;
            const auto add = [&](uint32_t species, int64_t coefficient) {
                const auto found = std::ranges::find(terms, species, &NetTerm::species);
                if (found != terms.end())
                    found->coefficient += coefficient;
                else
                    terms.push_back({.species = species, .coefficient = coefficient});
            };

            for (uint32_t i = 0; i < rule.reactantCount; ++i)
                add(rule.reactants[i], -int64_t{rule.reactantCoefficients[i]});

            for (uint32_t i = 0; i < rule.productCount; ++i)
                add(rule.products[i], int64_t{rule.productCoefficients[i]});

            std::erase_if(terms, [](const NetTerm& term) { return term.coefficient == 0; });

            return terms;
        }

        // --- 物質の G(T) ---

        // ln(T / 298.15)(Q32)。添字 = K(0 K は使わない)
        std::vector<int64_t> LogTemperatureRatios() {
            std::vector<int64_t> ratios(RX_RATE_TABLE_KELVINS, 0);
            const int64_t standard = FxLnU64(STANDARD_CENTIKELVIN);
            for (uint32_t kelvin = 1; kelvin < RX_RATE_TABLE_KELVINS; ++kelvin)
                ratios[kelvin] = FxLnU64(uint64_t{kelvin} * CENTIKELVIN_PER_KELVIN) - standard;

            return ratios;
        }

        // 物質 1 mol の G(T) = H − T S = h0 + Cp T − T S° − Cp T ln(T / 298.15)(mJ/mol)。Cp は定圧の比熱(SpeciesPressureHeatCapacity)
        int64_t SpeciesGibbs(const BakedReactionTable& baked, uint32_t id, int64_t entropy, uint32_t kelvin,
                             int64_t logRatio) {
            const int64_t heat = int64_t{SpeciesPressureHeatCapacity(baked, id)} * kelvin;

            return (baked.species[id].h0 * MILLIJOULES_PER_JOULE) + heat - (entropy * kelvin) -
                   FxMulShiftS64(heat, logRatio, Q32_SHIFT);
        }

        // 調べられるか(正味の物質のエントロピーがそろい、値が上限の中)。調べられなければ理由を slope に書く
        bool Evaluable(const BakedReactionTable& baked, std::span<const NetTerm> terms, RuleSlope& slope) {
            for (const NetTerm& term : terms) {
                const bool known = term.species < baked.standardEntropies.size() &&
                                   baked.standardEntropies[term.species].has_value();
                if (!known) {
                    slope.missingEntropy.push_back(baked.speciesNames[term.species]);
                    continue;
                }

                const int64_t enthalpy = baked.species[term.species].h0;
                const int64_t entropy = *baked.standardEntropies[term.species];
                const bool enthalpyFits = enthalpy > -ENTHALPY_LIMIT_JOULES && enthalpy < ENTHALPY_LIMIT_JOULES;
                const bool coefficientFits = term.coefficient > -COEFFICIENT_LIMIT &&
                                             term.coefficient < COEFFICIENT_LIMIT;
                if (!enthalpyFits || !coefficientFits || entropy >= ENTROPY_LIMIT_MILLIJOULES)
                    slope.tooLarge = true;
            }

            return slope.missingEntropy.empty() && !slope.tooLarge;
        }

        // 規則の ΔG°(T)(mJ/mol)。terms は Evaluable を通ったもの
        int64_t RuleGibbs(const BakedReactionTable& baked, std::span<const NetTerm> terms, uint32_t kelvin,
                          int64_t logRatio) {
            int64_t sum = 0;
            for (const NetTerm& term : terms) {
                const int64_t entropy = *baked.standardEntropies[term.species];
                sum += term.coefficient * SpeciesGibbs(baked, term.species, entropy, kelvin, logRatio);
            }

            return sum;
        }

        void Extend(KelvinRange& range, uint32_t kelvin) {
            range.lowKelvin = range.count == 0 ? kelvin : range.lowKelvin;
            range.highKelvin = kelvin;
            range.count += 1;
        }

        // --- 規則 1 本 ---

        struct SlopeContext {
            std::vector<int64_t> logRatios;  // ln(T / 298.15)(Q32)
            int64_t nearBalanceLog = 0;      // ln 10^6(Q32)
        };

        RuleSlope CheckRule(const BakedReactionTable& baked, uint32_t ruleId, const SlopeContext& context) {
            RuleSlope slope;
            const std::vector<NetTerm> terms = NetTerms(baked.rules[ruleId]);
            if (!Evaluable(baked, terms, slope))
                return slope;

            const auto gibbs = [&](uint32_t kelvin) {
                return RuleGibbs(baked, terms, kelvin, context.logRatios[kelvin]);
            };

            constexpr uint32_t ROOM_KELVIN = 300;
            constexpr uint32_t HOT_KELVIN = 1000;
            slope.state = RuleSlopeState::Downhill;
            slope.gibbs300MilliJoules = gibbs(ROOM_KELVIN);
            slope.gibbs1000MilliJoules = gibbs(HOT_KELVIN);

            bool previousUphill = gibbs(1) > 0;
            for (uint32_t kelvin = 1; kelvin < RX_RATE_TABLE_KELVINS; ++kelvin) {
                const int64_t value = gibbs(kelvin);
                const bool uphill = value > 0;
                if (slope.balanceKelvin == 0 && uphill != previousUphill)
                    slope.balanceKelvin = kelvin;

                previousUphill = uphill;

                // 速さが 0 の温度では規則は進まない(坂の向きは関係ない)
                if (RxRateMantissa(baked.rates[(size_t{ruleId} * RX_RATE_TABLE_KELVINS) + kelvin]) == 0)
                    continue;

                if (uphill) {
                    Extend(slope.uphill, kelvin);
                    continue;
                }

                const auto gasEnergy = static_cast<int64_t>(GAS_CONSTANT_NANOJOULES_PER_MOLE_KELVIN * kelvin);
                const int64_t threshold = FxMulShiftS64(context.nearBalanceLog, gasEnergy, Q32_SHIFT) /
                                          NANOJOULES_PER_MILLIJOULE;
                if (-value < threshold)
                    Extend(slope.nearBalance, kelvin);
            }

            if (slope.uphill.count != 0)
                slope.state = RuleSlopeState::Uphill;

            return slope;
        }

        // 正味の係数がちょうど符号違いか(逆向きの対。触媒の有無・次数・速さは問わない。正味が空の規則は対にしない)
        bool IsReverse(std::span<const NetTerm> forward, std::span<const NetTerm> backward) {
            if (forward.empty() || forward.size() != backward.size())
                return false;

            return std::ranges::all_of(forward, [&](const NetTerm& term) {
                return std::ranges::any_of(backward, [&](const NetTerm& other) {
                    return other.species == term.species && other.coefficient == -term.coefficient;
                });
            });
        }

        // 規則ごとに逆向きの規則を探す(最初に見つかった 1 本。T-0611)
        void FindReverseRules(const BakedReactionTable& baked, std::vector<RuleSlope>& slopes) {
            std::vector<std::vector<NetTerm>> terms;
            terms.reserve(baked.rules.size());
            for (const RxRule& rule : baked.rules)
                terms.push_back(NetTerms(rule));

            for (uint32_t ruleId = 0; ruleId < slopes.size(); ++ruleId) {
                for (uint32_t other = 0; other < slopes.size() && slopes[ruleId].reverseRule == NO_REVERSE_RULE;
                     ++other) {
                    if (other != ruleId && IsReverse(terms[ruleId], terms[other]))
                        slopes[ruleId].reverseRule = other;
                }
            }
        }

        std::string Join(const std::vector<std::string>& names) {
            std::string text;
            for (const std::string& name : names)
                text += (text.empty() ? "" : "・") + name;

            return text;
        }

    }  // namespace

    std::vector<RuleNetTerm> RuleNetTerms(const RxRule& rule) {
        return NetTerms(rule);
    }

    uint32_t SpeciesPressureHeatCapacity(const BakedReactionTable& baked, uint32_t id) {
        const bool condensed = id < baked.speciesCondensed.size() && baked.speciesCondensed[id] != 0;
        const uint32_t gasConstant = condensed ? 0 : GAS_CONSTANT_MILLIJOULES_PER_MOLE_KELVIN;

        return baked.species[id].heatCapacity + gasConstant;
    }

    std::vector<int64_t> RuleStandardGibbs(const BakedReactionTable& baked, uint32_t ruleId) {
        RuleSlope slope;
        const std::vector<NetTerm> terms = NetTerms(baked.rules[ruleId]);
        if (!Evaluable(baked, terms, slope))
            return {};

        const std::vector<int64_t> logRatios = LogTemperatureRatios();
        std::vector<int64_t> gibbs(RX_RATE_TABLE_KELVINS, 0);
        for (uint32_t kelvin = 1; kelvin < RX_RATE_TABLE_KELVINS; ++kelvin)
            gibbs[kelvin] = RuleGibbs(baked, terms, kelvin, logRatios[kelvin]);

        return gibbs;
    }

    std::vector<RuleSlope> CheckReactionSlopes(const BakedReactionTable& baked) {
        const SlopeContext context{.logRatios = LogTemperatureRatios(), .nearBalanceLog = FxLnU64(NEAR_BALANCE_RATIO)};
        std::vector<RuleSlope> slopes;
        slopes.reserve(baked.rules.size());
        for (uint32_t ruleId = 0; ruleId < baked.rules.size(); ++ruleId)
            slopes.push_back(CheckRule(baked, ruleId, context));

        FindReverseRules(baked, slopes);

        return slopes;
    }

    std::vector<std::string> SlopeWarnings(const BakedReactionTable& baked, const std::vector<RuleSlope>& slopes) {
        std::vector<std::string> warnings;
        std::vector<std::string> unevaluated;
        std::vector<std::string> missing;
        for (uint32_t ruleId = 0; ruleId < slopes.size(); ++ruleId) {
            const RuleSlope& slope = slopes[ruleId];
            const std::string& name = baked.ruleNames[ruleId];
            // 逆向きの対は向きを速さの比で決める(一方向ではない)ので警告しない
            if (slope.state == RuleSlopeState::Uphill && slope.reverseRule == NO_REVERSE_RULE) {
                const std::string balance = slope.balanceKelvin == 0
                                                ? ""
                                                : std::format("。釣り合いは {} K", slope.balanceKelvin);
                warnings.push_back(std::format(
                    "{}規則 {} は {}〜{} K で坂を上る向きに書かれている(標準の ΔG° > 0{})。一方向の規則なので、"
                    "この温度では周りの熱などを坂の上へ運び続けうる(永久機関になりうる)。表はこのまま当てる。"
                    "直すなら逆向きの規則を足すか、この温度の速さを小さくする",
                    SLOPE_WARNING_PREFIX, name, slope.uphill.lowKelvin, slope.uphill.highKelvin, balance));
            }

            if (slope.state != RuleSlopeState::Unevaluated)
                continue;

            unevaluated.push_back(name);
            for (const std::string& species : slope.missingEntropy) {
                if (std::ranges::find(missing, species) == missing.end())
                    missing.push_back(species);
            }
        }

        if (!unevaluated.empty()) {
            const std::string reason = missing.empty()
                                           ? "値が大きすぎる"
                                           : std::format(
                                                 "標準エントロピー standard_entropy_mj_per_mol_k の無い物質: {}",
                                                 Join(missing));
            warnings.push_back(std::format("{}坂を調べられない規則 {} 本: {}({})", SLOPE_WARNING_PREFIX,
                                           unevaluated.size(), Join(unevaluated), reason));
        }

        return warnings;
    }

}  // namespace bicameral::sim
