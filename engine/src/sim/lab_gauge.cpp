// lab_gauge.cpp — 実験室の計器と比べる段取り(lab_gauge.h)。GPU の箱を流すのは sim/lab_comparison。
#include "sim/lab_gauge.h"

#include <algorithm>
#include <format>
#include <limits>

namespace bicameral::sim {

    namespace {

        // 差を int64 に(溢れる差は端で止める。µmol の量はそこまで大きくならない)
        int64_t SignedDifference(uint64_t after, uint64_t before) {
            constexpr auto LIMIT = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
            if (after >= before)
                return static_cast<int64_t>(std::min(after - before, LIMIT));

            return -static_cast<int64_t>(std::min(before - after, LIMIT));
        }

        std::span<const Command> CommandsAt(std::span<const Command> commands, uint64_t tick) {
            const auto first = std::ranges::find_if(commands, [&](const Command& c) { return c.targetTick == tick; });
            const auto last = std::ranges::find_if(first, commands.end(),
                                                   [&](const Command& c) { return c.targetTick != tick; });

            return {first, last};
        }

        constexpr uint32_t FINE_BITS_PER_LEVEL = 3;  // 1 段細かいセルの量の単位は 8 分の 1(17 §1)

        // 葉のセル(本物のブロックの覆われていないセル)全部を枠の順 → 枠の中の番号の順に visit(位置, セル)
        template <typename Visit>
        void ForEachLabLeaf(const MultiresNest& nest, const Visit& visit) {
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                const multires::MrBlock& block = nest.blocks[slot];
                if (block.kind != multires::MR_BLOCK_REAL)
                    continue;

                for (uint32_t local = 0; local < multires::MR_BLOCK_CELLS; ++local) {
                    if (multires::MrIsCoveredCell(block, local))
                        continue;

                    const LabCellPosition position = {
                        .x = static_cast<uint32_t>(block.originX) + multires::MrCellX(local),
                        .y = static_cast<uint32_t>(block.originY) + multires::MrCellY(local),
                        .z = static_cast<uint32_t>(block.originZ) + multires::MrCellZ(local),
                        .level = static_cast<uint32_t>(block.level)};
                    visit(position, LoadNestCell(nest, slot, local));
                }
            }
        }

