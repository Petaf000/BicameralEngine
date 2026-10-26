// case_b_r2_test.cpp — 案 B の研究 R2(T-0869。docs/design/21 §2.3 B-a・§2.4・§5.2): 引きずりを陰の圧力と一緒に解く(case_b_flow の drag)。CPU だけ。
// 1 本目は置き場 ④(流体 1 つ + 粒)で、流体 ↔ 粒・流体 ↔ 据わる固体の孔の引きずりを面ごとに消去して圧力の式に入れる。
// 場面(この試験の中で絵を書く。21 §5.2 の R2 の行): 砂が水中を落ちる(±30 %)・粉が空気中を落ちる(しずくの代わりの強い引きずり)・
// Darcy(孔の栓を通る水 ±10 %)・染みる速さが刻み 1/60 と 1/120 で ±5 %・1 mg の粒で暴れない・0.5 m と 2 m(Darcy は 0.25 m)で同じ。
// 気体 ↔ 液体の引きずり(しずくの終端の速さ・水面の風)と詰まった砂の芯・砂の堤は置き場 ① で(T-0963。引数 5〜8)。
// 研究の記録なので、落ちて止まるのは保存・決定性・式の壊れ(負の質量)だけ。目安の合否は表に出す(結果は docs/tickets/T-0869)。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sim/case_b/case_b_box.h"
#include "sim/case_b/case_b_flow.h"
#include "sim/case_b/case_b_gauge.h"
#include "sim/case_b/case_b_scene.h"

namespace {

    using namespace bicameral::sim::case_b;

    int failureCount = 0;
    bool restsOnLayerOverride = false;    // 2 本目の見直し 1(T-1137)。引数のどこかに keeplayer2
    bool keepLayerOverride = false;       // 層は落ちても層のまま(T-1137 の 2 本目)。引数のどこかに keeplayer
    bool onSurfaceOverride = false;       // 横へ動く層は列の表面に置く(T-1138 の見直し 2)。引数 onsurface
    bool surfaceOnLayerOverride = false;  // 見直し 2 の 2(床は下に層があればよい)。引数 onsurface2
    bool twoStatesOverride = false;  // 粒を層と舞う粒の 2 つの状態で持つ(T-1136・D-659 の研究)。引数のどこかに twostate
    bool splitOverride =
        false;  // 層と舞う粒に別々の運動量の置き場(T-1138。④ の場面を GrainTwoState に)。引数のどこかに split

    MomentumLayout SplitLayout(MomentumLayout layout) {
        return splitOverride && layout == MomentumLayout::GrainSeparate ? MomentumLayout::GrainTwoState : layout;
    }

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr int64_t TOLERANCE_NL = 256;
    constexpr uint32_t CYCLES = 4;  // R1 の主の組(本物の水・V 4 + 物の粘り)

    const char* Verdict(bool passed) {
        return passed ? "合格" : "不合格";
    }

    // --- 場面 ---
    // 物質: 標準(air・water・sand・stone)+ 粉(20 µm)+ 孔のある石(k = 1000 µm²)
    Scene BaseR2Scene(std::string name, uint32_t width, uint32_t height) {
        Scene scene;
        scene.name = std::move(name);
        scene.materials = StandardMaterials();
        MaterialDefinition dust = scene.materials[2];
        dust.name = "dust";
        dust.grainSizeUm = 20;
        scene.materials.push_back(dust);
        MaterialDefinition porous = scene.materials[3];
        porous.name = "porous_stone";
        porous.permeabilityUm2 = 1000;
        scene.materials.push_back(porous);
        const std::vector<GasFill> air = {{.material = "air", .molePermille = 1000}};
        scene.legend = {
            {.symbol = '.', .condensed = {}, .gasPressurePa = (uint64_t)ATMOSPHERE_PA, .gas = air},
            {.symbol = '#', .condensed = {{.material = "stone", .volumePct = 100}}, .gasPressurePa = 0, .gas = {}},
            {.symbol = '~', .condensed = {{.material = "water", .volumePct = 100}}, .gasPressurePa = 0, .gas = {}},
            // 水の中に散らばった砂(粒そのもの 2 %)
            {.symbol = 's',
             .condensed = {{.material = "sand", .volumePct = 2}, {.material = "water", .volumePct = 98}},
             .gasPressurePa = 0,
             .gas = {}},
            // 空気の中に散らばった粉(粒そのもの 1 %)
            {.symbol = 'd',
             .condensed = {{.material = "dust", .volumePct = 1}},
             .gasPressurePa = (uint64_t)ATMOSPHERE_PA,
             .gas = air},
            // 孔のある石(半分)+ 水
            {.symbol = 'p',
             .condensed = {{.material = "porous_stone", .volumePct = 50}, {.material = "water", .volumePct = 50}},
             .gasPressurePa = 0,
             .gas = {}},
        };
        scene.rows.assign(height, std::string(width, '.'));

        return scene;
    }

    void Fill(Scene& scene, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1, char symbol) {
        for (uint32_t y = y0; y <= y1; ++y) {
            for (uint32_t x = x0; x <= x1; ++x)
                scene.rows[scene.rows.size() - 1 - y][x] = symbol;
        }
    }

    // 水の柱 2 m × 8 m。上の 1 m に砂が散らばる(水の上に 0 m の空気。閉じた水の箱)
    Scene MakeSandInWaterScene() {
        Scene scene = BaseR2Scene("sand_in_water", 4, 16);
        Fill(scene, 0, 3, 0, 15, '~');
        Fill(scene, 0, 3, 12, 13, 's');
        scene.regions = {{.name = "column", .xBegin = 0, .xEnd = 4, .yBegin = 0, .yEnd = 16}};

        return scene;
    }

