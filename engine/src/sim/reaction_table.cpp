// reaction_table.cpp — 反応表のベイク(定義 → 整数の表)と、セルを作る・調べる道具(02 §1〜3。T-0014)。
// ベイクは整数の数学ライブラリ(shaders/common/fixed.hlsli)だけで計算する。CRT の exp・log を使わないので、
// どの機械でベイクしても同じ表になる(04 R7。Mod やホットリロードは利用者の機械でベイクされる)。
// 定義の並びにも依らない(T-0021・ADR-0032): 名前のバイト順に並べ直してから ID を振る。
#include "sim/reaction_table.h"

#include <algorithm>
#include <bit>
#include <format>
#include <iterator>
#include <map>

namespace bicameral::sim {

    using namespace bicameral::fx;
    using namespace bicameral::reaction;

    namespace {

        // --- 物理の定数(整数)---
        // 気体定数 R = 8.314462618 J/(mol·K)(nJ/(mol·K) で正確に。器の定数 units.hlsli。ADR-0171)
        constexpr uint64_t GAS_CONSTANT_NANOJOULES = fx::GAS_CONSTANT_NANOJOULES_PER_MOLE_KELVIN;
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
        // 文献の反応熱と生成エンタルピーの差がこれより大きく違えば警告(文献の値は 0.01〜1 kJ/mol の桁で丸められている)
        constexpr int64_t DECLARED_ENTHALPY_TOLERANCE_JOULES = 1000;

        using NameIndex = std::map<std::string, uint32_t, std::less<>>;

        uint64_t HashName(std::string_view name) {
            uint64_t hash = FNV_OFFSET;
            for (const char character : name) {
                hash ^= static_cast<uint8_t>(character);
                hash *= FNV_PRIME;
            }

            return hash;
        }

        // --- 釣り合いの規則の逆向き(T-0623・ADR-0290)---

        // 釣り合いの規則ごとに逆向きの規則(反応物と生成物を入れ替え、釣り合いの急さの符号を変えたもの)を足す。
        // 逆向きは自分の反応物(元の生成物)の索引に入るので、元の反応物が無いセル(水だけ・湯気だけ)でも釣り合いへ向かえる
        ReactionTableDefinition WithReverseRules(const ReactionTableDefinition& definition) {
            ReactionTableDefinition expanded = definition;
            for (const RuleDefinition& rule : definition.rules) {
                if (!rule.balance)
                    continue;

                RuleDefinition reverse = rule;
                reverse.name = rule.name + std::string(REVERSE_RULE_SUFFIX);
                reverse.reactants = rule.products;
                reverse.products = rule.reactants;
                reverse.balance->enthalpy = -rule.balance->enthalpy;
                if (rule.declaredReactionEnthalpy)
                    reverse.declaredReactionEnthalpy = -*rule.declaredReactionEnthalpy;

                expanded.rules.push_back(std::move(reverse));
            }

            return expanded;
        }

        // --- 並び(T-0021・ADR-0032)---

