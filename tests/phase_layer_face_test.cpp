// phase_layer_face_test.cpp — 伝導の面を部分ごとに(shaders/common/phase_layer.hlsli の PlPartSideOf・PlPartFaceFlow。ADR-0512 の 2・T-1230 段 2)の CPU の試験。
// 0.5 m のセルの面 1 つを、両側の部分つきの片側から重なりで組んで流す形を確かめる(世界の当てる段へつなぐ前の式の試験)。
//
// 確かめること:
//   1. 混ざらないセル同士の面は今の HcFaceFlow(HcMakeCache)の 1 回とビット単位で同じ。層の分は 0
//   2. 混ざったセルの下の面・上の面は 1 列の試験の片側(PlBottomSide・PlTopSide)の流れと同じ値。下の面は全部が層へ・上の面は層へ入らない
//   3. 横の面は両側から同じ値(向きだけ逆): 和も、層が出した分と受けた分も
//   4. 横の面の高さ違い: 浅い層の熱は深い層の層の部分へだけ入る・石の壁 ⇄ 混ざったセルは層の高さの分だけ層へ
//   5. 横に並んだ混ざったセル 2 つを横の面と層の上の面で 600 刻み回すと、和はビット単位で一定・部分の熱は負にならない・
//      4 つの部分の温度の幅は広がらず、狭まる(0.5 m のセルの水は熱容量が大きいので 10 秒では揃い切らない)
// 表とセルの作り方は tests/phase_layer_scene.h(1 列の試験と共有)。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

