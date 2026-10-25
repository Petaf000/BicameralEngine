// gpu_time_tree_cost.cpp — 1 出来事の GPU の費用の式の値を起動時に測る口(D-662・T-1152)。何をするかは gpu_time_tree_cost.h。
// 箱は中身を持たない(セルの数と決まった係数だけ)。値は法則によらない「部品の GPU 版で 1 回解く」費用。浮動小数点は使わない(04 §4)。
#include "sim/gpu_time_tree_cost.h"

#include <algorithm>
#include <optional>
#include <vector>

#include "common/equilibrium.hlsli"
#include "common/implicit_conduction.hlsli"
#include "gpu/debug_ring.h"
#include "gpu/immediate_queue.h"

namespace bicameral::sim {

    namespace {

        // --- 箱の決まった係数(容量 ≫ 面の係数なので e = 0 では段 1 つ = 早送りの e = 0 の列の系と同じ形。ADR-0465)---
        constexpr uint64_t PROBE_CAPACITY = uint64_t{1} << 40;
        constexpr uint64_t PROBE_FACE = uint64_t{1} << 30;
        constexpr uint64_t PROBE_GROUND = uint64_t{1} << 24;
        constexpr int64_t PROBE_VALUE_UNIT = int64_t{1} << 16;
        constexpr int64_t PROBE_VALUE_KINDS = 64;

        // 1 つの物差しで 4 つの時間を測る回の上限(時刻印は 1 つの部品に 256 まで)
        constexpr uint32_t PROBE_MAX_ROUNDS = 32;
        constexpr uint32_t PROBE_PARTS = 2;  // 箱ごとに V 1・V 2

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

