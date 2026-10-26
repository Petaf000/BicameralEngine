// case_b_box_test.cpp — 案 B の試作の箱(B0。T-0866)と場面・計器(B1。T-0867)の試験。CPU だけ。docs/design/21 §5.2。
//
// B0(引数なし): 物質の欄を読む(既定・誤り・警告。21 §1.1・§1.3)/ 面の運動量の置き場 ①・④ / 受け渡しと帳簿で保存の誤差が 0 /
//   2 回の実行でハッシュの列が一致 / 重さの向きとレベルを引数に / 粗くすると真実が和 / 陰の圧力の口が式を解く。
//   動作確認の段(ToyStages)は**物理ではない**(箱の受け渡し・帳簿・口が使えることを見るためだけの、保存する作り物の式)。
// B1(--gauge): 6 つの場面が 0.25 m・0.5 m・2 m で読めて、同じ物の量になる / 計器の値(水位・角度・圧力・動く面・費用)と
//   列の関数(揺り戻し・戻る時間・届いた時・最大・比べ)が分かっている入力で正しい。
// 細かくする口(T-1106。引数なしの組): 詰まり切らない絵のセルの粒・液体を下の子から詰め、物質ごとの量は同じ数で配った時とビット一致。
// 表示の小数は FormatFixed(整数)。結果は docs/tickets/T-0866・T-0867。
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/fixed.hlsli"
#include "sim/case_b/case_b_box.h"
#include "sim/case_b/case_b_gauge.h"
#include "sim/case_b/case_b_scene.h"

namespace {

    using namespace bicameral::sim::case_b;
    namespace fx = bicameral::fx;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // --- 動作確認の段(物理ではない)---
    // 圧力: C = 1 mg/µPa・K = 1/4 mg/µPa・右辺 = 気体の質量の平均からの差 → 箱の圧力へ(口が呼べることだけを見る)
    void ToyPressure(Box& box, const StepContext& context) {
        PressureSystem system;
        system.sizeX = box.config.sizeX;
        system.sizeY = box.config.sizeY;
        system.sizeZ = box.config.sizeZ;
        const uint32_t cells = CellCount(box);
        int64_t total = 0;
        for (uint32_t cell = 0; cell < cells; ++cell)
            total += context.view.cells[cell].phaseMassMg[(uint32_t)Phase::Gas];

        for (uint32_t cell = 0; cell < cells; ++cell) {
            system.capacities.push_back((uint64_t)1 << 48);
            system.sources.push_back(context.view.cells[cell].phaseMassMg[(uint32_t)Phase::Gas] - (total / cells));
            for (uint32_t axis = 0; axis < 3; ++axis) {
                if (const auto neighbor = Neighbor(box, cell, axis))
                    system.links.push_back({.left = cell, .right = *neighbor, .coefficient = (uint64_t)1 << 46});
            }
        }

        const PressureCost cost = context.pressureSolver(system, box.pressureMicroPascal);
        EXPECT(cost.worstErrorMg <= 16);
    }

