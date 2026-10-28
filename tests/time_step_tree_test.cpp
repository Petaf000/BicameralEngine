// time_step_tree_test.cpp — 研究 R-T2 の 2(T-0950。刻みの長さの選び方を多重解像度の木の上の熱で。ADR-0410・ADR-0423)の CPU の試験。
// 試作は engine/src/sim/time_step_tree.{h,cpp}(C-0 の SolveEquilibrium で解く・セルは MultiresCellStore・導かれる値は C-1 の欄)。
// 場面(どれも y・z に一様 = 横は断熱の 1 本の列と同じ。違うレベルの面を通すため、混ぜた木は 1 m の根 4 つの 1 つに 0.5 m の子を 1 つ):
//   冷める池(静かな夜): 乱れで混ざる水(伝わりやすさ = ρc × 1 m²/s)・地面へ熱が逃げる・底でゆっくり発酵。初めに右側が 2 K 暖かい。
//     川の流れ(運ぶ)は C-0 の対称な面に入らないので T-0984 へ分けた。ここは熱(伝導)だけ
//   火事: 燃える床(着火 600 K)に左端 1 m で火を付ける(time_step_lab と同じ値)
// 確かめること:
//   1. 精度: 選んだ長い刻みと毎刻みが計器の精度の内(温度 1 mK・A は 100 万分の 1・地面へ逃げた熱は全体の温度で 1 mK)
//   2. 細かさ: 0.5 m(根 0.5 m)と 1 m(根 1 m)で同じ場所の e(合格: 平均 0.5 段以下・9 割が 1 段の内。最大も記録)
//   3. 早送り: 1 刻みずつ・一気に・ばらばらの塊・2 回の実行で要約がビット一致・どの塊の境目でも保存が厳密
//   4. 眠った所の導かれる値は眠った時の平均のまま(T-0854 の申し送り)
//   5. 費用: 夜・火事の CPU の秒数
//   6. 歩の途中のセーブ(Q146 の一旦の決定 (A)): 木の状態 + 歩の記録を写して戻すと、続けたのとビット一致。歩の記録のバイト/セル
// T-0985: 選び方の 2 通り目(select=retake)・細かさを同じ物の長さのブロックで(group=1)・毎刻みの写しを使い回す(ファイル)。
// T-1013: 退く道の 1 本目(select=jump。寝ている間だけ。e は 0 か上限)。
// T-1063(D-633): 予算を mK の整数で(budgetmk=10・100・1000)。毎刻みとの差は記録だけ(合格の判定に使わない)。
//   火事は前線(着火した・半分燃えた最も遠い x)が届いた刻みと燃えた量を毎刻みと比べる(遊びに効く結果が予算で変わるか)。
// 引数: <forward|grain|grain-pond|grain-fire|grain-fine-pond|grain-fine-fire|night|fire> [select=previous|retake|jump] [budget=予算のずらし]
//       [budgetmk=予算(mK)] [group=揃える段] [buffer=1(前線の緩衝)]
//       [zerocycles=1 刻みの V の回数(既定 1)] [unit=block(単位 4 m を外す)]
//       [bufferunits=前線の緩衝の単位の数(T-1074)] [shape=mixed|coarse|fine|finer(night・fire の木。1 m・0.5 m・0.25 m の根だけ)]
//       [cycles=V の回数] [trialcycles=試しの V の回数] [trial=fixed(試しも毎刻み = 床の測り)] [galerkin=1]
// T-1111: span-dump dump=<ファイル>(火事〔shape= の木〕の途中の列の系を書く。gpu_time_step_span_test が GPU で解き比べる)
//         forward-estimate(早送りの待ちの見積もり〔D-650〕と実際に進めた解きの数)
// T-1141: event-merge [check=0] [every=n dump=<ファイル>](1 出来事の列を組ごとに 1 本の系にまとめ、列ごとに解いた値とビット一致か・
//         組の数・費用の式での GPU の待ち。every= で n 出来事に 1 つ列の系を書く = gpu_time_step_span_test --events が解き比べて測る)
//         [scene=night](T-1152: 池〔静かな夜〕の出来事。既定は火事)。出来事のセルの数の分布も出す
// T-1153: place [offset=ずらし(m)] [heaterk=熱い物の温度(K)] [fuel=1] [rule=fixed|change|both] [ticks=n]
//         (置く場所による違い: 熱い物の隣の床が着火の温度に届くか〔しきい値の近く〕・燃え広がり。毎刻みと長い刻みで)
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "sim/multires_fields.h"
#include "sim/multires_nest.h"
#include "sim/multires_state.h"
#include "sim/time_step_tree.h"
#include "time_step_choose_file.h"
#include "time_step_span_file.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    constexpr double TICK_SECONDS = 1.0 / 60.0;
    constexpr double Q16 = 65536.0;
    constexpr double Q32 = 4294967296.0;
    constexpr double LEVEL0_VOLUME = 0.125;  // m³
    constexpr int32_t MAX_EXPONENT = 14;
    constexpr uint64_t COMPARE_EVERY = 1ull << MAX_EXPONENT;
    constexpr uint64_t NIGHT_TICKS = 7 * COMPARE_EVERY;
    constexpr uint64_t FIRE_TICKS = 4 * COMPARE_EVERY;
    constexpr uint64_t FIRE_IGNITION_TICK = 3 * 4096 + 17;

    uint64_t ToQ(double value, double scale) {
        return static_cast<uint64_t>(std::llround(value * scale));
    }

    // --- 場面の値(SI。どのレベルでも同じ数になる形: 容量・地面は 0.5 m のセルの体積で・面は 0.5 m の面で)---
    struct Material {
        double heatPerCubic = 0.0;    // J/(m³·K)
        double conductivity = 0.0;    // W/(m·K)
        double groundPerCubic = 0.0;  // W/(m³·K)
        double groundKelvin = 0.0;
        uint8_t fuel = TIME_STEP_FUEL_NONE;
        double rateHalfLifeSeconds = 0.0;  // 発酵
        double burnSeconds = 0.0;          // 燃える物が 1/e になる時間
        double ignitionKelvin = 0.0;
        double joulePerMol = 0.0;
        double molPerCubic = 0.0;
    };

    Material Pond() {
        return {.heatPerCubic = 4.18e6,
                .conductivity = 4.18e6 * 1.0,
                .groundPerCubic = 2.0,
                .groundKelvin = 283.0,
                .fuel = TIME_STEP_FUEL_SLOW,
                .rateHalfLifeSeconds = 7200.0,
                .joulePerMol = 1000.0,
                .molPerCubic = 400.0};
    }

    Material Fire() {
        return {.heatPerCubic = 2.0e5,
                .conductivity = 2000.0,
                .groundPerCubic = 200.0,
                .groundKelvin = 288.0,
                .fuel = TIME_STEP_FUEL_BURNS,
                .burnSeconds = 60.0,
                .ignitionKelvin = 600.0,
                .joulePerMol = 500.0e3,
                .molPerCubic = 500.0};
    }

    // 解きの回数(引数で変えて収束を見る)
    uint32_t solverCycles = 4;  // 毎刻み(写し)の V の回数(cycles=)
    uint32_t longCycles = 2;    // 長い刻み(e > shorte)の V の回数(T-1108。既定 2。trialcycles= か longcycles= で変える)
    // 予算を 1 mK ÷ 2^これ に。木の上では 1 mK で違うレベルの境の近くが 8 mK ずれた(T-0950 の作業ログ)ので仮に 1/16 mK
    uint32_t budgetShift = 4;
    uint64_t budgetMk = 100;    // 予算を mK の整数で(0 = budgetShift を使う。既定 0.1 K = D-642)
    uint32_t zeroCycles = 1;    // 短い刻みの列の V の回数(T-1090。0 = cycles と同じ)
    uint64_t residualStop = 0;  // 残差で止める閾値(stop=。T-1108)
    int32_t shortExponent = 8;  // 短い刻み = e がこれ以下(shorte=。既定 8)
    bool unitByLength = true;   // 刻みを選ぶ単位を物の長さ 4 m で(D-640 の一旦。unit=block で外す)
    bool solverGalerkin = false;
    TimeTreeSelect selectRule = TimeTreeSelect::Previous;
    uint32_t groupShift = 0;   // 細かさの比べで 0.5 m の木の枠を揃える段(1 = 8 m のブロックに)
    uint32_t trialCycles = 0;  // 0 = longCycles と同じ
    // 試しも毎刻みに(V の回数だけ変えて、毎刻みどうしの揺れ = 床を測る)
    bool trialFixed = false;
    // 試しの初めに x(m)のセル(y = z = 0)へ熱を 1 単位だけ足す(わずかな揺らしが前線でどこまで広がるか = 着火の閾値の床。T-1014)
    double perturbMeters = -1.0;
    // 前線の緩衝(buffer=1。T-1014)
    bool frontBuffer = false;
    // select= を書いた時は forward をその選び方だけで(T-1063。予算ごとに 1 通り目 + 前線の緩衝だけ確かめる)
    bool selectGiven = false;
    uint32_t bufferUnits = 0;  // 前線の緩衝を単位(groupShift の組)の数で(T-1074。0 = buffer= の形)
    // 滑る窓の半径(m。window=。D-669・T-1175。既定 2 m = D-669。0 = 外す = 4 m の格子)。窓の時は 4 m の格子の組(unitByLength)を使わない
    double windowMeters = 2.0;
    // 刻みを選ぶ単位の一辺のセルの数(stepcells=。T-1089。既定 1 = セルごと・8 = 枠〔T-1175 まで〕。窓の時だけ効く)
    uint32_t unitEdgeCells = 1;
    // 燃える物を置く所(T-1153。空 = どこにも置く。x〔m〕→ 置くか)
    std::function<bool(double)> fuelAt;

    TimeTreeSetup MakeSetup(const Material& material, TimeStepRule rule) {
        TimeTreeSetup setup;
        setup.cycles = rule == TimeStepRule::Fixed ? solverCycles : longCycles;
        setup.galerkin = solverGalerkin;
        setup.capacityQ32 = ToQ(material.heatPerCubic * LEVEL0_VOLUME, Q32);
        setup.conductLevel0Q32 = {0, ToQ(material.conductivity * 0.25 / 0.5 * TICK_SECONDS, Q32)};
        setup.groundQ32 = ToQ(material.groundPerCubic * LEVEL0_VOLUME * TICK_SECONDS, Q32);
        setup.groundTemperatureQ16 = static_cast<int64_t>(ToQ(material.groundKelvin * 1000.0, Q16));
        setup.fuel = material.fuel;
        if (material.rateHalfLifeSeconds > 0.0)
            setup.rateColdQ32 = ToQ(std::log(2.0) / material.rateHalfLifeSeconds * TICK_SECONDS, Q32);

        if (material.burnSeconds > 0.0)
            setup.rateHotQ32 = ToQ(TICK_SECONDS / material.burnSeconds, Q32);

        setup.ignitionTemperatureQ16 = static_cast<int64_t>(ToQ(material.ignitionKelvin * 1000.0, Q16));
        setup.heatPerAmount = std::llround(material.joulePerMol * 1e-3);
        setup.rule = rule;
        setup.fixedExponent = 0;
        setup.maxExponent = MAX_EXPONENT;
        setup.budgetQ16 = budgetMk != 0 ? budgetMk << 16 : (1ull << 16) >> budgetShift;
        setup.select = selectRule;
        setup.frontBuffer = frontBuffer;
        setup.frontBufferUnits = bufferUnits;
        setup.cyclesAtZero = zeroCycles;
        setup.shortExponent = shortExponent;
        setup.residualStop = residualStop;
        setup.unitByLength = unitByLength && windowMeters <= 0.0;
        setup.windowLevel0Cells = static_cast<uint32_t>(std::llround(windowMeters / 0.5));  // レベル 0 のセルは 0.5 m
        setup.unitEdgeCells = unitEdgeCells;

        return setup;
    }

    // --- 木: 根 roots 個を x に並べる(レベル rootLevel)。refine なら根 1 の (x 0〜4 m・y・z 0〜4 m) を 0.5 m に ---
    struct TreeShape {
        int32_t rootLevel = -1;
        uint32_t roots = 4;
        bool refine = true;
        uint32_t rootsY = 1;  // y・z にも根を並べる(広い世界の火事。T-1089)
        uint32_t rootsZ = 1;
    };

    // 火を付ける所(T-1089): 負なら x < 1 m の面の全部、0 以上なら x < 1 m かつ y・z がこれ(m)より小さい所だけ(角の 1 か所の火事)
    double igniteCorner = -1.0;
    uint64_t sceneTicks = 0;  // fire-cost の刻みの数(0 = 火事 18 分)

    // night・fire の木(shape=。T-1074): mixed = 既定(1 m の根 4 つ + 0.5 m の子 1 つ)・coarse / fine / finer = 1 m・0.5 m・0.25 m の根だけ
    // (どれも x に 32 m。ブロックの物の長さ 8 m・4 m・2 m)
    TreeShape sceneShape{};
    std::string shapeName = "mixed";

    double CellMeters(int32_t level) {
        return 0.5 * std::ldexp(1.0, -level);
    }

    using Profile = std::function<double(double)>;  // 中心の x(m)→ 温度(K)

    // 刻むセルを profile と量で埋める(セルの値はレベルの単位 = 同じ数)
    void FillCells(MultiresNest& nest, const Material& material, const Profile& profile) {
        const double capacity = material.heatPerCubic * LEVEL0_VOLUME;  // mJ/mK
        const auto amount = static_cast<uint64_t>(std::llround(material.molPerCubic * LEVEL0_VOLUME * 1e6));
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL)
                continue;

            const bool expanded = ExpandNestPage(nest, slot);
            Expect(expanded, "場面: 頁に広げられる");
            const double size = CellMeters(block.level);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (MrIsCoveredCell(block, index))
                    continue;

                const double x = (static_cast<double>(block.originX + MrCellX(index)) + 0.5) * size;
                RxCell cell = RxMakeEmptyCell(std::llround(capacity * profile(x) * 1000.0));
                if (!fuelAt || fuelAt(x)) {
                    cell.speciesCount = 1;
                    cell.species[0] = TIME_TREE_REACTANT;
                    cell.amounts[0] = amount;
                }

                StoreNestCell(nest, slot, index, cell);
            }
        }
    }

    TimeTreeWorld MakeWorld(const TimeTreeSetup& setup, const Material& material, const TreeShape& shape,
                            const Profile& profile) {
        const uint32_t blocks = std::max(16u, 2 * shape.roots * shape.rootsY * shape.rootsZ);
        MultiresCapacity capacity{.worldBlocks = blocks,
                                  .observerBlocks = 0,
                                  .fractions = 16,
                                  .pages = blocks,
                                  .indexEntries = 2 * blocks,
                                  .ledgerColumns = 4,
                                  .rootLevel = shape.rootLevel};
        MultiresNest nest = MakeMultiresNest(capacity);
        const uint32_t field = RegisterNestField(
            nest, "derived.temperature",
            state_field::SfMakeDerivedDesc(state_field::SF_PLACE_CELL, state_field::SF_TYPE_I64,
                                           state_field::SF_PHASE_NONE, false, 1));
        const std::vector<RxCell> empty(MR_BLOCK_CELLS, RxMakeEmptyCell(0));
        for (uint32_t root = 0; root < shape.roots; ++root)
            for (uint32_t y = 0; y < shape.rootsY; ++y) {
                for (uint32_t z = 0; z < shape.rootsZ; ++z)
                    PlaceRootBlock(nest, int64_t{root} * MR_BLOCK_EDGE, int64_t{y} * MR_BLOCK_EDGE,
                                   int64_t{z} * MR_BLOCK_EDGE, empty);
            }

        if (shape.refine) {
            // 根 1 の原点の近くの 0.5 m の点(1 m の根の 1 つ下 = レベル 0 の子が八分の一を覆う)
            const int32_t level = shape.rootLevel + 1;
            const int64_t x = (int64_t{MR_BLOCK_EDGE} + 1) << 1;
            const MrRequest request = MrMakeRequest(MR_REQUEST_REFINE, level, x, 2, 2);
            SubmitRequests(nest, std::span(&request, 1));
            ProcessRequests(nest);
        }

        FillCells(nest, material, profile);

        return MakeTimeTreeWorld(setup, std::move(nest), field);
    }

    Profile PondProfile(double length) {
        return [length](double x) {
            return 288.0 + 2.0 * 0.5 * (1.0 + std::tanh((x - length * 0.25) / 8.0));
        };
    }

    Profile Ambient(double kelvin) {
        return [kelvin](double) {
            return kelvin;
        };
    }

    double WorldLength(const TreeShape& shape) {
        return shape.roots * MR_BLOCK_EDGE * CellMeters(shape.rootLevel);
    }

}  // namespace