#include "phase_layer_scene.h"

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
    using Amount = test::PlAmount;

    constexpr uint32_t FACE_NEGATIVE_X = 0;
    constexpr uint32_t FACE_POSITIVE_X = 1;
    constexpr uint64_t CELL_VOLUME_PL = 125000000000000ull;

    struct MixedCell {
        rx::RxCell cell;
        int64_t condensedEnergy = 0;  // mJ(混ざったセルの時だけ)
    };

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

    // 凝縮相の部分(温度 condensedMk)と気体の部分(gasMk)。気体が無ければ混ざらないセル
    MixedCell MakeCell(const BakedReactionTable& table, const std::vector<Amount>& condensed, uint32_t condensedMk,
                       bool withAir, uint32_t gasMk) {
        const std::vector<Amount> gas = withAir ? AirFilling(table, condensed) : std::vector<Amount>{};
        const int64_t condensedMj = test::PlPartEnergyMicrojoules(table, condensed, condensedMk) / 1000;
        const int64_t gasMj = test::PlPartEnergyMicrojoules(table, gas, gasMk) / 1000;
        std::vector<Amount> all = condensed;
        all.insert(all.end(), gas.begin(), gas.end());

        MixedCell cell;
        cell.cell = test::MakePhaseLayerCell(table, all, condensedMj + gasMj);
        const bool mixed = rx::PlLayerOf(table.View(), cell.cell).mixed != 0;
        cell.condensedEnergy = mixed ? condensedMj : 0;

        return cell;
    }

    MixedCell MakeAir(const BakedReactionTable& table, uint32_t mk) {
        return MakeCell(table, {}, mk, true, mk);
    }

    rx::PlPartSide SideOf(const BakedReactionTable& table, const MixedCell& cell, uint32_t face) {
        return rx::PlPartSideOf(table.View(), cell.cell, cell.condensedEnergy, face);
    }

    rx::HcThermalCache WholeCache(const BakedReactionTable& table, const MixedCell& cell) {
        const rx::RxThermal thermal = rx::RxComputeThermal(table.View(), cell.cell);

        return rx::HcMakeCache(thermal,
                               rx::HcCellConductance(table.View(), cell.cell, rx::HcCacheTemperature(thermal)));
    }

    // 3: 両側から同じ値(向きだけ逆)
    bool Antisymmetric(const rx::PlFaceFlow& forward, const rx::PlFaceFlow& backward) {
        return forward.total == -backward.total && forward.selfCondensed == -backward.neighborCondensed &&
               forward.neighborCondensed == -backward.selfCondensed;
    }

    void CheckWholeFaces(const BakedReactionTable& table) {
        const MixedCell stone = MakeCell(table, {{"stone_real", test::PL_STONE_MICROMOL}}, 400000, false, 0);
        const MixedCell coldStone = MakeCell(table, {{"stone", test::PL_STONE_MICROMOL}}, 280000, false, 0);
        const MixedCell air = MakeAir(table, 300000);
        const MixedCell pairs[][2] = {{stone, air}, {stone, coldStone}, {air, coldStone}};
        for (const auto& pair : pairs) {
            for (uint32_t face = 0; face < 6; ++face) {
                const uint32_t opposite = face ^ 1u;
                const rx::PlFaceFlow flow = rx::PlPartFaceFlow(SideOf(table, pair[0], face),
                                                               SideOf(table, pair[1], opposite));
                EXPECT(flow.total == rx::HcFaceFlow(WholeCache(table, pair[0]), WholeCache(table, pair[1])));
                EXPECT(flow.selfCondensed == 0 && flow.neighborCondensed == 0);
            }
        }
    }

    void CheckVerticalFaces(const BakedReactionTable& table) {
        const MixedCell stone = MakeCell(table, {{"stone_real", test::PL_STONE_MICROMOL}}, 330000, false, 0);
        const MixedCell puddle = MakeCell(table, {{"water", 300000000}}, 290000, true, 300000);
        const MixedCell air = MakeAir(table, 310000);
        const rx::PlLayer layer = rx::PlLayerOf(table.View(), puddle.cell);
        const rx::PlParts parts = rx::PlPartsOf(table.View(), puddle.cell, puddle.condensedEnergy);
        EXPECT(layer.mixed != 0);

        // 石(下)⇄ 水たまりの下の面: 全部が層へ
        const rx::PlFaceFlow bottom = rx::PlPartFaceFlow(SideOf(table, stone, rx::PL_FACE_TOP),
                                                         SideOf(table, puddle, rx::PL_FACE_BOTTOM));
        EXPECT(bottom.total == rx::HcFaceFlow(WholeCache(table, stone), rx::PlBottomSide(parts, layer.heightQ16)));
        EXPECT(bottom.total > 0 && bottom.neighborCondensed == bottom.total && bottom.selfCondensed == 0);

        // 水たまりの上の面 ⇄ 空気: 気体だけ(層は出さない)
        const rx::PlFaceFlow top = rx::PlPartFaceFlow(SideOf(table, puddle, rx::PL_FACE_TOP),
                                                      SideOf(table, air, rx::PL_FACE_BOTTOM));
        EXPECT(top.total == rx::HcFaceFlow(rx::PlTopSide(parts, layer), WholeCache(table, air)));
        EXPECT(top.total < 0 && top.selfCondensed == 0 && top.neighborCondensed == 0);
        EXPECT(Antisymmetric(
            top, rx::PlPartFaceFlow(SideOf(table, air, rx::PL_FACE_BOTTOM), SideOf(table, puddle, rx::PL_FACE_TOP))));
    }

    void CheckLateralFaces(const BakedReactionTable& table) {
        // 浅い熱い水たまり(2 cm)と深い冷たい水(11 cm)。気体はどちらも 300 K
        const MixedCell shallow = MakeCell(table, {{"water", 300000000}}, 350000, true, 300000);
        const MixedCell deep = MakeCell(table, {{"water", 1500000000}}, 300000, true, 300000);
        const uint32_t shallowHeight = rx::PlLayerOf(table.View(), shallow.cell).heightQ16;
        const uint32_t deepHeight = rx::PlLayerOf(table.View(), deep.cell).heightQ16;
        EXPECT(shallowHeight < deepHeight);

        const rx::PlFaceFlow flow = rx::PlPartFaceFlow(SideOf(table, shallow, FACE_POSITIVE_X),
                                                       SideOf(table, deep, FACE_NEGATIVE_X));
        // 浅い層の熱は深い側の層へだけ(浅い側の気体 ⇄ 深い側の層は同じ温度で 0)
        EXPECT(flow.total > 0 && flow.selfCondensed == flow.total && flow.neighborCondensed == flow.total);
        EXPECT(Antisymmetric(
            flow, rx::PlPartFaceFlow(SideOf(table, deep, FACE_NEGATIVE_X), SideOf(table, shallow, FACE_POSITIVE_X))));

        // 熱い石の壁 ⇄ 冷たい水たまり: 層へは高さの分だけ(残りはセルの気体へ)
        const MixedCell wall = MakeCell(table, {{"stone_real", test::PL_STONE_MICROMOL}}, 400000, false, 0);
        const MixedCell cold = MakeCell(table, {{"water", 1500000000}}, 290000, true, 290000);
        const rx::PlFaceFlow wallFlow = rx::PlPartFaceFlow(SideOf(table, wall, FACE_POSITIVE_X),
                                                           SideOf(table, cold, FACE_NEGATIVE_X));
        EXPECT(wallFlow.selfCondensed == 0);
        EXPECT(wallFlow.neighborCondensed > 0 && wallFlow.total > wallFlow.neighborCondensed);
        EXPECT(Antisymmetric(
            wallFlow, rx::PlPartFaceFlow(SideOf(table, cold, FACE_NEGATIVE_X), SideOf(table, wall, FACE_POSITIVE_X))));
        std::printf("横の面: 浅い ⇄ 深い %lld mJ・石の壁 ⇄ 水たまり %lld mJ(うち層へ %lld mJ)\n",
                    static_cast<long long>(flow.total), static_cast<long long>(wallFlow.total),
                    static_cast<long long>(wallFlow.neighborCondensed));
    }

    int32_t Spread(const BakedReactionTable& table, const MixedCell (&cells)[2]) {
        int32_t low = INT32_MAX;
        int32_t high = INT32_MIN;
        for (const MixedCell& cell : cells) {
            const rx::PlParts parts = rx::PlPartsOf(table.View(), cell.cell, cell.condensedEnergy);
            low = std::min({low, parts.condensed.temperature, parts.gas.temperature});
            high = std::max({high, parts.condensed.temperature, parts.gas.temperature});
        }

        return high - low;
    }

    // 5: 横に並んだ 2 つの混ざったセル(横の面 1 つ + それぞれの層の上の面)を回す
    void CheckLateralRun(const BakedReactionTable& table) {
        MixedCell cells[2] = {MakeCell(table, {{"water", 300000000}}, 360000, true, 290000),
                              MakeCell(table, {{"water", 1500000000}}, 300000, true, 320000)};
        const int64_t total = cells[0].cell.energy + cells[1].cell.energy;
        const int32_t startSpread = Spread(table, cells);
        bool conserved = true;
        bool nonNegative = true;
        bool neverWider = true;
        for (int tick = 0; tick < 600; ++tick) {
            const rx::PlFaceFlow face = rx::PlPartFaceFlow(SideOf(table, cells[0], FACE_POSITIVE_X),
                                                           SideOf(table, cells[1], FACE_NEGATIVE_X));
            int64_t inner[2];
            for (int i = 0; i < 2; ++i)
                inner[i] = rx::PlInnerFlowOf(table.View(), cells[i].cell, cells[i].condensedEnergy);

            cells[0].cell.energy -= face.total;
            cells[1].cell.energy += face.total;
            cells[0].condensedEnergy -= face.selfCondensed + inner[0];
            cells[1].condensedEnergy += face.neighborCondensed - inner[1];
            conserved = conserved && cells[0].cell.energy + cells[1].cell.energy == total;
            for (const MixedCell& cell : cells) {
                const rx::PlParts parts = rx::PlPartsOf(table.View(), cell.cell, cell.condensedEnergy);
                nonNegative = nonNegative && parts.condensed.heat >= 0 && parts.gas.heat >= 0;
            }

            neverWider = neverWider && Spread(table, cells) <= startSpread;
        }

        const int32_t endSpread = Spread(table, cells);
        EXPECT(conserved);
        EXPECT(nonNegative);
        EXPECT(neverWider);
        EXPECT(endSpread < startSpread);
        std::printf("横の 2 セル 600 刻み: 4 つの部分の温度の幅 %d mK → %d mK\n", startSpread, endSpread);
    }

}  // namespace

int main() {
    const BakedReactionTable table = test::MakePhaseLayerTable();
    CheckWholeFaces(table);
    CheckVerticalFaces(table);
    CheckLateralFaces(table);
    CheckLateralRun(table);
    if (failureCount != 0) {
        std::printf("%d 件の失敗\n", failureCount);
        return 1;
    }

    std::printf("ok\n");
    return 0;
}
