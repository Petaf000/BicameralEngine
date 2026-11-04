// phase_layer_implicit_test.cpp — 混ざったセル(層 + 気体)を陰の伝導の系に節 2 つ(層・気体)+ 層の上の面で入れる形
// (ADR-0512 の 2 の改め・T-1230 段 2 ②)の CPU の試験と費用の測り。sim/implicit_conduction の系(BuildImplicitGrid・StepImplicit)をそのまま使う。
//
// 場面: レベル L の 8 × 8 × 8 のセル(端は断熱)。y < 3 は石(330 K)、y = 3 は混ざったセル(水の層 + 空気)、y > 3 は空気(300 K)。
// 比べる形:
//   1 節: 混ざったセルをセル全体の 1 節で入れる(今の系の形 = 混ざったセルの無い時と同じ大きさ。費用の基準)
//   2 節: 層の節・気体の節(同じ座標 = 多重格子の縮約で同じ親へ)。面は部分つきの片側(PlPartSideOf)の組ごとに 1 本
//         (重なり PlPairOverlapsOf・係数は両側の小さい方 PlSlotConductance × 4^L。陽の PlPartFaceFlow と同じ値)。層の上の面は PlInnerConductance
// 確かめること: 保存(ImplicitConservedTotal がビット単位で一定)・安全網が働かない・部分の温度が石と空気の間・
//   打ち切り条件(T-1230「(b) の研究の部分」): V サイクルの回数が 1 節の 2 倍を超えない。費用(節・面・1 刻みの節の計算の数)を出す。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

#include "common/implicit_conduction.hlsli"
#include "common/multires_conduction.hlsli"
#include "phase_layer_scene.h"
#include "sim/implicit_conduction.h"

using namespace bicameral;
using namespace bicameral::sim;
namespace {

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    namespace rx = bicameral::reaction;
    namespace mr = bicameral::multires;
    using Amount = test::PlAmount;

    constexpr int64_t EDGE = 8;
    constexpr int64_t SURFACE_Y = 3;
    constexpr uint64_t CELL_VOLUME_PL = 125000000000000ull;
    constexpr uint32_t TICKS = 60;
    constexpr uint32_t STONE_MK = 330000;
    constexpr uint32_t LAYER_MK = 290000;
    constexpr uint32_t AIR_MK = 300000;

    std::vector<Amount> AirFilling(const BakedReactionTable& table, const std::vector<Amount>& condensed) {
        uint64_t volume = 0;
        for (const Amount& entry : condensed)
            volume += entry.amount * table.View().Species(table.SpeciesId(entry.name)).molarVolume;

        const uint64_t open = volume < CELL_VOLUME_PL ? CELL_VOLUME_PL - volume : 0;
        std::vector<Amount> air = {{"nitrogen", test::PL_AIR_NITROGEN_MICROMOL},
                                   {"oxygen", test::PL_AIR_OXYGEN_MICROMOL}};
        for (Amount& entry : air)
            entry.amount = (uint64_t)fx::FxMulDivS64((int64_t)entry.amount, open, CELL_VOLUME_PL);

        return air;
    }

    struct SceneCell {
        rx::RxCell cell;
        int64_t condensedEnergy = 0;
        bool mixed = false;
    };

    // 水の層の厚さの割合(per mille)の混ざったセル
    SceneCell MakeMixed(const BakedReactionTable& table, uint32_t perMille) {
        const uint64_t molarVolume = table.View().Species(table.SpeciesId("water")).molarVolume;
        const std::vector<Amount> water = {{"water", CELL_VOLUME_PL / 1000 * perMille / molarVolume}};
        const std::vector<Amount> gas = AirFilling(table, water);
        const int64_t condensedMj = test::PlPartEnergyMicrojoules(table, water, LAYER_MK) / 1000;
        const int64_t gasMj = test::PlPartEnergyMicrojoules(table, gas, AIR_MK) / 1000;
        std::vector<Amount> all = water;
        all.insert(all.end(), gas.begin(), gas.end());

        SceneCell scene;
        scene.cell = test::MakePhaseLayerCell(table, all, condensedMj + gasMj);
        scene.mixed = rx::PlLayerOf(table.View(), scene.cell).mixed != 0;
        scene.condensedEnergy = condensedMj;

        return scene;
    }

    SceneCell MakeWhole(const BakedReactionTable& table, const std::vector<Amount>& amounts, uint32_t mk) {
        SceneCell scene;
        scene.cell = test::MakePhaseLayerCell(table, amounts, test::PlPartEnergyMicrojoules(table, amounts, mk) / 1000);

        return scene;
    }