namespace {

    // --- 走り(塊ごとに保存を確かめ、COMPARE_EVERY ごとに写しを取る)---

    struct Snapshot {
        std::vector<int64_t> energy;  // 刻むセルの順(最も細かいレベルの単位でなく、そのセルの単位)
        std::vector<int64_t> reactant;
        std::vector<double> x;  // セルの中心(m)
        std::vector<int32_t> level;
        int64_t groundHeat = 0;
    };

    struct Ignition {
        uint64_t tick = 0;  // 0 = 付けない
        double kelvin = 0.0;
    };

    // 火事の前線(遊びに効く結果。T-1063): 着火した(A が減った)・半分燃えた最も遠いセルの中心の x と、燃えた量の割合
    constexpr uint64_t FRONT_EVERY = 256;  // 前線を見る刻みの間隔(約 4.3 秒)

    struct FrontSample {
        uint64_t tick = 0;
        double ignitedX = -1.0;  // A が初めより減った最も遠いセルの中心(m。無ければ −1)
        double halfX = -1.0;     // A が初めの半分以下の最も遠いセルの中心(m)
        double burned = 0.0;     // 燃えた A ÷ 初めの A(体積で重み)
    };

    struct RunResult {
        uint64_t hash = 0;
        std::vector<Snapshot> snapshots;
        double seconds = 0.0;
        bool conserved = true;
        uint64_t asleepChecks = 0;  // 眠っている枠を見た数
        uint64_t asleepWrong = 0;   // 導かれる値が眠った時の平均でなかった数
        TimeTreeSteps steps;
        std::vector<FrontSample> fronts;  // FRONT_EVERY ごと(火事)
    };

    Snapshot TakeSnapshot(const TimeTreeWorld& world) {
        Snapshot snapshot;
        snapshot.groundHeat = world.steps.groundHeat;
        for (const uint32_t slot : world.layout.blocks) {
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (world.layout.stepped[slot * MR_BLOCK_CELLS + index] == 0)
                    continue;

                snapshot.energy.push_back(TimeTreeSettledEnergy(world, slot, index));
                snapshot.reactant.push_back(TimeTreeSettledReactant(world, slot, index));
                const MrBlock& block = world.nest.blocks[slot];
                snapshot.x.push_back((static_cast<double>(block.originX + MrCellX(index)) + 0.5) *
                                     CellMeters(block.level));
                snapshot.level.push_back(block.level);
            }
        }

        return snapshot;
    }

    FrontSample TakeFront(const TimeTreeWorld& world, const Material& material, uint64_t tick) {
        const auto start = static_cast<int64_t>(std::llround(material.molPerCubic * LEVEL0_VOLUME * 1e6));
        FrontSample sample{.tick = tick};
        double burned = 0.0;
        double total = 0.0;
        for (const uint32_t slot : world.layout.blocks) {
            const MrBlock& block = world.nest.blocks[slot];
            const double volume = std::ldexp(1.0, -3 * block.level);
            const double size = CellMeters(block.level);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (world.layout.stepped[slot * MR_BLOCK_CELLS + index] == 0)
                    continue;

                const int64_t reactant = TimeTreeSettledReactant(world, slot, index);
                const double x = (static_cast<double>(block.originX + MrCellX(index)) + 0.5) * size;
                if (reactant < start)
                    sample.ignitedX = std::max(sample.ignitedX, x);

                if (2 * reactant <= start)
                    sample.halfX = std::max(sample.halfX, x);

                burned += static_cast<double>(start - reactant) * volume;
                total += static_cast<double>(start) * volume;
            }
        }

        sample.burned = total > 0.0 ? burned / total : 0.0;

        return sample;
    }

    // 左端 1 m の刻むセルに火を付ける(着火の温度 + 400 K まで)
    void Ignite(const TimeTreeSetup& setup, const Material& material, TimeTreeWorld& world, const Ignition& ignition) {
        const auto heat = std::llround(material.heatPerCubic * LEVEL0_VOLUME *
                                       (material.ignitionKelvin + 400.0 - ignition.kelvin) * 1000.0);
        for (const uint32_t slot : world.layout.blocks) {
            const MrBlock block = world.nest.blocks[slot];
            const double size = CellMeters(block.level);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const double x = (static_cast<double>(block.originX + MrCellX(index)) + 0.5) * size;
                const double y = (static_cast<double>(block.originY + MrCellY(index)) + 0.5) * size;
                const double z = (static_cast<double>(block.originZ + MrCellZ(index)) + 0.5) * size;
                const bool corner = igniteCorner < 0.0 || (y < igniteCorner && z < igniteCorner);
                if (world.layout.stepped[slot * MR_BLOCK_CELLS + index] != 0 && x < 1.0 && corner)
                    InjectTimeTreeHeat(setup, world, slot, index, heat);
            }
        }
    }

    // 眠っている枠の導かれる値が眠った時の平均か(T-0854 の申し送り)
    void CheckAsleep(const TimeTreeWorld& world, RunResult& result) {
        for (const uint32_t slot : world.layout.blocks) {
            if (!MrIsUniform(world.nest.blocks[slot]))
                continue;

            // 覆われていないセルで読む(覆われたセルは 0 と読む)
            uint32_t index = 0;
            while (world.layout.stepped[slot * MR_BLOCK_CELLS + index] == 0)
                ++index;

            ++result.asleepChecks;
            const int64_t kept = LoadNestField(world.nest, world.temperatureField, slot, index, 0);
            if (kept == world.steps.sleepMean[slot])
                continue;

            ++result.asleepWrong;
            Log(Channel::Sim, Level::Warning, "眠った枠 {}(刻み {}): 導かれる値 {}・眠った時の平均 {}", slot,
                world.steps.tick, kept, world.steps.sleepMean[slot]);
        }
    }

    RunResult Run(const TimeTreeSetup& setup, const Material& material, TimeTreeWorld world, uint64_t ticks,
                  uint64_t chunk, const Ignition& ignition = {}, bool snapshots = false, bool fronts = false) {
        RunResult result;
        const TimeTreeTotals first = ConservedTimeTree(world);
        const auto begin = std::chrono::steady_clock::now();
        uint64_t done = 0;
        while (done < ticks) {
            uint64_t size = std::min(chunk, ticks - done);
            if (ignition.tick > done && ignition.tick < done + size)
                size = ignition.tick - done;

            if (snapshots && (done / COMPARE_EVERY + 1) * COMPARE_EVERY < done + size)
                size = (done / COMPARE_EVERY + 1) * COMPARE_EVERY - done;

            if (fronts && (done / FRONT_EVERY + 1) * FRONT_EVERY < done + size)
                size = (done / FRONT_EVERY + 1) * FRONT_EVERY - done;

            StepTimeTree(setup, world, size);
            done += size;
            if (ignition.tick != 0 && done == ignition.tick)
                Ignite(setup, material, world, ignition);

            if (snapshots && done % COMPARE_EVERY == 0)
                result.snapshots.push_back(TakeSnapshot(world));

            if (fronts && done % FRONT_EVERY == 0)
                result.fronts.push_back(TakeFront(world, material, done));

            CheckAsleep(world, result);
            result.conserved = result.conserved && first == ConservedTimeTree(world);
        }

        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        result.hash = HashTimeTree(world);
        result.steps = world.steps;

        return result;
    }

}  // namespace