        // 葉 leaf と見るセル watched が重なるか: 細かい方の座標を粗い方のレベルに縮めて同じなら重なる
        bool Overlaps(const LabCellPosition& leaf, const lab::LabTarget& watched) {
            const uint32_t coarse = std::min(leaf.level, watched.level);
            const uint32_t leafShift = leaf.level - coarse;
            const uint32_t watchedShift = watched.level - coarse;

            return (leaf.x >> leafShift) == (watched.region.lowX >> watchedShift) &&
                   (leaf.y >> leafShift) == (watched.region.lowY >> watchedShift) &&
                   (leaf.z >> leafShift) == (watched.region.lowZ >> watchedShift);
        }

    }  // namespace

    // --- 計器 ---

    LabGaugeSample SampleLabGauge(const MultiresNest& nest, const BakedReactionTable& table, uint64_t tick,
                                  uint32_t cellWord, lab::LabBoxShape shape) {
        const size_t speciesCount = table.species.size();
        LabGaugeSample sample = {.tick = tick,
                                 .cell = cellWord,
                                 .cellAmounts = std::vector<uint64_t>(speciesCount, 0),
                                 .boxAmounts = std::vector<uint64_t>(speciesCount, 0)};
        sample.maxTemperatureMilliKelvin = std::numeric_limits<int32_t>::min();

        // --- 量は「数える細かさ」(箱の細かくできる段か、見るセルのレベルの深い方)の単位で足し、最後に µmol に直す。
        //     細かくしていない箱の箱のセルを見るなら細かさ 0 = 今まで通り(T-0350)---
        const lab::LabTarget watched = lab::LabCellTarget(cellWord);
        const uint32_t finest = std::max(watched.level, shape.refineBlocks == 0 ? 0u : lab::LAB_MAX_REFINE_LEVELS);
        std::vector<uint64_t> boxFine(speciesCount, 0);
        std::vector<uint64_t> cellFine(speciesCount, 0);
        int64_t weightedTemperature = 0;
        uint64_t watchedVolume = 0;

        // --- 葉のセル全部(枠の順 → 枠の中の番号の順。足し算なので順は結果に効かない)---
        ForEachLabLeaf(nest, [&](const LabCellPosition& leaf, const reaction::RxCell& cell) {
            const int32_t temperature = reaction::RxComputeThermal(table.View(), cell).temperature;
            sample.maxTemperatureMilliKelvin = std::max(sample.maxTemperatureMilliKelvin, temperature);
            const uint32_t boxShift = FINE_BITS_PER_LEVEL * (finest - leaf.level);

            // 見るセルとは、粗い方の座標に縮めて同じなら重なる(木のセルは入れ子)。重なった体積 = 細かい方のセル 1 つ
            const bool overlaps = Overlaps(leaf, watched);
            const uint32_t cellShift = FINE_BITS_PER_LEVEL * (finest - std::max(leaf.level, watched.level));
            if (overlaps) {
                weightedTemperature += int64_t{temperature} << cellShift;
                watchedVolume += uint64_t{1} << cellShift;
            }

            // --- 物質量(表に無い ID は数えない)---
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                if (cell.species[i] >= speciesCount)
                    continue;

                boxFine[cell.species[i]] += cell.amounts[i] << boxShift;
                if (overlaps)
                    cellFine[cell.species[i]] += cell.amounts[i] << cellShift;
            }
        });

        // --- µmol に直す(端数は切り捨て)---
        const uint32_t toMicromoles = FINE_BITS_PER_LEVEL * finest;
        for (size_t species = 0; species < speciesCount; ++species) {
            sample.boxAmounts[species] = boxFine[species] >> toMicromoles;
            sample.cellAmounts[species] = cellFine[species] >> toMicromoles;
        }

        if (watchedVolume > 0)
            sample.cellTemperatureMilliKelvin = static_cast<int32_t>(weightedTemperature /
                                                                     static_cast<int64_t>(watchedVolume));

        return sample;
    }

    // --- 細かいセルを見る(T-0350)---

    std::optional<LabLeafCell> LabLeafAt(const MultiresNest& nest, LabCellPosition position) {
        if (position.level > lab::LAB_MAX_REFINE_LEVELS)
            return std::nullopt;

        // 粗いレベルから: 最初に見つかった覆われていないセルが葉(その位置を含む)
        for (uint32_t level = 0; level <= position.level; ++level) {
            const uint32_t shift = position.level - level;
            const LabCellPosition coarse = {
                .x = position.x >> shift, .y = position.y >> shift, .z = position.z >> shift, .level = level};
            if (const auto cell = LabBoxFineCell(nest, coarse); cell)
                return LabLeafCell{.cell = *cell, .position = coarse};
        }

        return std::nullopt;
    }

    std::vector<LabSliceCell> LabFineSlice(const MultiresNest& nest, const BakedReactionTable& table,
                                           lab::LabBoxShape shape, uint32_t cellZ, uint32_t fineZ) {
        constexpr uint32_t SCALE = LAB_FINE_SLICE_SCALE;
        const uint32_t cellWidth = LAB_BOX_EDGE * shape.rootsX;
        const uint32_t cellHeight = LAB_BOX_EDGE * shape.rootsY;
        const uint32_t width = cellWidth * SCALE;
        std::vector<LabSliceCell> slice(size_t{width} * cellHeight * SCALE);
        const auto sliceOf = [&](const LabLeafCell& leaf) {
            return LabSliceCell{
                .temperatureMilliKelvin = reaction::RxComputeThermal(table.View(), leaf.cell).temperature,
                .level = leaf.position.level};
        };

        for (uint32_t y = 0; y < cellHeight; ++y) {
            for (uint32_t x = 0; x < cellWidth; ++x) {
                // --- 細かくしていない箱のセルは 4×4 とも同じ(根のセルを 1 回だけ引く)---
                const auto whole = LabLeafAt(nest, {.x = x, .y = y, .z = cellZ});
                for (uint32_t subY = 0; subY < SCALE; ++subY) {
                    for (uint32_t subX = 0; subX < SCALE; ++subX) {
                        const LabCellPosition fine = {.x = (x * SCALE) + subX,
                                                      .y = (y * SCALE) + subY,
                                                      .z = (cellZ * SCALE) + fineZ,
                                                      .level = lab::LAB_MAX_REFINE_LEVELS};
                        const auto leaf = whole ? whole : LabLeafAt(nest, fine);
                        if (leaf)
                            slice[(size_t{fine.y} * width) + fine.x] = sliceOf(*leaf);
                    }
                }
            }
        }

        return slice;
    }

    int64_t LabGaugeRate(std::span<const LabGaugeSample> samples, size_t index, uint32_t species, bool wholeBox) {
        if (index == 0 || index >= samples.size())
            return 0;

        const LabGaugeSample& now = samples[index];
        const LabGaugeSample& before = samples[index - 1];
        const std::vector<uint64_t>& nowAmounts = wholeBox ? now.boxAmounts : now.cellAmounts;
        const std::vector<uint64_t>& beforeAmounts = wholeBox ? before.boxAmounts : before.cellAmounts;
        if (species >= nowAmounts.size() || species >= beforeAmounts.size())
            return 0;

        return SignedDifference(nowAmounts[species], beforeAmounts[species]);
    }

    std::expected<std::vector<LabGaugeSample>, std::string> RunLabGaugeOnCpu(const LabRecording& recording,
                                                                             const BakedReactionTable& table,
                                                                             uint32_t cellWord) {
        for (const Command& command : recording.commands) {
            if (LabTableVersionOf(command))
                return std::unexpected(std::format(
                    "刻み {} で表を替えた記録は CPU だけでは流さない(表が 1 つの実験だけ)", command.targetTick));
        }

        std::vector<Command> commands = recording.commands;
        std::ranges::sort(commands, CommandPrecedes);

        MultiresNest nest = MakeLabBoxNest(table, recording.shape);
        std::vector<LabGaugeSample> samples;
        samples.reserve(recording.tickCount);
        for (uint64_t tick = 0; tick < recording.tickCount; ++tick) {
            StepLabBox(nest, table, tick, CommandsAt(commands, tick), recording.shape);
            samples.push_back(SampleLabGauge(nest, table, tick, cellWord, recording.shape));
        }

        return samples;
    }

    // --- 比べる段取り ---

    LabRecording CutLabRecording(const LabRecording& recording, uint64_t tick) {
        const uint64_t end = std::min(tick, recording.tickCount);
        LabRecording cut = {.shape = recording.shape,
                            .tableVersion = recording.tableVersion,
                            .tickCount = end,
                            .tables = recording.tables};
        for (const Command& command : recording.commands) {
            if (command.targetTick < end)
                cut.commands.push_back(command);
        }

        const auto hashEnd = static_cast<std::ptrdiff_t>(std::min<uint64_t>(end, recording.hashes.size()));
        cut.hashes.assign(recording.hashes.begin(), recording.hashes.begin() + hashEnd);

        return cut;
    }

    LabComparisonPlan MakeLabComparisonPlan(const LabRecording& experiment, uint64_t savePointTick, uint32_t tickCount,
                                            Command change) {
        LabComparisonPlan plan = {.savePoint = CutLabRecording(experiment, savePointTick), .tickCount = tickCount};
        const uint64_t start = plan.savePoint.tickCount;
        const uint64_t end = start + tickCount;

        uint32_t lastSequence = 0;
        for (const Command& command : experiment.commands) {
            lastSequence = std::max(lastSequence, command.sequence);
            if (command.targetTick >= start && command.targetTick < end)
                plan.continued.push_back(command);
        }

        std::ranges::sort(plan.continued, CommandPrecedes);
        change.targetTick = start;
        change.sequence = experiment.commands.empty() ? 0 : lastSequence + 1;
        plan.change = change;

        return plan;
    }

    LabRecording MakeLabBranchRecording(const LabComparisonPlan& plan, bool withChange) {
        LabRecording branch = plan.savePoint;
        branch.tickCount = plan.savePoint.tickCount + plan.tickCount;
        branch.commands.insert(branch.commands.end(), plan.continued.begin(), plan.continued.end());
        if (withChange)
            branch.commands.push_back(plan.change);

        std::ranges::sort(branch.commands, CommandPrecedes);

        return branch;
    }

    std::optional<uint64_t> FirstGaugeDifference(std::span<const LabGaugeSample> a, std::span<const LabGaugeSample> b) {
        const size_t count = std::min(a.size(), b.size());
        for (size_t i = 0; i < count; ++i) {
            if (a[i] != b[i])
                return a[i].tick;
        }

        return std::nullopt;
    }

}  // namespace bicameral::sim
