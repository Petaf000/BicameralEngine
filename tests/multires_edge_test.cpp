// multires_edge_test.cpp — 世界の端(shaders/common/world_edge.hlsli・sim::MultiresNest::edges。T-0721・ADR-0337)の CPU のテスト。
// 流れの種類は熱の伝導(陽解法)。確かめること:
//   - 端が全部 wall(既定)なら、箱と外の柱を入れても入れなくても刻んだ結果がビット一致(端を流さない = T-0721 より前と同じ)
//   - 上だけ open の根 1 つ(400 K)と、上の外の行(レベル 0 の z = 8)だけ 250 K の外の柱: 熱が上の端から逃げ、上の層が下の層より冷たい。
//     保存の検査(ComputeConservedTotals)が帳簿込みで毎刻み一定・帳簿は「世界の端・出」にだけ載る
//   - 外の柱の高さの読み方: 冷たい行が世界の中の高さ(z = 7)なら上の外(z = 8 の行 = 400 K)とは釣り合っていて流れない
//   - 上の端に接して 2 段細かくした場面(端の面がレベル 0〜2): 保存量が毎刻み一定・活性だけ刻んでも全部刻んだ時とビット一致
//   - 横が open で外の柱が空(真空): 熱は伝わらず、全部 wall と同じ
// 環境から作る端(T-0772。sim/multires_world_edges.h。縦は world_edge.hlsli の WE_UP_AXIS。試験は縦の軸を替えてもそのまま通る形):
//   - パッケージの world_edges = { top = "open" }(earth_environment の上)から MakeMultiresWorldEdges: 上だけ open・外の柱は箱の中の 8 層で、
//     仮の世界の気体の区画の層の基準(ProbeGasField::ReferenceCell)とビット単位で同じ。温めた世界の熱が上の端から逃げ、保存量が一定
//   - 端を替える(SetWorldEdges): 開いた端に接するブロックだけが種になる・同じなら種にしない・外の温度が変わる / wall に戻すと種になる
//   - 基準の大気の無い世界の open: ベイクの「注意」と、外は真空(柱が空)
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/world_edge.hlsli"
#include "common/world_ledger.hlsli"
#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "multires_conduction_scene.h"
#include "multires_test_scene.h"
#include "reaction_test_table.h"
#include "script/reaction_table_loader.h"
#include "sim/multires_nest.h"
#include "sim/multires_world_edges.h"
#include "sim/probe_gas.h"

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

    constexpr MultiresStepOptions CONDUCTION = {.conduction = true};
    constexpr uint64_t EDGE_SEED = 20261017;
    constexpr int32_t WORLD_MK = 400000;  // 世界の空気の温度
    constexpr int32_t COLD_MK = 250000;   // 冷たい外の行の温度
    constexpr uint32_t COLUMN_ROWS = 10;  // 外の柱の行(レベル 0 の縦 = 0〜9。世界は 0〜7・上の外は 8)
    constexpr int64_t TOP_OUTSIDE_ROW = 8;
    constexpr int32_t REFINED_LEVELS = 2;

    // 横 2 つ(縦でない軸の順。WeAxisUpLast)と縦(WE_UP_AXIS)の座標を世界の軸 x・y・z に並べる
    std::array<int64_t, 3> WorldPoint(int64_t first, int64_t second, int64_t up) {
        std::array<int64_t, 3> point = {};
        point[WeAxisUpLast(0)] = first;
        point[WeAxisUpLast(1)] = second;
        point[WE_UP_AXIS] = up;

        return point;
    }

    uint32_t CellIndexUp(uint32_t first, uint32_t second, uint32_t up) {
        const std::array<int64_t, 3> point = WorldPoint(first, second, up);

        return MrCellIndex(static_cast<uint32_t>(point[0]), static_cast<uint32_t>(point[1]),
                           static_cast<uint32_t>(point[2]));
    }

    // --- 場面の道具 ---

    RxCell MakeAir(const BakedReactionTable& table, int32_t millikelvin) {
        return test::MakeConductionAir(table, millikelvin);
    }

    // 根 1 つ(原点。一様な 400 K・hot なら (3, 4, 5) だけ 1500 K)。世界の箱 = 根の箱・端は全部 wall
    MultiresNest MakeRootNest(const BakedReactionTable& table, uint32_t worldBlocks, uint32_t fractions, bool hot) {
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(table, worldBlocks, 0, fractions));
        std::vector<RxCell> cells(MR_BLOCK_CELLS, MakeAir(table, WORLD_MK));
        if (hot)
            cells[MrCellIndex(3, 4, 5)] = MakeAir(table, 1500000);

        PlaceRootBlock(nest, 0, 0, 0, cells);
        nest.edges.world = {.rootLevel = 0,
                            .lowX = 0,
                            .lowY = 0,
                            .lowZ = 0,
                            .highX = MR_BLOCK_EDGE,
                            .highY = MR_BLOCK_EDGE,
                            .highZ = MR_BLOCK_EDGE,
                            .top = WE_WALL,
                            .bottom = WE_WALL,
                            .sides = WE_WALL};

        return nest;
    }

    // 外の柱(行 coldRow だけ 250 K・ほかは世界と同じ 400 K)
    void SetColumn(MultiresNest& nest, const BakedReactionTable& table, int64_t coldRow) {
        nest.edges.columnBaseUp = 0;
        nest.edges.column.assign(COLUMN_ROWS, MakeAir(table, WORLD_MK));
        nest.edges.column[static_cast<size_t>(coldRow)] = MakeAir(table, COLD_MK);
    }

    // 帳簿の欄 slot(WlSlot)に 0 でない語があるか
    bool LedgerSlotUsed(const MultiresNest& nest, uint32_t slot) {
        const size_t words = size_t{MR_LEDGER_LEVELS} * nest.capacity.ledgerColumns;
        const auto first = nest.ledger.begin() + static_cast<std::ptrdiff_t>(slot * words);

        return std::any_of(first, first + static_cast<std::ptrdiff_t>(words), [](uint64_t word) { return word != 0; });
    }

    // 「世界の端・出」のほかの欄が全部 0 か
    bool OnlyEdgeOutUsed(const MultiresNest& nest) {
        const uint32_t edgeOut = WlSlot(WL_REASON_WORLD_EDGE, WL_DIRECTION_OUT);
        for (uint32_t slot = 0; slot < WL_SLOT_COUNT; ++slot) {
            if (slot != edgeOut && LedgerSlotUsed(nest, slot))
                return false;
        }

        return LedgerSlotUsed(nest, edgeOut);
    }

    // 根の層 up(レベル 0 の縦の座標)の平均の温度(mK)
    int64_t LayerTemperature(const MultiresNest& nest, const ReactionTableView& view, uint32_t up) {
        const uint32_t root = LookupBlock(nest, 0, 0, 0, 0);
        int64_t sum = 0;
        for (uint32_t z = 0; z < MR_BLOCK_EDGE; ++z) {
            for (uint32_t x = 0; x < MR_BLOCK_EDGE; ++x)
                sum += MrCellThermal(view, LoadNestCell(nest, root, CellIndexUp(x, z, up))).temperature;
        }

        return sum / (MR_BLOCK_EDGE * MR_BLOCK_EDGE);
    }

    // ticks 刻み全部刻み、保存量(帳簿込み)が動いた刻みの数を返す
    uint32_t StepAndCountDrifts(MultiresNest& nest, const BakedReactionTable& table, uint64_t ticks, int32_t finest) {
        const ConservedTotals initial = ComputeConservedTotals(nest, table, finest);
        uint32_t drifts = 0;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            StepNest(nest, table, EDGE_SEED, tick, CONDUCTION);
            drifts += ComputeConservedTotals(nest, table, finest) == initial ? 0 : 1;
        }

        return drifts;
    }

    // --- 全部 wall は前と同じ ---
    void CheckWallsUnchanged(const BakedReactionTable& table) {
        constexpr uint64_t TICKS = 40;
        MultiresNest before = MakeRootNest(table, 1, 0, true);
        before.edges = {};
        MultiresNest walls = MakeRootNest(table, 1, 0, true);
        SetColumn(walls, table, TOP_OUTSIDE_ROW);
        for (uint64_t tick = 0; tick < TICKS; ++tick) {
            StepNest(before, table, EDGE_SEED, tick, CONDUCTION);
            StepNest(walls, table, EDGE_SEED, tick, CONDUCTION);
        }

        Expect(HashWholeNest(before) == HashWholeNest(walls), "全部 wall: 端の無い世界とビット一致");
        Expect(!LedgerSlotUsed(walls, WlSlot(WL_REASON_WORLD_EDGE, WL_DIRECTION_OUT)), "全部 wall: 帳簿の世界の端は 0");
    }

    // --- 上だけ open: 熱が上の端から逃げる ---
    void CheckTopOpen(const BakedReactionTable& table) {
        constexpr uint64_t TICKS = 60;
        MultiresNest nest = MakeRootNest(table, 1, 0, false);
        nest.edges.world.top = WE_OPEN;
        SetColumn(nest, table, TOP_OUTSIDE_ROW);

        const ReactionTableView view = table.View();
        const uint32_t drifts = StepAndCountDrifts(nest, table, TICKS, 0);
        const int64_t top = LayerTemperature(nest, view, MR_BLOCK_EDGE - 1);
        const int64_t bottom = LayerTemperature(nest, view, 0);

        Expect(drifts == 0, "上だけ open: 保存量(帳簿込み)が毎刻み一定");
        Expect(top < bottom && top < WORLD_MK, "上だけ open: 上の層が冷えて下の層より冷たい");
        Expect(OnlyEdgeOutUsed(nest), "上だけ open: 帳簿は世界の端・出にだけ載る");
        Log(Channel::Sim, Level::Info, "上だけ open: {} 刻みで上の層 {} mK・下の層 {} mK", TICKS, top, bottom);
    }

    // --- 外の柱の高さの読み方: 冷たい行が世界の中の高さなら上の外とは釣り合う ---
    void CheckColumnHeight(const BakedReactionTable& table) {
        constexpr uint64_t TICKS = 8;
        MultiresNest nest = MakeRootNest(table, 1, 0, false);
        nest.edges.world.top = WE_OPEN;
        SetColumn(nest, table, TOP_OUTSIDE_ROW - 1);
        const uint64_t start = HashRealLeaves(nest);
        for (uint64_t tick = 0; tick < TICKS; ++tick)
            StepNest(nest, table, EDGE_SEED, tick, CONDUCTION);

        Expect(HashRealLeaves(nest) == start, "外の柱の高さ: 上の外は縦 = 8 の行(400 K)を読み、流れない");
        Expect(!LedgerSlotUsed(nest, WlSlot(WL_REASON_WORLD_EDGE, WL_DIRECTION_OUT)), "外の柱の高さ: 帳簿は 0");
    }

    // 上の端に接して 2 段細かくした場面(上の端の面がレベル 0・1・2 にある)
    MultiresNest MakeRefinedNest(const BakedReactionTable& table) {
        MultiresNest nest = MakeRootNest(table, 8, 8, false);
        nest.edges.world.top = WE_OPEN;
        SetColumn(nest, table, TOP_OUTSIDE_ROW);
        const int64_t topCell = (int64_t{MR_BLOCK_EDGE} << REFINED_LEVELS) - 1;
        const std::array<int64_t, 3> at = WorldPoint(13, 14, topCell);
        const MrRequest request = MrMakeRequest(MR_REQUEST_REFINE, REFINED_LEVELS, at[0], at[1], at[2]);
        SubmitRequests(nest, std::span(&request, 1));
        ProcessRequests(nest);
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot)
            nest.seeds[slot] = nest.blocks[slot].kind == MR_BLOCK_REAL ? 1 : 0;

        return nest;
    }

    // --- 細かくした場面: 保存量・活性 = 全部 ---
    void CheckRefined(const BakedReactionTable& table) {
        constexpr uint64_t TICKS = 40;
        MultiresNest full = MakeRefinedNest(table);
        MultiresNest active = MakeRefinedNest(table);
        const std::array<int64_t, 3> topBlock = WorldPoint(8, 8, 24);
        Expect(LookupBlock(full, REFINED_LEVELS, topBlock[0], topBlock[1], topBlock[2]) != MR_NO_BLOCK,
               "細かくした場面: レベル 2 のブロックが上の端に接する");

        const ConservedTotals initial = ComputeConservedTotals(full, table, REFINED_LEVELS);
        uint32_t drifts = 0;
        uint32_t differences = 0;
        for (uint64_t tick = 0; tick < TICKS; ++tick) {
            StepNest(full, table, EDGE_SEED, tick, CONDUCTION);
            StepActive(active, table, EDGE_SEED, tick, CONDUCTION);
            drifts += ComputeConservedTotals(full, table, REFINED_LEVELS) == initial ? 0 : 1;
            differences += test::SameWorld(full, active) ? 0 : 1;
        }

        Expect(drifts == 0, "細かくした場面: 保存量(帳簿込み)が毎刻み一定");
        Expect(differences == 0, "細かくした場面: 活性だけ刻んでも全部刻んだ時とビット一致");
        Expect(OnlyEdgeOutUsed(full), "細かくした場面: 帳簿は世界の端・出にだけ載る");
    }

    // --- 横が open で外が真空(柱が空): 全部 wall と同じ ---
    void CheckVacuum(const BakedReactionTable& table) {
        constexpr uint64_t TICKS = 20;
        MultiresNest walls = MakeRootNest(table, 1, 0, true);
        MultiresNest vacuum = MakeRootNest(table, 1, 0, true);
        vacuum.edges.world.sides = WE_OPEN;
        vacuum.edges.world.top = WE_OPEN;
        for (uint64_t tick = 0; tick < TICKS; ++tick) {
            StepNest(walls, table, EDGE_SEED, tick, CONDUCTION);
            StepNest(vacuum, table, EDGE_SEED, tick, CONDUCTION);
        }

        Expect(HashWholeNest(walls) == HashWholeNest(vacuum), "真空の外: 熱は伝わらず全部 wall とビット一致");
    }

    // --- 環境から作る端(T-0772)---

    const fs::path STANDARD_DIR = fs::path(BICAMERAL_DATA_STANDARD_DIR);
    const fs::path ATMOSPHERE_PACKAGES_DIR = fs::path(BICAMERAL_ATMOSPHERE_PACKAGES_DIR);
    constexpr MultiresWorldBox ROOT_BOX = {
        .rootLevel = 0, .low = {0, 0, 0}, .high = {MR_BLOCK_EDGE, MR_BLOCK_EDGE, MR_BLOCK_EDGE}};

    // tests/packages_atmosphere/<place> の <place>_world をゲーム本体として読む(依存は data/standard の棚から)
    std::expected<script::LoadedReactionTable, std::string> LoadWorld(const std::string& place) {
        return script::LoadReactionTable(
            {.packageRoot = ATMOSPHERE_PACKAGES_DIR / place, .standardRoot = STANDARD_DIR});
    }

    // RxCell は詰め物の欄まで値を持つ(RxMakeEmptyCell)のでバイトで比べる
    bool SameCell(const RxCell& left, const RxCell& right) {
        return std::memcmp(&left, &right, sizeof(RxCell)) == 0;
    }

    // パッケージの world_edges から作った端: 外の柱が仮の世界の気体の区画の層の基準と同じ・温めた世界の熱が上から逃げる
    void CheckPackageEdges(const BakedReactionTable& table) {
        constexpr uint64_t TICKS = 60;
        const WorldEdges& kinds = table.environment.edges;
        Expect(kinds.top == WorldEdge::Open && kinds.bottom == WorldEdge::Wall && kinds.sides == WorldEdge::Wall,
               "パッケージの world_edges: 上だけ open");

        const auto edges = MakeMultiresWorldEdges(table, ROOT_BOX);
        Expect(edges.has_value(), "パッケージの端: MakeMultiresWorldEdges が通る");
        if (!edges) {
            Log(Channel::Sim, Level::Error, "  {}", edges.error());
            return;
        }

        Expect(edges->world.top == WE_OPEN && edges->world.bottom == WE_WALL && edges->world.sides == WE_WALL,
               "パッケージの端: 世界の端の種類が環境のまま");
        Expect(edges->columnBaseUp == 0 && edges->column.size() == MR_BLOCK_EDGE,
               "パッケージの端: 外の柱は箱の中の 8 層");

        // --- 仮の世界の気体の区画(床 0・同じ箱)の層の基準とビット単位で同じ ---
        const auto field = ProbeGasField::Create(table, {.minCell = {0, 0, 0}, .sizeCells = {8, 8, 8}});
        Expect(field.has_value(), "パッケージの端: 仮の世界の気体の区画を作れる");
        if (field && edges->column.size() == MR_BLOCK_EDGE) {
            uint32_t same = 0;
            for (uint32_t y = 0; y < MR_BLOCK_EDGE; ++y)
                same += SameCell(field->ReferenceCell(table, y), edges->column[y]) ? 1 : 0;

            Expect(same == MR_BLOCK_EDGE, "パッケージの端: 外の柱が仮の世界の気体の区画の層の基準とビット一致");
        }

        // --- 外の柱の層で埋めて温めた世界: 熱が上の端から逃げる ---
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(table, 1, 0, 0));
        std::vector<RxCell> cells(MR_BLOCK_CELLS);
        for (uint32_t index = 0; index < MR_BLOCK_CELLS && edges->column.size() == MR_BLOCK_EDGE; ++index) {
            cells[index] = edges->column[WeCellAxis(index, WE_UP_AXIS)];
            cells[index].energy += cells[index].energy / 4;
        }

        PlaceRootBlock(nest, 0, 0, 0, cells);
        Expect(SetWorldEdges(nest, *edges) == 1, "パッケージの端: 根は上の端に接するので種になる");

        const ReactionTableView view = table.View();
        const int64_t warm = LayerTemperature(nest, view, 0);
        const uint32_t drifts = StepAndCountDrifts(nest, table, TICKS, 0);
        const int64_t top = LayerTemperature(nest, view, MR_BLOCK_EDGE - 1);
        const int64_t bottom = LayerTemperature(nest, view, 0);
        Expect(drifts == 0, "パッケージの端: 保存量(帳簿込み)が毎刻み一定");
        Expect(top < bottom && top < warm, "パッケージの端: 上の層が冷えて下の層より冷たい");
        Expect(OnlyEdgeOutUsed(nest), "パッケージの端: 帳簿は世界の端・出にだけ載る");
        Log(Channel::Sim, Level::Info, "パッケージの端: 初め {} mK → {} 刻みで上の層 {} mK・下の層 {} mK", warm, TICKS,
            top, bottom);
    }

    // 端を替える: 開いた端に接するブロックだけが種になる
    void CheckReloadSeeds(const BakedReactionTable& table) {
        constexpr int64_t BLOCKS = 3;
        constexpr int64_t EDGE = BLOCKS * MR_BLOCK_EDGE;
        constexpr uint32_t TOP_BLOCKS = BLOCKS * BLOCKS;
        constexpr MultiresWorldBox BOX = {.rootLevel = 0, .low = {0, 0, 0}, .high = {EDGE, EDGE, EDGE}};
        const auto open = MakeMultiresWorldEdges(table, BOX);
        Expect(open.has_value() && open->column.size() == EDGE, "端を替える: 24 層の外の柱");
        if (!open || open->column.empty())
            return;

        // --- 3 × 3 × 3 の根(一様。真ん中だけ端に接しない)---
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(table, BLOCKS * BLOCKS * BLOCKS, 0, 0));
        const std::vector<RxCell> cells(MR_BLOCK_CELLS, open->column.front());
        uint32_t center = MR_NO_BLOCK;
        for (int64_t z = 0; z < BLOCKS; ++z) {
            for (int64_t y = 0; y < BLOCKS; ++y) {
                for (int64_t x = 0; x < BLOCKS; ++x) {
                    const uint32_t slot = PlaceRootBlock(nest, x * MR_BLOCK_EDGE, y * MR_BLOCK_EDGE, z * MR_BLOCK_EDGE,
                                                         cells);
                    if (x == 1 && y == 1 && z == 1)
                        center = slot;
                }
            }
        }

        std::ranges::fill(nest.seeds, uint8_t{0});
        Expect(SetWorldEdges(nest, *open) == TOP_BLOCKS, "端を替える: 上だけ open で上の段の 9 ブロックが種");
        Expect(center != MR_NO_BLOCK && nest.seeds[center] == 0, "端を替える: 端に接しないブロックは種にならない");
        Expect(SetWorldEdges(nest, *open) == 0, "端を替える: 同じ端なら種にしない");

        // --- 外の温度が変わる(基準の大気のホットリロード)・wall に戻す ---
        BakedReactionTable warmer = table;
        for (AtmospherePoint& point : warmer.atmosphere.points)
            point.temperatureMilliKelvin += 10000;

        const auto reloaded = MakeMultiresWorldEdges(warmer, BOX);
        Expect(reloaded.has_value() && SetWorldEdges(nest, *reloaded) == TOP_BLOCKS,
               "端を替える: 外の温度が変わると上の段の 9 ブロックが種");

        BakedReactionTable walls = table;
        walls.environment.edges = {};
        const auto closed = MakeMultiresWorldEdges(walls, BOX);
        Expect(closed.has_value() && closed->column.empty() && SetWorldEdges(nest, *closed) == TOP_BLOCKS,
               "端を替える: wall に戻すと前に開いていた上の段が種");
    }

    // 基準の大気の無い世界の open: ベイクの注意と、外は真空
    void CheckVacuumWarning() {
        const auto loaded = LoadWorld("edges_vacuum");
        Expect(loaded.has_value(), "大気の無い open: パッケージを読める");
        if (!loaded) {
            Log(Channel::Sim, Level::Error, "  {}", loaded.error());
            return;
        }

        const BakedReactionTable& table = loaded->table;
        const bool warned = std::ranges::any_of(table.warnings, [](const std::string& warning) {
            return warning.find("基準の大気が無い") != std::string::npos;
        });
        Expect(warned, "大気の無い open: ベイクが注意を出す");

        const auto edges = MakeMultiresWorldEdges(table, ROOT_BOX);
        Expect(edges.has_value() && edges->world.top == WE_OPEN && edges->column.empty(),
               "大気の無い open: 外は真空(柱が空)");
    }

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "multires_edge_test: FAILED(表を作れない)");
            return 1;
        }

        CheckWallsUnchanged(*table);
        CheckTopOpen(*table);
        CheckColumnHeight(*table);
        CheckRefined(*table);
        CheckVacuum(*table);

        const auto open = LoadWorld("edges_open_top");
        Expect(open.has_value(), "上だけ open のパッケージを読める");
        if (open) {
            Expect(std::ranges::none_of(open->table.warnings,
                                        [](const std::string& warning) {
                                            return warning.find("基準の大気が無い") != std::string::npos;
                                        }),
                   "大気のある open: 真空の注意は出ない");
            CheckPackageEdges(open->table);
            CheckReloadSeeds(open->table);
        } else {
            Log(Channel::Sim, Level::Error, "  {}", open.error());
        }

        CheckVacuumWarning();

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "multires_edge_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "multires_edge_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
