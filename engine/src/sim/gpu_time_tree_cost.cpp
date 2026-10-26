// gpu_time_tree_cost.cpp — 1 出来事の GPU の費用の式の値を起動時に測る口(D-662・T-1152)。何をするかは gpu_time_tree_cost.h。
// 箱は中身を持たない(セルの数と決まった係数だけ)。値は法則によらない「部品の GPU 版で 1 回解く」費用。浮動小数点は使わない(04 §4)。
#include "sim/gpu_time_tree_cost.h"

#include <algorithm>
#include <optional>
#include <span>
#include <vector>

#include "common/equilibrium.hlsli"
#include "common/implicit_conduction.hlsli"
#include "gpu/debug_ring.h"
#include "gpu/immediate_queue.h"
#include "sim/gpu_time_tree_choose.h"

namespace bicameral::sim {

    namespace {

        // --- 箱の決まった係数(容量 ≫ 面の係数なので e = 0 では段 1 つ = 早送りの e = 0 の列の系と同じ形。ADR-0465)---
        constexpr uint64_t PROBE_CAPACITY = uint64_t{1} << 40;
        constexpr uint64_t PROBE_FACE = uint64_t{1} << 30;
        constexpr uint64_t PROBE_GROUND = uint64_t{1} << 24;
        constexpr int64_t PROBE_VALUE_UNIT = int64_t{1} << 16;
        constexpr int64_t PROBE_VALUE_KINDS = 64;

        // 測る回の上限(時刻印は 1 つの部品に 256 まで)
        constexpr uint32_t PROBE_MAX_ROUNDS = 32;
        constexpr uint32_t PROBE_PARTS = 2;  // 箱ごとに V 1・V 2
        constexpr size_t PROBE_BOXES = 3;    // 段 1 の小・大・段の多い大

        int64_t ProbeValue(uint32_t x, uint32_t y, uint32_t z) {
            return static_cast<int64_t>(((x * 37) + (y * 11) + (z * 5)) % PROBE_VALUE_KINDS) * PROBE_VALUE_UNIT;
        }

        // 節 node の +軸の隣(箱の外なら値 0 を固定した面)
        void AddProbeFace(EquilibriumSystem& system, uint32_t node, int64_t value, std::optional<uint32_t> next,
                          int64_t nextValue, const fx::FxU128& face) {
            if (!next) {
                system.fixedFaces.push_back({.node = node, .coefficient = face, .value = 0});
                system.sources.push_back({.node = node, .value = -value, .factor = face});
                return;
            }

            system.faces.push_back({.fine = node, .coarse = *next, .gap = 0, .coefficient = face});
            system.sources.push_back({.node = node, .value = nextValue - value, .factor = face});
            system.sources.push_back({.node = *next, .value = value - nextValue, .factor = face});
        }

        struct ProbeBox {
            EquilibriumSystem system;
            ImplicitGrid grid;
            std::optional<GpuEquilibriumUpload> gpu;
            uint64_t cells = 0;
        };

        // 段の形(CPU の SolveEquilibrium が組む道。GPU の部品はこの形で作る)
        ImplicitGrid GridOf(const EquilibriumSystem& system) {
            EquilibriumOptions options;
            options.solver.cycles = 1;
            options.faceValues = false;
            ImplicitGrid grid;
            const EquilibriumObserver observer = [&grid](const EquilibriumSolveView& view) {
                grid = view.grid;
            };
            EquilibriumState state;
            (void)SolveEquilibrium(system, options, state, &observer);

            return grid;
        }

        std::expected<void, std::string> Execute(gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                                 ID3D12GraphicsCommandList* list) {
            ring.RecordReadbackAndReset(list);
            if (!queue.ExecuteAndWait())
                return std::unexpected("GPU での実行に失敗");

            if (ring.Drain().assertCount > 0)
                return std::unexpected("GPU の FX_ASSERT");

            return {};
        }