    // 水面の割合: 平ら(全部 300‰)か段違い(x の偶奇で 300‰ と 600‰)、薄い露(35‰)、ほぼ満ちた氷の代わりの水(960‰ = 気体のすき間)
    struct Surface {
        const char* name;
        uint32_t perMille;
        uint32_t oddPerMille;
    };

    std::vector<SceneCell> MakeScene(const BakedReactionTable& table, const Surface& surface) {
        const SceneCell stone = MakeWhole(table, {{"stone_real", test::PL_STONE_MICROMOL}}, STONE_MK);
        const SceneCell air = MakeWhole(table, AirFilling(table, {}), AIR_MK);
        const SceneCell even = MakeMixed(table, surface.perMille);
        const SceneCell odd = MakeMixed(table, surface.oddPerMille);
        std::vector<SceneCell> cells;
        for (int64_t n = 0; n < EDGE * EDGE * EDGE; ++n) {
            const int64_t x = n % EDGE;
            const int64_t y = (n / EDGE) % EDGE;
            if (y < SURFACE_Y)
                cells.push_back(stone);
            else if (y > SURFACE_Y)
                cells.push_back(air);
            else
                cells.push_back((x & 1) == 0 ? even : odd);
        }

        return cells;
    }

    int64_t CellIndex(int64_t x, int64_t y, int64_t z) {
        return x + (y * EDGE) + (z * EDGE * EDGE);
    }

    fx::FxU128 Coefficient(uint64_t conductance, int32_t level) {
        return mr::ImWideShiftLeft(mr::ImWide(conductance * rx::HC_LIMIT_PER_CONDUCTANCE),
                                   2 * static_cast<uint32_t>(level));
    }

    ImplicitCell MakeNode(int32_t level, int64_t n, uint64_t heatCapacityNj, uint32_t conductance, int32_t mk) {
        const uint64_t capacity = 8 * ((heatCapacityNj >> rx::HC_CAPACITY_SHIFT) * rx::HC_LIMIT_PER_CAPACITY);

        return {.level = level,
                .x = n % EDGE,
                .y = (n / EDGE) % EDGE,
                .z = n / (EDGE * EDGE),
                .heatCapacity = capacity,
                .conductance = conductance,
                .energy = ImplicitEnergyFor(capacity, mk)};
    }

    struct Built {
        ImplicitGrid grid;
        std::vector<uint32_t> nodeOf;     // セルの節(混ざったセルの 2 節では層の節)
        std::vector<uint32_t> gasNodeOf;  // 混ざったセルの気体の節(1 節なら nodeOf と同じ)
        size_t faceCount = 0;
    };

    uint32_t NodeForPart(const Built& built, size_t cell, uint32_t part) {
        return part == rx::PL_PART_GAS ? built.gasNodeOf[cell] : built.nodeOf[cell];
    }

    void AddFace(std::vector<ImplicitFace>& faces, uint32_t a, uint32_t b, uint64_t conductance, int32_t level) {
        if (conductance == 0 || a == b)
            return;

        faces.push_back({.fine = std::min(a, b),
                         .coarse = std::max(a, b),
                         .gap = 0,
                         .coefficient = Coefficient(conductance, level)});
    }