namespace {

    // --- 比べる(計器の精度の単位)---

    struct Difference {
        double temperatureMk = 0.0;      // セルの温度の差の最大
        double reactantMillionth = 0.0;  // セルの A の差の最大(初めの量の 100 万分の 1)
        double groundMk = 0.0;           // 地面へ逃げた熱の差 ÷ 全体の熱容量(全体の温度で)
        double worstX = 0.0;             // 温度の差の最大のセルの中心の x(m)
        int32_t worstLevel = 0;
    };

    Difference Compare(const TimeTreeWorld& world, const Material& material, const Snapshot& trial,
                       const Snapshot& reference) {
        Difference difference;
        const double capacity = material.heatPerCubic * LEVEL0_VOLUME;
        const double amount = material.molPerCubic * LEVEL0_VOLUME * 1e6;
        for (size_t i = 0; i < trial.energy.size(); ++i) {
            const double energy = std::abs(static_cast<double>(trial.energy[i] - reference.energy[i]));
            const double reactant = std::abs(static_cast<double>(trial.reactant[i] - reference.reactant[i]));
            if (energy / capacity > difference.temperatureMk) {
                difference.temperatureMk = energy / capacity;
                difference.worstX = trial.x[i];
                difference.worstLevel = trial.level[i];
            }

            difference.reactantMillionth = std::max(difference.reactantMillionth, reactant / amount * 1e6);
        }

        // 全体の熱容量(最も細かいレベルの単位)
        double total = 0.0;
        for (const uint32_t slot : world.layout.blocks) {
            const double scale = std::ldexp(1.0, 3 * (world.layout.finestLevel - world.nest.blocks[slot].level));
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                total += world.layout.stepped[slot * MR_BLOCK_CELLS + index] != 0 ? capacity * scale : 0.0;
        }

        difference.groundMk = std::abs(static_cast<double>(trial.groundHeat - reference.groundHeat)) / total;

        return difference;
    }

    Difference Worst(const Difference& a, const Difference& b) {
        Difference worst = a.temperatureMk >= b.temperatureMk ? a : b;
        worst.reactantMillionth = std::max(a.reactantMillionth, b.reactantMillionth);
        worst.groundMk = std::max(a.groundMk, b.groundMk);

        return worst;
    }

    // --- 毎刻みの写しを使い回す(場面と V の回数ごとのファイル。試しの設定だけ変えて何度も測るため)---

