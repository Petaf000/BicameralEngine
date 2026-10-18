// multires_world_edges.cpp — 世界の端を環境から作る・替える(multires_world_edges.h。T-0772・ADR-0338 仮)。
#include "sim/multires_world_edges.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <span>
#include <utility>

#include "core/aliases.h"
#include "sim/gas_cells.h"
#include "sim/gas_reference.h"
#include "sim/world_atmosphere.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        // 根のレベルの座標 → レベル 0 の座標(下の端は下へ・上の端は上へ丸める。根がレベル 0 より細かい時だけ丸めが効く)
        int64_t LevelZeroLow(int32_t rootLevel, int64_t coordinate) {
            if (rootLevel <= 0)
                return coordinate * (int64_t{1} << static_cast<uint32_t>(-rootLevel));

            return coordinate >> static_cast<uint32_t>(rootLevel);
        }

        int64_t LevelZeroHigh(int32_t rootLevel, int64_t coordinate) {
            if (rootLevel <= 0)
                return coordinate * (int64_t{1} << static_cast<uint32_t>(-rootLevel));

            const int64_t cells = int64_t{1} << static_cast<uint32_t>(rootLevel);

            return (coordinate + cells - 1) >> static_cast<uint32_t>(rootLevel);
        }

        uint32_t EdgeKind(WorldEdge edge) {
            return edge == WorldEdge::Open ? WE_OPEN : WE_WALL;
        }

        // 静かな基準のセル(気体の物質の順 species → 物質 ID の順のインライン。0 の成分は載せない。ProbeGasField::ReferenceCell と同じ)
        RxCell ToReactionCell(const GasCell& gas, std::span<const uint32_t> species) {
            RxCell cell = RxMakeEmptyCell(gas.energy);
            for (uint32_t s = 0; s < species.size(); ++s) {
                if (gas.amounts[s] == 0)
                    continue;

                cell.species[cell.speciesCount] = species[s];
                cell.amounts[cell.speciesCount] = gas.amounts[s];
                ++cell.speciesCount;
            }

            return cell;
        }

        bool SameWorld(const WeWorld& left, const WeWorld& right) {
            return left.rootLevel == right.rootLevel && left.lowX == right.lowX && left.lowY == right.lowY &&
                   left.lowZ == right.lowZ && left.highX == right.highX && left.highY == right.highY &&
                   left.highZ == right.highZ && left.top == right.top && left.bottom == right.bottom &&
                   left.sides == right.sides;
        }

        // RxCell は詰め物の欄まで値を持つ(RxMakeEmptyCell)ので、バイトで比べてよい
        bool SameEdges(const MultiresWorldEdges& left, const MultiresWorldEdges& right) {
            if (!SameWorld(left.world, right.world) || left.columnBaseUp != right.columnBaseUp ||
                left.column.size() != right.column.size())
                return false;

            return left.column.empty() ||
                   std::memcmp(left.column.data(), right.column.data(), left.column.size() * sizeof(RxCell)) == 0;
        }

        // 世界 world でブロック block が開いた端に接するか(ブロックの面が箱の面に重なり、その端が open)
        bool TouchesOpenEdge(const WeWorld& world, const MrBlock& block) {
            const bool anyOpen = world.top == WE_OPEN || world.bottom == WE_OPEN || world.sides == WE_OPEN;
            if (!anyOpen || block.level < world.rootLevel)
                return false;

            const uint32_t shift = static_cast<uint32_t>(block.level - world.rootLevel);
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const int64_t origin = WeOriginAxis(block, axis);
                const int64_t low = WeLow(world, axis) * (int64_t{1} << shift);
                const int64_t high = WeHigh(world, axis) * (int64_t{1} << shift);
                const uint32_t lowEdge = axis == WE_UP_AXIS ? WE_EDGE_BOTTOM : WE_EDGE_SIDES;
                const uint32_t highEdge = axis == WE_UP_AXIS ? WE_EDGE_TOP : WE_EDGE_SIDES;
                if (origin <= low && WeEdgeKind(world, lowEdge) == WE_OPEN)
                    return true;

                if (origin + MR_BLOCK_EDGE >= high && WeEdgeKind(world, highEdge) == WE_OPEN)
                    return true;
            }

            return false;
        }

    }  // namespace

    std::expected<std::vector<RxCell>, std::string> MakeAtmosphereColumn(const BakedReactionTable& table,
                                                                         int64_t firstUp, uint32_t layers) {
        if (table.atmosphere.Empty() || layers == 0)
            return std::vector<RxCell>{};

        // --- 層ごとの基準の大気(仮の世界と MR の気体と同じ GasAtmosphereLayers。層 k の高さ = (firstUp + k) × 0.5 m)---
        const GasAtmosphereLayers atmosphere = GasAtmosphereLayers::Create(
            table, firstUp * ATMOSPHERE_PRESSURE_STEP_MILLIMETERS, layers);
        const std::span<const uint32_t> species = atmosphere.Species();
        if (species.size() > RX_MAX_CELL_SPECIES) {
            return std::unexpected(std::format("基準の大気の物質が {} 種(外のセルのインラインの数 {} を超える)",
                                               species.size(), RX_MAX_CELL_SPECIES));
        }

        // --- 層の基準(一番下の層の静水圧から、層の重さの分だけ下げる)。効くのは物質の値と重力だけで、
        //     音速は効かないので 0(気体を流さない世界)なら係数の検査を通す 1 を入れる ---
        constexpr std::array<GasBoundary, GAS_AXES> walls = {GasBoundary::Wall, GasBoundary::Wall, GasBoundary::Wall};
        GasConfig config = atmosphere.MakeConfig(table, species, {1, 1, layers}, walls, GasConfig{}.substeps, 0);
        if (config.soundSpeedMmPerS == 0)
            config.soundSpeedMmPerS = 1;

        const std::vector<GasCell> reference = atmosphere.MakeReference(config, species);

        // --- 静かなセル ---
        std::vector<RxCell> column;
        column.reserve(reference.size());
        for (const GasCell& gas : reference)
            column.push_back(ToReactionCell(gas, species));

        return column;
    }

    std::expected<MultiresWorldEdges, std::string> MakeMultiresWorldEdges(const BakedReactionTable& table,
                                                                          const MultiresWorldBox& box) {
        for (uint32_t axis = 0; axis < 3; ++axis) {
            if (box.high[axis] <= box.low[axis])
                return std::unexpected(
                    std::format("世界の箱が空(軸 {} の high {} ≤ low {})", axis, box.high[axis], box.low[axis]));
        }

        // --- 箱と端の種類(環境の world_edges)---
        const WorldEdges& kinds = table.environment.edges;
        MultiresWorldEdges edges;
        edges.world = {.rootLevel = box.rootLevel,
                       .lowX = box.low[0],
                       .lowY = box.low[1],
                       .lowZ = box.low[2],
                       .highX = box.high[0],
                       .highY = box.high[1],
                       .highZ = box.high[2],
                       .top = EdgeKind(kinds.top),
                       .bottom = EdgeKind(kinds.bottom),
                       .sides = EdgeKind(kinds.sides)};
        if (kinds.AllWall() || table.atmosphere.Empty())
            return edges;

        // --- 外の柱(箱の中の層。上下の外は端の層を読む)---
        const int64_t bottom = LevelZeroLow(box.rootLevel, box.low[WE_UP_AXIS]);
        const int64_t top = LevelZeroHigh(box.rootLevel, box.high[WE_UP_AXIS]);
        if (top - bottom > MULTIRES_COLUMN_LAYER_LIMIT) {
            return std::unexpected(std::format("世界の箱の高さが {} 層(外の柱の上限 {} 層を超える)", top - bottom,
                                               MULTIRES_COLUMN_LAYER_LIMIT));
        }

        auto column = MakeAtmosphereColumn(table, bottom, static_cast<uint32_t>(top - bottom));
        if (!column)
            return std::unexpected(column.error());

        edges.columnBaseUp = bottom;
        edges.column = std::move(*column);

        return edges;
    }

    uint32_t SetWorldEdges(MultiresNest& nest, MultiresWorldEdges edges) {
        if (SameEdges(nest.edges, edges))
            return 0;

        // --- 前か後で開いた端に接する本物のブロックを種に ---
        uint32_t seeded = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL)
                continue;

            if (!TouchesOpenEdge(nest.edges.world, block) && !TouchesOpenEdge(edges.world, block))
                continue;

            nest.seeds[slot] = 1;
            ++seeded;
        }

        nest.edges = std::move(edges);

        return seeded;
    }

}  // namespace bicameral::sim
