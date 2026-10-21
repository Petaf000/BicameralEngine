// case_b_r3_test.cpp — 案 B の研究 R3(T-0996。docs/design/21 §2.5・§5.2・B-b): 粒の降伏(case_b_flow の grainYield)。CPU だけ。
// 粒は詰まった時だけ押し返し(詰まりを超えて入らない)、支えられた粒だけが浮力を引いた重さを持つ。横の面の柱で
// 押す力が「tan φ × 重さ + くっつく強さ」を超えた分だけ動く(崩れる角度は φ から出る)。
// 場面(この試験の中で絵を書く): 砂山の崩れる角度・掘ると崩れ直す・高い柱が勢いで走る・砂時計の流量(Beverloo)・
// 濡れた砂の急な面(c_eff の山形)・詰まりで重くなる(D-613)・穴を通る口(S8。口だけ)。置き場は ④ を既定に、① でも流す。
// 研究の記録なので、落ちて止まるのは保存・決定性・式の壊れ(負の質量)だけ。目安の合否は表に出す(結果は docs/tickets/T-0996)。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
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

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr int64_t TOLERANCE_NL = 256;
    constexpr uint32_t CYCLES = 4;  // R1 の主の組(本物の水・V 4 + 物の粘り)
    constexpr uint32_t SAND_ANGLE = 34;

    const char* Verdict(bool passed) {
        return passed ? "合格" : "不合格";
    }

    const char* LayoutName(MomentumLayout layout) {
        return layout == MomentumLayout::PerPhase ? "①" : "④";
    }

    // --- 場面 ---
    // 物質: 標準(air・water・sand 300 µm 34° 詰まり 60 %・stone)+ 濡れる砂(吸い上げ)+ 緩い砂(詰まり 50 %)
    Scene BaseR3Scene(std::string name, uint32_t width, uint32_t height) {
        Scene scene;
        scene.name = std::move(name);
        scene.materials = StandardMaterials();
        MaterialDefinition wet = scene.materials[2];
        wet.name = "wet_sand";
        wet.suctionPa = 3000;  // 半分濡れた時のくっつく強さ 3 kPa(仮。湿った砂の見かけの粘着の桁)
        scene.materials.push_back(wet);
        MaterialDefinition loose = scene.materials[2];
        loose.name = "loose_sand";
        loose.packedFractionPct = 50;
        scene.materials.push_back(loose);
        const std::vector<GasFill> air = {{.material = "air", .molePermille = 1000}};
        const auto atmosphere = (uint64_t)ATMOSPHERE_PA;
        scene.legend = {
            {.symbol = '.', .condensed = {}, .gasPressurePa = atmosphere, .gas = air},
            {.symbol = '#', .condensed = {{.material = "stone", .volumePct = 100}}, .gasPressurePa = 0, .gas = {}},
            {.symbol = ':',
             .condensed = {{.material = "sand", .volumePct = 60}},
             .gasPressurePa = atmosphere,
             .gas = air},
            // 孔の半分が水の濡れた砂(S = 0.5 で c_eff = 吸い上げ)
            {.symbol = 'w',
             .condensed = {{.material = "wet_sand", .volumePct = 60}, {.material = "water", .volumePct = 20}},
             .gasPressurePa = atmosphere,
             .gas = air},
            {.symbol = 'l',
             .condensed = {{.material = "loose_sand", .volumePct = 50}},
             .gasPressurePa = atmosphere,
             .gas = air},
        };
        // 1〜9 = 詰まった砂の 1/10〜9/10(斜面の表面のセル)
        for (uint32_t digit = 1; digit <= 9; ++digit)
            scene.legend.push_back({.symbol = (char)('0' + digit),
                                    .condensed = {{.material = "sand", .volumePct = 6 * digit}},
                                    .gasPressurePa = atmosphere,
                                    .gas = air});

        scene.rows.assign(height, std::string(width, '.'));

        return scene;
    }

    void Fill(Scene& scene, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1, char symbol) {
        for (uint32_t y = y0; y <= y1; ++y) {
            for (uint32_t x = x0; x <= x1; ++x)
                scene.rows[scene.rows.size() - 1 - y][x] = symbol;
        }
    }

    // 砂山 4 列 × 12 段(2 m × 6 m)を 16 m の床に。21 §5.2 の R3 の主の場面(case_b_scene の sand_pile と同じ形)
    Scene MakePileScene(char symbol = ':') {
        Scene scene = BaseR3Scene("pile", 32, 16);
        Fill(scene, 0, 31, 0, 0, '#');
        Fill(scene, 14, 17, 1, 12, symbol);
        scene.regions = {{.name = "pile", .xBegin = 0, .xEnd = 32, .yBegin = 1, .yEnd = 16}};

        return scene;
    }

    // 左の壁に寄せた斜面(高さ 4 m・角度 tanQ16 = tan θ の Q16)。表面のセルは詰まりの 1/10 刻みで滑らかに
    Scene MakeSlopeScene(uint32_t tanQ16) {
        Scene scene = BaseR3Scene("slope", 32, 16);
        Fill(scene, 0, 31, 0, 0, '#');
        const uint64_t top = 8ull << 16;  // 8 セル(4 m)
        for (uint32_t x = 0; x < 32; ++x) {
            // 列の中心の高さ(セルの Q16)
            const uint64_t drop = (uint64_t)tanQ16 * (2 * x + 1) / 2;
            if (drop >= top)
                break;

            const uint64_t height = top - drop;
            const auto whole = (uint32_t)(height >> 16);
            if (whole > 0)
                Fill(scene, x, x, 1, whole, ':');

            const auto tenth = (uint32_t)(((height & 0xffff) * 10 + 0x8000) >> 16);
            if (tenth >= 10)
                Fill(scene, x, x, whole + 1, whole + 1, ':');
            else if (tenth > 0)
                Fill(scene, x, x, whole + 1, whole + 1, (char)('0' + tenth));
        }

        scene.regions = {{.name = "slope", .xBegin = 0, .xEnd = 32, .yBegin = 1, .yEnd = 16}};

        return scene;
    }

    // 左の壁に寄せた高い柱 2 列 × 12 段(1 m × 6 m)。勢いで走る(崩れる角度で止まる距離より遠くへ)
    Scene MakeColumnScene() {
        Scene scene = BaseR3Scene("column", 32, 16);
        Fill(scene, 0, 31, 0, 0, '#');
        Fill(scene, 0, 1, 1, 12, ':');
        scene.regions = {{.name = "floor", .xBegin = 0, .xEnd = 32, .yBegin = 1, .yEnd = 16}};

        return scene;
    }

    // 砂時計(2 次元のサイロ): 上の室 12 列 × 10 段の砂、底の石の板の真ん中に幅 hole 列の口、下の室は空。fill = 砂の段の数
    Scene MakeHourglassScene(uint32_t hole, uint32_t fill) {
        Scene scene = BaseR3Scene("hourglass", 16, 24);
        Fill(scene, 0, 15, 0, 0, '#');
        // 左の端の列は空気の通り道(下の室と上の室をつなぐ。口を逆向きに流れる空気で砂が遅くならないように)
        Fill(scene, 1, 1, 2, 22, '#');
        Fill(scene, 14, 15, 1, 23, '#');
        Fill(scene, 2, 13, 11, 11, '#');
        Fill(scene, 8 - (hole / 2), 8 - (hole / 2) + hole - 1, 11, 11, '.');
        Fill(scene, 2, 13, 12, 11 + fill, ':');
        scene.regions = {{.name = "upper", .xBegin = 2, .xEnd = 14, .yBegin = 12, .yEnd = 24},
                         {.name = "lower", .xBegin = 2, .xEnd = 14, .yBegin = 1, .yEnd = 11}};

        return scene;
    }

    // --- 走らせる ---
    struct Run {
        Recording recording;
        FlowStats stats;
        Box box;
        std::vector<int64_t> grainSpeed;  // 粒の面の速さの最大(刻みごと)
    };

    // 粒の面の速さの最大 = 面の粒の運動量 ÷ 面の粒の質量(両側の平均)
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

    // 速い粒の面を数える(粒の質量で重みを付けた「動く粒の割合」と、最も速い面の場所)
    void PrintFastFaces(const Box& box) {
        const View view = BuildView(box);
        const uint32_t slot = MomentumSlot(box.config.layout, Phase::Grain);
        int64_t movingMass = 0;
        int64_t totalMass = 0;
        int64_t best = 0;
        std::string where;
        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const std::optional<uint32_t> neighbor = Neighbor(box, cell, axis);
                if (!neighbor.has_value())
                    continue;

                const int64_t left = view.cells[cell].phaseMassMg[2];
                const int64_t right = view.cells[*neighbor].phaseMassMg[2];
                const int64_t mass = (left + right + 1) / 2;
                const int64_t momentum = box.faceMomentum[axis][FaceIndex(box, cell, slot)];
                const int64_t speed = mass == 0 ? 0 : std::abs(momentum) / mass;
                totalMass += mass;
                movingMass += speed >= MOVING_SPEED ? mass : 0;
                if (speed <= best)
                    continue;

                best = speed;
                const std::array<uint32_t, 3> at = CellCoordinates(box, cell);
                where = std::format("({}, {}) 軸 {}・{} mm/s・粒 {} / {} kg・詰まり {} / {} ‰", at[0], at[1], axis,
                                    (speed * 1000) >> 20, left / 1000000, right / 1000000,
                                    ((uint64_t)view.cells[cell].grainPackedQ16 * 1000) >> 16,
                                    ((uint64_t)view.cells[*neighbor].grainPackedQ16 * 1000) >> 16);
            }
        }

        std::printf("    動く粒の質量の割合 %lld ‰・最も速い面 %s\n",
                    (long long)(totalMass == 0 ? 0 : movingMass * 1000 / totalMass), where.c_str());
    }

    // 2^-20 m/s → mm/s
    int64_t MillimetersPerSecond(int64_t speed) {
        return (speed * 1000) >> 20;
    }

    std::string Fixed3(int64_t value) {
        return FormatFixed(value, 3);
    }

    struct RunOptions {
        int32_t level = 0;
        MomentumLayout layout = MomentumLayout::GrainSeparate;
        uint32_t ticksPerSecond = 60;
        uint64_t seconds = 60;
        bool grainYield = true;
        bool perFace = true;  // 2 本目(面ごと)。false = 1 本目(面の柱の和)
        std::function<uint32_t(const Box&, uint32_t, uint32_t)> holes;
        std::function<void(Box&, uint64_t)> afterTick;  // 刻みの後の操作(掘るなど。刻みの番号)
    };

    Run RunR3(const Scene& scene, const RunOptions& options, const std::vector<std::string>& regionNames) {
        auto created = BuildScene(scene, options.level, options.layout);
        EXPECT(created.has_value());
        Run run{.recording = {}, .stats = {}, .box = std::move(*created), .grainSpeed = {}};
        run.box.config.ticksPerSecond = options.ticksPerSecond;
        std::vector<BoxRegion> regions;
        for (const std::string& name : regionNames)
            regions.push_back(*FindRegion(scene, name, options.level));

        FlowOptions flowOptions = {.drag = true};
        flowOptions.grainYield = options.grainYield;
        flowOptions.yieldPerFace = options.perFace;
        flowOptions.faceHoleSizeUm = options.holes;
        const FlowStages flow = MakeFlowStages(flowOptions);
        const PressureSolver solver = MakeImplicitGridSolver(CYCLES, TOLERANCE_NL);
        uint64_t tick = 0;
        run.recording = RunScene(run.box, flow.stages, solver, options.seconds * options.ticksPerSecond, regions,
                                 [&](Box& box) {
                                     ++tick;
                                     if (options.afterTick)
                                         options.afterTick(box, tick);

                                     run.grainSpeed.push_back(MaxGrainSpeed(box));
                                 });
        run.stats = *flow.stats;
        EXPECT(run.recording.AllConserved());
        EXPECT(!CheckConservation(run.box).negativeMass);

        return run;
    }

    void PrintStats(const Run& run) {
        std::printf(
            "    費用 %lld ns/セル/刻み(計器込み)・降伏で動いた面の柱 %llu・止めた %llu・空きが無く止めた %llu・"
            "詰まりの最大 %u ‰・詰まりを超えたセル %llu・入る粒を縮めたセル %llu・V 最大 %u\n",
            (long long)run.recording.NanosecondsPerCellTick(), (unsigned long long)run.stats.yieldMovingPairs,
            (unsigned long long)run.stats.yieldHeldPairs, (unsigned long long)run.stats.yieldBlockedPairs,
            (unsigned)((uint64_t)run.stats.maxPackedQ16 * 1000 >> 16), (unsigned long long)run.stats.overpackedCells,
            (unsigned long long)run.stats.grainLimitedCells, run.stats.maxCycles);
    }

    // 1 mm/s 以上で動く粒の質量の割合(‰。面の粒の質量で重み)
    int64_t MovingGrainPermille(const Box& box) {
        const View view = BuildView(box);
        const uint32_t slot = MomentumSlot(box.config.layout, Phase::Grain);
        int64_t moving = 0;
        int64_t total = 0;
        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const std::optional<uint32_t> neighbor = Neighbor(box, cell, axis);
                if (!neighbor.has_value())
                    continue;

                const int64_t mass = (view.cells[cell].phaseMassMg[2] + view.cells[*neighbor].phaseMassMg[2] + 1) / 2;
                const int64_t momentum = box.faceMomentum[axis][FaceIndex(box, cell, slot)];
                total += mass;
                moving += mass > 0 && std::abs(momentum) / mass >= MOVING_SPEED ? mass : 0;
            }
        }

        return total == 0 ? 0 : moving * 1000 / total;
    }

    // 粒が「ほぼ止まる」(21 §2.11: 動く面が 1 % 未満。ここでは動く粒の質量が 1 % 未満。
    // 最大の速さ 1 mm/s は、山から離れて舞う少しの粒が空気中の終端の速さ〔約 2 m/s〕で落ちるので使えない)
    bool Calm(const Run& run) {
        return MovingGrainPermille(run.box) < 10;
    }

    uint32_t FinalAngle(const Run& run, uint32_t region = 0) {
        return run.recording.samples.back().regions[region].reposeAngleDegrees;
    }

    // 列ごとの粒の高さ(µm。範囲の詰まった粒の体積 ÷ 幅と同じ数え方)から、粒のある最も遠い列の端(µm。左の壁から)
    int64_t RunoutUm(const Box& box) {
        const View view = BuildView(box);
        const uint32_t edge = CellEdgeUm(box.config.level);
        int64_t far = 0;
        for (uint32_t x = 0; x < box.config.sizeX; ++x) {
            uint64_t sum = 0;
            for (uint32_t y = 0; y < box.config.sizeY; ++y)
                sum += view.cells[CellIndex(box, x, y, 0)].grainPackedQ16;

            // 高さが 1/20 セル(薄い層)以上の列だけ数える
            if (sum >= (1u << 16) / 20)
                far = (int64_t)(x + 1) * edge;
        }

        return far;
    }

    // 列ごとの粒の高さ(µm。詰まった粒の割合の和 × 辺)
    std::vector<int64_t> ColumnHeightsUm(const Box& box) {
        const View view = BuildView(box);
        const uint32_t edge = CellEdgeUm(box.config.level);
        std::vector<int64_t> heights;
        for (uint32_t x = 0; x < box.config.sizeX; ++x) {
            uint64_t sum = 0;
            for (uint32_t y = 0; y < box.config.sizeY; ++y)
                sum += std::min<uint32_t>(view.cells[CellIndex(box, x, y, 0)].grainPackedQ16, 1u << 16);

            heights.push_back((int64_t)((sum * edge) >> 16));
        }

        return heights;
    }

    // 斜面の芯の角度(度): 頂上の高さの 90 % と 10 % を横切る位置(列の中の線形の補間)の間の傾き。両側の大きい方。
    // 計器の角度(頂上の端から粒の端まで)は、勢いで走った薄い層の裾を含むので低く出る(両方を表に出す)
    uint32_t CoreAngle(const std::vector<int64_t>& heights, uint32_t edgeUm) {
        const auto top = *std::max_element(heights.begin(), heights.end());
        if (top <= 0)
            return 0;

        const auto peak = (int64_t)(std::max_element(heights.begin(), heights.end()) - heights.begin());
        const auto crossing = [&](int64_t level, int64_t step) -> int64_t {
            for (int64_t index = peak; index + step >= 0 && index + step < (int64_t)heights.size(); index += step) {
                const int64_t here = heights[(size_t)index];
                const int64_t next = heights[(size_t)(index + step)];
                if (next >= level || here == next)
                    continue;

                // 列の中心から列の中心へ線形に(µm)
                return (index * (int64_t)edgeUm) + (step * (here - level) * (int64_t)edgeUm / (here - next));
            }

            return -1;
        };
        const auto side = [&](int64_t step) -> uint32_t {
            const int64_t high = crossing(top * 9 / 10, step);
            const int64_t low = crossing(top / 10, step);
            if (high < 0 || low < 0 || high == low)
                return 0;

            const auto run = (uint64_t)std::abs(low - high);
            return AngleFromSlopeQ16((uint64_t)(top * 8 / 10) * 65536 / run);
        };

        return std::max(side(-1), side(1));
    }

    std::string Profile(const std::vector<int64_t>& heights) {
        std::string text;
        for (const int64_t height : heights)
            text += std::to_string(height / 10000) + " ";

        return text;
    }

    int64_t RegionGrainMg(const GaugeSample& sample, uint32_t region) {
        return sample.regions[region].phaseMassMg[(uint32_t)Phase::Grain];
    }

    // --- 1. 砂山(#28): 崩れる角度が φ から出る・細かさで同じ・ほぼ止まる ---
    struct PileResult {
        uint32_t angle = 0;
        uint32_t core = 0;
        bool calm = false;
        uint64_t hash = 0;
    };

    PileResult Pile(int32_t level, MomentumLayout layout, uint64_t seconds = 60) {
        const Run run = RunR3(MakePileScene(), {.level = level, .layout = layout, .seconds = seconds}, {"pile"});
        std::string series;
        for (const uint64_t second : {5ull, 10ull, 20ull, 30ull, 45ull, 60ull}) {
            if (second <= seconds)
                series += std::format(
                    "{}s {}° ", second,
                    run.recording.samples[second * 60 * (level == 1 ? 1 : 1)].regions[0].reposeAngleDegrees);
        }

        std::printf("    計器の角度の経過 %s\n", series.c_str());
        const std::vector<int64_t> heights = ColumnHeightsUm(run.box);
        const PileResult result = {.angle = FinalAngle(run),
                                   .core = CoreAngle(heights, CellEdgeUm(level)),
                                   .calm = Calm(run),
                                   .hash = run.recording.HashChain()};
        std::printf("    高さ(cm。列ごと)%s\n", Profile(heights).c_str());
        PrintFastFaces(run.box);
        std::printf("    斜面の芯(頂上の 90 %%〜10 %%)の角度 %u°・動く面 %u / %u\n", result.core,
                    run.recording.samples.back().movingFaces, run.recording.samples.back().totalFaces);
        const char* size = level == 0 ? "0.5" : (level == 1 ? "0.25" : "2");
        std::printf(
            "  砂山 %s m %s: %llu 秒後の角度 %u°(φ %u°)・粒の最大の速さ %lld mm/s(%s)・10/30 秒の角度 %u° / %u°\n",
            size, LayoutName(layout), (unsigned long long)seconds, result.angle, SAND_ANGLE,
            (long long)MillimetersPerSecond(run.grainSpeed.back()), result.calm ? "ほぼ止まる" : "動いている",
            run.recording.samples[std::min<size_t>(600, run.recording.samples.size() - 1)]
                .regions[0]
                .reposeAngleDegrees,
            run.recording.samples[std::min<size_t>(1800, run.recording.samples.size() - 1)]
                .regions[0]
                .reposeAngleDegrees);
        PrintStats(run);

        return result;
    }

    void PileScenes() {
        const PileResult half = Pile(0, MomentumLayout::GrainSeparate);
        const PileResult quarter = Pile(1, MomentumLayout::GrainSeparate);
        const PileResult coarse = Pile(-2, MomentumLayout::GrainSeparate);
        // 合格(仮): 0.5 m と 0.25 m の角度が φ ±4°(6 m の斜面で 1 セル 0.5 m の段の差)・2 つの差 3° 以内・60 秒でほぼ止まる
        const auto near = [](uint32_t angle) {
            return angle + 4 >= SAND_ANGLE && angle <= SAND_ANGLE + 4;
        };
        const bool same = (half.angle > quarter.angle ? half.angle - quarter.angle : quarter.angle - half.angle) <= 3;
        const bool coreNear = near(half.core) && near(quarter.core);
        const bool coreSame = (half.core > quarter.core ? half.core - quarter.core : quarter.core - half.core) <= 3;
        std::printf(
            "  砂山 ④: 計器の角度 0.5 m %u°・0.25 m %u°・2 m %u°(記録)→ φ から出る %s・細かさで同じ %s・ほぼ止まる "
            "%s\n",
            half.angle, quarter.angle, coarse.angle, Verdict(near(half.angle) && near(quarter.angle)), Verdict(same),
            Verdict(half.calm && quarter.calm));
        std::printf("  砂山 ④: 斜面の芯 0.5 m %u°・0.25 m %u°・2 m %u° → φ から出る %s・細かさで同じ %s\n", half.core,
                    quarter.core, coarse.core, Verdict(coreNear), Verdict(coreSame));

        const PileResult again = Pile(0, MomentumLayout::GrainSeparate, 10);
        const PileResult first = Pile(0, MomentumLayout::GrainSeparate, 10);
        EXPECT(again.hash == first.hash);
        std::printf("  決定性: 砂山 10 秒を 2 回 → %016llx(%s)\n", (unsigned long long)first.hash,
                    again.hash == first.hash ? "一致" : "違う");
    }

    // --- 2. 掘ると崩れ直す(#29): 30 秒で止まった山の右半分を取り去り、残りの縦の面が φ に崩れ直す ---
    void RemoveGrains(Box& box, uint32_t xBegin) {
        const uint32_t sand = MaterialIndex(box, "sand");
        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            if (CellCoordinates(box, cell)[0] < xBegin || CellMass(box, cell, sand) == 0)
                continue;

            ExchangeMass(box, cell, sand, -CellMass(box, cell, sand));
            const int64_t energy = ThermalEnergyMillijoules(box, cell, 300000);
            ExchangeEnergy(box, cell, energy - box.energyMillijoules[cell], EnergySource::Outside);
        }

        const uint32_t slot = MomentumSlot(box.config.layout, Phase::Grain);
        for (uint32_t axis = 0; axis < 3; ++axis) {
            for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
                const int64_t momentum = box.faceMomentum[axis][FaceIndex(box, cell, slot)];
                if (momentum != 0 && CellCoordinates(box, cell)[0] + (axis == 0 ? 1 : 0) >= xBegin)
                    ApplyImpulse(box, {.axis = axis, .cell = cell, .slot = slot}, -momentum, ImpulseSource::Outside);
            }
        }
    }

    void Dig(int32_t level) {
        const uint32_t factor = level == 1 ? 2 : 1;
        const uint64_t digTick = 30 * 60;
        const auto dig = [&](Box& box, uint64_t tick) {
            if (tick == digTick)
                RemoveGrains(box, 16 * factor);
        };
        const Run run = RunR3(MakePileScene(), {.level = level, .seconds = 60, .afterTick = dig}, {"pile"});
        const uint32_t before = run.recording.samples[digTick].regions[0].reposeAngleDegrees;
        const uint32_t just = run.recording.samples[digTick + 1].regions[0].reposeAngleDegrees;
        const uint32_t after = FinalAngle(run);
        const bool passed = just > SAND_ANGLE + 10 && after + 4 >= SAND_ANGLE && after <= SAND_ANGLE + 4 && Calm(run);
        std::printf(
            "  掘る %s m: 掘る前 %u°・掘った直後 %u°・30 秒後 %u°・最大の速さ %lld mm/s(崩れ直して φ に戻る %s)\n",
            level == 0 ? "0.5" : "0.25", before, just, after, (long long)MillimetersPerSecond(run.grainSpeed.back()),
            Verdict(passed));
        PrintStats(run);
    }

    // --- 3. 勢いで走る(#30): 高い柱(1 m × 6 m)が崩れて、崩れる角度で止まる長さより遠くへ走る ---
    // 角度だけで止まる長さ(同じ面積の三角形): h² ÷ (2 tan φ) = 6 m² → h = 2.845 m・L = h ÷ tan φ = 4.22 m。
    // 記録: 2 次元の柱の崩れの実験(Lube ら 2005: (L − L0) ÷ L0 ≈ 1.9 a^(2/3)。a = 6)で約 7.3 m
    void Runout(int32_t level, MomentumLayout layout) {
        const Run run = RunR3(MakeColumnScene(), {.level = level, .layout = layout, .seconds = 30}, {"floor"});
        const int64_t runout = RunoutUm(run.box);
        const bool passed = runout >= 4220000 + 500000;
        std::printf(
            "  柱の崩れ %s m %s: 走った長さ %s m(角度だけ 4.220 m・実験の記録 約 7.3 m)・角度 %u°・最大の速さ %lld "
            "mm/s(勢いで走る %s)\n",
            level == 0 ? "0.5" : "0.25", LayoutName(layout), Fixed3(runout / 1000).c_str(), FinalAngle(run),
            (long long)MillimetersPerSecond(run.grainSpeed.back()), Verdict(passed));
        PrintStats(run);
    }

    // --- 4. 砂時計(2 次元のサイロ。記録): 流量 W(kg/s・奥行き 1 m あたり)を Beverloo の 2 次元の形 C ρ_b √g D^(3/2) と比べる ---
    // ρ_b = 2650 × 0.6 = 1590 kg/m³(D-613)。C = 1 の時の W: D 0.5 m = 1760 kg/s・1 m = 4979 kg/s。
    // Beverloo の芯は「流量が積もった高さによらず D^(3/2)」。2 次元の C の文献値は未確認(0.5〜1 の桁の見込み)
    int64_t HourglassRate(int32_t level, uint32_t holeCellsAtHalf, uint32_t fill, MomentumLayout layout) {
        const uint32_t factor = level == 1 ? 2 : 1;
        const Scene scene = MakeHourglassScene(holeCellsAtHalf, fill);
        (void)factor;
        const Run run = RunR3(scene, {.level = level, .layout = layout, .seconds = 3}, {"upper", "lower"});
        const uint32_t edge = CellEdgeUm(level);
        const auto lower = [&](size_t tick) {
            return RegionGrainMg(run.recording.samples[tick], 1);
        };
        // 0.5〜1.5 秒の平均(mg/s ÷ 奥行き µm = kg/s/m)
        const int64_t rate = (lower(90) - lower(30)) / (int64_t)edge;
        const int64_t late = (lower(150) - lower(90)) / (int64_t)edge;
        const int64_t reference = holeCellsAtHalf == 1 ? 1760 : 4979;
        std::printf(
            "  砂時計 %s m %s 口 %s m・砂 %u 段: 流量 0.5〜1.5 秒 %lld kg/s/m・1.5〜2.5 秒 %lld(C = %s)・最大の速さ "
            "%lld mm/s\n",
            level == 0 ? "0.5" : "0.25", LayoutName(layout), holeCellsAtHalf == 1 ? "0.5" : "1", fill, (long long)rate,
            (long long)late, Fixed3(rate * 1000 / reference).c_str(),
            (long long)MillimetersPerSecond(PeakOf(run.grainSpeed).value));
        PrintStats(run);

        return rate;
    }

    void Hourglass() {
        const int64_t small = HourglassRate(0, 1, 10, MomentumLayout::GrainSeparate);
        const int64_t wide = HourglassRate(0, 2, 10, MomentumLayout::GrainSeparate);
        const int64_t shallow = HourglassRate(0, 2, 5, MomentumLayout::GrainSeparate);
        const int64_t fine = HourglassRate(1, 2, 10, MomentumLayout::GrainSeparate);
        // 2^(3/2) = 2.83
        std::printf(
            "  砂時計(記録): 口 1 m ÷ 0.5 m の流量の比 %s(Beverloo 2.828)・砂 5 段 ÷ 10 段 %s(Beverloo 1)・"
            "0.25 m ÷ 0.5 m %s\n",
            Fixed3(small == 0 ? 0 : wide * 1000 / small).c_str(), Fixed3(wide == 0 ? 0 : shallow * 1000 / wide).c_str(),
            Fixed3(wide == 0 ? 0 : fine * 1000 / wide).c_str());
    }

    // --- 5. 濡れた砂は急な面を保つ(#34。R4 の先取りの記録): 孔の半分が水の砂山(c_eff = 吸い上げ × 4S(1−S))---
    // 押す力の和が tan φ × 重さ + c_eff を超えるので、隣の列との段の差の上限は tan φ Δx + c_eff ÷ γ(高さの段で Δx によらない)
    void WetPile(int32_t level) {
        const Run run = RunR3(MakePileScene('w'), {.level = level, .seconds = 30}, {"pile"});
        const uint32_t angle = FinalAngle(run);
        std::printf(
            "  濡れた砂山 %s m: 30 秒後の角度 %u°(乾いた φ %u° より急なら合格の見込み・%s)・最大の速さ %lld mm/s\n",
            level == 0 ? "0.5" : "0.25", angle, SAND_ANGLE, Verdict(angle >= SAND_ANGLE + 5),
            (long long)MillimetersPerSecond(run.grainSpeed.back()));
        PrintStats(run);
    }

    // --- 9. 斜面(崩れる角度が φ から出るかの芯): φ より緩い 25° は動かない・急な 45° は φ まで崩れて止まる ---
    void Slope(uint32_t degrees, uint32_t tanQ16, int32_t level, bool perFace = true) {
        const Run run = RunR3(MakeSlopeScene(tanQ16), {.level = level, .seconds = 20, .perFace = perFace}, {"slope"});
        const std::vector<int64_t> heights = ColumnHeightsUm(run.box);
        const uint32_t core = CoreAngle(heights, CellEdgeUm(level));
        uint32_t startCore = 0;
        {
            auto created = BuildScene(MakeSlopeScene(tanQ16), level, MomentumLayout::GrainSeparate);
            startCore = CoreAngle(ColumnHeightsUm(*created), CellEdgeUm(level));
        }

        int64_t peakMoving = 0;
        std::printf("    高さ(cm)%s\n", Profile(heights).c_str());
        PrintFastFaces(run.box);
        const bool still = degrees < SAND_ANGLE;
        const bool passed = still ? core + 1 >= startCore && run.stats.yieldMovingPairs == 0
                                  : core + 4 >= SAND_ANGLE && core <= SAND_ANGLE + 4 && Calm(run);
        (void)peakMoving;
        std::printf(
            "  斜面 %u° %s m: 初めの芯 %u°・20 秒後の芯 %u°・計器 %u°・降伏で動いた面の柱 %llu・動く粒 %lld ‰(%s %s)\n",
            degrees, level == 0 ? "0.5" : "0.25", startCore, core, FinalAngle(run),
            (unsigned long long)run.stats.yieldMovingPairs, (long long)MovingGrainPermille(run.box),
            still ? "φ より緩いので動かない" : "φ ±4° で止まる", Verdict(passed));
        PrintStats(run);
    }

    // --- 6. 粒を詰めると重くなる(D-613): 同じ体積(壁から壁まで 4 m × 1 m)の詰まり 60 % と 50 % の砂が床を押す力 ---
    // 床の力 = 帳簿の Wall の重さの軸の力積 ÷ 時間。期待 = 粒の質量 × g − 押しのけた空気の重さ(粒そのものの密度 × 詰まり × 体積)
    void PackingWeight() {
        std::vector<int64_t> loads;
        for (const char symbol : {':', 'l'}) {
            Scene scene = BaseR3Scene("packing", 8, 8);
            Fill(scene, 0, 7, 0, 0, '#');
            Fill(scene, 0, 7, 1, 2, symbol);
            scene.regions = {{.name = "all", .xBegin = 0, .xEnd = 8, .yBegin = 1, .yEnd = 8}};
            auto created = BuildScene(scene, 0, MomentumLayout::GrainSeparate);
            EXPECT(created.has_value());
            Box box = std::move(*created);
            const FlowStages flow = MakeFlowStages({.drag = true, .grainYield = true});
            const PressureSolver solver = MakeImplicitGridSolver(CYCLES, TOLERANCE_NL);
            const std::vector<BoxRegion> regions = {*FindRegion(scene, "all", 0)};
            const Recording warm = RunScene(box, flow.stages, solver, 60, regions);
            const int64_t before = box.ledger.impulses[(uint32_t)ImpulseSource::Wall][1];
            const Recording timed = RunScene(box, flow.stages, solver, 60, regions);
            EXPECT(warm.AllConserved() && timed.AllConserved());
            const int64_t impulse = box.ledger.impulses[(uint32_t)ImpulseSource::Wall][1] - before;
            // 力積(mg·2^-20 m/s)÷ 1 秒 = N の 10^-6 × 2^20 → N = impulse ÷ 2^20 ÷ 10^6
            const int64_t loadNewton = impulse / ((int64_t)1 << 20) / 1000000;
            int64_t grainMg = 0;
            for (uint32_t cell = 0; cell < CellCount(box); ++cell)
                grainMg += CellMass(box, cell, MaterialIndex(box, symbol == ':' ? "sand" : "loose_sand"));

            const int64_t expected = grainMg / 1000000 * 980665 / 100000;
            loads.push_back(loadNewton);
            std::printf(
                "  詰まり %s %%: 4 m × 1 m の砂 %lld kg・床を押す力 %lld N(粒の重さ %lld N。積もった密度 %lld kg/m³)\n",
                symbol == ':' ? "60" : "50", (long long)(grainMg / 1000000), (long long)loadNewton, (long long)expected,
                (long long)(grainMg / 1000000 / 4));
        }

        const int64_t ratio = loads[1] == 0 ? 0 : loads[0] * 1000 / loads[1];
        std::printf("  詰めると重くなる: 60 %% ÷ 50 %% の床の力 %s(期待 1.200 ±2 %%。%s)\n", Fixed3(ratio).c_str(),
                    Verdict(ratio >= 1176 && ratio <= 1224));
    }

    // --- 7. 穴を通る口(S8。口だけ): 砂時計の口の面に穴 200 µm を返すと、300 µm の砂は通らない ---
    void HolePort() {
        const auto holes = [](const Box& box, uint32_t axis, uint32_t cell) -> uint32_t {
            const uint32_t plate = (11u << (uint32_t)std::max(box.config.level, 0)) - 1;
            return axis == 1 && CellCoordinates(box, cell)[1] == plate ? 200 : 0;
        };
        const Run run = RunR3(MakeHourglassScene(2, 10), {.seconds = 2, .holes = holes}, {"upper", "lower"});
        const int64_t passed = RegionGrainMg(run.recording.samples.back(), 1);
        std::printf("  穴の口(S8): 穴 200 µm・砂 300 µm で 2 秒後に下の室の砂 %lld mg・止めた面 %llu(通らない %s)\n",
                    (long long)passed, (unsigned long long)run.stats.holeBlockedFaces, Verdict(passed == 0));
    }

    // --- 8. 置き場 ① で同じ場面(④ との比べ)---
    void PlaceOne() {
        const PileResult pile = Pile(0, MomentumLayout::PerPhase);
        Runout(0, MomentumLayout::PerPhase);
        HourglassRate(0, 2, 10, MomentumLayout::PerPhase);
        std::printf("  ①: 砂山 %u°(ほぼ止まる %s)\n", pile.angle, Verdict(pile.calm));
    }

}  // namespace

