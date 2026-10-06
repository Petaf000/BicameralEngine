// probe_trace.cpp — 伝導の連鎖のトレースの木・CPU リファレンスの予想・食い違いの場所(T-0087)。使い方と木の組み方は probe_trace.h。
#include "sim/probe_trace.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <map>
#include <set>

#include "core/aliases.h"

namespace bicameral::sim {
    namespace {

        using gpu::GraphTraceFilter;
        using gpu::GraphTraceRecord;
        using Coordinates = std::array<uint32_t, 3>;

        // --- 座標 ---

        Coordinates BlockCoordinates(uint32_t block) {
            return {block % PROBE_BLOCKS_PER_AXIS, (block / PROBE_BLOCKS_PER_AXIS) % PROBE_BLOCKS_PER_AXIS,
                    block / (PROBE_BLOCKS_PER_AXIS * PROBE_BLOCKS_PER_AXIS)};
        }

        Coordinates CellCoordinates(uint32_t cell) {
            return {cell % PROBE_GRID_SIZE, (cell / PROBE_GRID_SIZE) % PROBE_GRID_SIZE, cell / PROBE_SLICE_CELL_COUNT};
        }

        std::string FormatCoordinates(const Coordinates& coordinates) {
            return std::format("({}, {}, {})", coordinates[0], coordinates[1], coordinates[2]);
        }

        // 自分と 6 面の隣(格子の中)のブロック。並びは probe_conduct.hlsl の FaceOffset と同じ(比べる前に並べるので順は効かない)
        std::vector<uint32_t> BlockAndNeighbors(uint32_t block) {
            constexpr auto AXIS = static_cast<int32_t>(PROBE_BLOCKS_PER_AXIS);
            constexpr std::array<std::array<int32_t, 3>, PROBE_WAKE_MAX_RECORDS> OFFSETS = {
                {{0, 0, 0}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}}};
            const Coordinates center = BlockCoordinates(block);

            std::vector<uint32_t> blocks;
            for (const auto& offset : OFFSETS) {
                const int32_t x = static_cast<int32_t>(center[0]) + offset[0];
                const int32_t y = static_cast<int32_t>(center[1]) + offset[1];
                const int32_t z = static_cast<int32_t>(center[2]) + offset[2];
                if (x < 0 || y < 0 || z < 0 || x >= AXIS || y >= AXIS || z >= AXIS)
                    continue;

                blocks.push_back(
                    ProbeBlockIndex(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z)));
            }