    template <typename T>
    void WriteVector(std::ofstream& out, const std::vector<T>& values) {
        const uint64_t count = values.size();
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));
        out.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(count * sizeof(T)));
    }

    template <typename T>
    bool ReadVector(std::ifstream& in, std::vector<T>& values) {
        uint64_t count = 0;
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!in || count > (1u << 24))
            return false;

        values.resize(count);
        in.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(count * sizeof(T)));

        return static_cast<bool>(in);
    }

    void SaveSnapshots(const std::string& path, const std::vector<Snapshot>& snapshots) {
        std::ofstream out(path, std::ios::binary);
        const uint64_t count = snapshots.size();
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));
        for (const Snapshot& snapshot : snapshots) {
            out.write(reinterpret_cast<const char*>(&snapshot.groundHeat), sizeof(snapshot.groundHeat));
            WriteVector(out, snapshot.energy);
            WriteVector(out, snapshot.reactant);
            WriteVector(out, snapshot.x);
            WriteVector(out, snapshot.level);
        }
    }

    std::optional<std::vector<Snapshot>> LoadSnapshots(const std::string& path, size_t expected) {
        std::ifstream in(path, std::ios::binary);
        uint64_t count = 0;
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!in || count != expected)
            return std::nullopt;

        std::vector<Snapshot> snapshots(count);
        for (Snapshot& snapshot : snapshots) {
            in.read(reinterpret_cast<char*>(&snapshot.groundHeat), sizeof(snapshot.groundHeat));
            const bool read = ReadVector(in, snapshot.energy) && ReadVector(in, snapshot.reactant) &&
                              ReadVector(in, snapshot.x) && ReadVector(in, snapshot.level);
            if (!read)
                return std::nullopt;
        }

        return snapshots;
    }

    std::string BudgetText() {
        if (budgetMk >= 100)
            return std::format("{:g} K", static_cast<double>(budgetMk) / 1000.0);

        if (budgetMk != 0)
            return std::format("{} mK", budgetMk);

        return budgetShift == 0 ? std::string("1 mK") : std::format("1/{} mK", 1u << budgetShift);
    }

    std::string TrialName() {
        if (trialFixed)
            return std::format("毎刻み V {}", trialCycles != 0 ? trialCycles : solverCycles);

        if (selectRule == TimeTreeSelect::Jump)
            return std::format("退く道 1 本目・予算 {}", BudgetText());

        std::string buffer = frontBuffer ? "・前線の緩衝" : "";
        if (bufferUnits > 0)
            buffer = std::format("・前線の緩衝 {} 単位", bufferUnits);

        return std::format("{} 通り目{}・予算 {}・木 {}・揃える段 {}・V {}(1 刻み {})・単位 {}",
                           selectRule == TimeTreeSelect::Retake ? 2 : 1, buffer, BudgetText(), shapeName, groupShift,
                           trialCycles != 0 ? trialCycles : longCycles, zeroCycles,
                           windowMeters > 0.0 ? std::format("窓 {:g} m・{} セル角", windowMeters, unitEdgeCells)
                                              : std::string(unitByLength ? "4 m" : "ブロック"));
    }

    void LogRun(std::string_view scene, std::string_view name, const TimeTreeWorld& world, const RunResult& run,
                uint64_t ticks) {
        uint64_t cells = 0;
        for (const uint8_t stepped : world.layout.stepped)
            cells += stepped;

        const double perCellTick = static_cast<double>(run.steps.cellSteps) /
                                   (static_cast<double>(cells) * static_cast<double>(ticks));
        Log(Channel::Sim, Level::Info,
            "{} {}: {:.2f} 秒(CPU 1 スレッド・{} セル・{} 刻み)・解き {:.5f} 回/セル/刻み・部品を呼んだ {}・出来事 "
            "{}・打ち切り {}・取り直し {}・"
            "隣の差の最大 {}・負 {}・眠る {}・起こす {}・残差の計器の最大 {}(V {} 回{})",
            scene, name, run.seconds, cells, ticks, perCellTick, run.steps.solves, run.steps.events,
            run.steps.truncations, run.steps.retakes, run.steps.widestNeighborGap, run.steps.negativeValues,
            run.steps.sleeps, run.steps.wakes, run.steps.worstResidual, solverCycles,
            solverGalerkin ? "・Galerkin" : "");
        const double cellTicks = static_cast<double>(cells) * static_cast<double>(ticks);
        Log(Channel::Sim, Level::Info,
            "{} {}: 毎刻み(e = 0)で解いたセル {:.5f} /セル/刻み・そのうち自分の望みの e が 0 でない枠 "
            "{:.5f}・解いたセル {}",
            scene, name, static_cast<double>(run.steps.zeroCellSteps) / cellTicks,
            static_cast<double>(run.steps.forcedCellSteps) / cellTicks, run.steps.cellSteps);
        Log(Channel::Sim, Level::Info,
            "{} {}: V の数 {}・e = 0 の解き {}・枠の組が変わった {}・組は同じで温度の帯が変わった 1 K {} / 10 K {} / "
            "100 K "
            "{}(T-1090)",
            scene, name, run.steps.vCycles, run.steps.zeroSolves, run.steps.zeroReshaped, run.steps.zeroRebanded[0],
            run.steps.zeroRebanded[1], run.steps.zeroRebanded[2]);
    }

    // --- 1・4・5. 精度と費用(混ぜた木)---

    // 試しの初めに perturbMeters の x を含むセル(y = z = 0)へ熱を 1 単位(T-1014 の床)
    void Perturb(const TimeTreeSetup& setup, TimeTreeWorld& world) {
        for (const uint32_t slot : world.layout.blocks) {
            const MrBlock& block = world.nest.blocks[slot];
            const double size = CellMeters(block.level);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const double x = static_cast<double>(block.originX + MrCellX(index)) * size;
                const bool inside = perturbMeters >= x && perturbMeters < x + size;
                const bool corner = block.originY + MrCellY(index) == 0 && block.originZ + MrCellZ(index) == 0;
                if (!inside || !corner || world.layout.stepped[slot * MR_BLOCK_CELLS + index] == 0)
                    continue;

                InjectTimeTreeHeat(setup, world, slot, index, 1);
                Log(Channel::Sim, Level::Info, "揺らし: x {:.2f} m のセル(レベル {})へ熱 1 単位", x, block.level);
                return;
            }
        }
    }

    std::optional<std::vector<FrontSample>> LoadFronts(const std::string& path, size_t expected) {
        std::ifstream in(path, std::ios::binary);
        std::vector<FrontSample> fronts;
        if (!ReadVector(in, fronts) || fronts.size() != expected)
            return std::nullopt;

        return fronts;
    }

    // 毎刻みの写し(ファイルにあれば読む。無ければ走らせて書く)。fronts なら前線も(別のファイル。T-1063)
    std::vector<Snapshot> ReferenceSnapshots(std::string_view scene, std::string_view tag, const Material& material,
                                             const Profile& profile, uint64_t ticks, const Ignition& ignition,
                                             bool& conserved, std::vector<FrontSample>* fronts = nullptr) {
        const std::string name = std::format("time_step_tree_reference_{}_v{}{}", tag, solverCycles,
                                             solverGalerkin ? "g" : "");
        const std::string path = name + ".bin";
        const std::string frontPath = name + "_front.bin";
        auto loaded = LoadSnapshots(path, ticks / COMPARE_EVERY);
        auto loadedFronts = fronts ? LoadFronts(frontPath, ticks / FRONT_EVERY) : std::nullopt;
        if (loaded && (fronts == nullptr || loadedFronts)) {
            Log(Channel::Sim, Level::Info, "{} 毎刻み: 写し {} を使う", scene, path);
            if (fronts)
                *fronts = *std::move(loadedFronts);

            return *std::move(loaded);
        }

        // 毎刻みは V 4 のまま(写しのファイルの名前の V の回数どおり)
        TimeTreeSetup setup = MakeSetup(material, TimeStepRule::Fixed);
        setup.cyclesAtZero = 0;
        const TimeTreeWorld world = MakeWorld(setup, material, sceneShape, profile);
        RunResult reference = Run(setup, material, world, ticks, ticks, ignition, true, fronts != nullptr);
        LogRun(scene, "毎刻み", world, reference, ticks);
        conserved = reference.conserved;
        SaveSnapshots(path, reference.snapshots);
        if (fronts) {
            std::ofstream out(frontPath, std::ios::binary);
            WriteVector(out, reference.fronts);
            *fronts = reference.fronts;
        }

        return std::move(reference.snapshots);
    }

    // 前線が x(1 m ごと)に届いた刻み(届かなければ 0)
    std::vector<uint64_t> Arrivals(const std::vector<FrontSample>& fronts, double length, bool half) {
        std::vector<uint64_t> arrivals;
        for (double x = 2.0; x < length; x += 1.0) {
            uint64_t tick = 0;
            for (const FrontSample& sample : fronts) {
                if ((half ? sample.halfX : sample.ignitedX) >= x) {
                    tick = sample.tick;
                    break;
                }
            }

            arrivals.push_back(tick);
        }

        return arrivals;
    }

    // 遊びに効く結果(着火・燃え広がり・燃えた量)が長い刻みと毎刻みでどれだけ違うか(T-1063。記録)
    void CompareFronts(std::string_view scene, const std::vector<FrontSample>& trial,
                       const std::vector<FrontSample>& reference, double length) {
        for (const bool half : {false, true}) {
            const std::vector<uint64_t> a = Arrivals(trial, length, half);
            const std::vector<uint64_t> b = Arrivals(reference, length, half);
            double worst = 0.0;
            double sum = 0.0;
            uint32_t both = 0;
            uint32_t onlyOne = 0;
            double farthest = 0.0;
            for (size_t i = 0; i < a.size(); ++i) {
                if ((a[i] == 0) != (b[i] == 0)) {
                    ++onlyOne;
                    continue;
                }

                if (a[i] == 0)
                    continue;

                const double gap = std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])) * TICK_SECONDS;
                worst = std::max(worst, gap);
                sum += gap;
                ++both;
                farthest = 2.0 + static_cast<double>(i);
            }

            Log(Channel::Sim, Level::Info,
                "{} 前線({}): 1 m ごとに届いた刻みの差 最大 {:.1f} 秒・平均 {:.2f} 秒({} か所・最も遠い {:.0f} m)・"
                "片方だけ届いた {} か所(前線を見る間隔 {:.1f} 秒)",
                scene, half ? "半分燃えた" : "着火", worst, both > 0 ? sum / both : 0.0, both, farthest, onlyOne,
                static_cast<double>(FRONT_EVERY) * TICK_SECONDS);
        }

        double burnedWorst = 0.0;
        double halfWorst = 0.0;
        for (size_t i = 0; i < trial.size() && i < reference.size(); ++i) {
            burnedWorst = std::max(burnedWorst, std::abs(trial[i].burned - reference[i].burned));
            halfWorst = std::max(halfWorst, std::abs(trial[i].halfX - reference[i].halfX));
        }

        const FrontSample& last = trial.back();
        const FrontSample& lastReference = reference.back();
        Log(Channel::Sim, Level::Info,
            "{} 燃えた量: 終わり 試し {:.4f} % / 毎刻み {:.4f} %・差の最大 {:.4f} % 点・半分燃えた前線の位置の差の最大 "
            "{:.2f} m・終わりの前線 試し {:.2f} m / 毎刻み {:.2f} m",
            scene, last.burned * 100.0, lastReference.burned * 100.0, burnedWorst * 100.0, halfWorst, last.halfX,
            lastReference.halfX);
    }

    void CheckScene(std::string_view scene, std::string_view tag, const Material& material, const Profile& profile,
                    uint64_t ticks, const Ignition& ignition, bool reactantExact) {
        const TreeShape shape = sceneShape;
        TimeTreeSetup trialSetup = MakeSetup(material, trialFixed ? TimeStepRule::Fixed : TimeStepRule::Change);
        trialSetup.groupShift = groupShift;
        if (trialCycles != 0)
            trialSetup.cycles = trialCycles;

        const std::string fullTag = shapeName == "mixed" ? std::string(tag) : std::format("{}_{}", tag, shapeName);

        bool referenceConserved = true;
        const bool fronts = ignition.tick != 0;
        std::vector<FrontSample> referenceFronts;
        const std::vector<Snapshot> referenceSnapshots = ReferenceSnapshots(scene, fullTag, material, profile, ticks,
                                                                            ignition, referenceConserved,
                                                                            fronts ? &referenceFronts : nullptr);
        TimeTreeWorld trialWorld = MakeWorld(trialSetup, material, shape, profile);
        if (perturbMeters >= 0.0)
            Perturb(trialSetup, trialWorld);

        const RunResult trial = Run(trialSetup, material, trialWorld, ticks, ticks, ignition, true, fronts);
        LogRun(scene, TrialName(), trialWorld, trial, ticks);
        if (fronts)
            CompareFronts(scene, trial.fronts, referenceFronts, WorldLength(shape));

        Difference worst;
        for (size_t i = 0; i < trial.snapshots.size(); ++i) {
            const Difference at = Compare(trialWorld, material, trial.snapshots[i], referenceSnapshots[i]);
            Log(Channel::Sim, Level::Info, "{} 刻み {}: セルの温度の差の最大 {:.3f} mK(x {:.2f} m・レベル {})", scene,
                (i + 1) * COMPARE_EVERY, at.temperatureMk, at.worstX, at.worstLevel);
            worst = Worst(worst, at);
        }

        // 発酵は温度によらないので A の本当の値は閉じた形(毎刻みも端数の丸めで揺れる。time_step_lab と同じく本当の値と比べる)
        if (reactantExact) {
            const double start = material.molPerCubic * LEVEL0_VOLUME * 1e6;
            const double rate = static_cast<double>(trialSetup.rateColdQ32) / Q32;
            const double exact = start * std::exp(-rate * static_cast<double>(ticks));
            // セルの A は決まった乱数の丸めで本当の値の周りに歩の数の平方根ほど揺れる(毎刻みも同じ。床の値として出す)。
            // 偏りは全体の A の平均で見る(合格はこちら)
            const auto against = [&](const Snapshot& snapshot, double& cellWorst) {
                double sum = 0.0;
                for (const int64_t value : snapshot.reactant) {
                    cellWorst = std::max(cellWorst, std::abs(static_cast<double>(value) - exact) / start * 1e6);
                    sum += static_cast<double>(value);
                }

                return std::abs(sum / static_cast<double>(snapshot.reactant.size()) - exact) / start * 1e6;
            };
            double trialCell = 0.0;
            double referenceCell = 0.0;
            const double trialMean = against(trial.snapshots.back(), trialCell);
            const double referenceMean = against(referenceSnapshots.back(), referenceCell);
            Log(Channel::Sim, Level::Info,
                "{} A の本当の値との差(100 万分の 1): 試し 1 セルの最大 {:.3f}・平均 {:.4f} / 毎刻み セルの最大 "
                "{:.3f}・平均 {:.4f}",
                scene, trialCell, trialMean, referenceCell, referenceMean);
            worst.reactantMillionth = trialMean;
        }

        Log(Channel::Sim, Level::Info,
            "{} 試し 1 と毎刻み: セルの温度 {:.3f} mK・A {:.3f}/100 万{}・地面へ逃げた熱 {:.4f} "
            "mK(全体)・眠った枠の導かれる値 "
            "{} 回見て違い {}",
            scene, worst.temperatureMk, worst.reactantMillionth, reactantExact ? "(本当の値と)" : "", worst.groundMk,
            trial.asleepChecks, trial.asleepWrong);
        Expect(referenceConserved && trial.conserved, "保存: どの塊の境目でも Σ − 帳簿 が最初と同じ");
        // 退く道は隣の差の決まりを使わない(0 と上限が隣り合う)
        Expect(selectRule == TimeTreeSelect::Jump || trial.steps.widestNeighborGap <= 1,
               "隣どうしの e の差は 1 段まで");
        Expect(trial.steps.negativeValues == 0, "真実が負にならない");
        // 毎刻みとの差は記録だけ(D-633: 長い刻みの結果はそれ自体がこの宇宙の真実。守るのは寝ても待っても同じ・
        // 細かさによらない一貫性・決定性。T-1063)
        Log(Channel::Sim, Level::Info, "{} 毎刻みとの差(記録・D-633。合格の判定に使わない): 予算 {}", scene,
            BudgetText());
        Expect(trial.asleepWrong == 0, "眠った所の導かれる値は眠った時の平均のまま");
    }

}  // namespace

namespace {

    // --- 2. 細かさ: 0.5 m(根 0.5 m・8 つ)と 1 m(根 1 m・4 つ)で同じ場所に選ばれる e ---

    // pair: 0 = 0.5 m と 1 m(既定)・1 = 0.25 m と 0.5 m(どちらも単位 4 m。T-1063 の既定の確かめ)
    void CheckGranularity(std::string_view scene, const Material& material, const Profile& profile, uint64_t ticks,
                          const Ignition& ignition, int32_t pair = 0) {
        constexpr uint64_t SAMPLE_EVERY = 1024;
        const TimeTreeSetup setup = MakeSetup(material, TimeStepRule::Change);
        // group=1 なら 0.5 m の木は x に隣り合う 2 枠(4 m × 2 = 8 m)を同じ e に揃える = 1 m の木のブロックと同じ物の長さ(T-0985)
        TimeTreeSetup fineSetup = setup;
        fineSetup.groupShift = groupShift;
        const TreeShape fineShape{.rootLevel = pair, .roots = 8u << pair, .refine = false};
        const TreeShape coarseShape{.rootLevel = pair - 1, .roots = 4u << pair, .refine = false};
        TimeTreeWorld fine = MakeWorld(fineSetup, material, fineShape, profile);
        TimeTreeWorld coarse = MakeWorld(setup, material, coarseShape, profile);
        // 根の枠を x の順に(0.5 m の根 i は 1 m の根 i / 2 の中)
        const auto byX = [](const TimeTreeWorld& world) {
            std::vector<uint32_t> slots = world.layout.blocks;
            std::ranges::sort(slots, [&](uint32_t a, uint32_t b) {
                return world.nest.blocks[a].originX < world.nest.blocks[b].originX;
            });

            return slots;
        };
        const std::vector<uint32_t> fineSlots = byX(fine);
        const std::vector<uint32_t> coarseSlots = byX(coarse);
        // 細かいセルの e(根 i・セル)と、それを含む粗いセルの e(T-1089: e は単位ごとなので、セルどうしで比べる)
        const auto fineAt = [&](uint32_t i, uint32_t index) {
            return TimeTreeExponentAt(fine, fineSlots[i], index);
        };
        const auto coarseOf = [&](uint32_t i, uint32_t index) {
            const uint32_t x = (MrCellX(index) + (i & 1u) * MR_BLOCK_EDGE) / 2;
            return TimeTreeExponentAt(coarse, coarseSlots[i / 2],
                                      MrCellIndex(x, MrCellY(index) / 2, MrCellZ(index) / 2));
        };
        int32_t widest = 0;
        double meanGap = 0.0;
        uint32_t withinOne = 0;
        uint32_t samples = 0;
        // 1 m のセルは中の 0.5 m のセル 8 つの変化の大きい方で縮むので、「同じ場所」は 1 m の e と中の 0.5 m の 8 つの e の
        // 小さい方とも比べる(記録。枠の単位では 0.5 m の枠 2 つの小さい方だった)
        double meanFine = 0.0;
        double meanCoarse = 0.0;
        int32_t widestPair = 0;
        double meanPair = 0.0;
        uint32_t withinOnePair = 0;
        uint32_t pairs = 0;
        for (uint64_t done = 0; done < ticks;) {
            uint64_t size = SAMPLE_EVERY;
            if (ignition.tick > done && ignition.tick < done + size)
                size = ignition.tick - done;

            StepTimeTree(fineSetup, fine, size);
            StepTimeTree(setup, coarse, size);
            done += size;
            if (ignition.tick != 0 && done == ignition.tick) {
                Ignite(fineSetup, material, fine, ignition);
                Ignite(setup, material, coarse, ignition);
            }

            for (uint32_t i = 0; i < fineShape.roots; ++i) {
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    const int32_t fineExponent = fineAt(i, index);
                    const int32_t coarseExponent = coarseOf(i, index);
                    const int32_t gap = std::abs(fineExponent - coarseExponent);
                    widest = std::max(widest, gap);
                    meanGap += gap;
                    withinOne += gap <= 1 ? 1u : 0u;
                    ++samples;
                    meanFine += fineExponent;
                    meanCoarse += coarseExponent;
                    if ((MrCellX(index) & 1u) != 0 || (MrCellY(index) & 1u) != 0 || (MrCellZ(index) & 1u) != 0)
                        continue;

                    // 粗いセル 1 つ = 細かいセル 2³(この角から)
                    int32_t least = fineExponent;
                    for (uint32_t corner = 1; corner < 8; ++corner) {
                        const uint32_t other = MrCellIndex(MrCellX(index) + (corner & 1u),
                                                           MrCellY(index) + ((corner >> 1) & 1u),
                                                           MrCellZ(index) + (corner >> 2));
                        least = std::min(least, fineAt(i, other));
                    }

                    const int32_t pairGap = std::abs(least - coarseExponent);
                    widestPair = std::max(widestPair, pairGap);
                    meanPair += pairGap;
                    withinOnePair += pairGap <= 1 ? 1u : 0u;
                    ++pairs;
                }
            }
        }