    // 空気の柱 2 m × 8 m。上の 1 m に粉が散らばる
    Scene MakeDustInAirScene() {
        Scene scene = BaseR2Scene("dust_in_air", 4, 16);
        Fill(scene, 0, 3, 12, 13, 'd');
        scene.regions = {{.name = "column", .xBegin = 0, .xEnd = 4, .yBegin = 0, .yEnd = 16}};

        return scene;
    }

    // 連通管の下の管(高さ 1 m)に孔のある石の栓(長さ 2 m)。左の池 4 m・右 1 m
    Scene MakeDarcyScene() {
        Scene scene = BaseR2Scene("darcy_plug", 24, 12);
        Fill(scene, 0, 23, 0, 0, '#');
        Fill(scene, 0, 7, 1, 8, '~');
        Fill(scene, 8, 15, 1, 2, '~');
        Fill(scene, 10, 13, 1, 2, 'p');
        Fill(scene, 8, 15, 3, 10, '#');
        Fill(scene, 16, 23, 1, 2, '~');
        scene.regions = {{.name = "left", .xBegin = 0, .xEnd = 8, .yBegin = 1, .yEnd = 12},
                         {.name = "right", .xBegin = 16, .xEnd = 24, .yBegin = 1, .yEnd = 12}};

        return scene;
    }

    // --- 走らせる ---
    struct Run {
        Recording recording;
        FlowStats stats;
        Box box;
        std::vector<int64_t> grainHeightUm;  // 粒の質量の重心の高さ(刻みごと)
        std::vector<int64_t> grainSpeed;     // 粒の面の速さの最大(刻みごと)
        int64_t maxGrainSpeed = 0;           // 粒の面の速さの最大(2^-20 m/s。全部の刻み)
    };

    int64_t GrainHeightUm(const Box& box) {
        int64_t mass = 0;
        int64_t moment = 0;
        const uint32_t edge = CellEdgeUm(box.config.level);
        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            const std::array<uint32_t, 3> at = CellCoordinates(box, cell);
            for (uint32_t index = 0; index < box.materials.size(); ++index) {
                if (box.materials[index].phase != Phase::Grain)
                    continue;

                const int64_t grain = CellMass(box, cell, index);
                mass += grain;
                moment += grain * (((int64_t)at[1] * edge) + (edge / 2)) / 1000;
            }
        }

