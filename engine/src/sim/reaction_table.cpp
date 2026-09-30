// reaction_table.cpp — 反応表のベイク(定義 → 整数の表)と、セルを作る・調べる道具(02 §1〜3。T-0014)。
// ベイクは整数の数学ライブラリ(shaders/common/fixed.hlsli)だけで計算する。CRT の exp・log を使わないので、
// どの機械でベイクしても同じ表になる(04 R7。Mod やホットリロードは利用者の機械でベイクされる)。
#include "sim/reaction_table.h"

#include <algorithm>
#include <bit>
#include <format>
#include <map>

namespace bicameral::sim {

    using namespace bicameral::fx;
    using namespace bicameral::reaction;

    namespace {

        // --- 物理の定数(整数)---
        // 気体定数 R = 8.314462618 J/(mol·K)(nJ/(mol·K) で正確に)
        constexpr uint64_t GAS_CONSTANT_NANOJOULES = 8314462618;
        constexpr uint64_t NANOJOULES_PER_JOULE = 1000000000;
        constexpr uint64_t MICROJOULES_PER_JOULE = 1000000;
        // 生成エンタルピーの基準の温度 298.15 K(mK)。比熱(mJ/(mol·K))× これ = µJ/mol
        constexpr int64_t STANDARD_TEMPERATURE_MILLIKELVIN = 298150;
        // 速度の表の値が 0 と見なせる 2 の指数(これより小さい k は、1 刻みの端数 2^-32 にも届かない)
        constexpr int32_t RATE_EXPONENT_MIN = -4096;
        constexpr int32_t RATE_EXPONENT_MAX = 4096;
        // レベル 0 のセルの体積 = (0.5 m)^3 = 2^-3 m³(units.hlsli の CELL_SIZE_LEVEL0)
        constexpr int64_t CELL_VOLUME_LOG2 = -3;
        constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
        constexpr uint64_t FNV_PRIME = 0x100000001b3ULL;

        using NameIndex = std::map<std::string, uint32_t, std::less<>>;

        uint64_t HashName(std::string_view name) {
            uint64_t hash = FNV_OFFSET;
            for (const char character : name) {
                hash ^= static_cast<uint8_t>(character);
                hash *= FNV_PRIME;
            }

            return hash;
        }

        // --- 元素と物質 ---

        std::expected<NameIndex, std::string> IndexElements(const ReactionTableDefinition& definition,
                                                            BakedReactionTable& baked) {
            NameIndex elements;
            for (const ElementDefinition& element : definition.elements) {
                if (!elements.emplace(element.name, static_cast<uint32_t>(elements.size())).second)
                    return std::unexpected(std::format("元素 {} が 2 回ある", element.name));

                baked.elementNames.push_back(element.name);
            }

            return elements;
        }

        // H0(0 K 基準の化学のエネルギー、J/mol)= 生成エンタルピー − 比熱 × 298.15 K
        int64_t ZeroKelvinEnthalpy(const SpeciesDefinition& species) {
            const int64_t sensibleMicrojoules = static_cast<int64_t>(species.heatCapacity) *
                                                STANDARD_TEMPERATURE_MILLIKELVIN;

            return species.formationEnthalpy -
                   FxDivS64(sensibleMicrojoules, static_cast<int64_t>(MICROJOULES_PER_JOULE));
        }

        std::expected<void, std::string> BakeSpecies(const SpeciesDefinition& definition, const NameIndex& elements,
                                                     const ReactionTableDefinition& table, BakedReactionTable& baked) {
            const size_t elementCount = elements.size();
            const size_t base = baked.speciesElements.size();
            baked.speciesElements.resize(base + elementCount, 0);
            uint64_t molarMass = 0;
            for (const ElementCount& part : definition.composition) {
                const auto element = elements.find(part.element);
                if (element == elements.end())
                    return std::unexpected(std::format("物質 {}: 知らない元素 {}", definition.name, part.element));

                baked.speciesElements[base + element->second] += part.count;
                molarMass += uint64_t{part.count} * table.elements[element->second].atomicMass;
            }

            if (definition.heatCapacity == 0 || molarMass == 0 || molarMass > UINT32_MAX)
                return std::unexpected(std::format("物質 {}: 比熱かモル質量が 0 か大きすぎる", definition.name));

            baked.species.push_back({.h0 = ZeroKelvinEnthalpy(definition),
                                     .heatCapacity = definition.heatCapacity,
                                     .ruleBegin = 0,
                                     .ruleCount = 0,
                                     .padding = 0});
            baked.speciesNames.push_back(definition.name);
            baked.molarMasses.push_back(static_cast<uint32_t>(molarMass));

            return {};
        }