        // --- 刻みを選ぶ段を測る箱の木(T-1214)---
        // 辺 side の立方体のセル = 単位(x が最も速い)・窓の格子 = 同じ箱・面の隣。半分(x < side / 2)は望み 0(変化の速い所)・
        // 残りは望み PROBE_CHOOSE_QUIET(静かな所)で、隣の差 1 段が境から広がる(区画の中を数回回す。中身によらない形)。
        // 望みを上限 10 にすると区画の中の回が火事の普通の出来事より多く、火事の平均(T-1213)の 1.6 倍に出た → 3(T-1214)
        constexpr int32_t PROBE_CHOOSE_MAX_EXPONENT = 10;
        constexpr uint32_t PROBE_CHOOSE_QUIET = 3;
        constexpr uint32_t PROBE_CHOOSE_RADIUS = 4;

        TimeTreeChooseTables ProbeChooseTables(uint32_t side) {
            TimeTreeChooseTables tables;
            tables.supported = true;
            tables.units = side * side * side;
            tables.gridSize = {side, side, side};
            tables.radius = PROBE_CHOOSE_RADIUS;
            tables.maxExponent = PROBE_CHOOSE_MAX_EXPONENT;
            tables.gridUnits.resize(tables.units);
            tables.neighborStarts.push_back(0);
            for (uint32_t unit = 0; unit < tables.units; ++unit) {
                tables.gridUnits[unit] = unit;
                const std::array<uint32_t, 3> at = {unit % side, (unit / side) % side, unit / (side * side)};
                uint32_t stride = 1;
                for (uint32_t axis = 0; axis < 3; ++axis, stride *= side) {
                    if (at[axis] > 0)
                        tables.neighbors.push_back(unit - stride);

                    if (at[axis] + 1 < side)
                        tables.neighbors.push_back(unit + stride);
                }

                tables.neighborStarts.push_back(static_cast<uint32_t>(tables.neighbors.size()));
            }

            return tables;
        }

        // 閉じた歩の記録(望みの e だけ。PackTimeTreeUnits の形)
        std::vector<uint16_t> ProbeChooseRecords(uint32_t side) {
            std::vector<uint16_t> records(size_t{side} * side * side);
            for (uint32_t unit = 0; unit < records.size(); ++unit) {
                const uint32_t desired = unit % side < side / 2 ? 0u : PROBE_CHOOSE_QUIET;
                records[unit] = static_cast<uint16_t>(desired << 4);
            }

            return records;
        }

        // 1 出来事を rounds 回続けて積んだ最小(ns)
        std::expected<uint64_t, std::string> MeasureChoose(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                           gpu::DebugRing& ring, uint64_t timestampFrequency,
                                                           uint32_t side, uint32_t rounds) {
            const auto choose = GpuTimeTreeChoose::Create(device, ProbeChooseTables(side), ProbeChooseRecords(side));
            if (!choose)
                return std::unexpected(choose.error());

            ID3D12GraphicsCommandList10* list = queue.Begin();
            if (list == nullptr)
                return std::unexpected("コマンドリストを始められない");

            ring.RecordBegin(list);
            choose->RecordUpload(list);
            choose->RecordTimestamp(list, 0);
            for (uint32_t repeat = 0; repeat < rounds; ++repeat) {
                choose->RecordChoose(list, ring.GpuAddress(), 0, PROBE_CHOOSE_MAX_EXPONENT);
                choose->RecordTimestamp(list, repeat + 1);
            }

            choose->RecordTimestampReadback(list, rounds + 1);
            const auto executed = Execute(queue, ring, list);
            if (!executed)
                return std::unexpected(executed.error());

            const std::vector<uint64_t> ticks = choose->ReadTimestamps(rounds + 1);
            if (ticks.size() != rounds + 1)
                return std::unexpected("刻みを選ぶ段の時刻印を読めない");

            uint64_t best = UINT64_MAX;
            for (uint32_t repeat = 0; repeat < rounds; ++repeat)
                best = std::min(best, (ticks[repeat + 1] - ticks[repeat]) * 1000000000 / timestampFrequency);

            return best;
        }

