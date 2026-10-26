// placement_refine.h — 細かくする時の「置かれ方で配る」の CPU リファレンス(T-1157。D-664・ADR-0469 の 2-1・20 §10.5・17 §3.2)。
// 親のセル 1 つの物質ごとの量(mg)を、横 n 列(3 次元なら n × n 列)× 縦 n 段の子へ配る:
//   凝縮相(粒・液体)は横の表面の傾きに沿って列に分け、列ごとに重さの向き(−y)に下の子から沈み固まった体積で詰める。
//   気体は子の空きの体積の重みで配る。据わる固体を含む・凝縮相が無い・詰まり切ったセルは同じ数で配る(SAME と同じ)。
// 1 つずつの式は shaders/common/state_field.hlsli(GPU と 1 つの源)。ここは回しだけ(GPU の回しは shaders/sim/placement_refine.hlsli。
// tests/gpu_placement_refine_test.cpp でビット一致。T-1159)。wt3 の案 B の場面の作り方(T-1106 の
// SettleColumn・SettlePictureCell)と同じ答えを出す(tests/placement_refine_test.cpp)。物質ごとの和はビット単位で保つ。
// 欄(登録の印 SF_FLAG_PLACE_*)へは PlaceChildWeights の重みを MultiresNest::refinePlacement で渡す(multires_fields の nest_fields::Refine)。
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "common/state_field.hlsli"

namespace bicameral::sim {

    // 置かれ方に要る物質の値(呼ぶ側の物質の表から写す)
    struct PlaceMaterial {
        uint32_t kind = state_field::SF_SETTLE_GAS;  // SF_SETTLE_*
        uint32_t densityKgPerM3 = 0;
        uint32_t packedFractionPct = 0;     // 粒の詰まり(%)。0 は 100
        uint32_t molarVolumeMm3PerMol = 0;  // 0 でなければ量は µmol で、体積はこちらから(世界の木のセル。T-1176)
    };

    // 沈み固まった体積(mm³)と、据わる固体を含むか
    [[nodiscard]] state_field::SfSettleSum PlaceSettleSum(std::span<const PlaceMaterial> materials,
                                                          std::span<const int64_t> massMg);

    // 縦の 1 列(childCount 個の子。下から)へ列の量を寄せる(wt3 の SettleColumn と同じ)。寄せない列と気体は同じ数で(端数は一番下の子へ)。
    // 戻り値は [子(下から)][物質]
    [[nodiscard]] std::vector<std::vector<int64_t>> PlaceSettleColumn(std::span<const PlaceMaterial> materials,
                                                                      std::span<const int64_t> columnMassMg,
                                                                      uint32_t childCount, uint64_t childVolumeMm3);

    // 親のセル 1 つ
    struct PlaceCell {
        std::vector<int64_t> massMg;  // 物質ごとの親のセルの量(子へ配る和)
        int64_t slopeXQ16 = 0;        // x の向きの表面の傾き(SfSurfaceSlopeQ16)
        int64_t slopeZQ16 = 0;        // z の向き(2 次元の時は使わない)
        bool blocked = false;         // 物を含む(寄せない。据わる固体は物質から分かる)
    };

    // 配った子: 子 (x, z, 段) の量は massMg[ChildIndex(x, z, 段)][物質]
    struct PlacedChildren {
        uint32_t columns = 1;  // 横の 1 辺の子の数(n)
        uint32_t depth = 1;    // z の子の数(2 次元は 1・3 次元は n)
        bool settled = false;  // 寄せた(false なら同じ数で配った)
        std::vector<std::vector<int64_t>> massMg;

        [[nodiscard]] uint32_t ChildIndex(uint32_t x, uint32_t z, uint32_t row) const {
            return (((z * columns) + x) * columns) + row;
        }
    };

    // 世界の木の子の番号 j(MrChildCell の j: x が bit 0・重さの向き y が bit 1・z が bit 2)→ PlacedChildren の番号。
    // 世界の木の細かくする(columns = 2・depth = 2)と GPU の回し(shaders/sim/placement_refine.hlsli)はこの並び(T-1159)
    [[nodiscard]] inline uint32_t PlaceIndexOfMrChild(const PlacedChildren& children, uint32_t j) {
        return children.ChildIndex(j & 1u, j >> 2, (j >> 1) & 1u);
    }

    // 親のセルを横 n 列 × (depth が n なら z も n 列)× 縦 n 段へ。x で分けてから z で分け、列ごとに縦へ寄せる。
    // 気体は子の空きの体積で配り直す。childVolumeMm3 は子 1 つの体積
    [[nodiscard]] PlacedChildren PlaceRefineCell(std::span<const PlaceMaterial> materials, const PlaceCell& cell,
                                                 uint32_t columns, uint32_t depth, uint64_t childVolumeMm3);

    // 欄へ渡す子の重み(子の番号は PlacedChildren::ChildIndex): 凝縮相の量・空きの体積
    struct PlaceWeights {
        std::vector<uint64_t> condensed;
        std::vector<uint64_t> open;
    };

    [[nodiscard]] PlaceWeights PlaceChildWeights(std::span<const PlaceMaterial> materials,
                                                 const PlacedChildren& children, uint64_t childVolumeMm3);

    // 親の値 v を重みで子へ(和 = 子の数 × v。重みのある最後の子が残り)。重みの和が 0 なら全部 v(SAME)
    [[nodiscard]] std::vector<int64_t> PlaceSplitValue(int64_t parentValue, std::span<const uint64_t> weights);

}  // namespace bicameral::sim