        std::expected<NameIndex, std::string> BakeAllSpecies(const ReactionTableDefinition& definition,
                                                             const NameIndex& elements, BakedReactionTable& baked) {
            // ID 0 は「無し」
            baked.species.push_back({});
            baked.speciesNames.emplace_back();
            baked.molarMasses.push_back(0);
            baked.speciesElements.resize(elements.size(), 0);

            NameIndex species;
            for (const SpeciesDefinition& entry : definition.species) {
                const auto id = static_cast<uint32_t>(baked.species.size());
                if (!species.emplace(entry.name, id).second)
                    return std::unexpected(std::format("物質 {} が 2 回ある", entry.name));

                if (auto result = BakeSpecies(entry, elements, definition, baked); !result)
                    return std::unexpected(result.error());
            }

            return species;
        }

        // --- 規則 ---

        struct TermsResult {
            uint32_t count = 0;
            uint32_t firstOrderMask = 0;
        };

        // 反応物か生成物の並びを規則に書き写す(ids・coefficients は RxRule の配列)
        std::expected<TermsResult, std::string> CopyTerms(const RuleDefinition& rule, std::span<const RuleTerm> terms,
                                                          const NameIndex& species, std::span<uint32_t> ids,
                                                          std::span<uint32_t> coefficients) {
            if (terms.empty() || terms.size() > ids.size())
                return std::unexpected(std::format("規則 {}: 反応物・生成物は 1〜{} 個", rule.name, ids.size()));

            TermsResult result;
            for (const RuleTerm& term : terms) {
                const auto found = species.find(term.species);
                if (found == species.end())
                    return std::unexpected(std::format("規則 {}: 知らない物質 {}", rule.name, term.species));

                if (term.coefficient == 0)
                    return std::unexpected(std::format("規則 {}: {} の係数が 0", rule.name, term.species));

                if (std::find(ids.begin(), ids.begin() + result.count, found->second) != ids.begin() + result.count)
                    return std::unexpected(std::format("規則 {}: {} が 2 回ある", rule.name, term.species));

                if (term.firstOrder)
                    result.firstOrderMask |= 1u << result.count;

                ids[result.count] = found->second;
                coefficients[result.count] = term.coefficient;
                result.count += 1;
            }

            return result;
        }

        // 元素の釣り合い(02 §2 の 1): 反応物と生成物で、元素ごとの数が完全に一致する
        std::expected<void, std::string> CheckElementBalance(const RuleDefinition& definition, const RxRule& rule,
                                                             const BakedReactionTable& baked) {
            const size_t elementCount = baked.elementNames.size();
            for (size_t element = 0; element < elementCount; ++element) {
                int64_t balance = 0;
                for (uint32_t i = 0; i < rule.reactantCount; ++i) {
                    const uint32_t count = baked.speciesElements[(rule.reactants[i] * elementCount) + element];
                    balance -= int64_t{rule.reactantCoefficients[i]} * count;
                }

                for (uint32_t i = 0; i < rule.productCount; ++i) {
                    const uint32_t count = baked.speciesElements[(rule.products[i] * elementCount) + element];
                    balance += int64_t{rule.productCoefficients[i]} * count;
                }

                if (balance != 0) {
                    return std::unexpected(std::format("規則 {}: 元素 {} が釣り合わない(生成物 − 反応物 = {})",
                                                       definition.name, baked.elementNames[element], balance));
                }
            }

            return {};
        }

        // 反応熱(02 §2 の 2): 生成物の H0 − 反応物の H0。手で書かないので、エネルギーの釣り合いは構造的に守られる
        int64_t ReactionEnthalpy(const RxRule& rule, const BakedReactionTable& baked) {
            int64_t enthalpy = 0;
            for (uint32_t i = 0; i < rule.productCount; ++i)
                enthalpy += int64_t{rule.productCoefficients[i]} * baked.species[rule.products[i]].h0;

            for (uint32_t i = 0; i < rule.reactantCount; ++i)
                enthalpy -= int64_t{rule.reactantCoefficients[i]} * baked.species[rule.reactants[i]].h0;

            return enthalpy;
        }