    Built Build(const BakedReactionTable& table, const std::vector<SceneCell>& scene, int32_t level, bool twoNodes) {
        Built built;
        std::vector<ImplicitCell> nodes;
        built.nodeOf.resize(scene.size());
        built.gasNodeOf.resize(scene.size());
        for (size_t n = 0; n < scene.size(); ++n) {
            const SceneCell& cell = scene[n];
            const bool split = twoNodes && cell.mixed;
            built.nodeOf[n] = static_cast<uint32_t>(nodes.size());
            if (!split) {
                const mr::MrThermal thermal = mr::MrCellThermal(table.View(), cell.cell);
                const rx::RxThermal whole = rx::RxComputeThermal(table.View(), cell.cell);
                nodes.push_back(MakeNode(level, static_cast<int64_t>(n), whole.heatCapacity, thermal.conductance,
                                         static_cast<int32_t>(thermal.temperature)));
                built.gasNodeOf[n] = built.nodeOf[n];
                continue;
            }

            const rx::PlParts parts = rx::PlPartsOf(table.View(), cell.cell, cell.condensedEnergy);
            nodes.push_back(
                MakeNode(level, static_cast<int64_t>(n), parts.condensed.heatCapacity, 0, parts.condensed.temperature));
            built.gasNodeOf[n] = static_cast<uint32_t>(nodes.size());
            nodes.push_back(MakeNode(level, static_cast<int64_t>(n), parts.gas.heatCapacity, 0, parts.gas.temperature));
        }

        // --- 面: +x・+y・+z の隣と 1 本ずつ(2 節は部分の組ごと)---
        std::vector<ImplicitFace> faces;
        const uint32_t positiveFaces[3] = {1, 3, 5};
        for (int64_t z = 0; z < EDGE; ++z) {
            for (int64_t y = 0; y < EDGE; ++y) {
                for (int64_t x = 0; x < EDGE; ++x) {
                    const size_t self = static_cast<size_t>(CellIndex(x, y, z));
                    for (uint32_t axis = 0; axis < 3; ++axis) {
                        const int64_t nx = x + (axis == 0 ? 1 : 0);
                        const int64_t ny = y + (axis == 1 ? 1 : 0);
                        const int64_t nz = z + (axis == 2 ? 1 : 0);
                        if (nx >= EDGE || ny >= EDGE || nz >= EDGE)
                            continue;

                        const size_t other = static_cast<size_t>(CellIndex(nx, ny, nz));
                        const uint32_t face = positiveFaces[axis];
                        const SceneCell& a = scene[self];
                        const SceneCell& b = scene[other];
                        if (!twoNodes || (!a.mixed && !b.mixed)) {
                            const uint64_t ga = mr::MrCellThermal(table.View(), a.cell).conductance;
                            const uint64_t gb = mr::MrCellThermal(table.View(), b.cell).conductance;
                            AddFace(faces, built.nodeOf[self], built.nodeOf[other], std::min(ga, gb), level);
                            continue;
                        }

                        const rx::PlPartSide sideA = rx::PlPartSideOf(table.View(), a.cell, a.condensedEnergy, face);
                        const rx::PlPartSide sideB = rx::PlPartSideOf(table.View(), b.cell, b.condensedEnergy,
                                                                      face ^ 1u);
                        const rx::PlPairOverlaps overlaps = rx::PlPairOverlapsOf(sideA, sideB);
                        auto pair = [&](const rx::PlSideSlot& slotA, const rx::PlSideSlot& slotB, uint32_t overlap) {
                            if (overlap == 0)
                                return;

                            const uint64_t conductance = std::min(rx::PlSlotConductance(slotA, overlap),
                                                                  rx::PlSlotConductance(slotB, overlap));
                            AddFace(faces, NodeForPart(built, self, slotA.part), NodeForPart(built, other, slotB.part),
                                    conductance, level);
                        };
                        pair(sideA.lower, sideB.lower, overlaps.lowerLower);
                        pair(sideA.upper, sideB.upper, overlaps.upperUpper);
                        pair(sideA.lower, sideB.upper, overlaps.lowerUpper);
                        pair(sideA.upper, sideB.lower, overlaps.upperLower);
                    }
                }
            }
        }

        // --- 層の上の面 ---
        if (twoNodes) {
            for (size_t n = 0; n < scene.size(); ++n) {
                if (!scene[n].mixed)
                    continue;

                const rx::PlLayer layer = rx::PlLayerOf(table.View(), scene[n].cell);
                const rx::PlParts parts = rx::PlPartsOf(table.View(), scene[n].cell, scene[n].condensedEnergy);
                AddFace(faces, built.nodeOf[n], built.gasNodeOf[n], rx::PlInnerConductance(parts, layer), level);
            }
        }

        built.faceCount = faces.size();
        built.grid = BuildImplicitGrid(std::move(nodes), std::move(faces));

        return built;
    }

    struct Run {
        uint32_t nodes = 0;
        size_t faces = 0;
        uint32_t gridLevels = 0;
        uint64_t cycles = 0;
        uint32_t worstCycles = 0;
        uint64_t cellUpdates = 0;
        uint32_t limitedCells = 0;
        bool conserved = true;
        int64_t layerMk = 0;  // 真ん中の水面のセルの層(1 節はセル全体)の温度
        int64_t gasMk = 0;
    };