        // 元素・物質・規則・規則の項を名前のバイト順に並べた写し。ID はこの順で振るので、定義の並びに依らない。
        // 名前の重なりは並べた後も隣に残り、索引を作るときに誤りになる
        ReactionTableDefinition CanonicalOrder(const ReactionTableDefinition& definition) {
            ReactionTableDefinition sorted = definition;
            const auto byName = [](const auto& left, const auto& right) {
                return left.name < right.name;
            };
            std::ranges::stable_sort(sorted.elements, byName);
            std::ranges::stable_sort(sorted.species, byName);
            std::ranges::stable_sort(sorted.rules, byName);

            for (RuleDefinition& rule : sorted.rules) {
                std::ranges::stable_sort(rule.reactants, {}, &RuleTerm::species);
                std::ranges::stable_sort(rule.products, {}, &RuleTerm::species);
            }

            return sorted;
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

        // 気体定数 R(mJ/(mol·K)。8.314462618 J/(mol·K)〔CODATA 2018〕の切り捨て)。定積の比熱 = 定圧 − R(T-0591)
        constexpr uint32_t GAS_CONSTANT_MILLIJOULES_PER_MOLE_KELVIN = 8314;

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

            // 気体の比熱を定圧で書く表(既定)は、セルが読む定積の比熱 c_v = c_p − R にする(T-0591・ADR-0259・T-0597。H0 は書いた c_p から)
            uint32_t heatCapacity = definition.heatCapacity;
            if (table.gasHeatCapacityAtConstantPressure && !definition.condensed) {
                if (heatCapacity <= GAS_CONSTANT_MILLIJOULES_PER_MOLE_KELVIN)
                    return std::unexpected(std::format(
                        "物質 {}: 気体の定圧の比熱が気体定数 R(8314 mJ/(mol·K))以下(凝縮相なら condensed = true)",
                        definition.name));

                heatCapacity -= GAS_CONSTANT_MILLIJOULES_PER_MOLE_KELVIN;
            }

            baked.species.push_back({.h0 = ZeroKelvinEnthalpy(definition),
                                     .heatCapacity = heatCapacity,
                                     .ruleBegin = 0,
                                     .ruleCount = 0,
                                     .conductivity = definition.thermalConductivity});
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
            baked.standardEntropies.emplace_back();
            baked.speciesCondensed.push_back(0);
            baked.speciesRadiationAbsorption.push_back(0);
            baked.speciesLiquid.push_back(0);
            baked.speciesMolarVolume.push_back(0);

            NameIndex species;
            for (const SpeciesDefinition& entry : definition.species) {
                const auto id = static_cast<uint32_t>(baked.species.size());
                if (!species.emplace(entry.name, id).second)
                    return std::unexpected(std::format("物質 {} が 2 回ある", entry.name));

                if (auto result = BakeSpecies(entry, elements, definition, baked); !result)
                    return std::unexpected(result.error());

                baked.standardEntropies.push_back(entry.standardEntropy);
                baked.speciesCondensed.push_back(entry.condensed || entry.liquid ? 1 : 0);
                baked.species.back().flags = entry.condensed || entry.liquid ? 0u : RX_SPECIES_GAS;  // T-0622
                baked.speciesRadiationAbsorption.push_back(entry.radiationAbsorption);

                // 液体(T-0609・ADR-0266): 密度が要る。モル体積(mm³/mol)= モル質量(mg/mol)× 1000 ÷ 密度(kg/m³)の最も近い整数
                if (entry.liquid && entry.density == 0)
                    return std::unexpected(std::format("物質 {}: 液体には密度(density_kg_per_m3)が要る", entry.name));

                // 密度の無い固体は場所を取らない(気体も液体も素通し。ADR-0268)。書き忘れを読み込みで知らせる(T-0616。表は当たる)
                if (entry.condensed && entry.density == 0)
                    baked.warnings.push_back(std::format(
                        "物質 {}: 凝縮相に密度(density_kg_per_m3)が無い(セルの場所を取らず、気体も液体も素通しする)",
                        entry.name));

                const uint64_t molarVolume = entry.density == 0
                                                 ? 0
                                                 : ((uint64_t{baked.molarMasses.back()} * 1000) + (entry.density / 2)) /
                                                       entry.density;
                baked.speciesLiquid.push_back(entry.liquid ? 1 : 0);
                baked.speciesMolarVolume.push_back(molarVolume);
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

        // 釣り合いの欄(T-0623・ADR-0290)。log2 K(T_eq) を µmol とセルの体積に直し(Q は気体の項の µmol で数える)、
        // 急さ = ΔH_eq ÷ (R ln 2)(K。Q32)。比熱の変化は進めた後の温度に使う
        std::expected<void, std::string> BakeBalance(const RuleDefinition& definition, const BakedReactionTable& baked,
                                                     RxRule& rule) {
            if (!definition.balance)
                return {};

            constexpr int64_t BALANCE_ENTHALPY_MAX = 10000000;  // J/mol(急さを 64bit に収める)
            constexpr uint32_t BALANCE_KELVIN_MIN = 1000;       // mK
            constexpr uint32_t BALANCE_KELVIN_MAX = (RX_RATE_TABLE_KELVINS - 1) * 1000;
            const BalanceDefinition& balance = *definition.balance;
            if (balance.temperatureMilliKelvin < BALANCE_KELVIN_MIN ||
                balance.temperatureMilliKelvin > BALANCE_KELVIN_MAX)
                return std::unexpected(
                    std::format("規則 {}: 釣り合いの温度は 1〜{} K", definition.name, RX_RATE_TABLE_KELVINS - 1));

            if (balance.enthalpy < -BALANCE_ENTHALPY_MAX || balance.enthalpy > BALANCE_ENTHALPY_MAX)
                return std::unexpected(
                    std::format("規則 {}: 釣り合いの ΔH は ±{} J/mol まで", definition.name, BALANCE_ENTHALPY_MAX));

            // --- 気体の項と Δn・比熱の変化 ---
            int64_t gasChange = 0;
            int64_t heatCapacityChange = 0;
            for (uint32_t i = 0; i < rule.reactantCount; ++i) {
                const bool gas = baked.speciesCondensed[rule.reactants[i]] == 0;
                rule.balanceGasMask |= gas ? 1u << i : 0u;
                gasChange -= gas ? int64_t{rule.reactantCoefficients[i]} : 0;
                heatCapacityChange -= int64_t{rule.reactantCoefficients[i]} *
                                      baked.species[rule.reactants[i]].heatCapacity;
            }

            for (uint32_t i = 0; i < rule.productCount; ++i) {
                const bool gas = baked.speciesCondensed[rule.products[i]] == 0;
                rule.balanceGasMask |= gas ? 1u << (RX_BALANCE_PRODUCT_SHIFT + i) : 0u;
                gasChange += gas ? int64_t{rule.productCoefficients[i]} : 0;
                heatCapacityChange += int64_t{rule.productCoefficients[i]} *
                                      baked.species[rule.products[i]].heatCapacity;
            }

            if (gasChange != 0 && balance.concentrationMantissa == 0)
                return std::unexpected(
                    std::format("規則 {}: 気体の係数が両辺で違う釣り合いには濃さ(concentration_mol_per_m3)が要る",
                                definition.name));

            // --- log2 K(T_eq)(µmol の単位)= Δn × (log2 c + log2 1e6 + log2 V)---
            const int64_t log2Ten = FxLog2U64(10);
            const int64_t log2Concentration = balance.concentrationMantissa == 0
                                                  ? 0
                                                  : FxLog2U64(balance.concentrationMantissa) +
                                                        (int64_t{balance.concentrationExponent10} * log2Ten);
            const int64_t log2PerCell = log2Concentration + (6 * log2Ten) + (CELL_VOLUME_LOG2 * (int64_t{1} << 32));
            rule.balanceLog2Constant = gasChange * log2PerCell;

            // --- 急さ = ΔH ÷ R(K。Q32)× log2 e ---
            const uint64_t magnitude = static_cast<uint64_t>(balance.enthalpy < 0 ? -balance.enthalpy
                                                                                  : balance.enthalpy) *
                                       NANOJOULES_PER_JOULE;
            const FxU128 numerator = {.hi = magnitude >> 32, .lo = magnitude << 32};
            const auto overR = static_cast<int64_t>(FxDivU128By64(numerator, GAS_CONSTANT_NANOJOULES).quotient);
            const int64_t slope = FxMulShiftS64(overR, static_cast<int64_t>(FX_LOG2E_Q62), 62);
            rule.balanceSlope = balance.enthalpy < 0 ? -slope : slope;
            rule.heatCapacityChange = heatCapacityChange;
            rule.balanceKelvin = balance.temperatureMilliKelvin;

            return {};
        }

        // 進行度 1 µmol で増える気体のぶん(µmol。減るか同じなら 0)。取り合いでセルの気体の器の残りまでに縮める(T-0622・ADR-0316)
        uint32_t GasGrowth(const RxRule& rule, const BakedReactionTable& baked) {
            int64_t change = 0;
            for (uint32_t i = 0; i < rule.reactantCount; ++i)
                change -= baked.speciesCondensed[rule.reactants[i]] == 0 ? int64_t{rule.reactantCoefficients[i]} : 0;

            for (uint32_t i = 0; i < rule.productCount; ++i)
                change += baked.speciesCondensed[rule.products[i]] == 0 ? int64_t{rule.productCoefficients[i]} : 0;

            return change > 0 ? static_cast<uint32_t>(change) : 0u;
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

            // 次数 1 の反応物は 0〜2 個。0 個 = 反応物が全部次数 0(あれば効く)の規則で、進みはセルの体積あたり一定
            // (凝縮相は量によらず活量 1: 蒸発・融解。A は mol/(m³·s)。T-0611・ADR-0272)。釣り合いの規則は次数を使わない(0)
            const int firstOrderCount = std::popcount(reactants->firstOrderMask);
            if (firstOrderCount > 2 && !definition.balance)
                return std::unexpected(std::format("規則 {}: 次数 1 の反応物は 0〜2 個", definition.name));

            rule.reactantCount = reactants->count;
            rule.productCount = products->count;
            rule.firstOrderMask = definition.balance ? 0u : reactants->firstOrderMask;
            if (auto balance = CheckElementBalance(definition, rule, baked); !balance)
                return std::unexpected(balance.error());

            rule.reactionEnthalpy = ReactionEnthalpy(rule, baked);
            rule.gasGrowth = GasGrowth(rule, baked);
            rule.key = HashName(definition.name);
            if (auto balance = BakeBalance(definition, baked, rule); !balance)
                return std::unexpected(balance.error());

            // 魔法の触媒が下げられる上限(T-0288)。範囲の外の Ea は BakeRates が断る
            const int64_t activationEnergy = definition.rate.activationEnergy;
            const bool storable = activationEnergy >= 0 && activationEnergy <= int64_t{UINT32_MAX};
            rule.activationEnergy = storable ? static_cast<uint32_t>(activationEnergy) : 0u;

            return rule;
        }

        // 文献の反応熱(書いた規則だけ)と、生成エンタルピーの差(298.15 K)を比べる(02 §2 の 2)。食い違えば警告の文を返す。
        // 表は生成エンタルピーの差を使う(手で書いた値は使わない)ので、食い違いはエネルギーの保存を壊さない
        std::optional<std::string> CheckDeclaredEnthalpy(const RuleDefinition& rule,
                                                         const ReactionTableDefinition& definition,
                                                         const NameIndex& species) {
            if (!rule.declaredReactionEnthalpy)
                return std::nullopt;

            // 物質 ID は 1 から(0 は「無し」)。項の物質は BakeRule が確かめ済み
            const auto formation = [&](const RuleTerm& term) {
                const SpeciesDefinition& entry = definition.species[species.find(term.species)->second - 1];

                return int64_t{term.coefficient} * entry.formationEnthalpy;
            };

            int64_t standard = 0;
            for (const RuleTerm& term : rule.products)
                standard += formation(term);

            for (const RuleTerm& term : rule.reactants)
                standard -= formation(term);

            const int64_t difference = *rule.declaredReactionEnthalpy - standard;
            if (difference >= -DECLARED_ENTHALPY_TOLERANCE_JOULES && difference <= DECLARED_ENTHALPY_TOLERANCE_JOULES)
                return std::nullopt;

            return std::format(
                "規則 {}: 書いた反応熱 {} J/mol が生成エンタルピーの差 {} J/mol と {} J/mol 違う"
                "(表は生成エンタルピーの差を使う)",
                rule.name, *rule.declaredReactionEnthalpy, standard, difference);
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

            // A の仮数の末尾の 0 を指数へ移す(28e18 と 280e17 が同じ表になる。T-0021)
            uint64_t mantissa = definition.rate.preExponentialMantissa;
            int64_t exponent10 = definition.rate.preExponentialExponent10;
            while (mantissa % 10 == 0) {
                mantissa /= 10;
                exponent10 += 1;
            }

            const int64_t ln10 = FxLnU64(10);
            const int64_t ln2 = FxLnU64(2);
            const int64_t lnPreExponential = FxLnU64(mantissa) + (exponent10 * ln10);
            const int64_t lnPerTick = -FxLnU64(TICKS_PER_SECOND);
            const int64_t lnAmountPerVolume = (-6 * ln10) - (CELL_VOLUME_LOG2 * ln2);
            // 釣り合いの規則の速さは「釣り合いまでの距離の 1 秒あたりの割合」なので、量の換算は無い(次数 1 として焼く)
            const int64_t order = definition.balance ? 1 : std::popcount(rule.firstOrderMask);
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
        const ReactionTableDefinition ordered = CanonicalOrder(WithReverseRules(definition));
        BakedReactionTable baked;
        const auto elements = IndexElements(ordered, baked);
        if (!elements)
            return std::unexpected(elements.error());

        const auto species = BakeAllSpecies(ordered, *elements, baked);
        if (!species)
            return std::unexpected(species.error());

        NameIndex ruleNames;
        for (const RuleDefinition& entry : ordered.rules) {
            if (!ruleNames.emplace(entry.name, static_cast<uint32_t>(ruleNames.size())).second)
                return std::unexpected(std::format("規則 {} が 2 回ある", entry.name));

            const auto rule = BakeRule(entry, *species, baked);
            if (!rule)
                return std::unexpected(rule.error());

            if (auto rates = BakeRates(entry, *rule, baked); !rates)
                return std::unexpected(rates.error());

            if (auto warning = CheckDeclaredEnthalpy(entry, ordered, *species))
                baked.warnings.push_back(*warning);

            baked.rules.push_back(*rule);
            baked.ruleNames.push_back(entry.name);
        }

        BuildRuleIndex(baked);

        // --- 坂の検査(D-487 ②: 警告して動かす。表は当たる。T-0492)---
        baked.ruleSlopes = CheckReactionSlopes(baked);
        std::ranges::move(SlopeWarnings(baked, baked.ruleSlopes), std::back_inserter(baked.warnings));

        // --- 同じ読み込みに足した分類(世界のデータ。T-0398〜T-0400)。物質の名前を ID に直して確かめる(知らない物質は誤り)。
        // 分類ごとに別のファイルで行い、ここで 1 行ずつつなぐ ---
        auto layout = BakeWorldLayout(ordered.worldLayout, baked);
        if (!layout)
            return std::unexpected(layout.error());

        baked.worldLayout = std::move(*layout);

        auto templates = BakeMaterialTemplates(definition.templates, baked);
        if (!templates)
            return std::unexpected(templates.error());

        baked.templates = std::move(*templates);

        baked.environment = ordered.environment;  // 環境の値は物質を指さないので、そのまま(T-0400)

        auto atmosphere = BakeWorldAtmosphere(ordered.atmosphere, baked);
        if (!atmosphere)
            return std::unexpected(atmosphere.error());

        baked.atmosphere = std::move(*atmosphere);

        auto playMetrics = BakePlayMetrics(definition.playMetrics, baked);
        if (!playMetrics)
            return std::unexpected(playMetrics.error());

        baked.playMetrics = std::move(*playMetrics);

        // 流れどうしの結合(引きずり・押す構造。1 つの検査 |q| ≤ 1 で、押さえた値は警告。T-0536・ADR-0238)
        auto couplings = BakeCouplings(definition.couplings, baked);
        if (!couplings)
            return std::unexpected(couplings.error());

        baked.couplings = std::move(couplings->couplings);
        std::ranges::move(couplings->warnings, std::back_inserter(baked.warnings));

        return baked;
    }

    // --- セル ---

    namespace {

        // 成分を足して温度からエネルギーを決める(Cell = RxCell か RxWideCell。cell は空のセル)
        template <typename Cell>
        Cell FillReactionCell(const BakedReactionTable& table, Cell cell, std::span<const SpeciesAmount> amounts,
                              int32_t temperatureMilliKelvin) {
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

        template <typename Cell>
        std::vector<uint64_t> CountCellElements(const BakedReactionTable& table, const Cell& cell) {
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

        template <typename Cell>
        uint64_t HashCell(const Cell& cell) {
            uint64_t hash = FxHashCombine(0, static_cast<uint64_t>(cell.energy));
            hash = FxHashCombine(hash, cell.speciesCount);
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                hash = FxHashCombine(hash, cell.species[i]);
                hash = FxHashCombine(hash, cell.amounts[i]);
            }

            return hash;
        }

    }  // namespace

    std::expected<void, std::string> CheckCellAmounts(const BakedReactionTable& table,
                                                      std::span<const SpeciesAmount> amounts) {
        // --- 合計(全部の相)と気体のぶん。足す前に残りと比べる(溢れない)---
        uint64_t total = 0;
        uint64_t gas = 0;
        for (const SpeciesAmount& entry : amounts) {
            if (entry.amount > fx::CELL_AMOUNT_LIMIT_MICROMOLES - total) {
                return std::unexpected(std::format("1 セルの量の合計は {} µmol(約 {} mol)まで",
                                                   fx::CELL_AMOUNT_LIMIT_MICROMOLES,
                                                   fx::CELL_AMOUNT_LIMIT_MICROMOLES / fx::MICROMOLES_PER_MOLE));
            }

            total += entry.amount;
            const bool condensed = entry.species < table.speciesCondensed.size() &&
                                   table.speciesCondensed[entry.species] != 0;
            if (!condensed)
                gas += entry.amount;
        }

        if (gas >= fx::CELL_GAS_AMOUNT_LIMIT_MICROMOLES) {
            return std::unexpected(std::format(
                "1 セルの気体のぶん(condensed でない物質の合計)は {} µmol(約 {} mol)未満",
                fx::CELL_GAS_AMOUNT_LIMIT_MICROMOLES, fx::CELL_GAS_AMOUNT_LIMIT_MICROMOLES / fx::MICROMOLES_PER_MOLE));
        }

        return {};
    }

    RxCell MakeReactionCell(const BakedReactionTable& table, std::span<const SpeciesAmount> amounts,
                            int32_t temperatureMilliKelvin) {
        return FillReactionCell(table, RxMakeEmptyCell(0), amounts, temperatureMilliKelvin);
    }

    RxWideCell MakeWideReactionCell(const BakedReactionTable& table, std::span<const SpeciesAmount> amounts,
                                    int32_t temperatureMilliKelvin) {
        return FillReactionCell(table, RxMakeEmptyWideCell(0), amounts, temperatureMilliKelvin);
    }

    std::vector<uint64_t> CountElements(const BakedReactionTable& table, const RxCell& cell) {
        return CountCellElements(table, cell);
    }

    std::vector<uint64_t> CountElements(const BakedReactionTable& table, const RxWideCell& cell) {
        return CountCellElements(table, cell);
    }

    uint64_t HashReactionCell(const RxCell& cell) {
        return HashCell(cell);
    }

    uint64_t HashReactionCell(const RxWideCell& cell) {
        return HashCell(cell);
    }

    RxLoneCell AdvanceLoneReactionCell(const BakedReactionTable& table, const RxLoneCell& lone, uint64_t worldSeed,
                                       uint64_t tickBegin, uint32_t tickCount, uint64_t cellId) {
        return RxAdvanceLoneCell(table.View(), lone, worldSeed, tickBegin, tickCount, cellId);
    }

    RxWaitStep StepReactionCellWait(const BakedReactionTable& table, const RxCell& cell, uint64_t worldSeed,
                                    uint64_t tick, uint64_t changedTick, uint64_t cellId) {
        return RxStepCellWait(table.View(), cell, worldSeed, tick, changedTick, cellId);
    }

    RxWideWaitStep StepReactionCellWait(const BakedReactionTable& table, const RxWideCell& cell, uint64_t worldSeed,
                                        uint64_t tick, uint64_t changedTick, uint64_t cellId) {
        return RxStepCellWait(table.View(), cell, worldSeed, tick, changedTick, cellId);
    }

}  // namespace bicameral::sim
