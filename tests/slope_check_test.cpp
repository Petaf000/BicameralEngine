// slope_check_test.cpp — 坂の検査の物差し(D-487・T-0491。docs/design/19-slope-principle-research.md §2・§5 の段 0)。CPU だけ。
//
// 坂の原理の P2「散逸の流れは坂を下る向きにだけ進む(エントロピーの生成 ≥ 0)」を、仮の世界の CPU リファレンス(sim::ProbeReference)の
// 燃える木箱で数える。試験のコードはシミュのコードではないので倍精度で数える(シミュの結果には触らない)。
//
// データの流れ:
//   試験の表(data/packages/combustion_test。物質に任意の欄 standard_entropy_mj_per_mol_k)→ 物質ごとの H(T)・S(T)
//   → (1) 規則ごとに、標準の坂 ΔG°(T) が正(坂を上る)で速さが 0 でない温度の範囲(ベイクの警告の元。T-0492 が使う)
//   → (2) 木箱を進め、毎刻み
//         - 面の熱の流れ(HcFaceFlow。S(t) の熱のキャッシュから。伝導の段が使う値そのもの)の向きと σ = Q (1/T冷 − 1/T熱)
//         - 反応が起きたセルで、刻みの核(RxStepCellWait)と同じ手順をなぞって規則ごとの進行度 ξ を取り出し(なぞった結果が
//           S(t + 1) とビット単位で同じことを確かめる)、σ = ξ · A / T(A = −ΔG。気体は分圧で活量を入れる)
//   → (3) 燃え方の計器(最高温度・セルロースが尽きる刻み・残った炭と CO・CO2)。「木箱が同じく動く」の幅の基準値(D-487 ③)
// 決めごと(試験の中だけ):
//   - H_i(T) = h0_i + Cp_i T(エンジンの化学 + 熱のエネルギーと同じ)、S_i(T) = S°_i + Cp_i ln(T / 298.15 K)(比熱一定)。
//   - 固体(セルロース・炭)は活量 1、ほかは理想気体で a = n R T / (V p°)(V = セル 0.125 m³、p° = 1 bar、0 の量は 1 µmol と置く)。
//     エンジンの物質は相を持たないので、固体の名前は試験が知っている(M2 で相を持ったら表から読む)。
//   - セルの中の反応は熱がそのセルに残る(体積一定)が、坂は定圧の ΔG で見る(近似。符号の判定には十分)。
// 使い方: 引数なし = ctest(短い刻み。伝導で坂を上る流れが 0・なぞりが一致・規則の一覧が出る)。
//         `--burn [刻み]` = 燃え方の計器を長く測る(既定 18000 刻み。ctest では流さない)。結果と「同じく動く」の幅は
//         docs/tickets/T-0491-slope-check-gauge.md と docs/design/19 §8。
// 開発ビルドの坂の計器(sim/slope_gauge。整数。T-0518)を同じ木箱に並べて走らせ、この倍精度の数え上げと進行度・坂を上った回が
// 同じことを確かめる(合成のセルでも ΔG を 1 J/mol の中で比べる)。
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/aliases.h"
#include "reaction_test_table.h"
#include "sim/probe_sim.h"
#include "sim/slope_gauge.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    constexpr uint64_t SHORT_TICKS = 600;      // ctest: 火がついて燃え広がる所まで(10 秒)
    constexpr uint64_t BURN_TICKS = 18000;     // --burn の既定(5 分。火が消えるまで。CPU で約 30 分)
    constexpr uint64_t PROGRESS_TICKS = 1000;  // 途中経過を出す間隔

    // --- 物理の定数(試験の中だけ。倍精度)---
    constexpr double GAS_CONSTANT = 8.314462618;  // J/(mol·K)
    constexpr double STANDARD_KELVIN = 298.15;
    constexpr double STANDARD_PRESSURE = 1.0e5;         // Pa(1 bar)
    constexpr double CELL_VOLUME = 0.125;               // m³(0.5 m 角)
    constexpr double MICRO = 1.0e-6;                    // µmol → mol
    constexpr double MILLI = 1.0e-3;                    // mJ → J・mK → K
    constexpr double CELLULOSE_ENTROPY_SPREAD = 100.0;  // J/(mol·K)。推定値(未確認)を動かして符号が変わらないかを見る幅

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // --- 物質の熱力学(添字 = 物質 ID)---

    struct SpeciesThermo {
        double zeroKelvinEnthalpy = 0.0;  // J/mol(エンジンの h0)
        double heatCapacity = 0.0;        // J/(mol·K)
        double standardEntropy = 0.0;     // J/(mol·K)(298.15 K)
        bool hasEntropy = false;
        bool condensed = false;  // 固体(活量 1)

        [[nodiscard]] double Enthalpy(double kelvin) const { return zeroKelvinEnthalpy + heatCapacity * kelvin; }
        [[nodiscard]] double Entropy(double kelvin) const {
            return standardEntropy + heatCapacity * std::log(kelvin / STANDARD_KELVIN);
        }
    };

    std::vector<SpeciesThermo> MakeThermo(const ReactionTableDefinition& definition, const BakedReactionTable& table) {
        std::vector<SpeciesThermo> thermo(table.species.size());
        for (size_t id = 1; id < table.species.size(); ++id) {
            thermo[id].zeroKelvinEnthalpy = static_cast<double>(table.species[id].h0);
            thermo[id].heatCapacity = static_cast<double>(table.species[id].heatCapacity) * MILLI;
            thermo[id].condensed = table.speciesNames[id] == "cellulose" || table.speciesNames[id] == "carbon";
        }

        for (const SpeciesDefinition& species : definition.species) {
            const uint32_t id = table.SpeciesId(species.name);
            if (id == 0 || !species.standardEntropy)
                continue;

            thermo[id].standardEntropy = static_cast<double>(*species.standardEntropy) * MILLI;
            thermo[id].hasEntropy = true;
        }

        return thermo;
    }

    // --- 規則の正味の係数(生成物 − 反応物。触媒は両辺で消える)---

    struct NetTerm {
        uint32_t species = 0;
        double coefficient = 0.0;
    };

    std::vector<NetTerm> NetTerms(const reaction::RxRule& rule) {
        std::vector<NetTerm> terms;
        const auto add = [&](uint32_t species, double coefficient) {
            const auto found = rng::find_if(terms, [&](const NetTerm& term) { return term.species == species; });
            if (found != terms.end())
                found->coefficient += coefficient;
            else
                terms.push_back({.species = species, .coefficient = coefficient});
        };

        for (uint32_t i = 0; i < rule.reactantCount; ++i)
            add(rule.reactants[i], -static_cast<double>(rule.reactantCoefficients[i]));

        for (uint32_t i = 0; i < rule.productCount; ++i)
            add(rule.products[i], static_cast<double>(rule.productCoefficients[i]));

        std::erase_if(terms, [](const NetTerm& term) { return term.coefficient == 0.0; });

        return terms;
    }

    bool HasAllEntropies(std::span<const NetTerm> terms, std::span<const SpeciesThermo> thermo) {
        return rng::all_of(terms, [&](const NetTerm& term) { return thermo[term.species].hasEntropy; });
    }

    // 標準の坂 ΔG°(T)(J/mol。正なら坂を上る)
    double StandardGibbs(std::span<const NetTerm> terms, std::span<const SpeciesThermo> thermo, double kelvin) {
        double gibbs = 0.0;
        for (const NetTerm& term : terms) {
            const SpeciesThermo& species = thermo[term.species];
            gibbs += term.coefficient * (species.Enthalpy(kelvin) - kelvin * species.Entropy(kelvin));
        }

        return gibbs;
    }

    // --- (1) 静的な一覧: 標準の坂を上り、速さが 0 でない温度の範囲 ---

    struct UphillRange {
        uint32_t lowKelvin = 0;  // 範囲の両端(count == 0 なら範囲なし)
        uint32_t highKelvin = 0;
        uint32_t count = 0;  // 範囲の中の温度の数(1 K ごと)
    };

    UphillRange StaticUphill(const ReactionTableView& view, uint32_t ruleId, std::span<const SpeciesThermo> thermo) {
        const std::vector<NetTerm> terms = NetTerms(view.Rule(ruleId));
        UphillRange range;
        for (uint32_t kelvin = 1; kelvin < reaction::RX_RATE_TABLE_KELVINS; ++kelvin) {
            if (reaction::RxRateMantissa(view.Rate(ruleId, kelvin)) == 0)
                continue;

            if (StandardGibbs(terms, thermo, static_cast<double>(kelvin)) <= 0.0)
                continue;

            range.lowKelvin = range.count == 0 ? kelvin : range.lowKelvin;
            range.highKelvin = kelvin;
            range.count += 1;
        }

        return range;
    }

    // ΔG°(T) = 0 になる温度(1〜4095 K で符号が変わる所。無ければ 0)
    uint32_t BalanceKelvin(std::span<const NetTerm> terms, std::span<const SpeciesThermo> thermo) {
        for (uint32_t kelvin = 2; kelvin < reaction::RX_RATE_TABLE_KELVINS; ++kelvin) {
            const bool before = StandardGibbs(terms, thermo, kelvin - 1.0) > 0.0;
            if (before != (StandardGibbs(terms, thermo, static_cast<double>(kelvin)) > 0.0))
                return kelvin;
        }

        return 0;
    }

    std::vector<UphillRange> PrintStaticList(const BakedReactionTable& table, std::span<const SpeciesThermo> thermo) {
        const ReactionTableView view = table.View();
        std::vector<UphillRange> ranges;
        std::printf("(1) 規則ごとの標準の坂(ΔG°。速さが 0 でない温度だけ):\n");
        for (uint32_t id = 0; id < table.rules.size(); ++id) {
            const std::vector<NetTerm> terms = NetTerms(view.Rule(id));
            ranges.push_back(StaticUphill(view, id, thermo));
            const UphillRange& range = ranges.back();
            if (!HasAllEntropies(terms, thermo)) {
                std::printf("  %-42s 未評価(エントロピーの無い物質がある)\n", table.ruleNames[id].c_str());
                continue;
            }

            const double gibbs300 = StandardGibbs(terms, thermo, 300.0) * MILLI;
            const double gibbs1000 = StandardGibbs(terms, thermo, 1000.0) * MILLI;
            if (range.count == 0)
                std::printf("  %-42s 下る    ΔG°(300 K) %9.1f kJ/mol・ΔG°(1000 K) %9.1f\n", table.ruleNames[id].c_str(),
                            gibbs300, gibbs1000);
            else
                std::printf("  %-42s 坂を上る %u〜%u K(釣り合い %u K)ΔG°(300 K) %9.1f・ΔG°(1000 K) %9.1f\n",
                            table.ruleNames[id].c_str(), range.lowKelvin, range.highKelvin,
                            BalanceKelvin(terms, thermo), gibbs300, gibbs1000);
        }

        return ranges;
    }

    // 推定のセルロースのエントロピーを ±幅 動かしても、規則ごとの静的な判定(坂を上る範囲の有無)が変わらない
    void CheckCelluloseSpread(const BakedReactionTable& table, std::span<const SpeciesThermo> thermo,
                              std::span<const UphillRange> ranges) {
        const uint32_t cellulose = table.SpeciesId("cellulose");
        for (const double delta : {-CELLULOSE_ENTROPY_SPREAD, CELLULOSE_ENTROPY_SPREAD}) {
            std::vector<SpeciesThermo> moved(thermo.begin(), thermo.end());
            moved[cellulose].standardEntropy += delta;
            for (uint32_t id = 0; id < table.rules.size(); ++id) {
                const UphillRange range = StaticUphill(table.View(), id, moved);
                EXPECT((range.count == 0) == (ranges[id].count == 0));
            }
        }
    }

    // ベイクの坂の検査(整数。sim/reaction_slope・T-0492)が、この倍精度の一覧と同じ温度の範囲を出す
    void CheckBakedSlopes(const BakedReactionTable& table, std::span<const SpeciesThermo> thermo,
                          std::span<const UphillRange> ranges) {
        EXPECT(table.ruleSlopes.size() == ranges.size());
        for (uint32_t id = 0; id < table.ruleSlopes.size() && id < ranges.size(); ++id) {
            const RuleSlope& baked = table.ruleSlopes[id];
            const std::vector<NetTerm> terms = NetTerms(table.View().Rule(id));
            if (!HasAllEntropies(terms, thermo)) {
                EXPECT(baked.state == RuleSlopeState::Unevaluated);
                continue;
            }

            EXPECT((baked.state == RuleSlopeState::Uphill) == (ranges[id].count != 0));
            EXPECT(baked.uphill.lowKelvin == ranges[id].lowKelvin && baked.uphill.highKelvin == ranges[id].highKelvin &&
                   baked.uphill.count == ranges[id].count);
            EXPECT(baked.balanceKelvin == BalanceKelvin(terms, thermo));
            std::printf("  ベイクの坂 %-42s %s(上る %u〜%u K・成分しだい %u〜%u K)\n", table.ruleNames[id].c_str(),
                        baked.state == RuleSlopeState::Uphill ? "上る" : "下る", baked.uphill.lowKelvin,
                        baked.uphill.highKelvin, baked.nearBalance.lowKelvin, baked.nearBalance.highKelvin);
        }
    }

    // --- (2) 毎刻みの数え上げ ---

    struct ConductionTally {
        uint64_t faces = 0;          // 流れが 0 でない面(延べ)
        uint64_t uphill = 0;         // 温度の低い側から高い側へ流れた面(坂を上る。0 のはず)
        uint64_t asymmetric = 0;     // 両側から見た流れが逆向きの同じ量でない面(0 のはず)
        uint64_t zeroKelvin = 0;     // 0 K のセルに接する面(σ を数えない)
        double production = 0.0;     // Σ σ(J/K)
        double minProduction = 0.0;  // 面の σ の最小(負なら坂を上った)
    };

    struct RuleTally {
        uint64_t events = 0;          // ξ > 0 のセル × 刻み
        uint64_t uphillEvents = 0;    // A < 0(活量込み)
        uint64_t standardUphill = 0;  // ΔG° > 0
        double extent = 0.0;          // mol
        double uphillExtent = 0.0;    // mol
        double production = 0.0;      // Σ σ(J/K。負の分も含む)
        double minKelvin = 1.0e9;     // 坂を上った温度の範囲
        double maxKelvin = 0.0;
    };

    struct BurnGauge {
        uint64_t initialCellulose = 0;   // µmol
        uint64_t celluloseGoneTick = 0;  // セルロースが初めの 1 % 以下になった刻み(0 = まだ)
        uint32_t maxTemperature = 0;     // mK(全部のセル・全部の刻み)
        uint64_t maxTemperatureTick = 0;
        uint64_t hotCellsPeak = 0;  // 1000 K 以上のセルの数の最大
        uint64_t hotCellsPeakTick = 0;
        uint64_t lastHotTick = 0;  // 1000 K 以上のセルがあった最後の刻み

        // --- 最後の刻みの物質量(µmol。FINAL_SPECIES の順)---
        std::array<uint64_t, 5> finalAmounts{};
    };

    constexpr std::array<std::string_view, 5> FINAL_SPECIES = {"cellulose", "carbon", "oxygen", "carbon_monoxide",
                                                               "carbon_dioxide"};

    struct Tally {
        ConductionTally conduction;
        std::vector<RuleTally> rules;
        uint64_t replayedCells = 0;     // なぞったセル(延べ)
        uint64_t replayMismatches = 0;  // なぞった結果が S(t + 1) と違うセル(0 のはず)
        uint64_t energyMismatches = 0;  // エネルギーの合計が保存されない刻み(0 のはず)
        BurnGauge burn;
        SlopeGaugeTally gauge;  // 開発ビルドの坂の計器(整数。T-0518)
    };

    double Kelvin(uint32_t milliKelvin) {
        return static_cast<double>(milliKelvin) * MILLI;
    }

    // 1 つの面(a → b が正)を数える
    void CountFace(const reaction::HcThermalCache& a, const reaction::HcThermalCache& b, ConductionTally& tally) {
        const int64_t flow = reaction::HcFaceFlow(a, b);
        if (flow != -reaction::HcFaceFlow(b, a))
            tally.asymmetric += 1;

        if (flow == 0)
            return;

        tally.faces += 1;
        const bool downhill = flow > 0 ? a.temperature > b.temperature : a.temperature < b.temperature;
        if (!downhill)
            tally.uphill += 1;

        if (a.temperature == 0 || b.temperature == 0) {
            tally.zeroKelvin += 1;

            return;
        }

        const double sigma = static_cast<double>(flow) * MILLI *
                             (1.0 / Kelvin(b.temperature) - 1.0 / Kelvin(a.temperature));
        tally.production += sigma;
        tally.minProduction = std::min(tally.minProduction, sigma);
    }

    void CountConduction(std::span<const reaction::HcThermalCache> caches, ConductionTally& tally) {
        constexpr uint32_t LAST = PROBE_GRID_SIZE - 1;
        for (uint32_t z = 0; z < PROBE_GRID_SIZE; ++z) {
            for (uint32_t y = 0; y < PROBE_GRID_SIZE; ++y) {
                for (uint32_t x = 0; x < PROBE_GRID_SIZE; ++x) {
                    const reaction::HcThermalCache& self = caches[ProbeCellIndex(x, y, z)];
                    if (x < LAST)
                        CountFace(self, caches[ProbeCellIndex(x + 1, y, z)], tally);
                    if (y < LAST)
                        CountFace(self, caches[ProbeCellIndex(x, y + 1, z)], tally);
                    if (z < LAST)
                        CountFace(self, caches[ProbeCellIndex(x, y, z + 1)], tally);
                }
            }
        }
    }

    // セルの伝導の後の状態(ProbeStepCell の前半と同じ。格子の外の面は自分 = 断熱)
    reaction::RxCell ConductedCell(std::span<const reaction::RxCell> cells,
                                   std::span<const reaction::HcThermalCache> caches, uint32_t index) {
        const uint32_t x = index % PROBE_GRID_SIZE;
        const uint32_t y = (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE;
        const uint32_t z = index / PROBE_SLICE_CELL_COUNT;
        const reaction::HcThermalCache& self = caches[index];
        const auto flow = [&](bool inside, uint32_t nx, uint32_t ny, uint32_t nz) {
            return inside ? reaction::HcFaceFlow(self, caches[ProbeCellIndex(nx, ny, nz)]) : int64_t{0};
        };

        constexpr uint32_t LAST = PROBE_GRID_SIZE - 1;
        const int64_t outflow = flow(x > 0, x - 1, y, z) + flow(x < LAST, x + 1, y, z) + flow(y > 0, x, y - 1, z) +
                                flow(y < LAST, x, y + 1, z) + flow(z > 0, x, y, z - 1) + flow(z < LAST, x, y, z + 1);
        reaction::RxCell conducted = cells[index];
        conducted.energy -= outflow;

        return conducted;
    }

    bool SameSpecies(const reaction::RxCell& a, const reaction::RxCell& b) {
        if (a.speciesCount != b.speciesCount)
            return false;

        for (uint32_t i = 0; i < a.speciesCount; ++i) {
            if (a.species[i] != b.species[i] || a.amounts[i] != b.amounts[i])
                return false;
        }

        return true;
    }

    // 刻みの核(RxStepCellWait)と同じ手順で、規則ごとの進行度(取り合いの後・入りきらない規則を待たせた後)を出す
    struct Replayed {
        reaction::RxCandidates extents{};
        reaction::RxThermal thermal{};
        reaction::RxCell cell{};
    };

    Replayed ReplayReaction(const ReactionTableView& view, const reaction::RxCell& conducted, uint64_t tick,
                            uint64_t changedMark, uint32_t index) {
        const uint64_t seed = ProbeWorldSeed();
        const uint64_t mark = ProbeChangeMark(tick);
        Replayed replayed;
        replayed.thermal = reaction::RxComputeThermal(view, conducted);
        replayed.cell = conducted;

        const uint32_t kelvin = static_cast<uint32_t>(replayed.thermal.temperature) /
                                static_cast<uint32_t>(fx::MILLIKELVIN_PER_KELVIN);
        const uint32_t tableKelvin = std::min(kelvin, reaction::RX_RATE_TABLE_KELVINS - 1);
        const reaction::RxWaitCandidates collected = reaction::RxCollectCandidatesWait(
            view, conducted, tableKelvin, reaction::RxWaitSeed(seed, changedMark, index), mark - changedMark,
            reaction::RxSelectSeed(seed, mark, index));
        if (collected.candidates.count == 0)
            return replayed;

        reaction::RxCandidates resolved = reaction::RxResolveContention(
            view, conducted, replayed.thermal, collected.candidates, reaction::RxRandomSeed(seed, mark, index));
        replayed.cell = reaction::RxApplyExtentsHeld(view, conducted, resolved).cell;
        if (reaction::RxApplyExtentsChecked(view, conducted, resolved).overflowed != 0)
            resolved = reaction::RxHoldNewSpeciesRules(view, conducted, resolved);

        replayed.extents = resolved;

        return replayed;
    }

    // 気体の活量(分圧 / 1 bar)。固体は 1。0 の量は 1 µmol と置く
    double Activity(const SpeciesThermo& species, const reaction::RxCell& cell, uint32_t speciesId, double kelvin) {
        if (species.condensed)
            return 1.0;

        uint64_t amount = 1;
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            if (cell.species[i] == speciesId)
                amount = std::max<uint64_t>(cell.amounts[i], 1);
        }

        return static_cast<double>(amount) * MICRO * GAS_CONSTANT * kelvin / (CELL_VOLUME * STANDARD_PRESSURE);
    }

    void CountRule(const ReactionTableView& view, std::span<const SpeciesThermo> thermo, const reaction::RxCell& cell,
                   double kelvin, uint32_t ruleId, uint64_t extentMicromoles, RuleTally& tally) {
        const std::vector<NetTerm> terms = NetTerms(view.Rule(ruleId));
        if (!HasAllEntropies(terms, thermo))
            return;

        if (kelvin <= 0.0)
            return;

        const double standard = StandardGibbs(terms, thermo, kelvin);
        double gibbs = standard;
        for (const NetTerm& term : terms)
            gibbs += term.coefficient * GAS_CONSTANT * kelvin *
                     std::log(Activity(thermo[term.species], cell, term.species, kelvin));

        const double extent = static_cast<double>(extentMicromoles) * MICRO;
        const double sigma = extent * -gibbs / kelvin;
        tally.events += 1;
        tally.extent += extent;
        tally.production += sigma;
        tally.standardUphill += standard > 0.0 ? 1 : 0;
        if (gibbs <= 0.0)
            return;

        tally.uphillEvents += 1;
        tally.uphillExtent += extent;
        tally.minKelvin = std::min(tally.minKelvin, kelvin);
        tally.maxKelvin = std::max(tally.maxKelvin, kelvin);
    }

    void CountReactions(const BakedReactionTable& table, std::span<const SpeciesThermo> thermo,
                        std::span<const reaction::RxCell> current, std::span<const reaction::HcThermalCache> caches,
                        std::span<const reaction::RxCell> next, std::span<const uint64_t> marks, uint64_t tick,
                        Tally& tally) {
        const ReactionTableView view = table.View();
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
            if (SameSpecies(current[index], next[index]))
                continue;  // 反応は成分を変える。成分が同じなら進んだ規則は無い(正味 0 になる組は試験の表に無い)

            const uint32_t block = ProbeBlockOfCell(
                index % PROBE_GRID_SIZE, (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE, index / PROBE_SLICE_CELL_COUNT);
            const reaction::RxCell conducted = ConductedCell(current, caches, index);
            const Replayed replayed = ReplayReaction(view, conducted, tick, marks[block], index);
            tally.replayedCells += 1;
            if (!reaction::RxSameCell(replayed.cell, next[index]))
                tally.replayMismatches += 1;

            const double kelvin = static_cast<double>(replayed.thermal.temperature) * MILLI;
            for (uint32_t c = 0; c < replayed.extents.count; ++c) {
                if (replayed.extents.extents[c] == 0)
                    continue;

                const uint32_t ruleId = replayed.extents.rules[c];
                CountRule(view, thermo, conducted, kelvin, ruleId, replayed.extents.extents[c], tally.rules[ruleId]);
            }
        }
    }

    // --- (3) 燃え方の計器 ---

    uint64_t TotalAmount(std::span<const reaction::RxCell> cells, uint32_t speciesId) {
        uint64_t total = 0;
        for (const reaction::RxCell& cell : cells)
            total += ProbeViewAmount(cell, speciesId);

        return total;
    }

    constexpr uint32_t HOT_MILLIKELVIN = 1000000;  // 炎の目安(遊びの指標 sealed_fire_out と同じ)

    void GaugeBurn(const BakedReactionTable& table, std::span<const reaction::RxCell> cells,
                   std::span<const reaction::HcThermalCache> caches, uint64_t tick, BurnGauge& gauge) {
        uint64_t hotCells = 0;
        for (const reaction::HcThermalCache& cache : caches) {
            hotCells += cache.temperature >= HOT_MILLIKELVIN ? 1 : 0;
            if (cache.temperature > gauge.maxTemperature) {
                gauge.maxTemperature = cache.temperature;
                gauge.maxTemperatureTick = tick;
            }
        }

        if (hotCells > gauge.hotCellsPeak) {
            gauge.hotCellsPeak = hotCells;
            gauge.hotCellsPeakTick = tick;
        }

        gauge.lastHotTick = hotCells > 0 ? tick : gauge.lastHotTick;
        if (gauge.celluloseGoneTick == 0 &&
            TotalAmount(cells, table.SpeciesId("cellulose")) * 100 <= gauge.initialCellulose)
            gauge.celluloseGoneTick = tick;
    }

    // --- 開発ビルドの坂の計器(整数)を倍精度と比べる(T-0518)---

    // 合成のセル(boudouard の物質だけ)の ΔG を、計器(整数)と倍精度(活量込み・1 K に切り捨てた温度)で比べる
    void CheckCellGibbs(const BakedReactionTable& table, std::span<const SpeciesThermo> thermo) {
        const SlopeGaugeTable gauge = MakeSlopeGaugeTable(table);
        const uint32_t carbon = table.SpeciesId("carbon");
        const uint32_t monoxide = table.SpeciesId("carbon_monoxide");
        const uint32_t dioxide = table.SpeciesId("carbon_dioxide");
        const auto found = rng::find(table.ruleNames, std::string("boudouard"));
        EXPECT(found != table.ruleNames.end() && carbon != 0 && monoxide != 0 && dioxide != 0);
        if (found == table.ruleNames.end())
            return;

        const auto ruleId = static_cast<uint32_t>(found - table.ruleNames.begin());
        EXPECT(table.speciesCondensed.size() == table.species.size());
        for (size_t id = 1; id < thermo.size() && id < table.speciesCondensed.size(); ++id)
            EXPECT((table.speciesCondensed[id] != 0) == thermo[id].condensed);

        constexpr uint64_t MANY = 10000000;  // 10 mol
        constexpr uint64_t FEW = 1;          // 1 µmol
        constexpr uint32_t HOT_KELVIN = 1700;
        constexpr uint32_t WARM_KELVIN = 900;
        struct Case {
            uint64_t monoxideAmount;
            uint64_t dioxideAmount;
            uint32_t kelvin;
            bool uphill;
        };

        // CO が多く CO2 が少ないと 1700 K でも坂を上る(T-0491 の行き過ぎ)。逆なら 900 K でも下る
        constexpr std::array<Case, 3> CASES = {Case{MANY, FEW, HOT_KELVIN, true}, Case{FEW, MANY, HOT_KELVIN, false},
                                               Case{FEW, MANY, WARM_KELVIN, false}};
        const std::vector<NetTerm> terms = NetTerms(table.View().Rule(ruleId));
        for (const Case& c : CASES) {
            reaction::RxCell cell{};
            cell.speciesCount = 3;
            cell.species[0] = carbon;
            cell.amounts[0] = MANY;
            cell.species[1] = monoxide;
            cell.amounts[1] = c.monoxideAmount;
            cell.species[2] = dioxide;
            cell.amounts[2] = c.dioxideAmount;

            int64_t gibbs = 0;
            EXPECT(CellRuleGibbs(gauge, ruleId, cell, c.kelvin, gibbs));
            const auto kelvin = static_cast<double>(c.kelvin);
            double expected = StandardGibbs(terms, thermo, kelvin);
            for (const NetTerm& term : terms)
                expected += term.coefficient * GAS_CONSTANT * kelvin *
                            std::log(Activity(thermo[term.species], cell, term.species, kelvin));

            const double integer = static_cast<double>(gibbs) * MILLI;
            std::printf("  計器の ΔG(boudouard・%u K・CO %llu・CO2 %llu µmol): 整数 %.3f J/mol・倍精度 %.3f J/mol\n",
                        c.kelvin, static_cast<unsigned long long>(c.monoxideAmount),
                        static_cast<unsigned long long>(c.dioxideAmount), integer, expected);
            EXPECT(std::abs(integer - expected) < 1.0);
            EXPECT((gibbs > 0) == c.uphill);
        }
    }

    // 木箱の数え上げ: 計器(整数・1 K に切り捨て)と倍精度(温度はそのまま)で、なぞり・進行度・坂を上った回が同じ
    void CheckGaugeAgainstDouble(const BakedReactionTable& table, const Tally& tally) {
        const SlopeGaugeTally& gauge = tally.gauge;
        EXPECT(gauge.replayMismatches == 0);
        EXPECT(gauge.replayedCells == tally.replayedCells);
        EXPECT(gauge.rules.size() == tally.rules.size());
        for (const std::string& line : SlopeGaugeReport(table, gauge))
            std::printf("  %s\n", line.c_str());

        for (uint32_t id = 0; id < gauge.rules.size() && id < tally.rules.size(); ++id) {
            const SlopeGaugeRuleTally& integer = gauge.rules[id];
            const RuleTally& reference = tally.rules[id];
            EXPECT(integer.events == reference.events);
            // 倍精度は刻みごとに mol で足すので丸めの差が残る(整数は µmol のまま足す)
            constexpr double EXTENT_RELATIVE_TOLERANCE = 1.0e-9;
            const double extent = static_cast<double>(integer.extentMicromoles) * MICRO;
            EXPECT(std::abs(extent - reference.extent) <= EXTENT_RELATIVE_TOLERANCE * std::max(1.0, reference.extent));
            std::printf("  計器 %-42s 坂を上った 整数 %llu 回・倍精度 %llu 回\n", table.ruleNames[id].c_str(),
                        static_cast<unsigned long long>(integer.uphillEvents),
                        static_cast<unsigned long long>(reference.uphillEvents));
            EXPECT(integer.uphillEvents == reference.uphillEvents);
        }
    }

    // --- 木箱を進める ---

    // 遊びの指標 sealed_fire_out と同じ: 木箱の壁 (28, 32, 32) を刻み 0 につつく
    std::vector<ProbeCommand> MakeCommands() {
        return {MakePokeCommand(0, 0, 28, 32, PROBE_VIEW_Z)};
    }

    Tally RunCrate(const BakedReactionTable& table, std::span<const SpeciesThermo> thermo, uint64_t ticks) {
        const std::vector<ProbeCommand> commands = MakeCommands();
        const SlopeGaugeTable gaugeTable = MakeSlopeGaugeTable(table);
        ProbeReference reference(table);
        Tally tally;
        tally.rules.resize(table.rules.size());
        tally.burn.initialCellulose = TotalAmount(reference.State(0), table.SpeciesId("cellulose"));
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            const std::vector<uint64_t> marks(reference.ChangedMarks().begin(), reference.ChangedMarks().end());
            const std::span<const reaction::RxCell> current = reference.State(tick);
            const std::span<const reaction::HcThermalCache> caches = reference.Caches(tick);
            const uint64_t energyBefore = ProbeEnergySum(current);
            CountConduction(caches, tally.conduction);

            reference.Advance(tick, commands);
            const std::span<const reaction::RxCell> next = reference.State(tick + 1);
            CountReactions(table, thermo, current, caches, next, marks, tick, tally);
            CountProbeSlopeTick(table, gaugeTable, current, caches, next, marks, tick, tally.gauge);
            if (ProbeEnergySum(next) != energyBefore + reference.SourceEnergy())
                tally.energyMismatches += 1;

            GaugeBurn(table, next, reference.Caches(tick + 1), tick + 1, tally.burn);
            if ((tick + 1) % PROGRESS_TICKS == 0) {
                std::printf("  刻み %llu: 最高温度 %.1f K・最後に 1000 K 以上があった刻み %llu\n",
                            static_cast<unsigned long long>(tick + 1), Kelvin(tally.burn.maxTemperature),
                            static_cast<unsigned long long>(tally.burn.lastHotTick));
                std::fflush(stdout);  // 長い --burn の途中経過をログへ(パイプは溜めるので)
            }
        }

        for (size_t i = 0; i < FINAL_SPECIES.size(); ++i)
            tally.burn.finalAmounts[i] = TotalAmount(reference.State(ticks), table.SpeciesId(FINAL_SPECIES[i]));

        return tally;
    }

    void PrintTally(const BakedReactionTable& table, const Tally& tally, uint64_t ticks) {
        const ConductionTally& conduction = tally.conduction;
        std::printf("(2) %llu 刻みの数え上げ\n", static_cast<unsigned long long>(ticks));
        std::printf(
            "  伝導: 流れのある面 %llu・坂を上った面 %llu・非対称 %llu・0 K に接する %llu・Σσ %.6e J/K・面の σ の最小 "
            "%.3e\n",
            static_cast<unsigned long long>(conduction.faces), static_cast<unsigned long long>(conduction.uphill),
            static_cast<unsigned long long>(conduction.asymmetric),
            static_cast<unsigned long long>(conduction.zeroKelvin), conduction.production, conduction.minProduction);
        std::printf("  反応: なぞったセル %llu・食い違い %llu・エネルギーが合わない刻み %llu\n",
                    static_cast<unsigned long long>(tally.replayedCells),
                    static_cast<unsigned long long>(tally.replayMismatches),
                    static_cast<unsigned long long>(tally.energyMismatches));
        for (uint32_t id = 0; id < tally.rules.size(); ++id) {
            const RuleTally& rule = tally.rules[id];
            std::printf("  %-42s 回 %8llu・ξ %.4e mol・Σσ %.4e J/K・標準で上る %llu・活量込みで上る %llu(ξ %.4e mol",
                        table.ruleNames[id].c_str(), static_cast<unsigned long long>(rule.events), rule.extent,
                        rule.production, static_cast<unsigned long long>(rule.standardUphill),
                        static_cast<unsigned long long>(rule.uphillEvents), rule.uphillExtent);
            if (rule.uphillEvents > 0)
                std::printf("・%.0f〜%.0f K", rule.minKelvin, rule.maxKelvin);

            std::printf(")\n");
        }

        const BurnGauge& burn = tally.burn;
        std::printf(
            "(3) 燃え方: 最高温度 %.1f K(刻み %llu)・1000 K 以上のセルの最大 %llu(刻み %llu)・最後に 1000 K "
            "以上があった刻み %llu・"
            "セルロースが 1 %% 以下になった刻み %llu\n",
            Kelvin(burn.maxTemperature), static_cast<unsigned long long>(burn.maxTemperatureTick),
            static_cast<unsigned long long>(burn.hotCellsPeak), static_cast<unsigned long long>(burn.hotCellsPeakTick),
            static_cast<unsigned long long>(burn.lastHotTick), static_cast<unsigned long long>(burn.celluloseGoneTick));
        std::printf("    最後の物質量(mol):");
        for (size_t i = 0; i < FINAL_SPECIES.size(); ++i)
            std::printf(" %.*s %.2f", static_cast<int>(FINAL_SPECIES[i].size()), FINAL_SPECIES[i].data(),
                        static_cast<double>(burn.finalAmounts[i]) * MICRO);

        std::printf("(初めのセルロース %.2f)\n", static_cast<double>(burn.initialCellulose) * MICRO);
    }

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<size_t>(argc));
    const bool burn = args.size() >= 2 && std::string_view(args[1]) == "--burn";
    const uint64_t ticks = burn ? (args.size() >= 3 ? std::strtoull(args[2], nullptr, 10) : BURN_TICKS) : SHORT_TICKS;

    const ReactionTableDefinition definition = MakeCombustionTestTable();
    const auto table = BakeReactionTable(definition);
    if (!table) {
        std::printf("FAILED: 表を作れない: %s\n", table.error().c_str());

        return 1;
    }

    // --- 試験の表のエントロピー(任意の欄)が読めている: 仮の魔素のほかは全部ある ---
    const std::vector<SpeciesThermo> thermo = MakeThermo(definition, *table);
    for (size_t id = 1; id < thermo.size(); ++id)
        EXPECT(thermo[id].hasEntropy || table->speciesNames[id] == "mana_test");

    const std::vector<UphillRange> ranges = PrintStaticList(*table, thermo);
    std::fflush(stdout);
    CheckCelluloseSpread(*table, thermo, ranges);
    CheckBakedSlopes(*table, thermo, ranges);
    CheckCellGibbs(*table, thermo);
    EXPECT(ranges.size() == table->rules.size());

    const auto begin = std::chrono::steady_clock::now();
    const Tally tally = RunCrate(*table, thermo, ticks);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    PrintTally(*table, tally, ticks);
    CheckGaugeAgainstDouble(*table, tally);
    std::printf("CPU の時間 %.1f 秒(%.1f ms/刻み)\n", seconds, seconds * 1000.0 / static_cast<double>(ticks));

    // --- 次へ進む条件(19 §5 の段 0): 伝導で坂を上る流れが 0・なぞりが一致(規則ごとの数が本物)・燃えている ---
    EXPECT(tally.conduction.uphill == 0);
    EXPECT(tally.conduction.asymmetric == 0);
    EXPECT(tally.conduction.faces > 0);
    EXPECT(tally.replayMismatches == 0);
    EXPECT(tally.replayedCells > 0);
    EXPECT(tally.energyMismatches == 0);
    EXPECT(tally.burn.maxTemperature > HOT_MILLIKELVIN);

    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("すべて通過\n");

    return 0;
}