        // 箱を作り、V の記録の形を前の解きの数から選ぶ(本物の GPU 版と同じ。GpuImplicit::ShapeFrom。値は同じ)
        std::expected<void, std::string> MakeProbeBox(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                      gpu::DebugRing& ring, uint32_t side, int32_t exponent,
                                                      ProbeBox& box) {
            box.system = TimeTreeProbeBox(side, exponent);
            box.cells = box.system.nodes.size();
            box.grid = GridOf(box.system);
            auto created = GpuEquilibriumUpload::Create(device, box.system, box.grid);
            if (!created)
                return std::unexpected(created.error());

            box.gpu.emplace(std::move(*created));
            ID3D12GraphicsCommandList10* list = queue.Begin();
            if (list == nullptr)
                return std::unexpected("コマンドリストを始められない");

            ring.RecordBegin(list);
            if (!box.gpu->RecordUpload(list, box.grid) || !box.gpu->RecordSolve(list, ring.GpuAddress(), 1))
                return std::unexpected("段の形が合わない");

            box.gpu->RecordCostReadback(list);
            box.gpu->RecordEnd(list);
            const auto executed = Execute(queue, ring, list);
            if (!executed)
                return executed;

            box.gpu->ReleaseStaging();
            GpuImplicitCost cost;
            if (!box.gpu->ReadCost(cost))
                return std::unexpected("数を読めない");

            box.gpu->SetSolveShape(GpuImplicit::ShapeFrom(cost));

            return {};
        }

        uint64_t TicksToNs(uint64_t begin, uint64_t end, uint64_t frequency) {
            return end > begin ? (end - begin) * 1000000000 / frequency : 0;
        }

        // 熱する: 大きい箱の解きを batchSolves 回ずつ、GPU の時間が heatNs に届くまで続けて投げる(T-1152: 休んだ GPU は
        // 約 0.25 秒続けて回すと時計が上がり、1 回の解きが 6〜7 倍速くなる。早送りは続けて積むので、その時計の値を測る)
        std::expected<void, std::string> Heat(gpu::ImmediateQueue& queue, gpu::DebugRing& ring, ProbeBox& box,
                                              uint64_t frequency, const TimeTreeCostProbeOptions& options,
                                              TimeTreeCostProbe& probe) {
            for (uint32_t batch = 0; batch < options.maxHeatBatches && probe.heatNs < options.heatNs; ++batch) {
                ID3D12GraphicsCommandList10* list = queue.Begin();
                if (list == nullptr)
                    return std::unexpected("コマンドリストを始められない");

                ring.RecordBegin(list);
                if (!box.gpu->RecordUpload(list, box.grid))
                    return std::unexpected("段の形が合わない");

                box.gpu->RecordTimestamp(list, 0);
                for (uint32_t solve = 0; solve < options.heatBatchSolves; ++solve) {
                    if (!box.gpu->RecordSolve(list, ring.GpuAddress(), 1))
                        return std::unexpected("解きを積めない");
                }

                box.gpu->RecordTimestamp(list, 1);
                box.gpu->RecordTimestampReadback(list, 2);
                box.gpu->RecordEnd(list);
                const auto executed = Execute(queue, ring, list);
                if (!executed)
                    return executed;

                box.gpu->ReleaseStaging();
                const std::vector<uint64_t> ticks = box.gpu->ReadTimestamps(2);
                if (ticks.size() != 2)
                    return std::unexpected("時刻印を読めない");

                probe.heatNs += TicksToNs(ticks[0], ticks[1], frequency);
                ++probe.heatBatches;
            }

            return {};
        }