        return mass == 0 ? 0 : moment / mass * 1000;
    }

    // 粒の面の速さの最大 = 面の粒の運動量 ÷ 面の粒の質量(解き方と同じ切り上げの平均。計器の FaceVelocity は片側だけの粒を 2 倍に見る)
    int64_t MaxGrainSpeed(const Box& box) {
        const uint32_t slot = MomentumSlot(box.config.layout, Phase::Grain);
        int64_t speed = 0;
        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const std::optional<uint32_t> neighbor = Neighbor(box, cell, axis);
                if (!neighbor.has_value())
                    continue;

                int64_t mass = 1;
                for (uint32_t index = 0; index < box.materials.size(); ++index) {
                    if (box.materials[index].phase == Phase::Grain)
                        mass += CellMass(box, cell, index) + CellMass(box, *neighbor, index);
                }

                const int64_t momentum = box.faceMomentum[axis][FaceIndex(box, cell, slot)];
                speed = std::max<int64_t>(speed, std::abs(momentum) / std::max<int64_t>(mass / 2, 1));
            }
        }

        return speed;
    }

    Run RunR2(const Scene& scene, int32_t level, uint32_t ticksPerSecond, uint64_t seconds,
              const std::vector<std::string>& regionNames, bool drag = true,
              const std::function<void(Box&)>& prepare = nullptr) {
        auto created = BuildScene(scene, level, SplitLayout(MomentumLayout::GrainSeparate));
        EXPECT(created.has_value());
        Run run{.recording = {},
                .stats = {},
                .box = std::move(*created),
                .grainHeightUm = {},
                .grainSpeed = {},
                .maxGrainSpeed = 0};
        run.box.config.ticksPerSecond = ticksPerSecond;
        if (prepare) {
            prepare(run.box);
            ResetLedger(run.box);
        }

        std::vector<BoxRegion> regions;
        for (const std::string& name : regionNames)
            regions.push_back(*FindRegion(scene, name, level));

        const FlowStages flow = MakeFlowStages({.drag = drag,
                                                .grainTwoStates = twoStatesOverride,
                                                .grainLayerKeepsFalling = keepLayerOverride,
                                                .grainLayerRestsOnLayer = restsOnLayerOverride,
                                                .grainLayerPlacesOnSurface = onSurfaceOverride,
                                                .grainLayerSurfaceOnLayer = surfaceOnLayerOverride});
        const PressureSolver solver = MakeImplicitGridSolver(CYCLES, TOLERANCE_NL);
        run.grainHeightUm.push_back(GrainHeightUm(run.box));
        run.recording = RunScene(run.box, flow.stages, solver, seconds * ticksPerSecond, regions, [&](Box& box) {
            run.grainHeightUm.push_back(GrainHeightUm(box));
            run.grainSpeed.push_back(MaxGrainSpeed(box));
            run.maxGrainSpeed = std::max(run.maxGrainSpeed, run.grainSpeed.back());
        });
        run.stats = *flow.stats;
        EXPECT(run.recording.AllConserved());
        EXPECT(!CheckConservation(run.box).negativeMass);

        return run;
    }

    std::string Fixed3(int64_t value) {
        return FormatFixed(value, 3);
    }

    // 2^-20 m/s → mm/s
    int64_t MillimetersPerSecond(int64_t speed) {
        return (speed * 1000) >> 20;
    }

    void PrintStats(const Run& run) {
        std::printf(
            "    費用 %lld ns/セル/刻み(計器込み)・引きずりの面 %llu・孔の面 %llu・頭打ち %llu・粒を止めた面 %llu・"
            "粒を縮めたセル %llu・粒が消えた面 %llu・V 最大 %u・式の誤差 最大 %lld nL\n",
            (long long)run.recording.NanosecondsPerCellTick(), (unsigned long long)run.stats.dragFaces,
            (unsigned long long)run.stats.porousFaces, (unsigned long long)run.stats.cappedDragFaces,
            (unsigned long long)run.stats.blockedGrainFaces, (unsigned long long)run.stats.grainLimitedCells,
            (unsigned long long)run.stats.vanishedGrainFaces, run.stats.maxCycles, (long long)run.stats.maxErrorNl);
    }

    // --- 落ちる速さ(#38・#39 の記録・相が消える所): 重心の高さの窓 [from, to) 秒の傾き ---
    int64_t FallSpeedMmPerS(const Run& run, uint32_t ticksPerSecond, uint32_t fromSecond, uint32_t toSecond) {
        const int64_t drop = run.grainHeightUm[(size_t)fromSecond * ticksPerSecond] -
                             run.grainHeightUm[(size_t)toSecond * ticksPerSecond];
        return drop / (int64_t)(toSecond - fromSecond) / 1000;
    }

    // 2 秒ごとの重心・粒の速さの最大・全部の置き場の速さの最大(mm/s)
    void PrintTrace(const Run& run, uint32_t ticksPerSecond) {
        std::string text;
        for (size_t tick = 0; tick + 1 < run.grainHeightUm.size(); tick += 2 * (size_t)ticksPerSecond) {
            text += std::format("{}:{}/{}/{} ", tick / ticksPerSecond, run.grainHeightUm[tick] / 1000,
                                tick == 0 ? 0 : MillimetersPerSecond(run.grainSpeed[tick - 1]),
                                MillimetersPerSecond(run.recording.samples[tick].maxSpeed));
        }

        std::printf("    経過(秒:重心 mm/粒の最大 mm/s/全部の最大 mm/s)%s\n", text.c_str());
    }

    // 終端の速さ(Ferguson & Church 2004。C₁ 18・C₂ 1.0。T-0869 で計算): 砂 300 µm 水中 42.7 mm/s・粉 20 µm 空気中 30.8 mm/s
    void Settling(const char* title, const Scene& scene, int64_t expectedMmPerS, int64_t tolerancePermille,
                  uint32_t fromSecond, uint32_t toSecond, const std::function<void(Box&)>& prepare = nullptr) {
        std::vector<int64_t> speeds;
        for (const int32_t level : {0, -2}) {
            for (const uint32_t ticks : {60u, 120u}) {
                if (level == -2 && ticks == 120)
                    continue;

                const Run run = RunR2(scene, level, ticks, toSecond, {"column"}, true, prepare);
                const int64_t speed = FallSpeedMmPerS(run, ticks, fromSecond, toSecond);
                speeds.push_back(speed);
                const bool passed = DifferencePermille(speed, expectedMmPerS) <= tolerancePermille;
                std::printf(
                    "  %s %s m・1/%u 秒: %u〜%u 秒の落ちる速さ %lld mm/s(終端 %lld。%s)・粒の速さの最大 %lld mm/s\n",
                    title, level == 0 ? "0.5" : "2", ticks, fromSecond, toSecond, (long long)speed,
                    (long long)expectedMmPerS, Verdict(passed), (long long)MillimetersPerSecond(run.maxGrainSpeed));
                PrintStats(run);
                PrintTrace(run, ticks);
            }
        }

        std::printf("  %s 0.5 m と 2 m の差 %lld ‰・1/60 と 1/120 の差 %lld ‰\n", title,
                    (long long)DifferencePermille(speeds[0], speeds[2]),
                    (long long)DifferencePermille(speeds[0], speeds[1]));
    }

    // 粉を 1/1000 に薄める(1 % → 0.001 %。粉の質量は空気の約 2 %)。1 % のままだと粉の雲が空気の 22 倍重く、
    // 1 粒の終端ではなく重い雲が崩れる流れ(Rayleigh–Taylor)になる(T-0869 の 1 回目: 0.5 m で 87 mm/s・2 m は 1 列で 30 mm/s)。
    // 空いた体積の分だけ空気を足して圧力をそろえる
    void DiluteDust(Box& box) {
        const uint32_t dust = MaterialIndex(box, "dust");
        const uint32_t air = MaterialIndex(box, "air");
        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            int64_t& grain = CellMass(box, cell, dust);
            if (grain == 0)
                continue;

            grain /= 1000;
            CellMass(box, cell, air) = CellMass(box, cell, air) * 10000 / 9901;
            // 熱も 300 K で作り直す(粉の熱容量が抜けて空気が 2800 K になり、2 m の箱が爆発した。T-0869)
            box.energyMillijoules[cell] = ThermalEnergyMillijoules(box, cell, 300000);
        }
    }

    // --- Darcy(#20・#32): 栓を通る流れ = k ρ g Δh ÷ (μ L) × 管の高さ。染みる速さが刻みによらない ---
    void Darcy() {
        const Scene scene = MakeDarcyScene();
        std::vector<int64_t> rates;
        for (const auto& [level, ticks] : {std::pair{0, 60u}, std::pair{0, 120u}, std::pair{1, 60u}}) {
            const Run run = RunR2(scene, level, ticks, 12, {"left", "right"});
            const auto left = RegionSeries(run.recording, 0, &RegionSample::liquidLevelUm);
            const auto right = RegionSeries(run.recording, 1, &RegionSample::liquidLevelUm);
            const size_t from = 2 * ticks;
            const size_t to = 12 * ticks;
            // 左の池(幅 4 m)から出た体積の流れ(m²/s の µ)÷ 管の高さ 1 m = 栓の見かけの速さ q(µm/s)
            const int64_t rate = (left[from] - left[to]) * 4 / 10;
            const int64_t head = ((left[from] - right[from]) + (left[to] - right[to])) / 2;
            // q = k ρ g Δh ÷ (μ L) = 1e-9 × 1000 × 9.80665 × Δh ÷ (8.9e-4 × 2)(µm/s。Δh は µm)
            const int64_t expected = head * 980665 / 178000000;
            rates.push_back(rate);
            const bool passed = DifferencePermille(rate, expected) <= 100;
            std::printf(
                "  Darcy %s m・1/%u 秒: 2〜12 秒の栓の速さ %lld µm/s(Darcy %lld・平均の水位差 %s m。%s)・左 %s → %s "
                "m\n",
                level == 0 ? "0.5" : "0.25", ticks, (long long)rate, (long long)expected, Fixed3(head / 1000).c_str(),
                Verdict(passed), Fixed3(left[from] / 1000).c_str(), Fixed3(left[to] / 1000).c_str());
            PrintStats(run);
        }

        std::printf("  Darcy 染みる速さ 1/60 と 1/120 の差 %lld ‰(±5 %%。%s)・0.5 m と 0.25 m の差 %lld ‰\n",
                    (long long)DifferencePermille(rates[0], rates[1]),
                    Verdict(DifferencePermille(rates[0], rates[1]) <= 50),
                    (long long)DifferencePermille(rates[0], rates[2]));
    }

    // --- 1 mg の粒で暴れない(相が消える所): 水の柱と空気の柱の 1 セルに砂 1 mg ---
    void OneMilligram() {
        for (const bool water : {true, false}) {
            const Scene scene = water ? MakeSandInWaterScene() : MakeDustInAirScene();
            const auto prepare = [&](Box& box) {
                for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
                    for (uint32_t index = 0; index < box.materials.size(); ++index) {
                        if (box.materials[index].phase == Phase::Grain)
                            CellMass(box, cell, index) = 0;
                    }
                }

                CellMass(box, CellIndex(box, 1, 10, 0), MaterialIndex(box, "sand")) = 1;
                // 抜いた粒の熱を戻す(残すと空気が熱くなって膨らむ)
                for (uint32_t cell = 0; cell < CellCount(box); ++cell)
                    box.energyMillijoules[cell] = ThermalEnergyMillijoules(box, cell, 300000);
            };
            const Run run = RunR2(scene, 0, 60, 10, {"column"}, true, prepare);
            const int64_t speed = MillimetersPerSecond(run.maxGrainSpeed);
            // 暴れない = 粒の速さが終端の 2 倍以下(水中 85 mm/s・空気中 300 µm の砂 4.2 m/s)
            const bool calm = speed <= (water ? 85 : 4200);
            std::printf("  1 mg の砂(%s): 粒の速さの最大 %lld mm/s(%s)・重心 %s → %s m\n", water ? "水中" : "空気中",
                        (long long)speed, Verdict(calm), Fixed3(run.grainHeightUm.front() / 1000).c_str(),
                        Fixed3(run.grainHeightUm.back() / 1000).c_str());
            PrintStats(run);
        }
    }

    // 粒・孔の無い場面は R1 とビット単位で同じ(drag の有無)
    void SameAsR1() {
        const Scene scene = MakeDamBreakScene();
        const Run with = RunR2(scene, 0, 60, 2, {"wall_column"}, true);
        const Run without = RunR2(scene, 0, 60, 2, {"wall_column"}, false);
        EXPECT(with.recording.HashChain() == without.recording.HashChain());
        std::printf("R1 と同じ: ダムの決壊 120 刻み drag あり/なし → %016llx / %016llx\n",
                    (unsigned long long)with.recording.HashChain(), (unsigned long long)without.recording.HashChain());
    }

    void Determinism() {
        const Scene scene = MakeSandInWaterScene();
        const Run first = RunR2(scene, 0, 60, 2, {"column"});
        const Run second = RunR2(scene, 0, 60, 2, {"column"});
        EXPECT(first.recording.HashChain() == second.recording.HashChain());
        std::printf("決定性: 水中の砂 120 刻みを 2 回 → %016llx(%s)\n", (unsigned long long)first.recording.HashChain(),
                    first.recording.HashChain() == second.recording.HashChain() ? "一致" : "違う");
    }

    // =====================================================================================================
    // 置き場 ①(T-0963): しずくの終端の速さ・水面の風・詰まった砂の芯を染みる水・砂の堤(記録)
    // =====================================================================================================

    // ① の場面の凡例: しずく(水 1 % + 空気。DiluteDrops で 0.001 % に薄める)・水面のセル(水 50 % + 空気)・詰まって水に満ちた砂
    void AddPlaceOneLegend(Scene& scene) {
        const std::vector<GasFill> air = {{.material = "air", .molePermille = 1000}};
        scene.legend.push_back({.symbol = 'r',
                                .condensed = {{.material = "water", .volumePct = 1}},
                                .gasPressurePa = (uint64_t)ATMOSPHERE_PA,
                                .gas = air});
        scene.legend.push_back({.symbol = 'w',
                                .condensed = {{.material = "water", .volumePct = 50}},
                                .gasPressurePa = (uint64_t)ATMOSPHERE_PA,
                                .gas = air});
        scene.legend.push_back(
            {.symbol = 'k',
             .condensed = {{.material = "sand", .volumePct = 60}, {.material = "water", .volumePct = 40}},
             .gasPressurePa = 0,
             .gas = {}});
        scene.legend.push_back({.symbol = ':',
                                .condensed = {{.material = "sand", .volumePct = 60}},
                                .gasPressurePa = (uint64_t)ATMOSPHERE_PA,
                                .gas = air});
    }

    // 空気の柱 2 m × 40 m。上の 37 m あたりに 2 mm のしずく(雨。薄めて 10 g/m³)
    Scene MakeRainScene() {
        Scene scene = BaseR2Scene("rain", 4, 80);
        AddPlaceOneLegend(scene);
        Fill(scene, 0, 3, 72, 75, 'r');
        scene.regions = {{.name = "column", .xBegin = 0, .xEnd = 4, .yBegin = 0, .yEnd = 80}};

        return scene;
    }

    // しずくを 1/1000 に薄める(1 % の水は空気の 8 倍重く、1 粒の終端ではなく重い雲が崩れる流れになる。粉と同じ)
    void DiluteDrops(Box& box) {
        const uint32_t water = MaterialIndex(box, "water");
        const uint32_t air = MaterialIndex(box, "air");
        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            int64_t& liquid = CellMass(box, cell, water);
            if (liquid == 0 || CellMass(box, cell, air) == 0)
                continue;

            liquid /= 1000;
            CellMass(box, cell, air) = CellMass(box, cell, air) * 10000 / 9901;
            box.energyMillijoules[cell] = ThermalEnergyMillijoules(box, cell, 300000);
        }
    }

    // 風洞(20 m × 7 m): 下の水 1.75 m(水面のセル 1 段)の上を空気が右へ流れ、仕切りの上の戻りの管を左へ戻る。
    // 送風は戻りの管の中だけ(毎刻みの後に気体の面の速さを −10 m/s にそろえる。外からの力積として帳簿へ)
    Scene MakeWindTunnelScene() {
        Scene scene = BaseR2Scene("wind_tunnel", 40, 14);
        AddPlaceOneLegend(scene);
        Fill(scene, 0, 39, 0, 0, '#');
        Fill(scene, 0, 39, 1, 3, '~');
        Fill(scene, 0, 39, 4, 4, 'w');
        Fill(scene, 2, 37, 8, 8, '#');
        Fill(scene, 0, 39, 12, 13, '#');
        scene.regions = {{.name = "water", .xBegin = 0, .xEnd = 40, .yBegin = 1, .yEnd = 5}};

        return scene;
    }

    constexpr int64_t WIND_SPEED = (int64_t)10 << 20;  // 10 m/s(2^-20 m/s)

    // 戻りの管(行 9〜11・x 8〜31)の気体の面の速さを −10 m/s に
    void DriveFan(Box& box) {
        const uint32_t slot = MomentumSlot(box.config.layout, Phase::Gas);
        const uint32_t air = MaterialIndex(box, "air");
        const auto edgeRows = (uint32_t)(box.config.sizeY / 14);
        for (uint32_t y = 9 * edgeRows; y < 12 * edgeRows; ++y) {
            for (uint32_t x = 8 * edgeRows; x < 32 * edgeRows; ++x) {
                const uint32_t cell = CellIndex(box, x, y, 0);
                const uint32_t right = CellIndex(box, x + 1, y, 0);
                const int64_t mass = (CellMass(box, cell, air) + CellMass(box, right, air)) / 2;
                const int64_t target = -mass * WIND_SPEED;
                const int64_t now = box.faceMomentum[0][FaceIndex(box, cell, slot)];
                ApplyImpulse(box, {.axis = 0, .cell = cell, .slot = slot}, target - now, ImpulseSource::Outside);
            }
        }
    }

    // 行 row(場面の 0.5 m の行)・x の真ん中 [x0, x1) の相の面の速さの平均(2^-20 m/s)
    int64_t MeanPhaseSpeed(const Box& box, Phase phase, uint32_t row, uint32_t x0, uint32_t x1) {
        const uint32_t slot = MomentumSlot(box.config.layout, phase);
        const auto scale = (uint32_t)(box.config.sizeY / 14);
        const uint32_t y = (row * scale) + (scale / 2);
        int64_t momentum = 0;
        int64_t mass = 0;
        for (uint32_t x = x0 * scale; x < x1 * scale; ++x) {
            const uint32_t cell = CellIndex(box, x, y, 0);
            const uint32_t right = CellIndex(box, x + 1, y, 0);
            for (uint32_t index = 0; index < box.materials.size(); ++index) {
                const bool match = (box.materials[index].phase == Phase::Liquid && phase == Phase::Liquid) ||
                                   (box.materials[index].phase == Phase::Gas && phase == Phase::Gas);
                if (match)
                    mass += (CellMass(box, cell, index) + CellMass(box, right, index) + 1) / 2;
            }

            momentum += box.faceMomentum[0][FaceIndex(box, cell, slot)];
        }

        return mass == 0 ? 0 : momentum / mass;
    }

    // 相の面の速さの最大 = 面の相の運動量 ÷ 面の相の質量(両側の和の半分の切り上げ。解き方と同じ)
    int64_t MaxPhaseSpeed(const Box& box, Phase phase) {
        const uint32_t slot = MomentumSlot(box.config.layout, phase);
        int64_t speed = 0;
        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const std::optional<uint32_t> neighbor = Neighbor(box, cell, axis);
                if (!neighbor.has_value())
                    continue;

                int64_t mass = 1;
                for (uint32_t index = 0; index < box.materials.size(); ++index) {
                    if (box.materials[index].phase == phase)
                        mass += CellMass(box, cell, index) + CellMass(box, *neighbor, index);
                }

                const int64_t momentum = box.faceMomentum[axis][FaceIndex(box, cell, slot)];
                speed = std::max<int64_t>(speed, std::abs(momentum) / std::max<int64_t>(mass / 2, 1));
            }
        }

        return speed;
    }

    // 液体の質量の重心の高さ(µm)
    int64_t LiquidHeightUm(const Box& box) {
        int64_t mass = 0;
        int64_t moment = 0;
        const uint32_t edge = CellEdgeUm(box.config.level);
        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            const std::array<uint32_t, 3> at = CellCoordinates(box, cell);
            for (uint32_t index = 0; index < box.materials.size(); ++index) {
                if (box.materials[index].phase != Phase::Liquid)
                    continue;

                const int64_t liquid = CellMass(box, cell, index);
                mass += liquid;
                moment += liquid * (((int64_t)at[1] * edge) + (edge / 2)) / 1000;
            }
        }

        return mass == 0 ? 0 : moment / mass * 1000;
    }

    // 置き場を選んで走らせる(RunR2 の ① 版。afterTick は毎刻みの後)
    struct PlaceRun {
        Recording recording;
        FlowStats stats;
        Box box;
        std::vector<int64_t> liquidHeightUm;
    };

    PlaceRun RunPlace(const Scene& scene, MomentumLayout layout, int32_t level, uint32_t ticksPerSecond,
                      uint64_t seconds, const std::vector<std::string>& regionNames, bool drag,
                      const std::function<void(Box&)>& prepare, const std::function<void(Box&)>& afterTick,
                      bool splitSurfaceLayer = false) {
        auto created = BuildScene(scene, level, SplitLayout(layout));
        EXPECT(created.has_value());
        PlaceRun run{.recording = {}, .stats = {}, .box = std::move(*created), .liquidHeightUm = {}};
        run.box.config.ticksPerSecond = ticksPerSecond;
        if (prepare) {
            prepare(run.box);
            ResetLedger(run.box);
        }

        std::vector<BoxRegion> regions;
        for (const std::string& name : regionNames)
            regions.push_back(*FindRegion(scene, name, level));

        const FlowStages flow = MakeFlowStages({.drag = drag,
                                                .splitSurfaceLayer = splitSurfaceLayer,
                                                .grainTwoStates = twoStatesOverride,
                                                .grainLayerKeepsFalling = keepLayerOverride,
                                                .grainLayerRestsOnLayer = restsOnLayerOverride,
                                                .grainLayerPlacesOnSurface = onSurfaceOverride,
                                                .grainLayerSurfaceOnLayer = surfaceOnLayerOverride});
        const PressureSolver solver = MakeImplicitGridSolver(CYCLES, TOLERANCE_NL);
        run.liquidHeightUm.push_back(LiquidHeightUm(run.box));
        run.recording = RunScene(run.box, flow.stages, solver, seconds * ticksPerSecond, regions, [&](Box& box) {
            if (afterTick)
                afterTick(box);

            run.liquidHeightUm.push_back(LiquidHeightUm(box));
        });
        run.stats = *flow.stats;
        EXPECT(run.recording.AllConserved());
        EXPECT(!CheckConservation(run.box).negativeMass);

        return run;
    }

    void PrintPlaceStats(const PlaceRun& run) {
        std::printf(
            "    費用 %lld ns/セル/刻み(計器込み)・二流体の面 %llu(層 %llu)・1 つの速さ %llu・止めた %llu・頭打ち "
            "%llu・"
            "縮めたセル %llu・孔の面 %llu・V 最大 %u・式の誤差 最大 %lld nL\n",
            (long long)run.recording.NanosecondsPerCellTick(), (unsigned long long)run.stats.fluidPairFaces,
            (unsigned long long)run.stats.surfaceFaces, (unsigned long long)run.stats.mergedFluidFaces,
            (unsigned long long)run.stats.stoppedFluidFaces, (unsigned long long)run.stats.cappedDragFaces,
            (unsigned long long)run.stats.limitedCells, (unsigned long long)run.stats.porousFaces, run.stats.maxCycles,
            (long long)run.stats.maxErrorNl);
    }

    // --- しずくの終端の速さ(#18・相が消える所): 2 mm の雨粒 6.5 m/s(Gunn & Kinzer 1949)±20 % ---
    void RainDrops() {
        const Scene scene = MakeRainScene();
        std::vector<int64_t> speeds;
        for (const auto& [level, ticks] : {std::pair{0, 60u}, std::pair{0, 120u}, std::pair{-2, 60u}}) {
            const PlaceRun run = RunPlace(scene, MomentumLayout::PerPhase, level, ticks, 4, {"column"}, false,
                                          DiluteDrops, nullptr);
            const int64_t drop = run.liquidHeightUm[(size_t)2 * ticks] - run.liquidHeightUm[(size_t)4 * ticks];
            const int64_t speed = drop / 2 / 1000;
            speeds.push_back(speed);
            const bool passed = DifferencePermille(speed, 6500) <= 200;
            std::string trace;
            for (uint32_t second = 0; second <= 4; ++second)
                trace += std::format("{}:{} ", second, run.liquidHeightUm[(size_t)second * ticks] / 1000);

            std::printf("  しずく 2 mm %s m・1/%u 秒: 2〜4 秒の落ちる速さ %lld mm/s(6500 ±20 %%。%s)・重心(秒:mm)%s\n",
                        level == 0 ? "0.5" : "2", ticks, (long long)speed, Verdict(passed), trace.c_str());
            PrintPlaceStats(run);
        }

        std::printf("  しずく 1/60 と 1/120 の差 %lld ‰・0.5 m と 2 m の差 %lld ‰\n",
                    (long long)DifferencePermille(speeds[0], speeds[1]),
                    (long long)DifferencePermille(speeds[0], speeds[2]));

        // 1 mg のしずく(相が消える所): 液体の面の速さの最大が終端の 2 倍(13 m/s)以下
        const auto single = [](Box& box) {
            const uint32_t water = MaterialIndex(box, "water");
            for (uint32_t cell = 0; cell < CellCount(box); ++cell)
                CellMass(box, cell, water) = 0;

            CellMass(box, CellIndex(box, 1, 70, 0), water) = 1;
            for (uint32_t cell = 0; cell < CellCount(box); ++cell)
                box.energyMillijoules[cell] = ThermalEnergyMillijoules(box, cell, 300000);
        };
        int64_t fastest = 0;
        const PlaceRun run = RunPlace(
            scene, MomentumLayout::PerPhase, 0, 60, 4, {"column"}, false, single,
            [&](Box& box) { fastest = std::max(fastest, MaxPhaseSpeed(box, Phase::Liquid)); });
        std::printf("  1 mg のしずく: 液体の速さの最大 %lld mm/s(13000 以下 %s)・重心 %s → %s m\n",
                    (long long)MillimetersPerSecond(fastest), Verdict(MillimetersPerSecond(fastest) <= 13000),
                    Fixed3(run.liquidHeightUm.front() / 1000).c_str(),
                    Fixed3(run.liquidHeightUm.back() / 1000).c_str());
        PrintPlaceStats(run);
    }

    // --- 水面の風(21 §2.4): 10 m/s 近くの風で水面のセルの水の速さが風の 5 % 以下 ---
    void SurfaceWind() {
        const Scene scene = MakeWindTunnelScene();
        // 水面の層の横の面を分けた形(splitSurfaceLayer)と既定(水面のセルは 1 つの速さ = ④ と同じ)を並べる
        for (const auto& [split, ticks] : {std::pair{true, 60u}, std::pair{true, 120u}, std::pair{false, 60u}}) {
            const PlaceRun run = RunPlace(scene, MomentumLayout::PerPhase, 0, ticks, 20, {"water"}, false, nullptr,
                                          DriveFan, split);
            const int64_t wind = MeanPhaseSpeed(run.box, Phase::Gas, 6, 10, 30);
            const int64_t surfaceGas = MeanPhaseSpeed(run.box, Phase::Gas, 4, 10, 30);
            const int64_t surfaceWater = MeanPhaseSpeed(run.box, Phase::Liquid, 4, 10, 30);
            const int64_t deepWater = MeanPhaseSpeed(run.box, Phase::Liquid, 2, 10, 30);
            const int64_t ratio = wind == 0 ? 0 : std::abs(surfaceWater) * 1000 / std::abs(wind);
            std::printf(
                "  水面の風 %s・1/%u 秒 20 秒後: 風 %lld mm/s・水面のセルの気体 %lld・水面の水 %lld・深い水 %lld "
                "mm/s"
                "(水面 ÷ 風 %lld ‰。≤ 50 ‰ %s)\n",
                split ? "水面の層を分ける" : "既定", ticks, (long long)MillimetersPerSecond(wind),
                (long long)MillimetersPerSecond(surfaceGas), (long long)MillimetersPerSecond(surfaceWater),
                (long long)MillimetersPerSecond(deepWater), (long long)ratio, Verdict(wind != 0 && ratio <= 50));
            std::string trace;
            for (size_t tick = 0; tick < run.recording.samples.size(); tick += 2 * (size_t)ticks)
                trace += std::format("{}:{} ", tick / ticks,
                                     MillimetersPerSecond(run.recording.samples[tick].maxSpeed));

            std::printf("    最大の速さ(秒:mm/s)%s\n", trace.c_str());
            PrintPlaceStats(run);
        }
    }

    // --- 詰まった砂の芯を染みる水(#20・#32。Kozeny–Carman): Darcy の栓を水に満ちた詰まった砂(300 µm・60 %)に ---
    // k = d² φ_f³ ÷ (180 φ_s²) = 88.9 µm²。① と ④ で同じか
    void SandCoreDarcy() {
        Scene scene = MakeDarcyScene();
        AddPlaceOneLegend(scene);
        Fill(scene, 10, 13, 1, 2, 'k');
        std::vector<int64_t> rates;
        for (const auto& [layout, ticks] :
             {std::pair{MomentumLayout::PerPhase, 60u}, std::pair{MomentumLayout::PerPhase, 120u},
              std::pair{MomentumLayout::GrainSeparate, 60u}}) {
            const PlaceRun run = RunPlace(scene, layout, 0, ticks, 12, {"left", "right"}, true, nullptr, nullptr);
            const auto left = RegionSeries(run.recording, 0, &RegionSample::liquidLevelUm);
            const auto right = RegionSeries(run.recording, 1, &RegionSample::liquidLevelUm);
            const size_t from = 2 * ticks;
            const size_t to = 12 * ticks;
            // 左の池(幅 4 m)から出た体積 ÷ 10 秒 ÷ 管の高さ 1 m = 見かけの速さ(µm/s。左の水位は µm)
            const int64_t rate = (left[from] - left[to]) * 4 / 10;
            const int64_t head = ((left[from] - right[from]) + (left[to] - right[to])) / 2;
            // q = k ρ g Δh ÷ (μ L) = 88.9e-12 × 1000 × 9.80665 × Δh ÷ (8.9e-4 × 2)
            const int64_t expected = head * 8718 / 17800000;
            rates.push_back(rate);
            std::printf(
                "  砂の芯 %s・1/%u 秒: 2〜12 秒の見かけの速さ %lld µm/s(Kozeny–Carman の Darcy %lld・水位差 %s "
                "m。%s)\n",
                layout == MomentumLayout::PerPhase ? "①" : "④", ticks, (long long)rate, (long long)expected,
                Fixed3(head / 1000).c_str(), Verdict(DifferencePermille(rate, expected) <= 100));
            PrintPlaceStats(run);
        }

        std::printf("  砂の芯 ① の 1/60 と 1/120 の差 %lld ‰(±5 %%。%s)・① と ④ の差 %lld ‰\n",
                    (long long)DifferencePermille(rates[0], rates[1]),
                    Verdict(DifferencePermille(rates[0], rates[1]) <= 50),
                    (long long)DifferencePermille(rates[0], rates[2]));
    }

    // --- 砂の堤(sand_levee。記録): 乾いた詰まった砂の堤へ水が染みる。① は孔の空気が水と別に抜ける ---
    void SandLevee() {
        const Scene scene = MakeSandLeveeScene();
        for (const MomentumLayout layout : {MomentumLayout::PerPhase, MomentumLayout::GrainSeparate}) {
            const PlaceRun run = RunPlace(scene, layout, 0, 60, 30, {"upstream", "levee", "downstream"}, true, nullptr,
                                          nullptr);
            const auto levee = RegionSeries(run.recording, 1, &RegionSample::liquidLevelUm);
            const auto upstream = RegionSeries(run.recording, 0, &RegionSample::liquidLevelUm);
            std::printf(
                "  砂の堤 %s: 堤の中の水(列あたり)0/10/30 秒 %s / %s / %s m・上流の水位 %s → %s m・最大の速さ %lld "
                "mm/s\n",
                layout == MomentumLayout::PerPhase ? "①" : "④", Fixed3(levee[0] / 1000).c_str(),
                Fixed3(levee[600] / 1000).c_str(), Fixed3(levee.back() / 1000).c_str(),
                Fixed3(upstream[0] / 1000).c_str(), Fixed3(upstream.back() / 1000).c_str(),
                (long long)MillimetersPerSecond(run.recording.samples.back().maxSpeed));
            PrintPlaceStats(run);
        }
    }

    void PlaceOneDeterminism() {
        const Scene scene = MakeWindTunnelScene();
        const PlaceRun first = RunPlace(scene, MomentumLayout::PerPhase, 0, 60, 2, {"water"}, true, nullptr, DriveFan);
        const PlaceRun second = RunPlace(scene, MomentumLayout::PerPhase, 0, 60, 2, {"water"}, true, nullptr, DriveFan);
        EXPECT(first.recording.HashChain() == second.recording.HashChain());
        std::printf("決定性(①): 風洞 120 刻みを 2 回 → %016llx(%s)\n", (unsigned long long)first.recording.HashChain(),
                    first.recording.HashChain() == second.recording.HashChain() ? "一致" : "違う");
    }

}  // namespace

