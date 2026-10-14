// slope_gauge.cpp — 開発ビルドの坂の計器(slope_gauge.h。T-0518・ADR-0230)。整数だけで数える(04 §4)。
// なぞりの手順は CPU リファレンスの刻みの核(probe_world.hlsli の RxStepCellWait の呼び方)と同じで、
// 試験 slope_check(倍精度の物差し。T-0491)の数え上げと、進行度・坂を上った回が同じことを確かめている。
#include "sim/slope_gauge.h"

#include <algorithm>
#include <format>
#include <limits>

#include "sim/probe_sim.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    using namespace bicameral::fx;
    using namespace bicameral::reaction;

    namespace {

        constexpr uint32_t Q32_SHIFT = 32;
        constexpr int64_t NANOJOULES_PER_MILLIJOULE = 1000000;
        constexpr uint64_t FEMTO_SCALE = 1000000000000000;  // µmol × nJ = 10^-15 mol·J

        // --- 活量 ---

        // 物質 species の ln a(Q32)。凝縮相は 0、気体は ln(n R T / (V p°))(0 の量は 1 µmol)
        int64_t LogActivity(const SlopeGaugeTable& gauge, const RxCell& cell, uint32_t species, int64_t logGasEnergy) {
            if (species < gauge.condensed.size() && gauge.condensed[species] != 0)
                return 0;

            uint64_t amount = 1;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                if (cell.species[i] == species)
                    amount = std::max<uint64_t>(cell.amounts[i], 1);
            }

            return FxLnU64(amount) + logGasEnergy - gauge.gasLogOffset;
        }

        uint64_t SaturatingAdd(uint64_t a, uint64_t b) {
            return a > std::numeric_limits<uint64_t>::max() - b ? std::numeric_limits<uint64_t>::max() : a + b;
        }

        // ξ[µmol] × ΔG[mJ/mol] = nJ(飽和する)
        uint64_t WorkNanojoules(uint64_t extentMicromoles, int64_t gibbs) {
            const auto perMole = static_cast<uint64_t>(gibbs);
            if (perMole != 0 && extentMicromoles > std::numeric_limits<uint64_t>::max() / perMole)
                return std::numeric_limits<uint64_t>::max();

            return extentMicromoles * perMole;
        }

        void NoteUphill(SlopeGaugeRuleTally& rule, uint64_t extent, int64_t gibbs, bool standardUphill, uint32_t kelvin,
                        uint64_t tick) {
            rule.uphillLowKelvin = rule.uphillEvents == 0 ? kelvin : std::min(rule.uphillLowKelvin, kelvin);
            rule.uphillHighKelvin = std::max(rule.uphillHighKelvin, kelvin);
            rule.firstUphillTick = rule.uphillEvents == 0 ? tick : rule.firstUphillTick;
            rule.lastUphillTick = tick;
            rule.uphillEvents += 1;
            rule.uphillExtentMicromoles = SaturatingAdd(rule.uphillExtentMicromoles, extent);
            rule.uphillWorkNanojoules = SaturatingAdd(rule.uphillWorkNanojoules, WorkNanojoules(extent, gibbs));
            if (standardUphill)
                return;

            rule.compositionUphillEvents += 1;
            rule.compositionUphillExtentMicromoles = SaturatingAdd(rule.compositionUphillExtentMicromoles, extent);
        }

        // --- 仮の世界のなぞり(probe_world.hlsli の ProbeStepCell と同じ手順)---

        // セルの伝導の後の状態(格子の外の面は自分 = 断熱)
        RxCell ConductedCell(std::span<const RxCell> cells, std::span<const HcThermalCache> caches, uint32_t index) {
            const uint32_t x = index % PROBE_GRID_SIZE;
            const uint32_t y = (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE;
            const uint32_t z = index / PROBE_SLICE_CELL_COUNT;
            const HcThermalCache& self = caches[index];
            const auto flow = [&](bool inside, uint32_t nx, uint32_t ny, uint32_t nz) {
                return inside ? HcFaceFlow(self, caches[ProbeCellIndex(nx, ny, nz)]) : int64_t{0};
            };

            constexpr uint32_t LAST = PROBE_GRID_SIZE - 1;
            const int64_t outflow = flow(x > 0, x - 1, y, z) + flow(x < LAST, x + 1, y, z) + flow(y > 0, x, y - 1, z) +
                                    flow(y < LAST, x, y + 1, z) + flow(z > 0, x, y, z - 1) +
                                    flow(z < LAST, x, y, z + 1);
            RxCell conducted = cells[index];
            conducted.energy -= outflow;

            return conducted;
        }

        bool SameSpecies(const RxCell& a, const RxCell& b) {
            if (a.speciesCount != b.speciesCount)
                return false;

            for (uint32_t i = 0; i < a.speciesCount; ++i) {
                if (a.species[i] != b.species[i] || a.amounts[i] != b.amounts[i])
                    return false;
            }

            return true;
        }

        struct Replayed {
            RxCandidates extents{};
            RxCell cell{};
            uint32_t kelvin = 0;  // 1 K に切り捨て、速さの表の端で止めた温度
        };

        // 刻みの核(RxStepCellWait)と同じ手順で、規則ごとの進行度(取り合いの後・入りきらない規則を待たせた後)を出す
        Replayed ReplayReaction(const ReactionTableView& view, const RxCell& conducted, uint64_t tick,
                                uint64_t changedMark, uint32_t index) {
            const uint64_t seed = ProbeWorldSeed();
            const uint64_t mark = ProbeChangeMark(tick);
            const RxThermal thermal = RxComputeThermal(view, conducted);
            Replayed replayed;
            replayed.cell = conducted;

            const uint32_t kelvin = static_cast<uint32_t>(thermal.temperature) /
                                    static_cast<uint32_t>(MILLIKELVIN_PER_KELVIN);
            replayed.kelvin = std::min(kelvin, RX_RATE_TABLE_KELVINS - 1);
            const RxWaitCandidates collected = RxCollectCandidatesWait(
                view, conducted, replayed.kelvin, RxWaitSeed(seed, changedMark, index), mark - changedMark,
                RxSelectSeed(seed, mark, index));
            if (collected.candidates.count == 0)
                return replayed;

            RxCandidates resolved = RxResolveContention(view, conducted, thermal, collected.candidates,
                                                        RxRandomSeed(seed, mark, index));
            replayed.cell = RxApplyExtentsHeld(view, conducted, resolved).cell;
            if (RxApplyExtentsChecked(view, conducted, resolved).overflowed != 0)
                resolved = RxHoldNewSpeciesRules(view, conducted, resolved);

            replayed.extents = resolved;

            return replayed;
        }

        std::string Micromoles(uint64_t value) {
            return std::format("{}.{:06} mol", value / 1000000, value % 1000000);
        }

    }  // namespace

    SlopeGaugeTable MakeSlopeGaugeTable(const BakedReactionTable& baked, uint64_t cellStandardJoules) {
        SlopeGaugeTable gauge;
        gauge.rules.reserve(baked.rules.size());
        for (uint32_t ruleId = 0; ruleId < baked.rules.size(); ++ruleId) {
            gauge.rules.push_back(
                {.terms = RuleNetTerms(baked.rules[ruleId]), .standardGibbs = RuleStandardGibbs(baked, ruleId)});
        }

        gauge.condensed = baked.speciesCondensed;
        gauge.gasLogOffset = FxLnU64(std::max<uint64_t>(cellStandardJoules, 1)) + FxLnU64(FEMTO_SCALE);

        return gauge;
    }

    bool CellRuleGibbs(const SlopeGaugeTable& gauge, uint32_t ruleId, const RxCell& cell, uint32_t kelvin,
                       int64_t& gibbs) {
        const SlopeGaugeTable::Rule& rule = gauge.rules[ruleId];
        if (rule.standardGibbs.empty() || kelvin == 0 || kelvin >= rule.standardGibbs.size())
            return false;

        // R T(nJ/mol)と、その ln(気体の活量の項)
        const auto gasEnergy = static_cast<int64_t>(GAS_CONSTANT_NANOJOULES_PER_MOLE_KELVIN * kelvin);
        const int64_t logGasEnergy = FxLnU64(static_cast<uint64_t>(gasEnergy));
        int64_t activityNanojoules = 0;
        for (const RuleNetTerm& term : rule.terms) {
            const int64_t logActivity = LogActivity(gauge, cell, term.species, logGasEnergy);
            activityNanojoules += term.coefficient * FxMulShiftS64(gasEnergy, logActivity, Q32_SHIFT);
        }

        gibbs = rule.standardGibbs[kelvin] + (activityNanojoules / NANOJOULES_PER_MILLIJOULE);

        return true;
    }

    bool CountCellSlope(const SlopeGaugeTable& gauge, const RxCell& conducted, uint32_t kelvin,
                        const RxCandidates& extents, uint64_t tick, SlopeGaugeTally& tally) {
        tally.rules.resize(gauge.rules.size());
        bool uphillFound = false;
        for (uint32_t c = 0; c < extents.count; ++c) {
            const uint64_t extent = extents.extents[c];
            const uint32_t ruleId = extents.rules[c];
            if (extent == 0 || ruleId >= gauge.rules.size())
                continue;

            int64_t gibbs = 0;
            if (!CellRuleGibbs(gauge, ruleId, conducted, kelvin, gibbs)) {
                tally.unevaluatedEvents += 1;
                continue;
            }

            SlopeGaugeRuleTally& rule = tally.rules[ruleId];
            rule.events += 1;
            rule.extentMicromoles = SaturatingAdd(rule.extentMicromoles, extent);
            if (gibbs <= 0)
                continue;

            const bool standardUphill = gauge.rules[ruleId].standardGibbs[kelvin] > 0;
            NoteUphill(rule, extent, gibbs, standardUphill, kelvin, tick);
            uphillFound = true;
        }

        return uphillFound;
    }

    void CountProbeSlopeTick(const BakedReactionTable& table, const SlopeGaugeTable& gauge,
                             std::span<const RxCell> current, std::span<const HcThermalCache> caches,
                             std::span<const RxCell> next, std::span<const uint64_t> changedMarks, uint64_t tick,
                             SlopeGaugeTally& tally) {
        const ReactionTableView view = table.View();
        tally.rules.resize(gauge.rules.size());
        bool uphillFound = false;
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
            // 反応は成分を変える。成分が同じなら進んだ規則は無い(正味 0 になる組は数えない)
            if (SameSpecies(current[index], next[index]))
                continue;

            const uint32_t block = ProbeBlockOfCell(
                index % PROBE_GRID_SIZE, (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE, index / PROBE_SLICE_CELL_COUNT);
            const RxCell conducted = ConductedCell(current, caches, index);
            const Replayed replayed = ReplayReaction(view, conducted, tick, changedMarks[block], index);
            tally.replayedCells += 1;
            if (!RxSameCell(replayed.cell, next[index]))
                tally.replayMismatches += 1;

            uphillFound = CountCellSlope(gauge, conducted, replayed.kelvin, replayed.extents, tick, tally) ||
                          uphillFound;
        }

        tally.ticks += 1;
        tally.uphillTicks += uphillFound ? 1 : 0;
    }

    std::vector<std::string> SlopeGaugeReport(const BakedReactionTable& table, const SlopeGaugeTally& tally) {
        std::vector<std::string> lines;
        uint64_t uphillEvents = 0;
        for (const SlopeGaugeRuleTally& rule : tally.rules)
            uphillEvents += rule.uphillEvents;

        lines.push_back(std::format("坂の計器: {} 刻み・坂を上った反応 {} 回({} 刻み)・なぞったセル {}", tally.ticks,
                                    uphillEvents, tally.uphillTicks, tally.replayedCells));
        for (uint32_t ruleId = 0; ruleId < tally.rules.size(); ++ruleId) {
            const SlopeGaugeRuleTally& rule = tally.rules[ruleId];
            if (rule.uphillEvents == 0)
                continue;

            lines.push_back(std::format(
                "坂の計器: 規則 {} が {} 回坂を上った(ξ {}・進んだ全部の {} 回 {} のうち)。{}〜{} K・刻み {}〜{}・"
                "上らせたエネルギー {} µJ。そのうち成分しだい(標準の ΔG° では下る){} 回 ξ {}",
                table.ruleNames[ruleId], rule.uphillEvents, Micromoles(rule.uphillExtentMicromoles), rule.events,
                Micromoles(rule.extentMicromoles), rule.uphillLowKelvin, rule.uphillHighKelvin, rule.firstUphillTick,
                rule.lastUphillTick, rule.uphillWorkNanojoules / 1000, rule.compositionUphillEvents,
                Micromoles(rule.compositionUphillExtentMicromoles)));
        }

        if (tally.unevaluatedEvents != 0) {
            lines.push_back(
                std::format("坂の計器: 調べられない規則(標準エントロピーの無い物質を含む)が {} 回進んだ(数えていない)",
                            tally.unevaluatedEvents));
        }

        if (tally.replayMismatches != 0) {
            lines.push_back(
                std::format("坂の計器: なぞった反応が世界と {} セル食い違った(計器の不具合。数は信用できない)",
                            tally.replayMismatches));
        }

        return lines;
    }

}  // namespace bicameral::sim
