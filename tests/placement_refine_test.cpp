// placement_refine_test.cpp — 細かくする時の「置かれ方で配る」(T-1157。D-664・ADR-0469 の 2-1・20 §10.5)の CPU のテスト。確かめること:
//   1. wt3 の案 B の場面の作り方(T-1106 の SettleColumn・SettlePictureCell。下に写した参照)と、詰まり・傾き・物質を変えた
//      絵(2 次元・n = 2 と 4)で、凝縮相の子の量がビット単位で同じ。寄せるかの判定も同じ
//   2. 3 次元(2 × 2 × 2)で物質ごとの和がビット単位で保たれる。気体だけ・詰まり切った・据わる固体・物は同じ数(SAME)。
//      気体は子の空きの体積で配られる(詰まり切った子には入らない)
//   3. 重みで欄の値を配る(PlaceSplitValue): 同じ重みなら子は親と同じ数・和は子の数 × 親
//   4. 欄の細かくする(nest_fields::Refine): 置かれ方の口が無い世界と、同じ重みを返す口の世界で、細かくした後の状態の要約がビット一致
//      (今の世界の結果は変わらない)。偏った重みでは印の欄だけが重みで配られ(和 = 8 × 親)、無印の欄は SAME。登録の検査
#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "common/fixed.hlsli"
#include "core/log.h"
#include "core/singleton.h"
#include "multires_activity_scene.h"
#include "reaction_test_table.h"
#include "sim/multires_fields.h"
#include "sim/multires_nest.h"
#include "sim/multires_state.h"
#include "sim/placement_refine.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;
using namespace bicameral::sim;
using namespace bicameral::state_field;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    // 決まった擬似乱数(線形合同)
    class Random {
    public:
        explicit Random(uint64_t seed) : m_state(seed) {}

        uint32_t Next(uint32_t bound) {
            m_state = (m_state * 6364136223846793005ull) + 1442695040888963407ull;
            return (uint32_t)((m_state >> 33) % bound);
        }

    private:
        uint64_t m_state;
    };

    // --- wt3 の参照(.worktrees/wt3 の case_b_scene.cpp・T-1106 の写し。式を変えない)---
    namespace wt3 {

        enum class Phase { Gas, Liquid, Grain, Fixed };

        struct Material {
            Phase phase;
            uint32_t densityKgPerM3;
            uint32_t packedFractionPct;
        };

        constexpr int64_t FULL_Q16 = 65536;

        uint64_t SubstanceVolumeMm3(const Material& material, int64_t massMg) {
            if (material.densityKgPerM3 == 0 || massMg <= 0)
                return 0;

            return (uint64_t)massMg * 1000 / material.densityKgPerM3;
        }

        uint64_t SettledVolumeMm3(std::span<const Material> materials, std::span<const int64_t> massMg) {
            uint64_t grainSettled = 0;
            uint64_t condensed = 0;
            for (uint32_t index = 0; index < materials.size(); ++index) {
                const Material& material = materials[index];
                const uint64_t volume = SubstanceVolumeMm3(material, massMg[index]);
                if (material.phase == Phase::Grain) {
                    const uint32_t packed = material.packedFractionPct == 0 ? 100 : material.packedFractionPct;
                    grainSettled += volume * 100 / packed;
                }

                if (material.phase != Phase::Gas)
                    condensed += volume;
            }

            return std::max(grainSettled, condensed);
        }

        void SpreadEvenly(int64_t total, uint32_t index, std::vector<std::vector<int64_t>>& children) {
            const auto count = (int64_t)children.size();
            for (std::vector<int64_t>& child : children)
                child[index] = total / count;

            children.front()[index] += total % count;
        }

        std::vector<std::vector<int64_t>> SettleColumn(std::span<const Material> materials,
                                                       std::span<const int64_t> columnMassMg, uint32_t childCount,
                                                       uint64_t childVolumeMm3) {
            std::vector<std::vector<int64_t>> children(childCount, std::vector<int64_t>(materials.size(), 0));
            if (childCount == 0)
                return children;

            bool fixed = false;
            for (uint32_t index = 0; index < materials.size(); ++index)
                fixed |= materials[index].phase == Phase::Fixed && columnMassMg[index] > 0;

            const uint64_t settled = SettledVolumeMm3(materials, columnMassMg);
            const bool settles = !fixed && settled > 0 && settled < childVolumeMm3 * childCount;
            const uint32_t top = settles ? (uint32_t)((settled - 1) / childVolumeMm3) : 0;
            for (uint32_t index = 0; index < materials.size(); ++index) {
                const Phase phase = materials[index].phase;
                const int64_t total = columnMassMg[index];
                if (!settles || phase == Phase::Gas || phase == Phase::Fixed) {
                    SpreadEvenly(total, index, children);
                    continue;
                }

                int64_t given = 0;
                for (uint32_t child = 0; child < top; ++child) {
                    children[child][index] = fx::FxMulDivS64(total, childVolumeMm3, settled);
                    given += children[child][index];
                }

                children[top][index] = total - given;
            }

            return children;
        }

        struct PictureSettle {
            bool settles = false;
            int64_t fullnessQ16 = 0;
            std::vector<int64_t> massMg;
        };

        // 絵のセル(凝縮相の体積 % と物)を読む(ReadPictureCell の式)
        PictureSettle ReadPictureCell(std::span<const Material> materials, std::span<const uint32_t> pct, bool body,
                                      uint32_t childCount, uint64_t childVolumeMm3) {
            PictureSettle cell;
            cell.massMg.assign(materials.size(), 0);
            bool fixed = body;
            for (uint32_t index = 0; index < materials.size(); ++index) {
                const uint64_t volume = childVolumeMm3 * pct[index] / 100;
                cell.massMg[index] = (int64_t)(volume * materials[index].densityKgPerM3 / 1000 * childCount);
                fixed |= pct[index] > 0 && materials[index].phase == Phase::Fixed;
            }

            if (fixed) {
                cell.fullnessQ16 = FULL_Q16;
                return cell;
            }

            const uint64_t capacity = childVolumeMm3 * childCount;
            const uint64_t settled = SettledVolumeMm3(materials, cell.massMg);
            cell.fullnessQ16 = (int64_t)std::min<uint64_t>(settled * FULL_Q16 / capacity, FULL_Q16);
            cell.settles = settled > 0 && settled < capacity;

            return cell;
        }

        int64_t LocalSurfaceQ16(std::span<const PictureSettle> cells, uint32_t width, uint32_t x, uint32_t y) {
            const auto rows = (int64_t)(cells.size() / width);
            int64_t sum = 0;
            for (int64_t row = (int64_t)y - 1; row <= (int64_t)y + 1; ++row) {
                if (row < 0)
                    sum += FULL_Q16;
                else if (row < rows)
                    sum += cells[(size_t)row * width + x].fullnessQ16;
            }

            return sum - FULL_Q16;
        }

        int64_t SurfaceSlopeQ16(std::span<const PictureSettle> cells, uint32_t width, uint32_t x, uint32_t y) {
            if (x == 0 || x + 1 >= width)
                return 0;

            const int64_t here = LocalSurfaceQ16(cells, width, x, y);
            const int64_t left = here - LocalSurfaceQ16(cells, width, x - 1, y);
            const int64_t right = LocalSurfaceQ16(cells, width, x + 1, y) - here;
            if (left == 0 || right == 0 || (left > 0) != (right > 0))
                return 0;

            return std::abs(left) < std::abs(right) ? left : right;
        }

        std::vector<std::vector<std::vector<int64_t>>> SettlePictureCell(std::span<const Material> materials,
                                                                         const PictureSettle& cell, int64_t slopeQ16,
                                                                         uint32_t n, uint64_t childVolumeMm3) {
            const int64_t fullness = cell.fullnessQ16;
            const int64_t limit = std::min(fullness, FULL_Q16 - fullness) * 2 * n / (n - 1);
            const int64_t slope = std::clamp(slopeQ16, -limit, limit);
            std::vector<int64_t> weight(n, 0);
            int64_t weightSum = 0;
            for (uint32_t j = 0; j < n; ++j) {
                weight[j] = (2 * (int64_t)n * fullness) + (slope * ((2 * (int64_t)j) + 1 - (int64_t)n));
                weightSum += weight[j];
            }

            std::vector<std::vector<int64_t>> columns(n, std::vector<int64_t>(materials.size(), 0));
            for (uint32_t index = 0; index < materials.size(); ++index) {
                int64_t given = 0;
                for (uint32_t j = 0; j + 1 < n; ++j) {
                    columns[j][index] = fx::FxMulDivS64(cell.massMg[index], (uint64_t)weight[j], (uint64_t)weightSum);
                    given += columns[j][index];
                }

                columns[n - 1][index] = cell.massMg[index] - given;
            }

            std::vector<std::vector<std::vector<int64_t>>> children;
            for (const std::vector<int64_t>& column : columns)
                children.push_back(SettleColumn(materials, column, n, childVolumeMm3));

            return children;
        }

    }  // namespace wt3

    // --- 物質: 空気・水・砂(60 %)・泥(50 %)・石(据わる)---
    const std::vector<wt3::Material> REFERENCE_MATERIALS = {
        {wt3::Phase::Gas, 1, 0},       {wt3::Phase::Liquid, 1000, 0}, {wt3::Phase::Grain, 2650, 60},
        {wt3::Phase::Grain, 1500, 50}, {wt3::Phase::Fixed, 2700, 0},
    };

    std::vector<PlaceMaterial> PlaceMaterials() {
        std::vector<PlaceMaterial> materials;
        for (const wt3::Material& material : REFERENCE_MATERIALS) {
            const uint32_t kind = material.phase == wt3::Phase::Gas      ? SF_SETTLE_GAS
                                  : material.phase == wt3::Phase::Liquid ? SF_SETTLE_LIQUID
                                  : material.phase == wt3::Phase::Grain  ? SF_SETTLE_GRAIN
                                                                         : SF_SETTLE_ANCHORED;
            materials.push_back({kind, material.densityKgPerM3, material.packedFractionPct});
        }

        return materials;
    }

    // 絵のセル 1 つの凝縮相の体積 %(空気・水・砂・泥・石)と物
    struct PictureCell {
        std::vector<uint32_t> pct;
        bool body = false;
    };

    PictureCell RandomPictureCell(Random& random) {
        PictureCell cell;
        cell.pct.assign(REFERENCE_MATERIALS.size(), 0);
        const uint32_t pick = random.Next(10);
        if (pick == 0) {
            cell.pct[4] = 100;  // 石
        } else if (pick == 1) {
            cell.body = true;
        } else if (pick >= 3) {
            const uint32_t grain = random.Next(61);
            cell.pct[random.Next(2) == 0 ? 2 : 3] = grain;
            cell.pct[1] = random.Next(101 - grain) / (random.Next(3) + 1);  // 水
        }

        return cell;
    }

    // --- 1. wt3 と同じ答え(2 次元)---
    void TestMatchesWt3(uint32_t n, uint64_t seed) {
        const std::vector<PlaceMaterial> materials = PlaceMaterials();
        constexpr uint32_t WIDTH = 12;
        constexpr uint32_t HEIGHT = 8;
        const uint64_t childVolume = (uint64_t)(500 / n) * (500 / n) * (500 / n);
        const uint32_t childCount = n * n;
        Random random(seed);
        int compared = 0;
        int sameSettle = 0;
        int sameMass = 0;
        int settledCount = 0;  // 寄せたセル(凝縮相の量を比べた数)
        for (int picture = 0; picture < 40; ++picture) {
            std::vector<PictureCell> cells;
            std::vector<wt3::PictureSettle> reference;
            for (uint32_t i = 0; i < WIDTH * HEIGHT; ++i) {
                cells.push_back(RandomPictureCell(random));
                reference.push_back(wt3::ReadPictureCell(REFERENCE_MATERIALS, cells.back().pct, cells.back().body,
                                                         childCount, childVolume));
            }

            // --- こちらの詰まり(SfSettleFullnessQ16)と表面(SfColumnSurfaceQ16)---
            std::vector<int64_t> fullness(cells.size(), 0);
            for (uint32_t i = 0; i < cells.size(); ++i) {
                const SfSettleSum sum = PlaceSettleSum(materials, reference[i].massMg);
                fullness[i] = SfSettleFullnessQ16(SfSettledVolume(sum), childVolume * childCount,
                                                  sum.anchored || cells[i].body);
            }

            const auto surface = [&](uint32_t x, uint32_t y) {
                const int64_t below = y == 0 ? SF_SETTLE_FULL_Q16 : fullness[((y - 1) * WIDTH) + x];
                const int64_t above = y + 1 >= HEIGHT ? 0 : fullness[((y + 1) * WIDTH) + x];
                return SfColumnSurfaceQ16(below, fullness[(y * WIDTH) + x], above);
            };

            for (uint32_t y = 0; y < HEIGHT; ++y) {
                for (uint32_t x = 0; x < WIDTH; ++x) {
                    const uint32_t i = (y * WIDTH) + x;
                    const bool sides = x > 0 && x + 1 < WIDTH;
                    PlaceCell cell;
                    cell.massMg = reference[i].massMg;
                    cell.blocked = cells[i].body;
                    cell.slopeXQ16 = sides
                                         ? SfSurfaceSlopeQ16(surface(x - 1, y), surface(x, y), surface(x + 1, y), true)
                                         : 0;
                    const PlacedChildren placed = PlaceRefineCell(materials, cell, n, 1, childVolume);
                    ++compared;
                    sameSettle += placed.settled == reference[i].settles ? 1 : 0;
                    if (!reference[i].settles) {
                        sameMass += 1;
                        continue;
                    }

                    const int64_t slope = wt3::SurfaceSlopeQ16(reference, WIDTH, x, y);
                    const auto expected = wt3::SettlePictureCell(REFERENCE_MATERIALS, reference[i], slope, n,
                                                                 childVolume);
                    bool same = slope == cell.slopeXQ16;
                    for (uint32_t column = 0; column < n; ++column) {
                        for (uint32_t row = 0; row < n; ++row) {
                            for (uint32_t m = 1; m < materials.size(); ++m)
                                same &= expected[column][row][m] == placed.massMg[placed.ChildIndex(column, 0, row)][m];
                        }
                    }

                    sameMass += same ? 1 : 0;
                    ++settledCount;
                }
            }
        }

        Log(Channel::Sim, Level::Info,
            "wt3 と比べた (n = {}): {} セル(寄せた {})・寄せる判定が同じ {}・凝縮相の量が同じ {}", n, compared,
            settledCount, sameSettle, sameMass);
        Expect(sameSettle == compared, "wt3: 寄せるかの判定が同じ");
        Expect(sameMass == compared, "wt3: 凝縮相の子の量・傾きがビット単位で同じ");

        // --- 縦の列だけ(SettleColumn)---
        int columnSame = 0;
        for (int trial = 0; trial < 200; ++trial) {
            std::vector<int64_t> column(materials.size(), 0);
            for (uint32_t m = 0; m < materials.size(); ++m)
                column[m] = random.Next(4) == 0 ? 0 : (int64_t)random.Next(400000000);

            if (random.Next(3) != 0)
                column[4] = 0;  // 石は 3 回に 1 回

            const auto expected = wt3::SettleColumn(REFERENCE_MATERIALS, column, n, childVolume * n);
            columnSame += expected == PlaceSettleColumn(materials, column, n, childVolume * n) ? 1 : 0;
        }

        Expect(columnSame == 200, "wt3: 縦の列(SettleColumn)がビット単位で同じ");
    }

    // --- 2. 3 次元の保存と SAME ---
    bool SumsKept(const PlaceCell& cell, const PlacedChildren& placed) {
        for (uint32_t m = 0; m < cell.massMg.size(); ++m) {
            int64_t sum = 0;
            for (const std::vector<int64_t>& child : placed.massMg)
                sum += child[m];

            if (sum != cell.massMg[m])
                return false;
        }

        return true;
    }

    bool AllSame(const PlaceCell& cell, const PlacedChildren& placed) {
        for (uint32_t m = 0; m < cell.massMg.size(); ++m) {
            for (const std::vector<int64_t>& child : placed.massMg) {
                if (cell.massMg[m] % 8 == 0 && child[m] != cell.massMg[m] / 8)
                    return false;
            }
        }

        return true;
    }

    void TestThreeDimensions() {
        const std::vector<PlaceMaterial> materials = PlaceMaterials();
        const uint64_t childVolume = 250ull * 250 * 250;
        const uint64_t capacity = childVolume * 8;
        Random random(77);
        int kept = 0;
        for (int trial = 0; trial < 500; ++trial) {
            PlaceCell cell;
            cell.massMg.assign(materials.size(), 0);
            for (uint32_t m = 0; m < 4; ++m)
                cell.massMg[m] = random.Next(3) == 0 ? 0 : (int64_t)random.Next(300000000);

            cell.massMg[0] = (int64_t)random.Next(200000);  // 空気
            cell.slopeXQ16 = (int64_t)random.Next(200000) - 100000;
            cell.slopeZQ16 = (int64_t)random.Next(200000) - 100000;
            kept += SumsKept(cell, PlaceRefineCell(materials, cell, 2, 2, childVolume)) ? 1 : 0;
        }

        Expect(kept == 500, "3 次元: 物質ごとの和がビット単位で保たれる");

        // --- 寄せないセルは同じ数(量は 8 の倍数で比べる = 世界の「子は親と同じ数」)---
        PlaceCell gas;
        gas.massMg = {800000, 0, 0, 0, 0};
        PlaceCell full;
        full.massMg = {8000, 0, (int64_t)(capacity * 2650 / 1000 * 7 / 10) / 8 * 8, 0, 0};
        PlaceCell stone;
        stone.massMg = {8000, 8000000, 0, 0, 80000000};
        PlaceCell body;
        body.massMg = {8000, 8000000, 0, 0, 0};
        body.blocked = true;
        bool same = true;
        for (const PlaceCell* cell : {&gas, &full, &stone, &body}) {
            const PlacedChildren placed = PlaceRefineCell(materials, *cell, 2, 2, childVolume);
            same &= !placed.settled && AllSame(*cell, placed);
        }

        Expect(same, "3 次元: 気体だけ・詰まり切った・据わる固体・物は同じ数(SAME)");

        // --- 半分の砂: 下の段だけに砂、気体は上の段(空き)へ ---
        PlaceCell sand;
        sand.massMg = {800000, 0, (int64_t)(capacity / 2 * 2650 / 1000 * 6 / 10), 0, 0};
        const PlacedChildren placed = PlaceRefineCell(materials, sand, 2, 2, childVolume);
        bool layered = placed.settled && SumsKept(sand, placed);
        int64_t lowGas = 0;
        int64_t highGas = 0;
        for (uint32_t x = 0; x < 2; ++x) {
            for (uint32_t z = 0; z < 2; ++z) {
                layered &= placed.massMg[placed.ChildIndex(x, z, 1)][2] == 0;
                lowGas += placed.massMg[placed.ChildIndex(x, z, 0)][0];
                highGas += placed.massMg[placed.ChildIndex(x, z, 1)][0];
            }
        }

        Expect(layered && highGas > 0 && lowGas < highGas, "3 次元: 半分の砂は下の段に詰まり、気体は空きへ多く入る");
    }

    // --- 3. 重みで欄の値を配る ---
    void TestSplitValue() {
        const std::vector<uint64_t> equal(8, 7);
        const std::vector<uint64_t> tilted = {9, 0, 3, 0, 5, 0, 1, 0};
        bool same = true;
        bool kept = true;
        for (const int64_t value : {int64_t{0}, int64_t{5}, int64_t{-13}, int64_t{123456789}, int64_t{-987654321}}) {
            for (const int64_t child : PlaceSplitValue(value, equal))
                same &= child == value;

            const std::vector<int64_t> split = PlaceSplitValue(value, tilted);
            int64_t sum = 0;
            for (const int64_t child : split)
                sum += child;

            kept &= sum == value * 8 && split[1] == 0 && split[3] == 0 && split[5] == 0 && split[7] == 0;
        }

        Expect(same, "重み: 同じ重みなら子は親と同じ数");
        Expect(kept, "重み: 和は 8 × 親・重み 0 の子は 0");
        Expect(PlaceSplitValue(42, std::vector<uint64_t>(8, 0)) == std::vector<int64_t>(8, 42), "重み: 和 0 は SAME");
    }

    // --- 4. 欄の細かくする ---
    struct FieldWorld {
        MultiresNest nest;
        uint32_t plain = MULTIRES_NO_FIELD;      // 無印(SAME)
        uint32_t condensed = MULTIRES_NO_FIELD;  // 凝縮相に乗る(セル)
        uint32_t open = MULTIRES_NO_FIELD;       // 空きに乗る(面 ×3)
    };

    enum class Provider { None, Equal, Tilted };

    FieldWorld MakeFieldWorld(const BakedReactionTable& table, Provider provider) {
        FieldWorld world{.nest = MakeMultiresNest(test::MakeMultiresCapacity(table, 16, 0, 16))};
        MultiresNest& nest = world.nest;
        constexpr uint32_t CONSERVED = SF_FLAG_CONSERVED | SF_FLAG_HASHED;
        world.plain = RegisterNestField(
            nest, "truth.plain",
            SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_VOLUME, CONSERVED, 1));
        world.condensed = RegisterNestField(nest, "truth.condensed",
                                            SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_VOLUME,
                                                            CONSERVED | SF_FLAG_PLACE_CONDENSED, 1));
        world.open = RegisterNestField(nest, "truth.open",
                                       SfMakeFieldDesc(SF_PLACE_FACE, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_VOLUME,
                                                       CONSERVED | SF_FLAG_PLACE_OPEN, 3));

        const std::vector<RxCell> air(MR_BLOCK_CELLS, test::MakeAirCell(table));
        const uint32_t slot = PlaceRootBlock(nest, 0, 0, 0, air);
        const bool expanded = ExpandNestPage(nest, slot);
        Expect(expanded, "欄: 根を頁に広げられる");
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            StoreNestField(nest, world.plain, slot, index, 0, (int64_t)(index * 7) - 300);
            StoreNestField(nest, world.condensed, slot, index, 0, (int64_t)(index * 13) + 5);
            for (uint32_t c = 0; c < 3; ++c)
                StoreNestField(nest, world.open, slot, index, c, (int64_t)(index * 3) - (int64_t)(c * 400));
        }

        if (provider == Provider::Equal) {
            nest.refinePlacement = [](const MultiresNest&, uint32_t, uint32_t, NestChildWeights& weights) {
                weights.condensed.fill(11);
                weights.open.fill(5);
                return true;
            };
        } else if (provider == Provider::Tilted) {
            // 下の段(y の bit が 0)だけに凝縮相・空きは上の段が多い。セル 0 は口が無いと答える(SAME)
            nest.refinePlacement = [](const MultiresNest&, uint32_t, uint32_t parentCell, NestChildWeights& weights) {
                for (uint32_t j = 0; j < 8; ++j) {
                    const bool low = ((j >> 1) & 1u) == 0;
                    weights.condensed[j] = low ? 3 + j : 0;
                    weights.open[j] = low ? 1 : 5;
                }

                return parentCell != 0;
            };
        }

        const MrRequest refine = MrMakeRequest(MR_REQUEST_REFINE, 1, 1, 1, 1);
        SubmitRequests(nest, std::span(&refine, 1));
        ProcessRequests(nest);

        return world;
    }

    uint32_t FindChild(const MultiresNest& nest) {
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            if (nest.blocks[slot].kind == MR_BLOCK_REAL && nest.blocks[slot].level == 1)
                return slot;
        }

        return UINT32_MAX;
    }

    void TestNestRefine(const BakedReactionTable& table) {
        const FieldWorld none = MakeFieldWorld(table, Provider::None);
        const FieldWorld equal = MakeFieldWorld(table, Provider::Equal);
        const FieldWorld tilted = MakeFieldWorld(table, Provider::Tilted);
        const uint32_t child = FindChild(none.nest);
        Expect(child != UINT32_MAX && FindChild(equal.nest) == child && FindChild(tilted.nest) == child,
               "欄: 細かくした子がある");
        if (child == UINT32_MAX)
            return;

        Expect(HashWholeNest(none.nest) == HashWholeNest(equal.nest),
               "欄: 同じ重みの口は口の無い世界と状態の要約が同じ");

        // --- 口の無い世界は子は親と同じ数(今と同じ)---
        bool same = true;
        bool kept = true;
        bool lowOnly = true;
        bool plainSame = true;
        bool cellZeroSame = true;
        for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
            const uint32_t parentCell = MrOctantCell(0, local);
            for (uint32_t j = 0; j < 8; ++j) {
                const uint32_t index = MrChildCell(local, j);
                same &= LoadNestField(none.nest, none.condensed, child, index, 0) == (int64_t)(parentCell * 13) + 5;
                plainSame &= LoadNestField(tilted.nest, tilted.plain, child, index, 0) ==
                             (int64_t)(parentCell * 7) - 300;
                if (((j >> 1) & 1u) != 0 && parentCell != 0)
                    lowOnly &= LoadNestField(tilted.nest, tilted.condensed, child, index, 0) == 0;

                if (parentCell == 0)
                    cellZeroSame &= LoadNestField(tilted.nest, tilted.condensed, child, index, 0) == 5;
            }

            int64_t condensedSum = 0;
            int64_t openSum = 0;
            for (uint32_t j = 0; j < 8; ++j) {
                condensedSum += LoadNestField(tilted.nest, tilted.condensed, child, MrChildCell(local, j), 0);
                openSum += LoadNestField(tilted.nest, tilted.open, child, MrChildCell(local, j), 2);
            }

            kept &= condensedSum == 8 * ((int64_t)(parentCell * 13) + 5) &&
                    openSum == 8 * ((int64_t)(parentCell * 3) - 800);
        }

        Expect(same, "欄: 口の無い世界は子は親と同じ数");
        Expect(plainSame, "欄: 無印の欄は口があっても SAME");
        Expect(kept, "欄: 印の欄の子の和は 8 × 親(凝縮相・空き)");
        Expect(lowOnly, "欄: 凝縮相に乗る欄は凝縮相の無い子に入らない");
        Expect(cellZeroSame, "欄: 口が重みを返さないセルは SAME");

        // --- 登録の検査 ---
        const SfFieldDesc both = SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_SAME,
                                                 SF_FLAG_PLACE_CONDENSED | SF_FLAG_PLACE_OPEN, 1);
        const SfFieldDesc derived = SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_SAME,
                                                    SF_FLAG_DERIVED | SF_FLAG_PLACE_OPEN, 1);
        const SfFieldDesc block = SfMakeFieldDesc(SF_PLACE_BLOCK, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_SAME,
                                                  SF_FLAG_PLACE_CONDENSED, 1);
        const SfFieldDesc open = SfMakeFieldDesc(SF_PLACE_FACE, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_SAME,
                                                 SF_FLAG_PLACE_OPEN, 3);
        Expect(!SfDescValid(both) && !SfDescValid(derived) && !SfDescValid(block) && SfDescValid(open) &&
                   SfRefinePlacement(open) == SF_REFINE_OPEN,
               "登録: 置かれ方の印は真実のセル・面の欄に 1 つだけ");
    }

    int Run() {
        TestMatchesWt3(2, 11);
        TestMatchesWt3(4, 23);
        TestThreeDimensions();
        TestSplitValue();

        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "試験の表をベイクできない: {}", table.error());
            return 1;
        }

        TestNestRefine(*table);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "placement_refine_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "placement_refine_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