int main(int argc, char** argv) {
    // 引数で場面を 1 つに絞れる(0 = 決定性・1 = 水中の砂・2 = 空気中の粉・3 = Darcy・4 = 1 mg・
    // 5 = しずく・6 = 水面の風・7 = 砂の芯・8 = 砂の堤。5〜8 は置き場 ①。T-0963)。無ければ全部
    int kept = 1;
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "twostate")
            twoStatesOverride = true;
        else if (std::string(argv[index]) == "keeplayer2")
            twoStatesOverride = keepLayerOverride = restsOnLayerOverride = true;
        else if (std::string(argv[index]) == "keeplayer")
            twoStatesOverride = keepLayerOverride = true;
        else if (std::string(argv[index]) == "split")
            twoStatesOverride = splitOverride = true;
        else if (std::string(argv[index]) == "onsurface")
            onSurfaceOverride = true;
        else if (std::string(argv[index]) == "onsurface2")
            onSurfaceOverride = surfaceOnLayerOverride = true;
        else
            argv[kept++] = argv[index];
    }

    argc = kept;
    const int only = argc > 1 ? std::stoi(argv[1]) : -1;
    if (only < 0 || only == 0) {
        Determinism();
        SameAsR1();
    }

    if (only < 0 || only == 1)
        Settling("水中の砂 300 µm", MakeSandInWaterScene(), 43, 300, 4, 24);

    if (only < 0 || only == 2)
        Settling("空気中の粉 20 µm(0.001 %)", MakeDustInAirScene(), 31, 300, 4, 24, DiluteDust);

    if (only < 0 || only == 3)
        Darcy();

    if (only < 0 || only == 4)
        OneMilligram();

    if (only < 0 || only == 0)
        PlaceOneDeterminism();

    if (only < 0 || only == 5)
        RainDrops();

    if (only < 0 || only == 6)
        SurfaceWind();

    if (only < 0 || only == 7)
        SandCoreDarcy();

    if (only < 0 || only == 8)
        SandLevee();

    std::printf("case_b r2: %d failure(s)\n", failureCount);

    return failureCount == 0 ? 0 : 1;
}
