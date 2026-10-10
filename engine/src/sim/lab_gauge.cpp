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

    }  // namespace

    // --- 計器 ---

    LabGaugeSample SampleLabGauge(const MultiresNest& nest, const BakedReactionTable& table, uint64_t tick,
                                  uint32_t cellIndex) {
        const size_t speciesCount = table.species.size();
        LabGaugeSample sample = {.tick = tick,
                                 .cell = cellIndex,
                                 .cellAmounts = std::vector<uint64_t>(speciesCount, 0),
                                 .boxAmounts = std::vector<uint64_t>(speciesCount, 0)};
        sample.maxTemperatureMilliKelvin = std::numeric_limits<int32_t>::min();

        for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; ++index) {
            const reaction::RxCell cell = LabBoxCell(nest, index);
            const int32_t temperature = reaction::RxComputeThermal(table.View(), cell).temperature;
            sample.maxTemperatureMilliKelvin = std::max(sample.maxTemperatureMilliKelvin, temperature);
            if (index == cellIndex)
                sample.cellTemperatureMilliKelvin = temperature;

            // --- 物質量(表に無い ID は数えない)---
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                if (cell.species[i] >= speciesCount)
                    continue;

                sample.boxAmounts[cell.species[i]] += cell.amounts[i];
                if (index == cellIndex)
                    sample.cellAmounts[cell.species[i]] += cell.amounts[i];
            }
        }

        return sample;
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
                                                                             uint32_t cellIndex) {
        for (const Command& command : recording.commands) {
            if (LabTableVersionOf(command))
                return std::unexpected(std::format(
                    "刻み {} で表を替えた記録は CPU だけでは流さない(表が 1 つの実験だけ)", command.targetTick));
        }

        std::vector<Command> commands = recording.commands;
        std::ranges::sort(commands, CommandPrecedes);

        MultiresNest nest = MakeLabBoxNest(table);
        std::vector<LabGaugeSample> samples;
        samples.reserve(recording.tickCount);
        for (uint64_t tick = 0; tick < recording.tickCount; ++tick) {
            StepLabBox(nest, table, tick, CommandsAt(commands, tick));
            samples.push_back(SampleLabGauge(nest, table, tick, cellIndex));
        }

        return samples;
    }

    // --- 比べる段取り ---

    LabRecording CutLabRecording(const LabRecording& recording, uint64_t tick) {
        const uint64_t end = std::min(tick, recording.tickCount);
        LabRecording cut = {.tableVersion = recording.tableVersion, .tickCount = end, .tables = recording.tables};
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