        // 回ごとに 箱 × V 1・V 2 の時刻印(箱ごとの番号 = 回 × 4 + 部 × 2)
        bool RecordProbe(ID3D12GraphicsCommandList* list, uint64_t ring, std::span<ProbeBox* const> boxes,
                         uint32_t rounds) {
            for (ProbeBox* box : boxes) {
                if (!box->gpu->RecordUpload(list, box->grid))
                    return false;
            }

            for (uint32_t repeat = 0; repeat < rounds; ++repeat) {
                for (ProbeBox* box : boxes) {
                    for (uint32_t part = 0; part < PROBE_PARTS; ++part) {
                        const uint32_t stamp = (repeat * 2 * PROBE_PARTS) + (2 * part);
                        box->gpu->RecordTimestamp(list, stamp);
                        if (!box->gpu->RecordSolve(list, ring, 1 + part))
                            return false;

                        box->gpu->RecordTimestamp(list, stamp + 1);
                    }
                }
            }

            for (ProbeBox* box : boxes) {
                box->gpu->RecordTimestampReadback(list, rounds * 2 * PROBE_PARTS);
                box->gpu->RecordEnd(list);
            }

            return true;
        }

        // 回ごとの最小(箱 × V 1・V 2 = solveNs の 2 × 箱 + 部)と、測りの GPU の時間
        std::expected<void, std::string> ReadProbe(std::array<ProbeBox, PROBE_BOXES>& boxes, uint32_t rounds,
                                                   uint64_t frequency, TimeTreeCostProbe& probe) {
            const uint32_t stamps = rounds * 2 * PROBE_PARTS;
            std::array<std::vector<uint64_t>, PROBE_BOXES> ticks;
            for (size_t index = 0; index < PROBE_BOXES; ++index) {
                boxes[index].gpu->ReleaseStaging();
                ticks[index] = boxes[index].gpu->ReadTimestamps(stamps);
                if (ticks[index].size() != stamps || frequency == 0)
                    return std::unexpected("時刻印を読めない");
            }

            probe.times.solveNs.fill(UINT64_MAX);
            for (uint32_t repeat = 0; repeat < rounds; ++repeat) {
                for (size_t index = 0; index < PROBE_BOXES; ++index) {
                    for (uint32_t part = 0; part < PROBE_PARTS; ++part) {
                        const uint32_t stamp = (repeat * 2 * PROBE_PARTS) + (2 * part);
                        const uint64_t ns = TicksToNs(ticks[index][stamp], ticks[index][stamp + 1], frequency);
                        uint64_t& best = probe.times.solveNs[(index * PROBE_PARTS) + part];
                        best = std::min(best, ns);
                    }
                }
            }

            probe.gpuNs = TicksToNs(ticks.front().front(), ticks.back().back(), frequency);

            return {};
        }

        int64_t AtLeastZero(int64_t value) {
            return std::max<int64_t>(value, 0);
        }

    }  // namespace