            return blocks;
        }

        // --- 範囲(graph_trace.hlsli の GtWantsTick・GtWantsPlace と同じ判定)---

        bool WantsTick(const GraphTraceFilter& filter, uint64_t tick) {
            return filter.enabled && tick >= filter.tickBegin && tick < filter.tickEnd;
        }

        bool WantsBlock(const GraphTraceFilter& filter, uint32_t block) {
            const Coordinates place = BlockCoordinates(block);
            for (uint32_t axis = 0; axis < 3; ++axis) {
                if (place[axis] < filter.boxMin[axis] || place[axis] >= filter.boxMax[axis])
                    return false;
            }

            return true;
        }

        // --- 木(1 刻みぶん)---

        struct TickTree {
            std::map<uint32_t, std::vector<uint32_t>> pokes;  // 根のブロック → つついたセル
            std::map<uint32_t, uint32_t> selfWakes;           // 根のブロック → 自分を起こした数(一覧に入った回数)
            std::set<uint32_t> roots;
            std::map<uint32_t, uint32_t> parents;    // 起こされたブロック → 親(起こした根のうち一番小さい番号)
            std::map<uint32_t, uint32_t> conducted;  // 計算したブロック → 結果(PROBE_BLOCK_FLAG_*)
        };

        // (返すと std::map のムーブが noexcept でないので、呼ぶ側の物に書く)
        void BuildTickTree(std::span<const GraphTraceRecord> records, TickTree& tree) {
            for (const GraphTraceRecord& record : records) {
                if (record.kind == PROBE_TRACE_POKE) {
                    tree.pokes[record.subject].push_back(record.object);
                    tree.roots.insert(record.subject);
                } else if (record.kind == PROBE_TRACE_WAKE) {
                    tree.roots.insert(record.subject);
                    if (record.subject == record.object)
                        ++tree.selfWakes[record.subject];

                    const auto [parent, inserted] = tree.parents.try_emplace(record.object, record.subject);
                    if (!inserted)
                        parent->second = std::min(parent->second, record.subject);
                } else if (record.kind == PROBE_TRACE_CONDUCT)
                    tree.conducted[record.subject] = record.object;
            }
        }

        std::string FormatChild(const TickTree& tree, uint32_t block) {
            const auto conducted = tree.conducted.find(block);
            const char* state = conducted == tree.conducted.end()                      ? "(計算の記録なし)"
                                : (conducted->second & PROBE_BLOCK_FLAG_CHANGED) != 0  ? "変わった"
                                : (conducted->second & PROBE_BLOCK_FLAG_POSSIBLE) != 0 ? "変わらない・次の刻みも計算"
                                                                                       : "変わらない";

            return std::format("    → {} {} {}\n", block, FormatCoordinates(BlockCoordinates(block)), state);
        }

        std::string FormatRoot(const TickTree& tree, uint32_t root, uint64_t tick,
                               const std::set<uint32_t>& changedBefore, bool previousTraced) {
            std::string origin;
            if (const auto poked = tree.pokes.find(root); poked != tree.pokes.end()) {
                origin = "つつき";
                for (const uint32_t cell : poked->second)
                    origin += " セル " + FormatCoordinates(CellCoordinates(cell));
            } else if (changedBefore.contains(root))
                origin = std::format("刻み {} で変わった(か、まだ進めた)", tick - 1);
            else
                origin = previousTraced ? "(前の刻みで変わった記録なし。箱の外から)" : "(前の刻みは記録していない)";

            const auto self = tree.selfWakes.find(root);
            const std::string repeat = self != tree.selfWakes.end() && self->second > 1
                                           ? std::format(" ×{}", self->second)
                                           : "";

            return std::format("  根 {} {}: {}{}\n", root, FormatCoordinates(BlockCoordinates(root)), origin, repeat);
        }

        std::string FormatTick(uint64_t tick, const TickTree& tree, const std::set<uint32_t>& changedBefore,
                               bool previousTraced, size_t recordCount) {
            const auto changed = rng::count_if(
                tree.conducted, [](const auto& entry) { return (entry.second & PROBE_BLOCK_FLAG_CHANGED) != 0; });
            std::string text = std::format("刻み {}: 根 {}・計算 {} ブロック(変わった {})・記録 {}\n", tick,
                                           tree.roots.size(), tree.conducted.size(), changed, recordCount);

            // --- 根ごとに、自分が親の子 ---
            for (const uint32_t root : tree.roots) {
                text += FormatRoot(tree, root, tick, changedBefore, previousTraced);
                for (const auto& [block, parent] : tree.parents) {
                    if (parent == root)
                        text += FormatChild(tree, block);
                }
            }

            // --- 起こした記録が無いのに計算したもの(範囲の決め方では起きないはず)---
            bool orphanHeader = false;
            for (const auto& [block, flags] : tree.conducted) {
                if (tree.parents.contains(block))
                    continue;

                if (!orphanHeader)
                    text += "  (起こした記録なし)\n";

                orphanHeader = true;
                text += FormatChild(tree, block);
            }

            return text;
        }

        // ブロック block の中で食い違うセルの数。最初のもの(ブロックの中の z, y, x の順)を first に
        // セル index の抽出の語(温度と見る物質 3 つ)が全部同じか
        bool SameCellWords(std::span<const uint32_t> gpuCells, std::span<const uint32_t> cpuCells, uint32_t index) {
            const size_t base = size_t{index} * PROBE_EXTRACTION_CELL_WORDS;

            return rng::equal(gpuCells.subspan(base, PROBE_EXTRACTION_CELL_WORDS),
                              cpuCells.subspan(base, PROBE_EXTRACTION_CELL_WORDS));
        }

        uint32_t CountBlockDifferences(uint32_t block, std::span<const uint32_t> gpuCells,
                                       std::span<const uint32_t> cpuCells, Coordinates& first) {
            constexpr uint32_t CELLS_PER_BLOCK = PROBE_BLOCK_SIZE * PROBE_BLOCK_SIZE * PROBE_BLOCK_SIZE;
            const Coordinates origin = BlockCoordinates(block);
            uint32_t differences = 0;

            for (uint32_t local = 0; local < CELLS_PER_BLOCK; ++local) {
                const Coordinates cell = {origin[0] * PROBE_BLOCK_SIZE + local % PROBE_BLOCK_SIZE,
                                          origin[1] * PROBE_BLOCK_SIZE + (local / PROBE_BLOCK_SIZE) % PROBE_BLOCK_SIZE,
                                          origin[2] * PROBE_BLOCK_SIZE + local / (PROBE_BLOCK_SIZE * PROBE_BLOCK_SIZE)};
                const uint32_t index = ProbeCellIndex(cell[0], cell[1], cell[2]);
                if (SameCellWords(gpuCells, cpuCells, index))
                    continue;

                if (differences == 0)
                    first = cell;

                ++differences;
            }

            return differences;
        }

        std::string FormatRecord(const GraphTraceRecord& record) {
            const char* kind = record.kind == PROBE_TRACE_POKE      ? "つつき"
                               : record.kind == PROBE_TRACE_WAKE    ? "起こす"
                               : record.kind == PROBE_TRACE_CONDUCT ? "計算"
                                                                    : "?";

            return std::format("刻み {} {} 主 {} 従 {}", record.tick, kind, record.subject, record.object);
        }

    }  // namespace

    gpu::GraphTraceFilter ProbeTraceFilterForCells(uint64_t tickBegin, uint64_t tickEnd,
                                                   std::array<uint32_t, 3> cellMin, std::array<uint32_t, 3> cellMax,
                                                   uint32_t capacity) {
        GraphTraceFilter filter{.enabled = true, .capacity = capacity, .tickBegin = tickBegin, .tickEnd = tickEnd};
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const uint32_t last = std::min(cellMax[axis], PROBE_GRID_SIZE);
            filter.boxMin[axis] = std::min(cellMin[axis], last) / PROBE_BLOCK_SIZE;
            filter.boxMax[axis] = (last + PROBE_BLOCK_SIZE - 1) / PROBE_BLOCK_SIZE;  // セルを含むブロックまで
        }

        return filter;
    }

    std::string FormatProbeTrace(std::span<const gpu::GraphTraceRecord> sorted) {
        std::string text;
        std::set<uint32_t> changedBefore;  // 前の刻みで値が変わったブロック(記録にあるもの)
        std::optional<uint64_t> previousTick;

        for (size_t begin = 0; begin < sorted.size();) {
            const uint64_t tick = sorted[begin].tick;
            size_t end = begin;
            while (end < sorted.size() && sorted[end].tick == tick)
                ++end;

            const std::span<const GraphTraceRecord> records = sorted.subspan(begin, end - begin);
            TickTree tree;
            BuildTickTree(records, tree);
            const bool previousTraced = previousTick == tick - 1;
            if (!previousTraced)
                changedBefore.clear();

            text += FormatTick(tick, tree, changedBefore, previousTraced, records.size());

            changedBefore.clear();
            for (const auto& [block, flags] : tree.conducted) {
                if (flags != 0)
                    changedBefore.insert(block);  // 変わったか、まだ進めた(次の刻みの根)
            }

            previousTick = tick;
            begin = end;
        }

        return text;
    }

    std::expected<void, std::string> WriteProbeTraceFile(const fs::path& path,
                                                         std::vector<gpu::GraphTraceRecord> records,
                                                         const gpu::GraphTraceFilter& filter, uint64_t droppedCount) {
        gpu::SortGraphTrace(records);
        std::ofstream file(path, std::ios::binary);
        if (!file)
            return std::unexpected("トレースのファイルを開けない");

        // --- 見出し: 範囲(場所はブロックの座標)と、欠けているかどうか ---
        file << "# 伝導の連鎖のトレース(T-0087。読み方は engine/src/sim/probe_trace.h)\n"
             << std::format("# 刻み [{}, {})・ブロックの箱 [{}, {}, {}] 〜 [{}, {}, {})・記録 {} 件", filter.tickBegin,
                            filter.tickEnd, filter.boxMin[0], filter.boxMin[1], filter.boxMin[2], filter.boxMax[0],
                            filter.boxMax[1], filter.boxMax[2], records.size())
             << (droppedCount > 0 ? std::format("・容量を越えて落とした {} 件(欠けている)\n", droppedCount) : "\n")
             << FormatProbeTrace(records);

        if (!file)
            return std::unexpected("トレースのファイルに書けない");

        return {};
    }

    std::vector<gpu::GraphTraceRecord> UniqueProbeTrace(std::vector<gpu::GraphTraceRecord> records) {
        gpu::SortGraphTrace(records);
        const auto duplicates = rng::unique(records);
        records.erase(duplicates.begin(), duplicates.end());

        return records;
    }

    void AppendExpectedProbeTrace(const gpu::GraphTraceFilter& filter, uint64_t tick,
                                  std::span<const ProbeCommand> commands, std::span<const uint8_t> flagsBefore,
                                  std::span<const uint8_t> flagsAfter, std::vector<gpu::GraphTraceRecord>& expected) {
        if (!WantsTick(filter, tick))
            return;

        // --- つつき(適用の単位と同じく、格子の中のものだけ)→ 根になる ---
        std::vector<uint8_t> seeds(flagsBefore.begin(), flagsBefore.end());
        for (const ProbeCommand& command : commands) {
            const uint32_t x = command.payload[0];
            const uint32_t y = command.payload[1];
            const uint32_t z = command.payload[2];
            if (command.targetTick != tick || command.type != PROBE_COMMAND_TYPE_POKE || x >= PROBE_GRID_SIZE ||
                y >= PROBE_GRID_SIZE || z >= PROBE_GRID_SIZE) {
                continue;
            }

            const uint32_t block = ProbeBlockOfCell(x, y, z);
            seeds[block] = 1;
            if (WantsBlock(filter, block))
                expected.push_back(
                    {.tick = tick, .kind = PROBE_TRACE_POKE, .subject = block, .object = ProbeCellIndex(x, y, z)});
        }

        // --- 根が起こす(自分と 6 面の隣)→ 起こされたブロックを計算する ---
        std::vector<uint8_t> scheduled(PROBE_BLOCK_COUNT, 0);
        for (uint32_t source = 0; source < PROBE_BLOCK_COUNT; ++source) {
            if (seeds[source] == 0)
                continue;

            const bool sourceWanted = WantsBlock(filter, source);
            for (const uint32_t target : BlockAndNeighbors(source)) {
                scheduled[target] = 1;
                if (sourceWanted || WantsBlock(filter, target))
                    expected.push_back({.tick = tick, .kind = PROBE_TRACE_WAKE, .subject = source, .object = target});
            }
        }

        for (uint32_t block = 0; block < PROBE_BLOCK_COUNT; ++block) {
            if (scheduled[block] != 0 && WantsBlock(filter, block)) {
                expected.push_back(
                    {.tick = tick, .kind = PROBE_TRACE_CONDUCT, .subject = block, .object = flagsAfter[block]});
            }
        }
    }

    std::optional<std::string> FirstProbeTraceMismatch(std::span<const gpu::GraphTraceRecord> gpu,
                                                       std::span<const gpu::GraphTraceRecord> cpu) {
        const auto [gpuAt, cpuAt] = rng::mismatch(gpu, cpu);
        if (gpuAt == gpu.end() && cpuAt == cpu.end())
            return std::nullopt;

        const std::string gpuText = gpuAt == gpu.end() ? "(終わり)" : FormatRecord(*gpuAt);
        const std::string cpuText = cpuAt == cpu.end() ? "(終わり)" : FormatRecord(*cpuAt);

        return std::format("トレースの {} 件目で食い違った: GPU {} / CPU {}", gpuAt - gpu.begin(), gpuText, cpuText);
    }

    std::optional<uint64_t> FirstDivergentTick(std::span<const ProbeTickHash> gpu, std::span<const ProbeTickHash> cpu) {
        for (const ProbeTickHash& gpuTick : gpu) {
            const auto cpuTick = rng::find(cpu, gpuTick.tick, &ProbeTickHash::tick);
            if (cpuTick == cpu.end())
                continue;

            if (cpuTick->hash != gpuTick.hash || cpuTick->energy != gpuTick.energy)
                return gpuTick.tick;
        }

        return std::nullopt;
    }

    std::optional<ProbeDivergence> FindCellDivergence(uint64_t tick, std::span<const uint32_t> gpuCells,
                                                      std::span<const uint32_t> cpuCells) {
        constexpr size_t WORDS = size_t{PROBE_CELL_COUNT} * PROBE_EXTRACTION_CELL_WORDS;
        if (gpuCells.size() < WORDS || cpuCells.size() < WORDS)
            return std::nullopt;

        std::optional<ProbeDivergence> divergence;
        uint32_t differingCells = 0;
        uint32_t differingBlocks = 0;

        // ブロックの番号の順に。最初に食い違ったブロックの、最初のセルを残す
        for (uint32_t block = 0; block < PROBE_BLOCK_COUNT; ++block) {
            Coordinates first{};
            const uint32_t differences = CountBlockDifferences(block, gpuCells, cpuCells, first);
            if (differences == 0)
                continue;

            differingCells += differences;
            ++differingBlocks;
            if (!divergence) {
                const uint32_t index = ProbeCellIndex(first[0], first[1], first[2]);
                divergence = ProbeDivergence{.tick = tick,
                                             .block = block,
                                             .cell = first,
                                             .gpuValue = gpuCells[size_t{index} * PROBE_EXTRACTION_CELL_WORDS],
                                             .cpuValue = cpuCells[size_t{index} * PROBE_EXTRACTION_CELL_WORDS]};
            }
        }

        if (divergence) {
            divergence->differingCells = differingCells;
            divergence->differingBlocks = differingBlocks;
        }

        return divergence;
    }

    std::string FormatProbeDivergence(const ProbeDivergence& divergence) {
        return std::format(
            "S({}) で CPU リファレンスと食い違った: 最初のブロック {} {}・セル {} 温度 GPU {} CPU {} mK"
            "(食い違ったセル {}・ブロック {})",
            divergence.tick, divergence.block, FormatCoordinates(BlockCoordinates(divergence.block)),
            FormatCoordinates(divergence.cell), divergence.gpuValue, divergence.cpuValue, divergence.differingCells,
            divergence.differingBlocks);
    }

}  // namespace bicameral::sim