        const double share = static_cast<double>(withinOne) / samples;
        Log(Channel::Sim, Level::Info,
            "細かさ {}({}・組 {}〔1 = 0.25 m と 0.5 m。e の平均の名前は 0.5 m / 1 m のまま〕・細かい方のブロック {:g} "
            "m): 同じ場所の e の差 平均 {:.2f}・1 段の内 {:.1f} %・最大 {}・e の平均 "
            "0.5 m "
            "{:.2f} / 1 m {:.2f}・解き 0.5 m {} / 1 m {}・取り直し {} / {}",
            scene, TrialName(), pair, std::ldexp(4.0, static_cast<int>(groupShift) - pair), meanGap / samples,
            share * 100.0, widest, meanFine / samples, meanCoarse / samples, fine.steps.cellSteps,
            coarse.steps.cellSteps, fine.steps.retakes, coarse.steps.retakes);
        Log(Channel::Sim, Level::Info,
            "細かさ {}(1 m の e と中の 0.5 m の 8 つの小さい方): 差 平均 {:.2f}・1 段の内 {:.1f} %・最大 {}", scene,
            meanPair / pairs, 100.0 * withinOnePair / pairs, widestPair);
        // D-669: 窓の時、D-640 は「結果がどちらの細かさでも予算の内」に読み替えた(選ぶ e の揃い方は記録だけ。結果は night・fire の
        // shape=fine / coarse の毎刻みとの差で見る)。格子(window=0)は元の線
        if (windowMeters <= 0.0)
            Expect(meanGap / samples <= 0.5 && share >= 0.9, "細かさ: 平均 0.5 段以下・9 割が 1 段の内");
    }

    // --- T-1089: 広い世界の火事の解きの数(毎刻みとは比べない。試しだけ)---
    void CheckFireCost(const Material& material, const Ignition& ignition) {
        TimeTreeSetup setup = MakeSetup(material, TimeStepRule::Change);
        setup.groupShift = groupShift;

        // 実際に解いた列を出来事(歩の初めの刻み)ごとに 1 本の系にまとめた時の GPU の待ち(T-1111 の費用の式。D-650 の見積もりと比べる)
        std::map<uint64_t, std::pair<uint64_t, uint64_t>> eventCells;  // 歩の初め → (セル, セル × V)
        const TimeTreeSpanObserver observer = [&eventCells](const TimeTreeSpanSolve& solve) {
            auto& [cells, cycles] = eventCells[solve.start];
            cells += solve.system.nodes.size();
            cycles += solve.system.nodes.size() * solve.cycles;
        };
        setup.spanObserver = &observer;
        const TimeTreeWorld world = MakeWorld(setup, material, sceneShape, Ambient(material.groundKelvin));
        const uint64_t ticks = sceneTicks != 0 ? sceneTicks : FIRE_TICKS;
        const RunResult run = Run(setup, material, world, ticks, ticks, ignition, false, true);
        LogRun("火事の費用", TrialName(), world, run, ticks);
        uint64_t waitNs = 0;
        uint64_t largest = 0;
        for (const auto& [start, counts] : eventCells) {
            waitNs += TimeTreeEventNs(sim::TimeTreeSolveCost{}, counts.first, counts.second);
            largest = std::max(largest, counts.first);
        }

        Log(Channel::Sim, Level::Info,
            "火事の費用: 出来事 {}(1 本の系にまとめて)・最も大きい出来事 {} セル・GPU の待ち(RTX 3070 Ti の式・V は e "
            "の表・"
            "重みは毎回){:.2f} 秒",
            eventCells.size(), largest, (double)waitNs * 1.0e-9);
        const FrontSample& last = run.fronts.back();
        Log(Channel::Sim, Level::Info, "火事の費用: 角の火 {:g} m・終わりの着火した前線 x {:.2f} m・燃えた量 {:.4f} %",
            igniteCorner, last.ignitedX, last.burned * 100.0);
        Expect(run.conserved && run.steps.negativeValues == 0, "火事の費用: 保存・負 0");
    }

    // --- T-1111: 火事の途中の列の系を書く(GPU で解き比べる)---
    std::string dumpPath;

    // 着火から何刻み後の e = 0 の系を取るか(着火の直後・前線が広がる途中。1 つ目の 1 刻みの系から)
    constexpr std::array<uint64_t, 8> SPAN_DUMP_AFTER = {1, 16, 64, 256, 512, 1024, 2048, 4000};
    constexpr uint32_t SPAN_DUMP_LONG = 4;  // e > 0 の系(着火の 256 刻み後から、違う e を 4 つ。前線の外の長い歩)

    void CheckSpanDump(const Material& material, const Ignition& ignition) {
        TimeTreeSetup setup = MakeSetup(material, TimeStepRule::Change);
        setup.groupShift = groupShift;
        std::vector<test::TimeStepSpanRecord> records;
        size_t nextZero = 0;
        std::vector<int32_t> longSeen;
        const TimeTreeSpanObserver observer = [&](const TimeTreeSpanSolve& solve) {
            if (solve.exponent == 0 && nextZero < SPAN_DUMP_AFTER.size() &&
                solve.start >= ignition.tick + SPAN_DUMP_AFTER[nextZero]) {
                ++nextZero;
            } else if (solve.exponent > 0 && longSeen.size() < SPAN_DUMP_LONG &&
                       std::ranges::find(longSeen, solve.exponent) == longSeen.end() &&
                       solve.start >= ignition.tick + SPAN_DUMP_AFTER[3]) {
                longSeen.push_back(solve.exponent);
            } else
                return;

            records.push_back(
                {.exponent = solve.exponent, .start = solve.start, .cycles = solve.cycles, .system = solve.system});
        };

        setup.spanObserver = &observer;
        const TimeTreeWorld world = MakeWorld(setup, material, sceneShape, Ambient(material.groundKelvin));
        const uint64_t ticks = ignition.tick + SPAN_DUMP_AFTER.back() + 1;
        const RunResult run = Run(setup, material, world, ticks, ticks, ignition);
        LogRun("火事の系", TrialName(), world, run, ticks);
        for (const test::TimeStepSpanRecord& record : records) {
            Log(Channel::Sim, Level::Info,
                "火事の系: e {}・歩の初め {}・V {}・節 {}・面 {}・固定した面 {}・右辺の項 {}", record.exponent,
                record.start, record.cycles, record.system.nodes.size(), record.system.faces.size(),
                record.system.fixedFaces.size(), record.system.sources.size());
        }

        Expect(nextZero == SPAN_DUMP_AFTER.size(), "火事の系: e = 0 の系を全部取った");
        Expect(!dumpPath.empty() && test::WriteTimeStepSpans(dumpPath, records), "火事の系: ファイルに書けた");
    }

    // --- T-1141: 1 出来事の列を 1 本の系にまとめる形(GroupTimeTreeEvent)を、列ごとに解いた値とビット一致で確かめ、数える ---
    bool mergeCheck = true;       // check=0 で比べない(数えるだけ)
    uint32_t mergeDumpEvery = 0;  // every=n: n 出来事に 1 つ、列の系を dump= に書く(GPU の試験が組ごとに解き比べて測る)
    bool mergeNight = false;      // scene=night: 池(静かな夜)の出来事で(T-1152。e > 8 の V 2 の組)

    std::vector<int64_t> SolveSystem(const EquilibriumSystem& system, uint32_t cycles) {
        EquilibriumOptions options;
        options.solver.cycles = cycles;
        options.faceValues = false;
        EquilibriumState state;
        (void)SolveEquilibrium(system, options, state);

        return state.values;
    }

    struct MergeCounts {
        uint64_t events = 0;
        uint64_t spans = 0;
        uint64_t groups = 0;
        uint64_t mergedGroups = 0;  // 2 つ以上の列をまとめた組
        uint64_t compared = 0;
        uint64_t mismatches = 0;
        std::array<uint64_t, 4> spansPerEvent{};  // 1・2・3・4 以上
        std::array<uint64_t, 4> groupsPerEvent{};
        std::map<int32_t, uint32_t> levelsByExponent;  // e → 段の数の最大
        uint64_t spanWaitNs = 0;                       // 列ごとの系(今の口)
        uint64_t groupWaitNs = 0;                      // 組ごと
        uint64_t eventWaitNs = 0;                      // 出来事を全部 1 本(T-1111 の見積もりの前提。値は組と違いうる)
        uint64_t largestGroup = 0;

        // --- 出来事の大きさの分布(T-1152。費用の式の分け方を決める)---
        // 出来事のセルの数: ≤512・≤1,024・≤2,048・≤4,096・≤8,192・≤16,384・≤65,536・それより上
        std::array<uint64_t, 8> cellsPerEvent{};
        std::map<uint32_t, uint64_t> groupsByCycles;  // 組の V の回数 → 組の数
        uint64_t eventCells = 0;                      // Σ 出来事のセル
        uint64_t eventCellCycles = 0;                 // Σ セル × V の回数
    };

    size_t EventSizeBucket(uint64_t cells) {
        constexpr std::array<uint64_t, 7> BOUNDS = {512, 1024, 2048, 4096, 8192, 16384, 65536};
        for (size_t bucket = 0; bucket < BOUNDS.size(); ++bucket) {
            if (cells <= BOUNDS[bucket])
                return bucket;
        }

        return BOUNDS.size();
    }

    void CountEvent(const std::vector<TimeTreeEventSpan>& spans, MergeCounts& counts) {
        const sim::TimeTreeSolveCost cost;
        const std::vector<TimeTreeEventGroup> groups = GroupTimeTreeEvent(spans);
        ++counts.events;
        counts.spans += spans.size();
        counts.groups += groups.size();
        ++counts.spansPerEvent[std::min<size_t>(spans.size(), 4) - 1];
        ++counts.groupsPerEvent[std::min<size_t>(groups.size(), 4) - 1];
        uint64_t eventCells = 0;
        uint64_t eventCycles = 0;
        for (const TimeTreeEventSpan& span : spans) {
            const uint64_t cells = span.system.nodes.size();
            counts.spanWaitNs += TimeTreeEventNs(cost, cells, cells * span.cycles);
            eventCells += cells;
            eventCycles += cells * span.cycles;
        }

        counts.eventWaitNs += TimeTreeEventNs(cost, eventCells, eventCycles);
        ++counts.cellsPerEvent[EventSizeBucket(eventCells)];
        counts.eventCells += eventCells;
        counts.eventCellCycles += eventCycles;
        for (const TimeTreeEventGroup& group : groups) {
            const uint64_t cells = group.system.nodes.size();
            counts.groupWaitNs += TimeTreeEventNs(cost, cells, cells * group.cycles, group.levels);
            counts.largestGroup = std::max(counts.largestGroup, cells);
            ++counts.groupsByCycles[group.cycles];
            for (const uint32_t index : group.spans) {
                uint32_t& levels = counts.levelsByExponent[spans[index].exponent];
                levels = std::max(levels, group.levels);
            }

            if (group.spans.size() < 2)
                continue;

            ++counts.mergedGroups;
            if (!mergeCheck)
                continue;

            // --- まとめた系の値 = 列ごとに解いた値を並べたもの ---
            const std::vector<int64_t> merged = SolveSystem(group.system, group.cycles);
            std::vector<int64_t> each;
            for (const uint32_t index : group.spans) {
                const std::vector<int64_t> values = SolveSystem(spans[index].system, spans[index].cycles);
                each.insert(each.end(), values.begin(), values.end());
            }

            ++counts.compared;
            counts.mismatches += merged != each ? 1u : 0u;
        }
    }

    void CheckEventMerge(const Material& material, const Profile& profile, uint64_t defaultTicks,
                         const Ignition& ignition) {
        TimeTreeSetup setup = MakeSetup(material, TimeStepRule::Change);
        setup.groupShift = groupShift;
        MergeCounts counts;
        std::vector<TimeTreeEventSpan> pending;
        uint64_t pendingStart = 0;
        std::vector<test::TimeStepSpanRecord> records;
        const auto flush = [&]() {
            if (pending.empty())
                return;

            if (mergeDumpEvery != 0 && counts.events % mergeDumpEvery == 0) {
                for (const TimeTreeEventSpan& span : pending) {
                    records.push_back({.exponent = span.exponent,
                                       .start = pendingStart,
                                       .cycles = span.cycles,
                                       .event = static_cast<uint32_t>(counts.events),
                                       .system = span.system});
                }
            }

            CountEvent(pending, counts);
            pending.clear();
        };

        // 同じ出来事の列は続けて(同じ歩の初めで)来る
        const TimeTreeSpanObserver observer = [&](const TimeTreeSpanSolve& solve) {
            if (!pending.empty() && solve.start != pendingStart)
                flush();

            pendingStart = solve.start;
            pending.push_back({.system = solve.system, .exponent = solve.exponent, .cycles = solve.cycles});
        };

        setup.spanObserver = &observer;
        const TimeTreeWorld world = MakeWorld(setup, material, sceneShape, profile);
        const uint64_t ticks = sceneTicks != 0 ? sceneTicks : defaultTicks;
        const RunResult run = Run(setup, material, world, ticks, ticks, ignition);
        flush();
        LogRun("列のまとめ", TrialName(), world, run, ticks);

        std::string levels;
        for (const auto& [exponent, count] : counts.levelsByExponent)
            levels += std::format(" e{}:{}", exponent, count);

        Log(Channel::Sim, Level::Info,
            "列のまとめ: 出来事 {}・列 {}・組 {}(2 つ以上をまとめた組 {})・列の数 1/2/3/4+ = {}/{}/{}/{}・組の数 "
            "1/2/3/4+ = "
            "{}/{}/{}/{}・最も大きい組 {} セル・段の数の最大{}",
            counts.events, counts.spans, counts.groups, counts.mergedGroups, counts.spansPerEvent[0],
            counts.spansPerEvent[1], counts.spansPerEvent[2], counts.spansPerEvent[3], counts.groupsPerEvent[0],
            counts.groupsPerEvent[1], counts.groupsPerEvent[2], counts.groupsPerEvent[3], counts.largestGroup, levels);
        Log(Channel::Sim, Level::Info,
            "列のまとめ: GPU の待ち(費用の式)列ごと {:.2f} 秒・組ごと {:.2f} 秒・出来事を全部 1 本 {:.2f} 秒・"
            "比べた組 {}・食い違い {}",
            (double)counts.spanWaitNs * 1.0e-9, (double)counts.groupWaitNs * 1.0e-9,
            (double)counts.eventWaitNs * 1.0e-9, counts.compared, counts.mismatches);
        std::string sizes;
        for (const uint64_t count : counts.cellsPerEvent)
            sizes += std::format(" {}", count);

        std::string cyclesText;
        for (const auto& [cycles, count] : counts.groupsByCycles)
            cyclesText += std::format(" V{}:{}", cycles, count);

        Log(Channel::Sim, Level::Info,
            "列のまとめ: 出来事のセルの数の分布(≤512/1Ki/2Ki/4Ki/8Ki/16Ki/64Ki/上)={}・1 出来事の平均 {} セル・"
            "V × セル {}・組の V の回数{}",
            sizes, counts.events != 0 ? counts.eventCells / counts.events : 0,
            counts.events != 0 ? counts.eventCellCycles / counts.events : 0, cyclesText);
        Expect(run.conserved && run.steps.negativeValues == 0, "列のまとめ: 保存・負 0");
        Expect(counts.spans == run.steps.solves, "列のまとめ: 見た列の数が試作の解きの数と同じ");
        Expect(counts.mismatches == 0, "列のまとめ: まとめた系の値が列ごとに解いた値とビット一致");
        if (mergeDumpEvery != 0) {
            Log(Channel::Sim, Level::Info, "列のまとめ: {} 出来事に 1 つ書いた系 {}", mergeDumpEvery, records.size());
            Expect(!dumpPath.empty() && test::WriteTimeStepSpans(dumpPath, records), "列のまとめ: ファイルに書けた");
        }
    }

    // --- D-650・T-1111: 早送りの待ちの見積もりを、実際に進めた解きの数と比べる(合格の判定には使わない)---
    // 1 出来事の GPU の費用(既定 = RTX 3070 Ti の測り。TimeTreeSolveCost)
    const sim::TimeTreeSolveCost forwardCost;

    void CheckForwardEstimate(const Material& material, const Ignition& ignition) {
        TimeTreeSetup setup = MakeSetup(material, TimeStepRule::Change);
        setup.groupShift = groupShift;
        TimeTreeWorld world = MakeWorld(setup, material, sceneShape, Ambient(material.groundKelvin));
        StepTimeTree(setup, world, ignition.tick);
        Ignite(setup, material, world, ignition);
        uint64_t done = 0;
        for (const uint64_t at : {uint64_t{256}, uint64_t{2048}}) {
            StepTimeTree(setup, world, at - done);
            done = at;
            for (const uint64_t horizon : {uint64_t{1024}, uint64_t{8192}}) {
                const TimeTreeForwardEstimate estimate = EstimateTimeTreeForward(setup, world, horizon, forwardCost);
                TimeTreeWorld ahead = world;
                StepTimeTree(setup, ahead, horizon);
                const uint64_t cells = ahead.steps.cellSteps - world.steps.cellSteps;
                const uint64_t events = ahead.steps.events - world.steps.events;
                Log(Channel::Sim, Level::Info,
                    "早送りの見積もり(着火の {} 刻み後から {} 刻み): 見積もり 出来事 {}・解くセル {}・V × セル {}・GPU "
                    "の待ち "
                    "{:.3f} 秒(うち刻みを選ぶ段 {:.3f} 秒)/ 実際 出来事 {}・解いたセル {}(見積もり ÷ 実際 {:.2f})",
                    at, horizon, estimate.events, estimate.cellSolves, estimate.cellCycles,
                    (double)estimate.waitMicroseconds * 1.0e-6, (double)estimate.chooseMicroseconds * 1.0e-6, events,
                    cells, cells != 0 ? (double)estimate.cellSolves / (double)cells : 0.0);
            }
        }
    }

    // --- T-1153: 置く場所による違い(刻みを選ぶ単位〔4 m〕は世界の座標にそろう。同じ場面を 0〜3 m ずらして置く)---
    // 場面(しきい値の近く・0.5 m の根だけの木 = shape=fine・単位 4 m = ブロック): 燃えない熱い物(幅 2 m・heaterk=)を x = 12 + o から置き、
    // 0.5 m あけて燃える床(x ≥ 14.5 + o)。熱い物から伝わった熱で床の最も熱いセルが着火の温度(600 K)に届くかがしきい値。
    // 着火の前は反応が 0 なので、燃える物を置かない走り(既定)の真実(歩の境目ごとの値 = 反応が読む値)の山で、着火するか・いつかが決まる。
    // fuel=1: 燃える物を置いて、着火の刻み・前線(床の初めからの距離)の届いた刻み・燃えた量を見る(燃え広がりの場面)
    constexpr double PLACE_HEATER_X = 12.0;
    constexpr double PLACE_HEATER_METERS = 2.0;
    constexpr double PLACE_GAP_METERS = 0.5;
    constexpr uint64_t PLACE_TICKS = 8192;  // 山は約 110 秒(6,600 刻み)

    double placeOffset = 0.0;           // offset=(m。セルの長さの倍数)
    double placeHeaterKelvin = 1570.0;  // heaterk=
    bool placeFuel = false;             // fuel=1
    std::string placeRule = "both";     // rule=fixed|change|both

    struct PlaceResult {
        int64_t peakQ16 = 0;  // 床の範囲のセルの真実の温度の最大(mK × 2^16)
        uint64_t peakTick = 0;
        double peakDistance = 0.0;       // 床の初めからの距離(m)
        int32_t peakExponent = 0;        // 山を取った刻みのその枠の e
        uint64_t crossTick = 0;          // 真実が着火の温度に初めて届いた刻み(0 = 届かない)
        uint64_t ignitedTick = 0;        // fuel=1: A が初めて減った刻み(0 = 減らない)
        std::vector<uint64_t> arrivals;  // fuel=1: 床の初めから d m(1, 2, …)のセルが着火した刻み(0 = 届かない)
        double burned = 0.0;             // fuel=1: 燃えた A ÷ 初めの A
        double seconds = 0.0;
        bool conserved = true;
        TimeTreeSteps steps;
        Snapshot last;
    };

    double PlaceFuelStart() {
        return PLACE_HEATER_X + PLACE_HEATER_METERS + PLACE_GAP_METERS + placeOffset;
    }

    struct PlaceProbe {
        uint32_t slot = 0;
        uint32_t index = 0;
        double distance = 0.0;  // 床の初めから(m)
    };

    std::vector<PlaceProbe> PlaceProbes(const TimeTreeWorld& world) {
        const double fuelStart = PlaceFuelStart();
        std::vector<PlaceProbe> probes;
        for (const uint32_t slot : world.layout.blocks) {
            const MrBlock& block = world.nest.blocks[slot];
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const double x = (static_cast<double>(block.originX + MrCellX(index)) + 0.5) * CellMeters(block.level);
                if (world.layout.stepped[slot * MR_BLOCK_CELLS + index] != 0 && x >= fuelStart)
                    probes.push_back({slot, index, x - fuelStart});
            }
        }

        return probes;
    }

    PlaceResult RunPlace(const TimeTreeSetup& setup, const Material& material, TimeTreeWorld world, uint64_t ticks) {
        PlaceResult result;
        const auto start = static_cast<int64_t>(std::llround(material.molPerCubic * LEVEL0_VOLUME * 1e6));
        const std::vector<PlaceProbe> probes = PlaceProbes(world);
        const auto distances = static_cast<size_t>(WorldLength(sceneShape) - PlaceFuelStart());
        result.arrivals.assign(distances, 0);
        const TimeTreeTotals first = ConservedTimeTree(world);
        const auto begin = std::chrono::steady_clock::now();
        for (uint64_t tick = 1; tick <= ticks; ++tick) {
            StepTimeTree(setup, world, 1);

            // --- 床のセルの真実(反応が読む値)の山と、着火の温度に届いた刻み ---
            double farthest = -1.0;
            for (const PlaceProbe& probe : probes) {
                const int64_t temperature = TimeTreeTemperatureQ16(
                    setup, LoadNestCell(world.nest, probe.slot, probe.index).energy);
                if (temperature > result.peakQ16) {
                    result.peakQ16 = temperature;
                    result.peakTick = tick;
                    result.peakDistance = probe.distance;
                    result.peakExponent = TimeTreeExponentAt(world, probe.slot, probe.index);
                }

                if (result.crossTick == 0 && temperature >= setup.ignitionTemperatureQ16)
                    result.crossTick = tick;

                if (placeFuel && TimeTreeSettledReactant(world, probe.slot, probe.index) < start)
                    farthest = std::max(farthest, probe.distance);
            }

            // --- 燃え広がり(fuel=1)---
            if (farthest < 0.0)
                continue;

            if (result.ignitedTick == 0)
                result.ignitedTick = tick;

            for (size_t d = 1; d <= distances && static_cast<double>(d) <= farthest; ++d) {
                if (result.arrivals[d - 1] == 0)
                    result.arrivals[d - 1] = tick;
            }
        }

        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        result.conserved = first == ConservedTimeTree(world);
        result.steps = world.steps;
        result.last = TakeSnapshot(world);
        double burned = 0.0;
        for (const PlaceProbe& probe : probes)
            burned += static_cast<double>(start - TimeTreeSettledReactant(world, probe.slot, probe.index));

        result.burned = placeFuel && !probes.empty() ? burned / (static_cast<double>(start) * probes.size()) : 0.0;

        return result;
    }

    std::string ArrivalText(const std::vector<uint64_t>& arrivals) {
        std::string text;
        for (const uint64_t tick : arrivals)
            text += std::format("{}{}", text.empty() ? "" : ",", tick);

        return text;
    }

    void LogPlace(std::string_view name, const PlaceResult& result, uint64_t ticks, uint64_t cells) {
        const double perCellTick = static_cast<double>(result.steps.cellSteps) /
                                   (static_cast<double>(cells) * static_cast<double>(ticks));
        Log(Channel::Sim, Level::Info,
            "置く場所 {}(ずらし {:.2f} m・熱い物 {:.4f} K{}): 床の山 {:.4f} K(刻み {}・床の初めから {:.2f} m・e {})・"
            "着火の温度に届いた刻み {}・着火 {}・燃えた {:.4f} %・解き {:.5f} 回/セル/刻み・出来事 {}・保存 {}・{:.1f} "
            "秒",
            name, placeOffset, placeHeaterKelvin, placeFuel ? "・燃える物あり" : "",
            static_cast<double>(result.peakQ16) / Q16 / 1000.0, result.peakTick, result.peakDistance,
            result.peakExponent, result.crossTick, result.ignitedTick, result.burned * 100.0, perCellTick,
            result.steps.events, result.conserved ? "厳密" : "崩れた", result.seconds);
        Log(Channel::Sim, Level::Info,
            "PLACE rule={} offset={:.2f} heater={:.4f} fuel={} peakQ16={} peakTick={} peakDistance={:.2f} peakE={} "
            "cross={} ignited={} burned={:.6f} solves={:.6f} events={} arrivals={}",
            name, placeOffset, placeHeaterKelvin, placeFuel ? 1 : 0, result.peakQ16, result.peakTick,
            result.peakDistance, result.peakExponent, result.crossTick, result.ignitedTick, result.burned, perCellTick,
            result.steps.events, ArrivalText(result.arrivals));
    }

    void CheckPlace() {
        if (shapeName == "mixed") {
            sceneShape = {.rootLevel = 0, .roots = 8, .refine = false};  // 0.5 m の根だけ(混ぜた木は x で一様でない)
            shapeName = "fine";
        }

        const Material material = Fire();
        const double heaterStart = PLACE_HEATER_X + placeOffset;
        const double fuelStart = PlaceFuelStart();
        const double ground = material.groundKelvin;
        if (placeFuel)
            fuelAt = [fuelStart](double x) {
                return x >= fuelStart;
            };
        else
            fuelAt = [](double) {
                return false;
            };

        const Profile profile = [heaterStart, ground](double x) {
            return x >= heaterStart && x < heaterStart + PLACE_HEATER_METERS ? placeHeaterKelvin : ground;
        };
        const uint64_t ticks = sceneTicks != 0 ? sceneTicks : PLACE_TICKS;
        Log(Channel::Sim, Level::Info, "置く場所: {}・{} 刻み", TrialName(), ticks);

        std::optional<PlaceResult> fixed;
        std::optional<PlaceResult> change;
        for (const TimeStepRule rule : {TimeStepRule::Fixed, TimeStepRule::Change}) {
            const bool isFixed = rule == TimeStepRule::Fixed;
            if (placeRule != "both" && placeRule != (isFixed ? "fixed" : "change"))
                continue;

            TimeTreeSetup setup = MakeSetup(material, rule);
            setup.groupShift = groupShift;
            const TimeTreeWorld world = MakeWorld(setup, material, sceneShape, profile);
            uint64_t cells = 0;
            for (const uint8_t stepped : world.layout.stepped)
                cells += stepped;

            PlaceResult result = RunPlace(setup, material, world, ticks);
            LogPlace(isFixed ? "fixed" : "change", result, ticks, cells);
            Expect(result.conserved, "置く場所: 保存");
            (isFixed ? fixed : change) = std::move(result);
        }

        if (!fixed || !change)
            return;

        const Difference difference = Compare(
            MakeWorld(MakeSetup(material, TimeStepRule::Fixed), material, sceneShape, profile), material, change->last,
            fixed->last);
        Log(Channel::Sim, Level::Info,
            "置く場所 ずらし {:.2f} m: 床の山の差(長い刻み − 毎刻み){:.3f} mK・終わりのセルの温度の差の最大 {:.3f} "
            "mK(x "
            "{:.2f} m)",
            placeOffset, static_cast<double>(change->peakQ16 - fixed->peakQ16) / Q16, difference.temperatureMk,
            difference.worstX);
    }

    // --- 3. 早送り = 待った(ビット一致)・6. 歩の途中のセーブ ---

    void CheckFastForward() {
        const Material material = Fire();
        const TimeTreeSetup setup = MakeSetup(material, TimeStepRule::Change);
        Log(Channel::Sim, Level::Info, "早送り・セーブ: {}", TrialName());
        const TimeTreeWorld world = MakeWorld(setup, material, TreeShape{}, Ambient(material.groundKelvin));
        const Ignition ignition{FIRE_IGNITION_TICK, material.groundKelvin};
        const uint64_t ticks = 2 * COMPARE_EVERY + 777;
        const RunResult waited = Run(setup, material, world, ticks, 1, ignition);
        const RunResult skipped = Run(setup, material, world, ticks, ticks, ignition);
        const RunResult uneven = Run(setup, material, world, ticks, 997, ignition);
        const RunResult again = Run(setup, material, world, ticks, ticks, ignition);
        Expect(waited.hash == skipped.hash, "早送り: 1 刻みずつと一気にがビット一致");
        Expect(waited.hash == uneven.hash, "早送り: ばらばらの塊とビット一致");
        Expect(waited.hash == again.hash, "2 回の実行でビット一致");
        Expect(waited.conserved && skipped.conserved && uneven.conserved, "早送り: どの塊の境目でも保存");
        Log(Channel::Sim, Level::Info, "早送り: 要約 {:016x}・1 刻みずつ {:.2f} 秒・一気に {:.2f} 秒", waited.hash,
            waited.seconds, skipped.seconds);
    }

    void CheckSave() {
        const Material material = Fire();
        const TimeTreeSetup setup = MakeSetup(material, TimeStepRule::Change);
        const TimeTreeWorld start = MakeWorld(setup, material, TreeShape{}, Ambient(material.groundKelvin));
        const Ignition ignition{FIRE_IGNITION_TICK, material.groundKelvin};
        const uint64_t saveTick = COMPARE_EVERY + 4096 + 333;  // 長い歩の途中
        const uint64_t ticks = 2 * COMPARE_EVERY + 777;

        TimeTreeWorld world = start;
        StepTimeTree(setup, world, ignition.tick);
        Ignite(setup, material, world, ignition);
        StepTimeTree(setup, world, saveTick - ignition.tick);
        uint32_t midStep = 0;
        for (const uint32_t unit : world.layout.units)
            midStep += world.steps.open[unit] != 0 && world.steps.start[unit] < saveTick ? 1u : 0u;

        const MultiresSaveState save = SaveNestState(world.nest);
        const TimeTreeSteps savedSteps = world.steps;
        const TimeTreeRecordBytes bytes = MeasureTimeTreeRecords(world);
        const std::vector<uint16_t> packed = PackTimeTreeUnits(world);  // 単位の見出しは詰めた 2 B から戻す(T-1112)
        StepTimeTree(setup, world, ticks - saveTick);

        TimeTreeWorld loaded = start;
        const bool restored = RestoreNestState(loaded.nest, save);
        loaded.steps = savedSteps;
        for (const uint32_t unit : loaded.layout.units) {
            loaded.steps.exponent[unit] = -1;
            loaded.steps.desired[unit] = -1;
            loaded.steps.open[unit] = 7;
            loaded.steps.touched[unit] = 7;
            loaded.steps.start[unit] = ~0ull;
            loaded.steps.end[unit] = ~0ull;
        }

        const bool unpacked = !packed.empty() && UnpackTimeTreeUnits(loaded, packed);
        StepTimeTree(setup, loaded, ticks - saveTick);
        Expect(restored && midStep > 0, "セーブ: 歩の途中の枠がある刻みで写せる");
        Expect(unpacked, "セーブ: 単位の見出しを 2 B に詰めた記録から戻せる");
        Expect(HashTimeTree(world) == HashTimeTree(loaded), "セーブ: 歩の記録ごと戻すと続けたのとビット一致");
        Log(Channel::Sim, Level::Info,
            "セーブ(刻み {}・歩の途中の枠 {}): 歩の記録 最小 {} B({:.2f} B/セル)・見出しを詰めると {} B({:.2f} "
            "B/セル)・歩の変化も持つと {} B({:.2f} B/セル)・{} セル・{} 単位",
            saveTick, midStep, bytes.minimal, static_cast<double>(bytes.minimal) / bytes.cells, bytes.packed,
            static_cast<double>(bytes.packed) / bytes.cells, bytes.held, static_cast<double>(bytes.held) / bytes.cells,
            bytes.cells, bytes.units);
    }

    // --- T-1112: 刻みを選ぶ段(窓・隣の差 1 段)の入力と CPU の答えを書く(gpu_time_step_choose_test が GPU で選び直す)・
    //     詰めた歩の記録が 1 刻みずつと一気にでビット一致 ---
    constexpr uint64_t CHOOSE_RARE_EVENTS = 64;  // 打ち切り・隣の差で縮んだ出来事は every によらずこの数まで書く

    struct ChooseTrace {
        test::TimeStepChooseFile file;
        uint64_t events = 0;
        uint64_t packFailures = 0;
        uint64_t truncated = 0;  // 選ぶ段で打ち切った単位(歩の途中で次の e が決まった)
        uint64_t limited = 0;    // 隣の差 1 段で窓の上限・望みより短くなった単位
        uint64_t truncatedEvents = 0;
        uint64_t limitedEvents = 0;
        uint64_t hash = 0;  // 出来事ごとの (刻み・後の詰めた記録) の要約
    };

    ChooseTrace TraceChoose(TimeTreeSetup setup, const Material& material, const TimeTreeWorld& world, uint64_t ticks,
                            uint64_t chunk, const Ignition& ignition, uint32_t every) {
        ChooseTrace trace;
        const TimeTreeChooseObserver observer = [&](const TimeTreeChooseView& view) {
            const bool packed = !view.before.empty() && !view.after.empty();
            trace.packFailures += packed ? 0 : 1;
            trace.hash = fx::FxHash64(trace.hash, view.tick, view.after.size(), 0);
            for (const uint16_t record : view.after)
                trace.hash = fx::FxHashCombine(trace.hash, record);

            // 打ち切り・隣の差で縮んだ単位(歩の途中で選び直した・窓の上限と望みより短い)を数え、そういう出来事も書く
            uint64_t truncated = 0;
            uint64_t limited = 0;
            for (size_t u = 0; u < view.next.size() && packed; ++u) {
                if (view.next[u] < 0)
                    continue;

                const bool open = (view.before[u] & TIME_TREE_RECORD_OPEN) != 0;
                const auto desired = static_cast<int32_t>((view.before[u] >> TIME_TREE_RECORD_DESIRED_SHIFT) &
                                                          TIME_TREE_RECORD_EXPONENT_MASK);
                truncated += open ? 1 : 0;
                limited += !open && view.next[u] < std::min({desired, view.cap, int32_t{view.windowCaps[u]}}) ? 1 : 0;
            }

            trace.truncated += truncated;
            trace.limited += limited;
            const bool rare = (truncated != 0 && trace.truncatedEvents < CHOOSE_RARE_EVENTS) ||
                              (limited != 0 && trace.limitedEvents < CHOOSE_RARE_EVENTS);
            trace.truncatedEvents += truncated != 0 ? 1 : 0;
            trace.limitedEvents += limited != 0 ? 1 : 0;
            if (every != 0 && packed && (trace.events % every == 0 || rare)) {
                test::TimeStepChooseEvent event{.tick = view.tick,
                                                .cap = view.cap,
                                                .before = {view.before.begin(), view.before.end()},
                                                .windowCaps = {view.windowCaps.begin(), view.windowCaps.end()},
                                                .after = {view.after.begin(), view.after.end()}};
                for (const int32_t next : view.next)
                    event.next.push_back(static_cast<int8_t>(next));

                trace.file.events.push_back(std::move(event));
            }

            ++trace.events;
        };

        setup.chooseObserver = &observer;
        (void)Run(setup, material, world, ticks, chunk, ignition);

        return trace;
    }

    void CheckChooseDump(const Material& material, const Ignition& ignition) {
        TimeTreeSetup setup = MakeSetup(material, TimeStepRule::Change);
        setup.groupShift = groupShift;
        const TimeTreeWorld world = MakeWorld(setup, material, sceneShape, Ambient(material.groundKelvin));
        const uint64_t ticks = sceneTicks != 0 ? sceneTicks : ignition.tick + 4096;
        const uint32_t every = mergeDumpEvery != 0 ? mergeDumpEvery : 16;
        ChooseTrace bulk = TraceChoose(setup, material, world, ticks, ticks, ignition, every);
        bulk.file.tables = MakeTimeTreeChooseTables(setup, world.layout);
        Expect(bulk.file.tables.supported, "刻みを選ぶ段: GPU 版の範囲の設定(窓・隣の差 1 段)");
        Expect(bulk.packFailures == 0, "刻みを選ぶ段: どの出来事も歩の記録を 2 B に詰められる");
        if (mergeCheck) {
            const ChooseTrace single = TraceChoose(setup, material, world, ticks, 1, ignition, 0);
            Expect(single.events == bulk.events && single.hash == bulk.hash,
                   "刻みを選ぶ段: 詰めた歩の記録が 1 刻みずつと一気にでビット一致(出来事ごと)");
        }

        Log(Channel::Sim, Level::Info,
            "刻みを選ぶ段({}・{}): 刻み {}・出来事 {}・書いた出来事 {}・単位 {}・窓の格子 {}×{}×{}・半径 {}・隣 {}・"
            "打ち切り {}(出来事 {})・隣の差で縮んだ {}(出来事 {})・要約 {:016x}",
            shapeName, TrialName(), ticks, bulk.events, bulk.file.events.size(), bulk.file.tables.units,
            bulk.file.tables.gridSize[0], bulk.file.tables.gridSize[1], bulk.file.tables.gridSize[2],
            bulk.file.tables.radius, bulk.file.tables.neighbors.size(), bulk.truncated, bulk.truncatedEvents,
            bulk.limited, bulk.limitedEvents, bulk.hash);
        if (!dumpPath.empty())
            Expect(test::WriteTimeStepChoose(dumpPath, bulk.file), "刻みを選ぶ段: ファイルに書けた");
    }

    int Run(int argc, char** argv) {
        const std::string_view part = argc > 1 ? argv[1] : "all";
        for (int i = 2; i < argc; ++i) {
            const std::string_view argument = argv[i];
            const size_t equal = argument.find('=');
            const std::string_view key = argument.substr(0, equal);
            const std::string value(equal == std::string_view::npos ? "" : argument.substr(equal + 1));
            if (key == "select") {
                selectGiven = true;
                selectRule = value == "retake" ? TimeTreeSelect::Retake
                             : value == "jump" ? TimeTreeSelect::Jump
                                               : TimeTreeSelect::Previous;
            } else if (key == "budget") {
                budgetShift = static_cast<uint32_t>(std::atoi(value.c_str()));
                budgetMk = 0;
            } else if (key == "budgetmk")
                budgetMk = static_cast<uint64_t>(std::atoll(value.c_str()));
            else if (key == "zerocycles")
                zeroCycles = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "longcycles")
                longCycles = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "corner")
                igniteCorner = std::atof(value.c_str());
            else if (key == "ticks")
                sceneTicks = static_cast<uint64_t>(std::atoll(value.c_str()));
            else if (key == "stop")
                residualStop = static_cast<uint64_t>(std::atoll(value.c_str()));
            else if (key == "shorte")
                shortExponent = std::atoi(value.c_str());
            else if (key == "unit")
                unitByLength = value != "block";
            else if (key == "window")
                windowMeters = std::atof(value.c_str());
            else if (key == "stepcells")
                unitEdgeCells = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "bufferunits")
                bufferUnits = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "shape") {
                shapeName = value;
                if (value == "coarse")
                    sceneShape = {.rootLevel = -1, .roots = 4, .refine = false};
                else if (value == "fine")
                    sceneShape = {.rootLevel = 0, .roots = 8, .refine = false};
                else if (value == "finer")
                    sceneShape = {.rootLevel = 1, .roots = 16, .refine = false};
                else if (value == "big")  // 1 m の根 8 × 4 × 4 = 65,536 セル(64 m × 32 m × 32 m)
                    sceneShape = {.rootLevel = -1, .roots = 8, .refine = false, .rootsY = 4, .rootsZ = 4};
                else if (value == "bigfine")  // 0.5 m の根 8 × 4 × 4 = 65,536 セル(32 m × 16 m × 16 m)
                    sceneShape = {.rootLevel = 0, .roots = 8, .refine = false, .rootsY = 4, .rootsZ = 4};
                else
                    shapeName = "mixed";
            } else if (key == "group")
                groupShift = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "cycles")
                solverCycles = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "trialcycles")
                trialCycles = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "buffer")
                frontBuffer = value == "1";
            else if (key == "perturb")
                perturbMeters = std::atof(value.c_str());
            else if (key == "trial")
                trialFixed = value == "fixed";
            else if (key == "galerkin")
                solverGalerkin = value == "1";
            else if (key == "dump")
                dumpPath = value;
            else if (key == "check")
                mergeCheck = value != "0";
            else if (key == "every")
                mergeDumpEvery = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "scene")
                mergeNight = value == "night";
            else if (key == "offset")
                placeOffset = std::atof(value.c_str());
            else if (key == "heaterk")
                placeHeaterKelvin = std::atof(value.c_str());
            else if (key == "fuel")
                placeFuel = value == "1";
            else if (key == "rule")
                placeRule = value;
        }

        const bool all = part == "all";
        const Material pond = Pond();
        const Material fire = Fire();
        const Ignition fireIgnition{FIRE_IGNITION_TICK, fire.groundKelvin};
        if (all || part == "forward") {
            // 1 通り目と退く道の 1 本目で(2 通り目は採らないので外した。T-1013)
            for (const TimeTreeSelect select : {TimeTreeSelect::Previous, TimeTreeSelect::Jump}) {
                if (selectGiven && select != selectRule)
                    continue;

                selectRule = select;
                CheckFastForward();
                CheckSave();
            }
        }

        if (all || part == "grain" || part == "grain-pond")
            CheckGranularity("池", pond, PondProfile(32.0), NIGHT_TICKS, {});

        if (all || part == "grain" || part == "grain-fire")
            CheckGranularity("火事", fire, Ambient(fire.groundKelvin), FIRE_TICKS, fireIgnition);

        if (part == "fire-cost")
            CheckFireCost(fire, fireIgnition);

        if (part == "span-dump")
            CheckSpanDump(fire, fireIgnition);

        if (part == "choose-dump")
            CheckChooseDump(fire, fireIgnition);

        if (part == "forward-estimate")
            CheckForwardEstimate(fire, fireIgnition);

        if (part == "place")
            CheckPlace();

        if (part == "event-merge") {
            if (mergeNight)
                CheckEventMerge(pond, PondProfile(WorldLength(sceneShape)), NIGHT_TICKS, {});
            else
                CheckEventMerge(fire, Ambient(fire.groundKelvin), FIRE_TICKS, fireIgnition);
        }

        if (part == "grain-fine-pond")
            CheckGranularity("池", pond, PondProfile(32.0), NIGHT_TICKS, {}, 1);

        if (part == "grain-fine-fire")
            CheckGranularity("火事", fire, Ambient(fire.groundKelvin), FIRE_TICKS, fireIgnition, 1);

        if (all || part == "night")
            CheckScene("池(静かな夜)", "night", pond, PondProfile(WorldLength(TreeShape{})), NIGHT_TICKS, {}, true);

        if (all || part == "fire")
            CheckScene("火事", "fire", fire, Ambient(fire.groundKelvin), FIRE_TICKS, fireIgnition, false);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "time_step_tree_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "time_step_tree_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(argc, argv);
    SingletonFinalizer::Finalize();

    return exitCode;
}