int main(int argc, char** argv) {
    // 引数で場面を 1 つに絞れる(1 = 砂山と決定性・2 = 掘る・3 = 勢いで走る・4 = 砂時計・5 = 濡れた砂・
    // 6 = 詰めると重い・7 = 穴の口・8 = 置き場 ①・9 = 斜面 25° と 45°)。無ければ全部
    const int only = argc > 1 ? std::stoi(argv[1]) : -1;
    if (only < 0 || only == 1)
        PileScenes();

    if (only < 0 || only == 2) {
        Dig(0);
        Dig(1);
    }

    if (only < 0 || only == 3) {
        Runout(0, MomentumLayout::GrainSeparate);
        Runout(1, MomentumLayout::GrainSeparate);
    }

    if (only < 0 || only == 4)
        Hourglass();

    if (only < 0 || only == 5) {
        WetPile(0);
        WetPile(1);
    }

    if (only < 0 || only == 6)
        PackingWeight();

    if (only < 0 || only == 7)
        HolePort();

    if (only < 0 || only == 8)
        PlaceOne();

    if (only < 0 || only == 9) {
        // tan 25° = 0.4663・tan 45° = 1
        for (const int32_t level : {0, 1}) {
            Slope(25, 30560, level);
            Slope(45, 65536, level);
        }
    }

    std::printf("case_b r3: %d failure(s)\n", failureCount);

    return failureCount == 0 ? 0 : 1;
}