    EquilibriumSystem TimeTreeProbeBox(uint32_t side, int32_t exponent) {
        EquilibriumSystem system;
        system.rounding = multires::EQ_ROUNDING_WEIGHT;
        const auto shift = static_cast<uint32_t>(std::max(exponent, 0));
        const fx::FxU128 face = multires::ImWideShiftLeft(multires::ImWide(PROBE_FACE), shift);
        const fx::FxU128 ground = multires::ImWideShiftLeft(multires::ImWide(PROBE_GROUND), shift);
        const auto indexOf = [side](uint32_t x, uint32_t y, uint32_t z) {
            return (((z * side) + y) * side) + x;
        };

        // --- 節と地面(値 0 を固定した面)---
        for (uint32_t z = 0; z < side; ++z) {
            for (uint32_t y = 0; y < side; ++y) {
                for (uint32_t x = 0; x < side; ++x) {
                    const uint32_t node = indexOf(x, y, z);
                    system.nodes.push_back({.level = 0, .x = x, .y = y, .z = z, .capacity = PROBE_CAPACITY});
                    system.fixedFaces.push_back({.node = node, .coefficient = ground, .value = 0});
                    system.sources.push_back({.node = node, .value = -ProbeValue(x, y, z), .factor = ground});
                }
            }
        }

        // --- +x・+y・+z の面 ---
        for (uint32_t z = 0; z < side; ++z) {
            for (uint32_t y = 0; y < side; ++y) {
                for (uint32_t x = 0; x < side; ++x) {
                    const uint32_t node = indexOf(x, y, z);
                    const int64_t value = ProbeValue(x, y, z);
                    const std::array<std::array<uint32_t, 3>, 3> nexts = {
                        {{x + 1, y, z}, {x, y + 1, z}, {x, y, z + 1}}};
                    for (const auto& [nx, ny, nz] : nexts) {
                        const bool inside = nx < side && ny < side && nz < side;
                        std::optional<uint32_t> next;
                        if (inside)
                            next = indexOf(nx, ny, nz);

                        AddProbeFace(system, node, value, next, inside ? ProbeValue(nx, ny, nz) : 0, face);
                    }
                }
            }
        }

        return system;
    }

    namespace {

        // 刻みを選ぶ段: t = 固定費 + 単位 × 傾き(2 つの木。測っていなければ既定の値。負は 0)
        std::pair<uint64_t, uint64_t> FitChoose(const TimeTreeProbeTimes& times) {
            const TimeTreeSolveCost defaults;
            if (times.chooseNs[0] == 0 || times.chooseUnits[1] <= times.chooseUnits[0])
                return {defaults.chooseNs, defaults.chooseUnitPs};

            const auto smallPs = static_cast<int64_t>(times.chooseNs[0]) * 1000;
            const auto largePs = static_cast<int64_t>(times.chooseNs[1]) * 1000;
            const auto units = static_cast<int64_t>(times.chooseUnits[1] - times.chooseUnits[0]);
            const int64_t unitPs = AtLeastZero((largePs - smallPs) / units);
            const int64_t fixedPs = AtLeastZero(smallPs - (static_cast<int64_t>(times.chooseUnits[0]) * unitPs));

            return {static_cast<uint64_t>(fixedPs / 1000), static_cast<uint64_t>(unitPs)};
        }

    }  // namespace

    TimeTreeSolveCost FitTimeTreeSolveCost(const TimeTreeProbeTimes& times, uint32_t marginPercent) {
        // t = 固定費 + (段 − 1) × 段の固定費 + V × (V の固定費 + (段 − 1) × 段の V の固定費) + n × セル + n × V × セル × V。
        // 段 1 の小・大の箱で 段 1 の値を、段 levels の箱で段ごとの値を解く(ps で。負は 0)
        const auto ps = [&times](size_t index) {
            return static_cast<int64_t>(times.solveNs[index]) * 1000;
        };
        const auto smallCount = static_cast<int64_t>(times.smallCells);
        const auto largeCount = static_cast<int64_t>(std::max(times.largeCells, times.smallCells + 1));
        const int64_t extraLevels = std::max<int64_t>(static_cast<int64_t>(times.levels) - 1, 1);
        const int64_t smallStep = ps(1) - ps(0);  // 小さい箱の V 1 回 = V の固定費 + 小 × 傾き
        const int64_t largeStep = ps(3) - ps(2);
        const int64_t levelStep = ps(5) - ps(4);  // 段の多い箱(大と同じセル)の V 1 回
        const int64_t cellCycle = AtLeastZero((largeStep - smallStep) / (largeCount - smallCount));
        const int64_t cycle = AtLeastZero(smallStep - (smallCount * cellCycle));
        const int64_t cell = AtLeastZero(((ps(2) - ps(0)) / (largeCount - smallCount)) - cellCycle);
        const int64_t event = AtLeastZero(ps(0) - cycle - (smallCount * (cell + cellCycle)));
        const int64_t levelCycle = times.levels > 1 ? AtLeastZero((levelStep - largeStep) / extraLevels) : 0;
        const int64_t level = times.levels > 1 ? AtLeastZero(((ps(4) - ps(2)) / extraLevels) - levelCycle) : 0;

        const std::pair<uint64_t, uint64_t> choose = FitChoose(times);

        return {.eventNs = static_cast<uint64_t>(event / 1000),
                .levelNs = static_cast<uint64_t>(level / 1000),
                .cycleNs = static_cast<uint64_t>(cycle / 1000),
                .levelCycleNs = static_cast<uint64_t>(levelCycle / 1000),
                .cellPs = static_cast<uint64_t>(cell),
                .cellCyclePs = static_cast<uint64_t>(cellCycle),
                .chooseNs = choose.first,
                .chooseUnitPs = choose.second,
                .marginPercent = marginPercent};
    }