        std::expected<RxRule, std::string> BakeRule(const RuleDefinition& definition, const NameIndex& species,
                                                    const BakedReactionTable& baked) {
            RxRule rule{};
            const auto reactants = CopyTerms(definition, definition.reactants, species, rule.reactants,
                                             rule.reactantCoefficients);
            if (!reactants)
                return std::unexpected(reactants.error());

            const auto products = CopyTerms(definition, definition.products, species, rule.products,
                                            rule.productCoefficients);
            if (!products)
                return std::unexpected(products.error());

            const int firstOrderCount = std::popcount(reactants->firstOrderMask);
            if (firstOrderCount < 1 || firstOrderCount > 2)
                return std::unexpected(std::format("規則 {}: 次数 1 の反応物は 1〜2 個", definition.name));

            rule.reactantCount = reactants->count;
            rule.productCount = products->count;
            rule.firstOrderMask = reactants->firstOrderMask;
            if (auto balance = CheckElementBalance(definition, rule, baked); !balance)
                return std::unexpected(balance.error());

            rule.reactionEnthalpy = ReactionEnthalpy(rule, baked);
            rule.key = HashName(definition.name);

            return rule;
        }

        // --- 速度の表 ---

        // 温度 kelvin(K)での 1 刻みあたりの速度の係数を、仮数 32bit × 2^指数 に詰める(0 K は 0)。
        // ln k = ln A − Ea / RT − ln 60(1 刻み)+ (n − 1)(ln 1e-6 − ln V)(µmol とセルの体積への換算。n = 次数 1 の反応物の数)
        std::expected<uint64_t, std::string> BakeRate(const RuleDefinition& definition, int64_t lnConstantPart,
                                                      uint32_t kelvin) {
            if (kelvin == 0)
                return 0;

            const uint64_t activation = static_cast<uint64_t>(definition.rate.activationEnergy) * NANOJOULES_PER_JOULE;
            const FxU128 numerator = {.hi = activation >> 32, .lo = activation << 32};
            const uint64_t divisor = GAS_CONSTANT_NANOJOULES * kelvin;
            const auto activationOverRt = static_cast<int64_t>(FxDivU128By64(numerator, divisor).quotient);
            const int64_t lnRate = lnConstantPart - activationOverRt;

            // ln → log2 → 整数の部分と端数に分け、端数を 2^端数(Q31 の仮数)にする
            const int64_t log2Rate = FxMulShiftS64(lnRate, static_cast<int64_t>(FX_LOG2E_Q62), 62);
            const int64_t integerPart = log2Rate >> 32;  // 算術シフト = 下への切り捨て
            const auto fraction = static_cast<int64_t>(static_cast<uint64_t>(log2Rate) & FX_LOW32_MASK);
            const int64_t exponent = integerPart - 31;
            if (exponent < RATE_EXPONENT_MIN)
                return 0;

            if (exponent > RATE_EXPONENT_MAX)
                return std::unexpected(std::format("規則 {}: 速度が大きすぎる({} K)", definition.name, kelvin));

            const uint64_t mantissa = FxExp2Q32(fraction, 31);

            return (mantissa << 32) | static_cast<uint32_t>(static_cast<int32_t>(exponent));
        }

        std::expected<void, std::string> BakeRates(const RuleDefinition& definition, const RxRule& rule,
                                                   BakedReactionTable& baked) {
            // Ea の上限は nJ にして 64bit に収まる範囲(約 1.8e10 J/mol)より十分小さく
            constexpr int64_t ACTIVATION_ENERGY_MAX = 1000000000;
            const int64_t activationEnergy = definition.rate.activationEnergy;
            if (definition.rate.preExponentialMantissa == 0 || activationEnergy < 0 ||
                activationEnergy > ACTIVATION_ENERGY_MAX)
                return std::unexpected(std::format("規則 {}: 速度の A が 0 か Ea が範囲の外", definition.name));

            const int64_t ln10 = FxLnU64(10);
            const int64_t ln2 = FxLnU64(2);
            const int64_t lnPreExponential = FxLnU64(definition.rate.preExponentialMantissa) +
                                             (int64_t{definition.rate.preExponentialExponent10} * ln10);
            const int64_t lnPerTick = -FxLnU64(TICKS_PER_SECOND);
            const int64_t lnAmountPerVolume = (-6 * ln10) - (CELL_VOLUME_LOG2 * ln2);
            const int64_t order = std::popcount(rule.firstOrderMask);
            const int64_t lnConstantPart = lnPreExponential + lnPerTick + ((order - 1) * lnAmountPerVolume);
            for (uint32_t kelvin = 0; kelvin < RX_RATE_TABLE_KELVINS; ++kelvin) {
                const auto rate = BakeRate(definition, lnConstantPart, kelvin);
                if (!rate)
                    return std::unexpected(rate.error());

                baked.rates.push_back(*rate);
            }

            return {};
        }

