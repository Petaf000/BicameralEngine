// tree_slope_scene.h — 世界の木の複数の根(0.5 m)に描いた 25° の砂の斜面を、斜面を横切る八分の一ごとに細かくして(0.25 m)、
// 子の粒の量を 0.25 m の格子(z = 0 の断面)に並べる(T-1257。D-664 の「前線が通って細かくなった所の砂の斜面が勝手に動かない」の測り)。
// 斜面の形は case_b_r3_test の MakeSlopeScene(32 × 16 セルの絵。床 1 段・頂上 4 m・表面のセルは詰まりの 1/10 刻み)と同じ。
// 根は x に 4 つ・y に 2 つ(z は 1 つ。z には一様)。細かくするのは z の下の半分の八分の一の全部(32 個。空気だけの一様な根は細かくしない)。
// 要求の確定は親のブロック 1 つに 1 刻み 1 つなので、根ごとに 1 つずつ 4 回。順は 2 つ: 一度に(八分の一ごとに細かくする前の木から)・
// 前線の順(1 つの木で 4 刻み。前に細かくした隣は子に覆われていて「無い」と読む)。
// 細かくし方は 3 つ: 隣のブロックの列を読む(T-1189・既定)/ 読まない(T-1188 の形 = 根ごとに別の世界で細かくする。外は「無い」)/
// 置かれ方を使わない(SAME。参考)。
// 流体の段は案 B の箱で回す(case_b_r3_test の 15): 世界の木には 2 つ以上のレベルで空気と粒を流す段がまだ無い(multires_gas は根の
// レベルだけ・粒の運動は案 B の箱だけ)ので、木で細かくした子の粒の量を 0.25 m の箱に写す。
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "reaction_test_table.h"
#include "sim/multires_nest.h"
#include "tree_refine_placement_scene.h"

namespace bicameral::test::tree_slope {

    using namespace bicameral::multires;
    using namespace bicameral::reaction;

    constexpr uint32_t ROOTS_X = 4;
    constexpr uint32_t ROOTS_Y = 2;
    constexpr uint32_t PICTURE_WIDTH = ROOTS_X * MR_BLOCK_EDGE;   // 32(0.5 m のセル)
    constexpr uint32_t PICTURE_HEIGHT = ROOTS_Y * MR_BLOCK_EDGE;  // 16
    constexpr uint32_t FINE_WIDTH = 2 * PICTURE_WIDTH;            // 64(0.25 m のセル)
    constexpr uint32_t FINE_HEIGHT = 2 * PICTURE_HEIGHT;          // 32
    constexpr uint32_t PACKING_PERCENT = 60;                      // 粒の詰まり(D-679 の既定)
    constexpr uint32_t SLOPE_TAN_Q16 = 30560;                     // tan 25°

    // 細かくし方
    enum class RefineMode : uint8_t {
        Neighbors,  // 1 つの世界に根 8 つ。親のブロックの外は隣のブロックの写しから読む(T-1189)
        Isolated,   // 根ごとに別の世界(T-1188 の形: 親のブロックの外は無い = 横は傾き 0・下は詰まり切る・上は空)
        Same,       // 置かれ方を使わない(子は親と同じ数)
    };

    struct Refined {
        std::vector<uint64_t> grain;  // FINE_WIDTH × FINE_HEIGHT(y × FINE_WIDTH + x。z = 0 の断面。レベル 1 の単位)
        bool conserved = true;        // 細かくした八分の一ごとに、物質ごとの子の和 = 親 × 8(ビット単位)
        bool allRefined = true;       // 一様でない八分の一は全部子を持った(一様な根は細かくしない。子の格子は親の値)
        uint32_t refinedOctants = 0;  // 子を持った八分の一の数
        uint64_t hash = 0;            // 世界の状態の要約(根ごとの世界は順に混ぜる)
    };

    inline std::optional<tree_place::Materials> MakeSlopeMaterials() {
        const auto baked = sim::BakeReactionTable(sim::MakeSpeciesChangedTestTable());
        if (!baked)
            return std::nullopt;

        return tree_place::MakeMaterials(*baked, PACKING_PERCENT);
    }

    // 絵のセル (x, y) の粒の体積 %(MakeSlopeScene と同じ: 床と詰まった砂 = 60 %・表面のセルは 6 % 刻み・上は空気)
    inline uint32_t SlopeGrainPercent(uint32_t x, uint32_t y) {
        if (y == 0)
            return PACKING_PERCENT;  // 床(箱では石。木では詰まり切った砂)

        const uint64_t top = 8ull << 16;  // 8 セル(4 m)
        const uint64_t drop = (uint64_t)SLOPE_TAN_Q16 * (2 * x + 1) / 2;
        if (drop >= top)
            return 0;

        const uint64_t height = top - drop;
        const auto whole = (uint32_t)(height >> 16);
        if (y <= whole)
            return PACKING_PERCENT;

        if (y > whole + 1)
            return 0;

        const auto tenth = (uint32_t)(((height & 0xffff) * 10 + 0x8000) >> 16);

        return tenth >= 10 ? PACKING_PERCENT : PACKING_PERCENT / 10 * tenth;
    }