    std::expected<TimeTreeCostProbe, std::string> ProbeTimeTreeSolveCost(ID3D12Device5* device,
                                                                         gpu::ImmediateQueue& queue,
                                                                         gpu::DebugRing& ring,
                                                                         uint64_t timestampFrequency,
                                                                         const TimeTreeCostProbeOptions& options) {
        // 段 1 の小・大の箱と、大と同じセルで段の多い箱(面の係数 × 2^levelExponent)
        std::array<ProbeBox, PROBE_BOXES> boxes;
        const std::array<std::pair<uint32_t, int32_t>, PROBE_BOXES> shapes = {
            {{options.smallSide, 0}, {options.largeSide, 0}, {options.largeSide, options.levelExponent}}};
        for (size_t index = 0; index < PROBE_BOXES; ++index) {
            const auto made = MakeProbeBox(device, queue, ring, shapes[index].first, shapes[index].second,
                                           boxes[index]);
            if (!made)
                return std::unexpected(made.error());
        }

        TimeTreeCostProbe probe;
        const auto heated = Heat(queue, ring, boxes[1], timestampFrequency, options, probe);
        if (!heated)
            return std::unexpected(heated.error());

        // --- 6 つの時間を続けて積む ---
        const uint32_t rounds = std::clamp<uint32_t>(options.rounds, 1, PROBE_MAX_ROUNDS);
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        const std::array<ProbeBox*, PROBE_BOXES> pointers = {&boxes[0], &boxes[1], &boxes[2]};
        ring.RecordBegin(list);
        if (!RecordProbe(list, ring.GpuAddress(), pointers, rounds))
            return std::unexpected("解きを積めない");

        const auto executed = Execute(queue, ring, list);
        if (!executed)
            return std::unexpected(executed.error());

        const auto read = ReadProbe(boxes, rounds, timestampFrequency, probe);
        if (!read)
            return std::unexpected(read.error());

        probe.times.smallCells = boxes[0].cells;
        probe.times.largeCells = boxes[1].cells;
        probe.times.levels = static_cast<uint32_t>(boxes[2].grid.levels.size());

        // --- 刻みを選ぶ段(GPU の時計が高いうちに)---
        if (options.chooseSmallSide > 0 && options.chooseLargeSide > options.chooseSmallSide) {
            const std::array<uint32_t, 2> sides = {options.chooseSmallSide, options.chooseLargeSide};
            for (size_t index = 0; index < sides.size(); ++index) {
                const auto ns = MeasureChoose(device, queue, ring, timestampFrequency, sides[index], rounds);
                if (!ns)
                    return std::unexpected(ns.error());

                probe.times.chooseNs[index] = *ns;
                probe.times.chooseUnits[index] = uint64_t{sides[index]} * sides[index] * sides[index];
            }
        }

        probe.cost = FitTimeTreeSolveCost(probe.times, options.marginPercent);

        return probe;
    }

}  // namespace bicameral::sim