    Run Measure(const BakedReactionTable& table, const std::vector<SceneCell>& scene, int32_t level, bool twoNodes) {
        Built built = Build(table, scene, level, twoNodes);
        Run run;
        run.nodes = static_cast<uint32_t>(built.grid.cells.size());
        run.faces = built.faceCount;
        run.gridLevels = static_cast<uint32_t>(built.grid.levels.size());
        ImplicitOptions options;
        options.method = ImplicitMethod::Multigrid;
        options.cycles = 64;
        options.toleranceMillikelvin = 1;
        const fx::FxU128 total = ImplicitConservedTotal(built.grid);
        for (uint32_t tick = 0; tick < TICKS; ++tick) {
            const ImplicitCost cost = StepImplicit(built.grid, options);
            run.cycles += cost.cycles;
            run.worstCycles = std::max(run.worstCycles, cost.cycles);
            run.cellUpdates += cost.cellUpdates;
            run.limitedCells += cost.limitedCells;
            const fx::FxU128 now = ImplicitConservedTotal(built.grid);
            run.conserved = run.conserved && now.hi == total.hi && now.lo == total.lo;
        }

        const size_t middle = static_cast<size_t>(CellIndex(EDGE / 2, SURFACE_Y, EDGE / 2));
        run.layerMk = ImplicitTemperature(built.grid.cells[built.nodeOf[middle]]) >> 16;
        run.gasMk = ImplicitTemperature(built.grid.cells[built.gasNodeOf[middle]]) >> 16;

        return run;
    }

}  // namespace

int main() {
    const BakedReactionTable table = test::MakePhaseLayerTable();
    const Surface surfaces[] = {
        {"平らな水面 30 %", 300, 300},
        {"段違いの水面 30 % / 60 %", 300, 600},
        {"薄い層 3.5 %(露)", 35, 35},
        {"気体のすき間 4 %(ほぼ満ちた層)", 960, 960},
    };
    // レベル 8(0.5 m ÷ 256 ≈ 2.0 mm)と 10(≈ 0.49 mm): 陰の領域の細かさ(T-1230 の段 2 の測り)
    const int32_t levels[] = {8, 10};
    std::printf(
        "混ざったセルを陰の系に 2 節で入れた時の費用(8³・水面 64 セル・%u 刻み。V サイクルの止め 1 mK・上限 64)\n",
        TICKS);
    for (const Surface& surface : surfaces) {
        const std::vector<SceneCell> scene = MakeScene(table, surface);
        EXPECT(std::ranges::count_if(scene, [](const SceneCell& cell) { return cell.mixed; }) == EDGE * EDGE);
        for (int32_t level : levels) {
            const Run one = Measure(table, scene, level, false);
            const Run two = Measure(table, scene, level, true);
            EXPECT(one.conserved && two.conserved);
            EXPECT(two.limitedCells == 0);
            EXPECT(two.worstCycles < 64);
            // 打ち切り条件: V サイクルの回数が 1 節の 2 倍を超えない
            EXPECT(two.cycles <= 2 * one.cycles);
            // 部分の温度は石と空気(と層の初め)の間
            EXPECT(two.layerMk >= LAYER_MK - 1 && two.layerMk <= STONE_MK + 1);
            EXPECT(two.gasMk >= LAYER_MK - 1 && two.gasMk <= STONE_MK + 1);
            const double nodeRatio = static_cast<double>(two.nodes) / static_cast<double>(one.nodes);
            const double faceRatio = static_cast<double>(two.faces) / static_cast<double>(one.faces);
            const double cycleRatio = static_cast<double>(two.cycles) /
                                      static_cast<double>(std::max<uint64_t>(one.cycles, 1));
            const double updateRatio = static_cast<double>(two.cellUpdates) /
                                       static_cast<double>(std::max<uint64_t>(one.cellUpdates, 1));
            std::printf(
                "  %-32s L%-2d 節 %u → %u (×%.3f)  面 %zu → %zu (×%.3f)  段 %u → %u  V %llu → %llu (×%.2f・最大 %u → "
                "%u)  "
                "節の計算 ×%.3f  安全網 %u → %u  水面の層 %lld K・気体 %lld mK(1 節 %lld mK)\n",
                surface.name, level, one.nodes, two.nodes, nodeRatio, one.faces, two.faces, faceRatio, one.gridLevels,
                two.gridLevels, static_cast<unsigned long long>(one.cycles),
                static_cast<unsigned long long>(two.cycles), cycleRatio, one.worstCycles, two.worstCycles, updateRatio,
                one.limitedCells, two.limitedCells, static_cast<long long>(two.layerMk / 1000),
                static_cast<long long>(two.gasMk), static_cast<long long>(one.layerMk));
        }
    }

    if (failureCount != 0) {
        std::printf("%d 件の失敗\n", failureCount);
        return 1;
    }

    std::printf("ok\n");
    return 0;
}