    // 絵のセルの中身: 粒 + 空きに 1 気圧の空気(空きの割合で減らす)
    inline RxCell SlopeCell(const tree_place::Materials& materials, uint32_t x, uint32_t y) {
        const uint32_t grainPercent = SlopeGrainPercent(x, y);
        const uint32_t openPercent = 100 - grainPercent;
        std::vector<sim::SpeciesAmount> amounts = {
            {.species = materials.table.SpeciesId("oxygen"), .amount = 1067000ull * openPercent / 100},
            {.species = materials.table.SpeciesId("nitrogen"), .amount = 4013000ull * openPercent / 100}};
        if (grainPercent != 0)
            amounts.push_back({.species = materials.grain,
                               .amount = tree_place::AmountOfPercent(grainPercent, tree_place::GRAIN_MOLAR_VOLUME)});

        std::ranges::sort(amounts, {}, &sim::SpeciesAmount::species);

        return sim::MakeReactionCell(materials.table, amounts, tree_place::ROOM_MILLIKELVIN);
    }

    inline std::vector<RxCell> RootCells(const tree_place::Materials& materials, uint32_t rootX, uint32_t rootY) {
        std::vector<RxCell> cells;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
            cells.push_back(
                SlopeCell(materials, rootX * MR_BLOCK_EDGE + MrCellX(index), rootY * MR_BLOCK_EDGE + MrCellY(index)));

        return cells;
    }

    inline uint64_t AmountOf(const RxCell& cell, uint32_t species) {
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            if (cell.species[i] == species)
                return cell.amounts[i];
        }