        // 回ごとに 箱 × V 1・V 2 の時刻印。時刻印の番号: 小 = 回 × 4 + 部 × 2・大 = 1 + 同じ
        bool RecordProbe(ID3D12GraphicsCommandList* list, uint64_t ring, ProbeBox& smallBox, ProbeBox& largeBox,
                         uint32_t rounds) {
            if (!smallBox.gpu->RecordUpload(list, smallBox.grid) || !largeBox.gpu->RecordUpload(list, largeBox.grid))
                return false;

            largeBox.gpu->RecordTimestamp(list, 0);

            for (uint32_t repeat = 0; repeat < rounds; ++repeat) {
                for (uint32_t part = 0; part < 2 * PROBE_PARTS; ++part) {
                    ProbeBox& box = part < PROBE_PARTS ? smallBox : largeBox;
                    const uint32_t cycles = 1 + (part % PROBE_PARTS);
                    const uint32_t stamp = (repeat * 2 * PROBE_PARTS) + (2 * (part % PROBE_PARTS)) +
                                           (part < PROBE_PARTS ? 0 : 1);
                    box.gpu->RecordTimestamp(list, stamp);
                    if (!box.gpu->RecordSolve(list, ring, cycles))
                        return false;

                    box.gpu->RecordTimestamp(list, stamp + 1);
                }
            }

            const uint32_t stamps = rounds * 2 * PROBE_PARTS;
            largeBox.gpu->RecordTimestamp(list, stamps + 1);
            smallBox.gpu->RecordTimestampReadback(list, stamps);
            largeBox.gpu->RecordTimestampReadback(list, stamps + 2);
            smallBox.gpu->RecordEnd(list);
            largeBox.gpu->RecordEnd(list);

            return true;
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

    TimeTreeSolveCost FitTimeTreeSolveCost(const std::array<uint64_t, 4>& solveNs, uint64_t smallCells,
                                           uint64_t largeCells, uint32_t marginPercent) {
        // t(n, V) = 固定費 + V × V の固定費 + n × セルの傾き + n × V × セル × V の傾き(ps で解く)
        const auto ps = [&solveNs](size_t index) {
            return static_cast<int64_t>(solveNs[index]) * 1000;
        };
        const auto smallCount = static_cast<int64_t>(smallCells);
        const auto largeCount = static_cast<int64_t>(std::max(largeCells, smallCells + 1));
        const int64_t smallStep = ps(1) - ps(0);  // 小さい箱の V 1 回 = V の固定費 + 小 × 傾き
        const int64_t largeStep = ps(3) - ps(2);
        const int64_t cellCycle = AtLeastZero((largeStep - smallStep) / (largeCount - smallCount));
        const int64_t cycle = AtLeastZero(smallStep - (smallCount * cellCycle));
        const int64_t cell = AtLeastZero(((ps(2) - ps(0)) / (largeCount - smallCount)) - cellCycle);
        const int64_t event = AtLeastZero(ps(0) - cycle - (smallCount * (cell + cellCycle)));

        return {.eventNs = static_cast<uint64_t>(event / 1000),
                .cycleNs = static_cast<uint64_t>(cycle / 1000),
                .cellPs = static_cast<uint64_t>(cell),
                .cellCyclePs = static_cast<uint64_t>(cellCycle),
                .marginPercent = marginPercent};
    }

    std::expected<TimeTreeCostProbe, std::string> ProbeTimeTreeSolveCost(ID3D12Device5* device,
                                                                         gpu::ImmediateQueue& queue,
                                                                         gpu::DebugRing& ring,
                                                                         uint64_t timestampFrequency,
                                                                         const TimeTreeCostProbeOptions& options) {
        ProbeBox smallBox;
        ProbeBox largeBox;
        for (auto [box, side] : {std::pair{&smallBox, options.smallSide}, std::pair{&largeBox, options.largeSide}}) {
            const auto made = MakeProbeBox(device, queue, ring, side, options.exponent, *box);
            if (!made)
                return std::unexpected(made.error());
        }

        // --- 熱してから 4 つの時間を続けて積む ---
        TimeTreeCostProbe probe;
        const auto heated = Heat(queue, ring, largeBox, timestampFrequency, options, probe);
        if (!heated)
            return std::unexpected(heated.error());

        const uint32_t rounds = std::clamp<uint32_t>(options.rounds, 1, PROBE_MAX_ROUNDS);
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        ring.RecordBegin(list);
        if (!RecordProbe(list, ring.GpuAddress(), smallBox, largeBox, rounds))
            return std::unexpected("解きを積めない");

        const auto executed = Execute(queue, ring, list);
        if (!executed)
            return std::unexpected(executed.error());

        smallBox.gpu->ReleaseStaging();
        largeBox.gpu->ReleaseStaging();
        const uint32_t stamps = rounds * 2 * PROBE_PARTS;
        const std::vector<uint64_t> smallTicks = smallBox.gpu->ReadTimestamps(stamps);
        const std::vector<uint64_t> largeTicks = largeBox.gpu->ReadTimestamps(stamps + 2);
        if (smallTicks.size() != stamps || largeTicks.size() != stamps + 2 || timestampFrequency == 0)
            return std::unexpected("時刻印を読めない");

        // --- 回ごとの最小 ---
        probe.solveNs.fill(UINT64_MAX);
        for (uint32_t repeat = 0; repeat < rounds; ++repeat) {
            for (uint32_t part = 0; part < 2 * PROBE_PARTS; ++part) {
                const bool isSmall = part < PROBE_PARTS;
                const uint32_t stamp = (repeat * 2 * PROBE_PARTS) + (2 * (part % PROBE_PARTS)) + (isSmall ? 0 : 1);
                const std::vector<uint64_t>& ticks = isSmall ? smallTicks : largeTicks;
                const uint64_t ns = TicksToNs(ticks[stamp], ticks[stamp + 1], timestampFrequency);
                probe.solveNs[part] = std::min(probe.solveNs[part], ns);
            }
        }

        probe.gpuNs = TicksToNs(largeTicks[0], largeTicks[stamps + 1], timestampFrequency);
        probe.cost = FitTimeTreeSolveCost(probe.solveNs, smallBox.cells, largeBox.cells, options.marginPercent);

        return probe;
    }

}  // namespace bicameral::sim