    // 重さ: 重さの軸の面に、両側の置き場の質量の平均 × 加速 × 1 刻み(帳簿の Gravity)
    void ToyGravity(Box& box, const StepContext& context) {
        const uint32_t axis = context.view.gravityAxis;
        const int64_t acceleration = box.config.accelerationUmPerS2[axis];
        const uint32_t slots = SlotCount(box.config.layout);
        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            const auto neighbor = Neighbor(box, cell, axis);
            if (!neighbor.has_value())
                continue;

            for (uint32_t slot = 0; slot < slots; ++slot) {
                int64_t mass = 0;
                for (uint32_t phase = 0; phase < MOVING_PHASE_COUNT; ++phase) {
                    if (MomentumSlot(box.config.layout, (Phase)phase) != slot)
                        continue;

                    mass += context.view.cells[cell].phaseMassMg[phase] +
                            context.view.cells[*neighbor].phaseMassMg[phase];
                }

                // mg × µm/s² × 2^20 ÷ 10^6 ÷ 刻み/秒 ÷ 2(平均)
                const int64_t impulse = fx::FxMulDivS64(mass * (acceleration / 1000), (uint64_t)1 << 20,
                                                        1000ull * box.config.ticksPerSecond * 2);
                ApplyImpulse(box, {.axis = axis, .cell = cell, .slot = slot}, impulse, ImpulseSource::Gravity);
            }
        }
    }

    // 運ぶ: 面の速さで、風上のセルの置き場の物質を 1/16 まで風下へ(エネルギーは質量の割合で一緒に)
    void ToyTransport(Box& box, const StepContext& context) {
        const uint32_t slots = SlotCount(box.config.layout);
        const uint32_t edge = CellEdgeUm(box.config.level);
        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const auto neighbor = Neighbor(box, cell, axis);
                if (!neighbor.has_value())
                    continue;

                for (uint32_t slot = 0; slot < slots; ++slot) {
                    const int64_t velocity = FaceVelocity(box, context.view,
                                                          {.axis = axis, .cell = cell, .slot = slot});
                    const uint32_t from = velocity > 0 ? cell : *neighbor;
                    const uint32_t to = velocity > 0 ? *neighbor : cell;
                    // 動く割合(Q16)= |v| × 1/60 s ÷ Δx、1/16 まで
                    const uint64_t share = std::min<uint64_t>(
                        fx::FxAbsU64(velocity) * 1000000 / ((uint64_t)edge * box.config.ticksPerSecond * 16), 4096);
                    for (uint32_t material = 0; material < box.materials.size(); ++material) {
                        const Phase phase = box.materials[material].phase;
                        if (phase == Phase::Fixed || MomentumSlot(box.config.layout, phase) != slot)
                            continue;

                        const int64_t mass = CellMass(box, from, material);
                        const auto moved = (int64_t)(((uint64_t)mass * share) >> 16);
                        if (moved <= 0)
                            continue;

                        const int64_t total = context.view.cells[from].totalMassMg;
                        const int64_t energy = total == 0 ? 0
                                                          : fx::FxMulDivS64(box.energyMillijoules[from],
                                                                            (uint64_t)moved, (uint64_t)total);
                        MoveMass(box, from, to, material, moved);
                        MoveEnergy(box, from, to, energy);
                    }
                }
            }
        }
    }

    // 当てる段: 面の運動量の 1/8 を同じ軸の + 側の隣の面へ移す(受け渡し)、残りの 1/8 を壁の力積で消す(帳簿の Wall)
    void ToyApply(Box& box, const StepContext&) {
        const uint32_t slots = SlotCount(box.config.layout);
        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const auto neighbor = Neighbor(box, cell, axis);
                if (!neighbor.has_value())
                    continue;

                for (uint32_t slot = 0; slot < slots; ++slot) {
                    const FaceRef face = {.axis = axis, .cell = cell, .slot = slot};
                    const int64_t momentum = box.faceMomentum[axis][FaceIndex(box, cell, slot)];
                    ApplyImpulse(box, face, -(momentum / 8), ImpulseSource::Wall);
                    if (Neighbor(box, *neighbor, axis).has_value())
                        MoveMomentum(box, face, {.axis = axis, .cell = *neighbor, .slot = slot}, momentum / 8);
                }
            }
        }
    }

    Stages ToyStages() {
        return {.drag = nullptr,
                .pressure = ToyPressure,
                .shockZone = nullptr,
                .momentum = ToyGravity,
                .grainYield = nullptr,
                .transport = ToyTransport,
                .apply = ToyApply};
    }

    // =============================================================================================
    // B0
    // =============================================================================================

    void TestMaterials() {
        Diagnostics diagnostics;
        std::vector<MaterialDefinition> materials = StandardMaterials();
        const std::vector<Material> resolved = ResolveMaterials(materials, LEVEL0_CELL_EDGE_UM, diagnostics);
        EXPECT(diagnostics.errors.empty());
        EXPECT(resolved[0].phase == Phase::Gas && resolved[1].phase == Phase::Liquid);
        EXPECT(resolved[2].phase == Phase::Grain && resolved[3].phase == Phase::Fixed);
        EXPECT(resolved[2].grainSizeUm == 300 && resolved[2].reposeAngleDegrees == 34);
        EXPECT(resolved[2].packedFractionPct == 60 && resolved[2].cohesionPa == 0);

        // 既定: 書かない粒は詰まり 60・くっつく 0・角度 35(警告)。古い loose だけは 300 µm(警告)
        MaterialDefinition loose = {.name = "old", .phase = WrittenPhase::Solid, .molarMassMgPerMol = 1};
        loose.densityKgPerM3 = 1500;
        loose.solidMotionLoose = true;
        Diagnostics looseDiagnostics;
        const std::vector<Material> looseResolved = ResolveMaterials(std::span<const MaterialDefinition>(&loose, 1),
                                                                     LEVEL0_CELL_EDGE_UM, looseDiagnostics);
        EXPECT(looseDiagnostics.errors.empty() && looseDiagnostics.warnings.size() == 2u);
        EXPECT(looseResolved[0].phase == Phase::Grain && looseResolved[0].grainSizeUm == 300);
        EXPECT(looseResolved[0].packedFractionPct == 60 && looseResolved[0].reposeAngleDegrees == 35);

        // 誤り(21 §1.3): セル以上の粒・気体に粒の欄・詰まりの範囲・粒の密度なし・液体の密度なし
        std::vector<MaterialDefinition> wrong(5);
        wrong[0] = {.name = "boulder", .phase = WrittenPhase::Solid, .molarMassMgPerMol = 1};
        wrong[0].densityKgPerM3 = 2600;
        wrong[0].grainSizeUm = 600000;
        wrong[1] = {.name = "dusty_gas", .phase = WrittenPhase::Gas, .molarMassMgPerMol = 1};
        wrong[1].grainSizeUm = 20;
        wrong[2] = {.name = "loose_pack", .phase = WrittenPhase::Solid, .molarMassMgPerMol = 1};
        wrong[2].densityKgPerM3 = 2600;
        wrong[2].grainSizeUm = 300;
        wrong[2].packedFractionPct = 80;
        wrong[3] = {.name = "light_grain", .phase = WrittenPhase::Solid, .molarMassMgPerMol = 1};
        wrong[3].grainSizeUm = 300;
        wrong[4] = {.name = "thin_liquid", .phase = WrittenPhase::Liquid, .molarMassMgPerMol = 1};
        Diagnostics wrongDiagnostics;
        (void)ResolveMaterials(wrong, LEVEL0_CELL_EDGE_UM, wrongDiagnostics);
        EXPECT(wrongDiagnostics.errors.size() == 5u);

        // 環境: 既定(B-d)と誤り
        Diagnostics environmentDiagnostics;
        const Environment environment = ResolveEnvironment({}, environmentDiagnostics);
        EXPECT(environment.shockZonePa == 10000 && environment.pressureRelaxMsPerM == 3 &&
               environment.dropSizeUm == 2000);
        EXPECT(environmentDiagnostics.errors.empty() && environmentDiagnostics.warnings.empty());
        Diagnostics badEnvironment;
        (void)ResolveEnvironment({.shockZonePa = 0, .pressureRelaxMsPerM = -1}, badEnvironment);
        EXPECT(badEnvironment.errors.size() == 2u);

        // 誤りがあれば箱を作らない
        const auto refused = CreateBox({}, wrong, {});
        EXPECT(!refused.has_value());
        std::printf("物質: 誤り %zu 件・警告 %zu 件を知らせた(既定の粒 = 300 µm・詰まり 60・角度 35)\n",
                    wrongDiagnostics.errors.size(), looseDiagnostics.warnings.size());
    }

    void TestShape() {
        BoxConfig config = {.sizeX = 4, .sizeY = 3, .sizeZ = 2, .level = -2};
        auto created = CreateBox(config, StandardMaterials(), {});
        EXPECT(created.has_value());
        const Box& box = *created;
        EXPECT(CellCount(box) == 24 && CellEdgeUm(-2) == 2000000 && CellEdgeUm(1) == 250000);
        EXPECT(CellVolumeMm3(box) == 8000000000ull);
        const uint32_t cell = CellIndex(box, 3, 2, 1);
        EXPECT(CellCoordinates(box, cell) == (std::array<uint32_t, 3>{3, 2, 1}));
        EXPECT(!Neighbor(box, cell, 0).has_value() &&
               Neighbor(box, CellIndex(box, 0, 2, 1), 0) == CellIndex(box, 1, 2, 1));
        EXPECT(SlotCount(MomentumLayout::PerPhase) == 3 && SlotCount(MomentumLayout::GrainSeparate) == 2);
        EXPECT(MomentumSlot(MomentumLayout::GrainSeparate, Phase::Gas) ==
               MomentumSlot(MomentumLayout::GrainSeparate, Phase::Liquid));
        EXPECT(MomentumSlot(MomentumLayout::GrainSeparate, Phase::Grain) == 1);
        // 層と舞う粒(T-1138): 流体・層・舞う粒の 3 つ。ほかの置き場では舞う粒は粒の置き場
        EXPECT(SlotCount(MomentumLayout::GrainTwoState) == 3);
        EXPECT(MomentumSlot(MomentumLayout::GrainTwoState, Phase::Gas) == 0 &&
               MomentumSlot(MomentumLayout::GrainTwoState, Phase::Liquid) == 0);
        EXPECT(MomentumSlot(MomentumLayout::GrainTwoState, Phase::Grain) == 1 &&
               SuspendedGrainSlot(MomentumLayout::GrainTwoState) == 2);
        EXPECT(SuspendedGrainSlot(MomentumLayout::GrainSeparate) == 1 &&
               SuspendedGrainSlot(MomentumLayout::PerPhase) == MomentumSlot(MomentumLayout::PerPhase, Phase::Grain));
        EXPECT(box.faceMomentum[0].size() == 24u * 3);
    }

    // 重さの向き: 加速が + x なら柱は x の軸で、+ x の端が「下」(柱の重さが最も大きい)
    void TestGravityDirection() {
        auto created = BuildScene(MakeDamBreakScene(), 0);
        EXPECT(created.has_value());
        Box box = std::move(*created);
        const View down = BuildView(box);
        EXPECT(down.gravityAxis == 1 && down.gravityDown);
        const uint32_t bottom = CellIndex(box, 0, 1, 0);
        const uint32_t top = CellIndex(box, 0, 4, 0);
        // 水 4 段の一番下のセルの上には 3 段の水 + 空気(約 3 × 4900 Pa)
        EXPECT(down.cells[bottom].columnAboveMicroPascal > down.cells[top].columnAboveMicroPascal);
        EXPECT(down.cells[bottom].columnAboveMicroPascal / 1000000 >= 14000 &&
               down.cells[bottom].columnAboveMicroPascal / 1000000 <= 15000);

        box.config.accelerationUmPerS2 = {9806650, 0, 0};
        const View sideways = BuildView(box);
        EXPECT(sideways.gravityAxis == 0 && !sideways.gravityDown);
        // 加速が + x なので − x の端が「上」(x 0 の上には何も無い)
        EXPECT(sideways.cells[CellIndex(box, 0, 1, 0)].columnAboveMicroPascal == 0);
        EXPECT(sideways.cells[CellIndex(box, 5, 1, 0)].columnAboveMicroPascal >
               sideways.cells[CellIndex(box, 1, 1, 0)].columnAboveMicroPascal);
    }

    Recording RunToy(MomentumLayout layout, int32_t level, uint64_t ticks) {
        auto created = BuildScene(MakeDamBreakScene(), level, layout);
        EXPECT(created.has_value());
        Box box = std::move(*created);
        const Stages stages = ToyStages();
        const PressureSolver solver = MakeImplicitGridSolver(8, 4);
        const std::vector<BoxRegion> regions = {*FindRegion(MakeDamBreakScene(), "reservoir", level)};

        return RunScene(box, stages, solver, ticks, regions);
    }

    // 保存の誤差 0・2 回の実行でハッシュ一致・案 ① と ④ の両方(21 §5.2 の B0 の合格)
    void TestConservationAndDeterminism() {
        for (const MomentumLayout layout : {MomentumLayout::PerPhase, MomentumLayout::GrainSeparate}) {
            const Recording first = RunToy(layout, 0, 120);
            const Recording second = RunToy(layout, 0, 120);
            EXPECT(first.AllConserved());
            EXPECT(first.HashChain() == second.HashChain());
            EXPECT(first.samples.back().hash != first.samples.front().hash);  // 動いた
            EXPECT(first.samples.back().movingFaces > 0);
            const auto water = RegionSeries(first, 0, &RegionSample::liquidLevelUm);
            std::printf(
                "動作確認の段(物理ではない)%s: 120 刻み 保存 %s・ハッシュ %016llx(2 回目 %s)・池の水位 %s → %s m・%s "
                "ns/セル/刻み\n",
                layout == MomentumLayout::PerPhase ? "案 ①" : "案 ④", first.AllConserved() ? "0" : "ずれた",
                (unsigned long long)first.HashChain(), first.HashChain() == second.HashChain() ? "一致" : "違う",
                FormatFixed(water.front() / 1000, 3).c_str(), FormatFixed(water.back() / 1000, 3).c_str(),
                std::to_string(first.NanosecondsPerCellTick()).c_str());
        }

        // 保存の検査が壊れを捕まえる: 帳簿を通さない書き換え
        auto created = BuildScene(MakeDamBreakScene(), 0);
        Box box = std::move(*created);
        CellMass(box, 0, MaterialIndex(box, "water")) += 1;
        box.faceMomentum[1][FaceIndex(box, 0, 0)] += 1;
        const ConservationError error = CheckConservation(box);
        EXPECT(!error.IsZero() && error.massMg[MaterialIndex(box, "water")] == 1 && error.momentum[1] == 1);

        // 壁の面に運動量を置くのは誤り
        Box wall = std::move(*BuildScene(MakeDamBreakScene(), 0));
        ApplyImpulse(wall, {.axis = 0, .cell = CellIndex(wall, 23, 1, 0), .slot = 0}, 5, ImpulseSource::Outside);
        EXPECT(CheckConservation(wall).wallFaces == 1);
    }

    // 粗くする: 真実(物質・エネルギー)は和(厚さ 1 セルの z は板として 4 倍に押し出す)。場面の絵から 2 m で作った箱と同じ
    void TestCoarsen() {
        Box fine = std::move(*BuildScene(MakeSandPileScene(), 0));
        const auto coarse = CoarsenBox(fine, 4);
        EXPECT(coarse.has_value());
        const ConservedTotals a = MeasureTotals(fine);
        const ConservedTotals b = MeasureTotals(*coarse);
        for (size_t material = 0; material < a.massMg.size(); ++material)
            EXPECT(a.massMg[material] * 4 == b.massMg[material]);

        EXPECT(a.energyMillijoules * 4 == b.energyMillijoules);
        const Box direct = std::move(*BuildScene(MakeSandPileScene(), -2));
        EXPECT(MeasureTotals(direct).massMg[MaterialIndex(direct, "sand")] == b.massMg[MaterialIndex(direct, "sand")]);
        EXPECT(coarse->config.level == -2 && coarse->config.sizeX == 8);
        EXPECT(!CoarsenBox(fine, 3).has_value());
    }

    // 陰の圧力の口: 小さい系を解いて式の誤差が許容差の内(C-0 を差し込む口の形の確かめ)
    void TestPressurePort() {
        PressureSystem system = {.sizeX = 8, .sizeY = 8, .sizeZ = 1};
        for (uint32_t cell = 0; cell < 64; ++cell) {
            system.capacities.push_back((uint64_t)1 << 40);
            system.sources.push_back(cell == 0 ? 1000000 : (cell == 63 ? -1000000 : 0));
            if (cell % 8 != 7)
                system.links.push_back({.left = cell, .right = cell + 1, .coefficient = (uint64_t)1 << 48});

            if (cell < 56)
                system.links.push_back({.left = cell, .right = cell + 8, .coefficient = (uint64_t)1 << 48});
        }

        std::vector<int64_t> values(64, 0);
        const PressureCost cost = MakeImplicitGridSolver(40, 8)(system, values);
        EXPECT(cost.worstErrorMg <= 8 && cost.cycles > 0);
        EXPECT(PressureSystemWorstError(system, values) <= 8 + 8);
        EXPECT(values[0] > 0 && values[63] < 0);
        std::printf("陰の圧力の口(C-0 の代わりの多重格子): V サイクル %u 回・式の誤差 %lld mg\n", cost.cycles,
                    (long long)cost.worstErrorMg);
    }

    // =============================================================================================
    // B1
    // =============================================================================================

    int64_t MaterialTotal(const Box& box, std::string_view name) {
        return MeasureTotals(box).massMg[MaterialIndex(box, name)];
    }

    // 6 つの場面が 3 つの細かさで読め、凝縮相の量が厚さあたりで同じ(気体は丸めで ±0.1 %)。
    // z は 1 セルなので、2 m の箱は 0.5 m の 4 倍・0.25 m の箱は 1/2 倍の厚さ
    void TestScenesLoad() {
        for (const Scene& scene : AllScenes()) {
            const auto middle = BuildScene(scene, 0);
            EXPECT(middle.has_value());
            if (!middle.has_value()) {
                std::printf("  %s: %s\n", scene.name.c_str(), middle.error().c_str());
                continue;
            }

            const auto coarse = BuildScene(scene, -2);
            const auto fine = BuildScene(scene, 1);
            EXPECT(coarse.has_value() && fine.has_value());
            for (const char* name : {"water", "sand", "stone"}) {
                EXPECT(MaterialTotal(*coarse, name) == MaterialTotal(*middle, name) * 4);
                EXPECT(MaterialTotal(*fine, name) * 2 == MaterialTotal(*middle, name));
            }

            EXPECT(DifferencePermille(MaterialTotal(*coarse, "air"), MaterialTotal(*middle, "air") * 4) <= 1);
            EXPECT(DifferencePermille(MaterialTotal(*fine, "air") * 2, MaterialTotal(*middle, "air")) <= 1);
            EXPECT(CheckConservation(*middle).IsZero());
            std::printf("場面 %s: %u × %u セル(0.5 m)・2 m で %u × %u・水 %lld g・砂 %lld g・空気 %lld g\n",
                        scene.name.c_str(), middle->config.sizeX, middle->config.sizeY, coarse->config.sizeX,
                        coarse->config.sizeY, (long long)(MaterialTotal(*middle, "water") / 1000),
                        (long long)(MaterialTotal(*middle, "sand") / 1000),
                        (long long)(MaterialTotal(*middle, "air") / 1000));
        }

        // 場面の書き間違い
        Scene broken = MakeDamBreakScene();
        broken.rows[0][0] = '?';
        EXPECT(!BuildScene(broken, 0).has_value());
        EXPECT(!BuildScene(MakeSiphonScene(), 3).has_value());
    }

    // 計器が分かっている形で正しい値を出す
    void TestGaugeValues() {
        const Scene dam = MakeDamBreakScene();
        for (const int32_t level : {0, -2, 1}) {
            Box box = std::move(*BuildScene(dam, level));
            const std::vector<BoxRegion> regions = {*FindRegion(dam, "reservoir", level),
                                                    *FindRegion(dam, "wall_column", level)};
            const GaugeSample sample = MeasureSample(box, regions);
            // 池の水位 2 m(4 セル)・壁の列は 0・止まっている
            EXPECT(sample.regions[0].liquidLevelUm >= 1999000 && sample.regions[0].liquidLevelUm <= 2000000);
            EXPECT(sample.regions[1].liquidLevelUm == 0);
            EXPECT(sample.movingFaces == 0 && sample.maxSpeed == 0 && sample.conserved);
            // 2 m の箱は x 0 の列だけ(下のセルが水 3/4・上が 1/4)
            EXPECT(sample.regions[0].surfaceCells == (level < 0 ? 1u : 6u << (level > 0 ? 1 : 0)));
        }

        // 砂山: 4 列 × 6 m の柱(斜面なし = 90°)
        const Scene pile = MakeSandPileScene();
        Box pileBox = std::move(*BuildScene(pile, 0));
        const GaugeSample pileSample = MeasureSample(pileBox, std::vector<BoxRegion>{*FindRegion(pile, "pile", 0)});
        EXPECT(pileSample.regions[0].reposeAngleDegrees == 90);
        EXPECT(pileSample.regions[0].grainLevelUm == 6000000 * 4 / 32);

        // 角度: 34° の斜面の山(高さ 0 から 1 列ごとに tan 34° × 0.5 m)
        const int64_t step = 337252;  // 0.5 m × tan 34°(µm)
        std::vector<int64_t> heights;
        for (int64_t column = 0; column < 9; ++column)
            heights.push_back(step * (4 - (column > 4 ? column - 4 : 4 - column)) + step);

        EXPECT(SlopeAngleDegrees(heights, 500000) == 34);
        EXPECT(AngleFromSlopeQ16(65536) == 45 && AngleFromSlopeQ16(0) == 0);
        heights[0] = 0;
        EXPECT(SlopeAngleDegrees(heights, 500000) == 34);
    }

    // 列の関数
    void TestSeries() {
        // 減衰する揺れ: 100, −50, 25, −12, 6, −3, 0 ...(閾値 10 を超えて戻ったのは 4 回)
        const std::vector<int64_t> ringing = {0, 100, -50, 25, -12, 6, -3, 0, 0, 0};
        EXPECT(CountReversals(ringing, 10) == 4);
        EXPECT(CountReversals(ringing, 200) == 0);
        // 過減衰: 揺り戻し 0・戻る時間
        const std::vector<int64_t> relax = {1000, 500, 250, 125, 62, 31, 15, 7, 3, 1};
        EXPECT(CountReversals(relax, 10) == 0);
        EXPECT(SettleTick(relax, 0, 10) == 7u);
        EXPECT(!SettleTick(relax, 0, 0).has_value());
        EXPECT(FirstTickAtLeast(ringing, 100) == 1u && !FirstTickAtLeast(ringing, 101).has_value());
        const Peak peak = PeakOf(ringing);
        EXPECT(peak.value == 100 && peak.tick == 1);
        EXPECT(DifferencePermille(1000, 1100) == 90 && DifferencePermille(0, 0) == 0);
        EXPECT(FormatFixed(2130, 3) == "2.130" && FormatFixed(-5, 2) == "-0.05" && FormatFixed(7, 0) == "7");
    }

    // 弱い差の戻る時間を 0.5 m と 2 m で同じ形で測れる(B-c の口。値の合否は R1)
    void TestRelaxComparisonPort() {
        const Scene scene = MakePressureRelaxScene();
        for (const int32_t level : {0, -2}) {
            Box box = std::move(*BuildScene(scene, level));
            const std::vector<BoxRegion> regions = {*FindRegion(scene, "high", level),
                                                    *FindRegion(scene, "low", level)};
            const Recording recording = RunScene(box, ToyStages(), MakeImplicitGridSolver(8, 4), 30, regions);
            const auto high = RegionSeries(recording, 0, &RegionSample::meanPressureMicroPascal);
            EXPECT(high.size() == 31u && recording.AllConserved());
            const auto gas = recording.samples.front().regions[0].phaseMassMg[0];
            const auto gasLow = recording.samples.front().regions[1].phaseMassMg[0];
            // 高い側は約 2 % 多い空気
            EXPECT(gas > gasLow && DifferencePermille(gas, gasLow) >= 15 && DifferencePermille(gas, gasLow) <= 25);
        }
    }

    // --- 細かくする口(T-1106)---
    // 8 列 × 2 段の絵: 下の段は石、上の段に石で隔てて 3/10・7/10 の詰まった砂・水 50 %・空気に舞う砂 1 %(隔てるので表面の傾きは 0)
    Scene MakeRefineScene() {
        Scene scene;
        scene.name = "refine";
        scene.materials = StandardMaterials();
        const std::vector<GasFill> air = {{.material = "air", .molePermille = 1000}};
        const auto atmosphere = (uint64_t)ATMOSPHERE_PA;
        scene.legend = {
            {.symbol = '#', .condensed = {{.material = "stone", .volumePct = 100}}, .gasPressurePa = 0, .gas = {}},
            {.symbol = '3',
             .condensed = {{.material = "sand", .volumePct = 18}},
             .gasPressurePa = atmosphere,
             .gas = air},
            {.symbol = '7',
             .condensed = {{.material = "sand", .volumePct = 42}},
             .gasPressurePa = atmosphere,
             .gas = air},
            {.symbol = 'w',
             .condensed = {{.material = "water", .volumePct = 50}},
             .gasPressurePa = atmosphere,
             .gas = air},
            {.symbol = 'd',
             .condensed = {{.material = "sand", .volumePct = 1}},
             .gasPressurePa = atmosphere,
             .gas = air,
             .suspended = true},
        };
        scene.rows = {"#3#7#w#d", "########"};

        return scene;
    }

    // 物質 name の、絵の列 pictureX・絵の段 1 の子(下から row 段目。左の子)の質量
    int64_t ChildMass(const Box& box, uint32_t fine, uint32_t pictureX, uint32_t row, std::string_view name) {
        return CellMass(box, CellIndex(box, pictureX * fine, fine + row, 0), MaterialIndex(box, name));
    }

    void TestRefineSettles() {
        Scene scene = MakeRefineScene();
        for (const int32_t level : {1, 2}) {
            const uint32_t fine = 1u << level;
            scene.refineFill = RefineFill::Same;
            const Box same = std::move(*BuildScene(scene, level));
            scene.refineFill = RefineFill::Settle;
            const Box settled = std::move(*BuildScene(scene, level));
            const Box again = std::move(*BuildScene(scene, level));

            // 物質ごとの量は同じ数で配った時とビット一致・作り直すと同じ・帳簿は 0
            for (const char* name : {"water", "sand", "stone"})
                EXPECT(MaterialTotal(settled, name) == MaterialTotal(same, name));

            EXPECT(HashBox(settled) == HashBox(again));
            EXPECT(CheckConservation(settled).IsZero());

            // 3/10: 列の和(fine × 18 %)は下の子に収まる
            const int64_t packedChild = ChildMass(same, fine, 1, 0, "sand") * 60 / 18;
            const int64_t columnThree = ChildMass(same, fine, 1, 0, "sand") * fine;
            if (level == 1) {
                EXPECT(ChildMass(settled, fine, 1, 0, "sand") == columnThree);
                EXPECT(ChildMass(settled, fine, 1, 1, "sand") == 0);
            }

            // 7/10: 下の子は詰まり切る(±1 ‰)・上は残り。同じ数で配ると子は同じ
            const int64_t bottom = ChildMass(settled, fine, 3, 0, "sand");
            EXPECT(DifferencePermille(bottom, packedChild) <= 1);
            int64_t columnSeven = 0;
            for (uint32_t row = 0; row < fine; ++row)
                columnSeven += ChildMass(settled, fine, 3, row, "sand");

            EXPECT(columnSeven == ChildMass(same, fine, 3, 0, "sand") * fine);
            EXPECT(level == 1 || ChildMass(settled, fine, 3, fine - 1, "sand") == 0);
            EXPECT(ChildMass(same, fine, 3, 0, "sand") == ChildMass(same, fine, 3, fine - 1, "sand"));

            // 水 50 %: 下の半分の子が水で満ちて空気はほぼ無い・上の子に水は無い
            EXPECT(ChildMass(settled, fine, 5, 0, "water") > ChildMass(same, fine, 5, 0, "water") * 19 / 10);
            EXPECT(ChildMass(settled, fine, 5, 0, "air") < ChildMass(same, fine, 5, 0, "air") / 100);
            EXPECT(ChildMass(settled, fine, 5, fine - 1, "water") == 0);

            // 舞う砂は同じ濃さのまま
            for (uint32_t row = 0; row < fine; ++row)
                EXPECT(ChildMass(settled, fine, 7, row, "sand") == ChildMass(same, fine, 7, row, "sand"));

            std::printf("細かくする口 %s m: 7/10 の列の砂 下 %lld mg・上 %lld mg(同じ数なら %lld mg ずつ)\n",
                        level == 1 ? "0.25" : "0.125", (long long)bottom,
                        (long long)ChildMass(settled, fine, 3, 1, "sand"),
                        (long long)ChildMass(same, fine, 3, 0, "sand"));
        }

        // 表面の傾き: 9/10・7/10・5/10・3/10 と下がる面は、中の 2 つの絵のセルで左の子の列が多い(絵のセルの中で量は同じ)。端の列は傾き 0
        Scene tilt = MakeRefineScene();
        tilt.rows = {"9753", "####"};
        tilt.legend.push_back({.symbol = '9',
                               .condensed = {{.material = "sand", .volumePct = 54}},
                               .gasPressurePa = (uint64_t)ATMOSPHERE_PA,
                               .gas = {{.material = "air", .molePermille = 1000}}});
        tilt.legend.push_back({.symbol = '5',
                               .condensed = {{.material = "sand", .volumePct = 30}},
                               .gasPressurePa = (uint64_t)ATMOSPHERE_PA,
                               .gas = {{.material = "air", .molePermille = 1000}}});
        const Box tilted = std::move(*BuildScene(tilt, 1));
        tilt.refineFill = RefineFill::Same;
        const Box flat = std::move(*BuildScene(tilt, 1));
        const uint32_t sandIndex = MaterialIndex(tilted, "sand");
        for (uint32_t x = 0; x < 4; ++x) {
            const auto columnSum = [&](const Box& box, uint32_t childX) {
                return CellMass(box, CellIndex(box, childX, 2, 0), sandIndex) +
                       CellMass(box, CellIndex(box, childX, 3, 0), sandIndex);
            };
            const int64_t left = columnSum(tilted, 2 * x);
            const int64_t right = columnSum(tilted, (2 * x) + 1);
            EXPECT(left + right == columnSum(flat, 2 * x) + columnSum(flat, (2 * x) + 1));
            EXPECT((x == 0 || x == 3) ? left == right : left > right);
            std::printf("  傾き: 絵の列 %u の砂 左 %lld mg・右 %lld mg\n", x, (long long)left, (long long)right);
        }

        // 据わる固体を含む列・沈み固まった体積が列以上の列は同じ数で(端数は下へ)
        const Box box = std::move(*BuildScene(scene, 1));
        std::vector<int64_t> column(box.materials.size(), 0);
        column[MaterialIndex(box, "stone")] = 7;
        column[MaterialIndex(box, "sand")] = 5;
        const auto even = PlaceSettleColumn(ScenePlaceMaterials(box.materials), column, 2, CellVolumeMm3(box));
        EXPECT(even[0][MaterialIndex(box, "stone")] == 4 && even[1][MaterialIndex(box, "stone")] == 3);
        EXPECT(even[0][MaterialIndex(box, "sand")] == 3 && even[1][MaterialIndex(box, "sand")] == 2);
    }

    void RunBoxTests() {
        TestMaterials();
        TestShape();
        TestGravityDirection();
        TestConservationAndDeterminism();
        TestCoarsen();
        TestPressurePort();
        TestRefineSettles();
    }

    void RunGaugeTests() {
        TestScenesLoad();
        TestGaugeValues();
        TestSeries();
        TestRelaxComparisonPort();
    }

}  // namespace

int main(int argc, char** argv) {
    const bool gauge = argc > 1 && std::string_view(argv[1]) == "--gauge";
    if (gauge)
        RunGaugeTests();
    else
        RunBoxTests();

    std::printf("case_b %s: %d failure(s)\n", gauge ? "gauge" : "box", failureCount);

    return failureCount == 0 ? 0 : 1;
}