        return 0;
    }

    // 詰まり切ったセルの粒の量(レベルの単位。どのレベルでも同じ数)
    inline uint64_t FullGrainAmount() {
        return tree_place::AmountOfPercent(PACKING_PERCENT, tree_place::GRAIN_MOLAR_VOLUME);
    }

    // 粒の量(レベルの単位)→ 体積 volumeMm3 のセルの中の粒の体積(mm³)。詰まり切ったセル = 体積 × 詰まり ちょうど
    // (モル体積で割り戻すと切り捨てで 1 mm³ 足りず、案 B の箱が「詰まっていない」と読むため、詰まり切った量との比で直す)
    inline uint64_t GrainVolumeMm3(uint64_t amount, uint64_t volumeMm3) {
        return volumeMm3 * PACKING_PERCENT / 100 * amount / FullGrainAmount();
    }

    // --- 細かくする ---
    // 根 (rootX, rootY) の z の下の半分の八分の一 (octantX, octantY)(レベル 1 の点の要求)
    inline MrRequest OctantRequest(uint32_t rootX, uint32_t rootY, uint32_t octantX, uint32_t octantY) {
        return MrMakeRequest(MR_REQUEST_REFINE, 1, 16 * rootX + 8 * octantX + 1, 16 * rootY + 8 * octantY + 1, 1);
    }

    struct OctantSums {
        std::array<uint64_t, 3> parent = {};  // 粒・酸素・窒素(親のセル 64 個の和)
    };

    struct SlopeNest {
        sim::MultiresNest nest;
        std::vector<std::array<uint32_t, 2>> roots;  // この世界にある根
    };

    inline SlopeNest MakeSlopeNest(const tree_place::Materials& materials,
                                   std::span<const std::array<uint32_t, 2>> roots, bool placed) {
        SlopeNest world{.nest = sim::MakeMultiresNest(MakeMultiresCapacity(materials.table, 48, 0, 16)),
                        .roots = {roots.begin(), roots.end()}};
        for (const auto& root : roots)
            sim::PlaceRootBlock(world.nest, root[0] * MR_BLOCK_EDGE, root[1] * MR_BLOCK_EDGE, 0,
                                RootCells(materials, root[0], root[1]));

        if (placed)
            sim::EnableRefineCellPlacement(world.nest, materials.table);

        return world;
    }

    // 世界の根ごとに八分の一 octant(z の下の半分の 4 つのうち)を細かくし、子を格子へ写す(要求の確定は親のブロック 1 つに 1 刻み 1 つ
    // なので、1 回の処理で細かくするのは根ごとに 1 つ)。保存は八分の一ごとに子の和と親 × 8 を比べる
    inline void RefineOctantAndGather(const tree_place::Materials& materials, SlopeNest& world, uint32_t octant,
                                      Refined& refined) {
        const std::array<uint32_t, 3> species = {materials.grain, materials.table.SpeciesId("oxygen"),
                                                 materials.table.SpeciesId("nitrogen")};
        const uint32_t octantX = octant & 1u;
        const uint32_t octantY = octant >> 1;

        // --- 親の和・一様か(要求の前の木から) ---
        std::vector<MrRequest> requests;
        std::vector<std::array<uint64_t, 3>> parents;
        std::vector<uint64_t> uniforms;  // 八分の一の初めのセルの粒(一様な八分の一の子の格子に使う)
        std::vector<bool> uniformFlags;
        for (const auto& root : world.roots) {
            const uint32_t rootSlot = sim::LookupBlock(world.nest, 0, root[0] * MR_BLOCK_EDGE, root[1] * MR_BLOCK_EDGE,
                                                       0);
            requests.push_back(OctantRequest(root[0], root[1], octantX, octantY));
            std::array<uint64_t, 3> sums = {};
            bool uniform = true;
            const RxCell first = sim::LoadNestCell(world.nest, rootSlot, MrCellIndex(4 * octantX, 4 * octantY, 0));
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (MrCellX(index) / 4 != octantX || MrCellY(index) / 4 != octantY || MrCellZ(index) >= 4)
                    continue;

                const RxCell cell = sim::LoadNestCell(world.nest, rootSlot, index);
                uniform &= MrSameCell(cell, first);
                for (size_t i = 0; i < species.size(); ++i)
                    sums[i] += AmountOf(cell, species[i]) * 8;
            }

            parents.push_back(sums);
            uniforms.push_back(AmountOf(first, materials.grain));
            uniformFlags.push_back(uniform);
        }

        sim::SubmitRequests(world.nest, requests);
        sim::ProcessRequests(world.nest);

        // --- 子を格子へ ---
        for (size_t next = 0; next < world.roots.size(); ++next) {
            const auto& root = world.roots[next];
            const int64_t originX = 16 * root[0] + 8 * octantX;
            const int64_t originY = 16 * root[1] + 8 * octantY;
            const uint32_t child = sim::LookupBlock(world.nest, 1, originX, originY, 0);
            if (child == MR_NO_BLOCK) {
                // 一様な根(空気だけ)は細かくしない。子は親と同じ数(SAME)なので格子は親の値
                refined.allRefined &= uniformFlags[next];
                for (int64_t y = originY; y < originY + 8; ++y) {
                    for (int64_t x = originX; x < originX + 8; ++x)
                        refined.grain[(size_t)y * FINE_WIDTH + (size_t)x] = uniforms[next];
                }

                continue;
            }

            ++refined.refinedOctants;
            std::array<uint64_t, 3> sums = {};
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const RxCell cell = sim::LoadNestCell(world.nest, child, index);
                for (size_t i = 0; i < species.size(); ++i)
                    sums[i] += AmountOf(cell, species[i]);

                if (MrCellZ(index) != 0)
                    continue;

                const uint64_t x = (uint64_t)originX + MrCellX(index);
                const uint64_t y = (uint64_t)originY + MrCellY(index);
                refined.grain[y * FINE_WIDTH + x] = AmountOf(cell, materials.grain);
            }

            refined.conserved &= sums == parents[next];
        }

        refined.hash = refined.hash * 1099511628211ull ^ sim::HashWholeNest(world.nest);
    }

    // 細かくする順
    enum class RefineOrder : uint8_t {
        AtOnce,  // 八分の一ごとに、細かくする前の木から(隣の八分の一はどれもまだ粗い = 一度に全部を細かくしたのと同じ読み方)
        Front,  // 1 つの木で 1 刻みに根ごと 1 つずつ(八分の一 0 → 1 → 2 → 3。前に細かくした隣は子に覆われている = 前線が通る形)
    };

    inline Refined RefineSlope(const tree_place::Materials& materials, RefineMode mode, RefineOrder order) {
        Refined refined{.grain = std::vector<uint64_t>(FINE_WIDTH * FINE_HEIGHT, 0)};
        std::vector<std::array<uint32_t, 2>> roots;
        for (uint32_t rootY = 0; rootY < ROOTS_Y; ++rootY) {
            for (uint32_t rootX = 0; rootX < ROOTS_X; ++rootX)
                roots.push_back({rootX, rootY});
        }

        // 世界の分け方: 根 8 つの 1 つの世界か、根ごとに別の世界(Isolated)
        std::vector<std::vector<std::array<uint32_t, 2>>> groups;
        if (mode == RefineMode::Isolated) {
            for (const auto& root : roots)
                groups.push_back({root});
        } else {
            groups.push_back(roots);
        }

        const bool placed = mode != RefineMode::Same;
        for (const auto& group : groups) {
            if (order == RefineOrder::Front) {
                SlopeNest world = MakeSlopeNest(materials, group, placed);
                for (uint32_t octant = 0; octant < 4; ++octant)
                    RefineOctantAndGather(materials, world, octant, refined);

                continue;
            }

            for (uint32_t octant = 0; octant < 4; ++octant) {
                SlopeNest world = MakeSlopeNest(materials, group, placed);
                RefineOctantAndGather(materials, world, octant, refined);
            }
        }

        return refined;
    }

}  // namespace bicameral::test::tree_slope