        // 規則を「ID が最小の反応物」の索引に入れる(1 つの規則は 1 つの物質の一覧にだけ入る = 重複なく引ける)
        void BuildRuleIndex(BakedReactionTable& baked) {
            const auto ruleCount = static_cast<uint32_t>(baked.rules.size());
            for (uint32_t speciesId = 1; speciesId < baked.species.size(); ++speciesId) {
                RxSpecies& species = baked.species[speciesId];
                species.ruleBegin = static_cast<uint32_t>(baked.ruleIndex.size());
                for (uint32_t ruleId = 0; ruleId < ruleCount; ++ruleId) {
                    const RxRule& rule = baked.rules[ruleId];
                    const uint32_t* reactantsEnd = rule.reactants + rule.reactantCount;
                    if (*std::min_element(rule.reactants, reactantsEnd) == speciesId)
                        baked.ruleIndex.push_back(ruleId);
                }

                species.ruleCount = static_cast<uint32_t>(baked.ruleIndex.size()) - species.ruleBegin;
            }

            // GPU のバッファを 0 バイトにしないため
            if (baked.ruleIndex.empty())
                baked.ruleIndex.push_back(0);
        }

    }  // namespace

    uint32_t BakedReactionTable::SpeciesId(std::string_view name) const {
        const auto found = std::find(speciesNames.begin() + 1, speciesNames.end(), name);

        return found == speciesNames.end() ? 0 : static_cast<uint32_t>(found - speciesNames.begin());
    }

    std::expected<BakedReactionTable, std::string> BakeReactionTable(const ReactionTableDefinition& definition) {
        BakedReactionTable baked;
        const auto elements = IndexElements(definition, baked);
        if (!elements)
            return std::unexpected(elements.error());

        const auto species = BakeAllSpecies(definition, *elements, baked);
        if (!species)
            return std::unexpected(species.error());

        NameIndex ruleNames;
        for (const RuleDefinition& entry : definition.rules) {
            if (!ruleNames.emplace(entry.name, static_cast<uint32_t>(ruleNames.size())).second)
                return std::unexpected(std::format("規則 {} が 2 回ある", entry.name));

            const auto rule = BakeRule(entry, *species, baked);
            if (!rule)
                return std::unexpected(rule.error());

            if (auto rates = BakeRates(entry, *rule, baked); !rates)
                return std::unexpected(rates.error());

            baked.rules.push_back(*rule);
            baked.ruleNames.push_back(entry.name);
        }

        BuildRuleIndex(baked);

        return baked;
    }

    // --- セル ---

    RxCell MakeReactionCell(const BakedReactionTable& table, std::span<const SpeciesAmount> amounts,
                            int32_t temperatureMilliKelvin) {
        RxCell cell = RxMakeEmptyCell(0);
        for (const SpeciesAmount& entry : amounts)
            cell = RxAddSpecies(cell, entry.species, entry.amount);

        // 熱(µJ)= 熱容量(nJ/K)× 温度(mK)÷ 1e6
        const int64_t chemical = RxChemicalEnergy(table.View(), cell);
        const uint64_t heatCapacity = RxHeatCapacity(table.View(), cell);
        const FxU128 product = FxMulU64Full(heatCapacity, static_cast<uint64_t>(temperatureMilliKelvin));
        const auto heat = static_cast<int64_t>(FxDivU128By64(product, RX_MILLIKELVIN_NUMERATOR).quotient);

        // mJ に切り上げる(C++ の割り算は 0 方向なので、負の数はそのまま切り上げになる)
        const int64_t total = chemical + heat;
        const auto perMillijoule = static_cast<int64_t>(RX_MICROJOULES_PER_MILLIJOULE);
        cell.energy = total >= 0 ? (total + perMillijoule - 1) / perMillijoule : total / perMillijoule;

        return cell;
    }

    std::vector<uint64_t> CountElements(const BakedReactionTable& table, const RxCell& cell) {
        const size_t elementCount = table.elementNames.size();
        std::vector<uint64_t> counts(elementCount, 0);
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            for (size_t element = 0; element < elementCount; ++element) {
                const uint32_t perSpecies = table.speciesElements[(cell.species[i] * elementCount) + element];
                counts[element] += cell.amounts[i] * perSpecies;
            }
        }

        return counts;
    }

    uint64_t HashReactionCell(const RxCell& cell) {
        uint64_t hash = FxHashCombine(0, static_cast<uint64_t>(cell.energy));
        hash = FxHashCombine(hash, cell.speciesCount);
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            hash = FxHashCombine(hash, cell.species[i]);
            hash = FxHashCombine(hash, cell.amounts[i]);
        }

        return hash;
    }

    RxCell EvaluateReactionCell(const BakedReactionTable& table, const RxCell& cell, uint64_t worldSeed, uint64_t tick,
                                uint64_t cellId) {
        return RxEvaluateCell(table.View(), cell, worldSeed, tick, cellId);
    }

}  // namespace bicameral::sim
