// multires_world_edges.h — 世界の端を環境から作る・替える(T-0772・ADR-0338 仮。T-0721・ADR-0337 の続き)。
//
// データの流れ: パッケージの environment.world_edges(T-0710。sim::WorldEdges)と基準の大気(T-0499。BakedReactionTable::atmosphere)
//   + 呼ぶ側が決める世界の箱(MultiresWorldBox。根のレベルのセル)→ MakeMultiresWorldEdges → MultiresWorldEdges
//   → SetWorldEdges で MultiresNest::edges へ(外が変わったら開いた端に接するブロックを種にする)
//   → 当てる段の World::Neighbor・Outside(multires_conduction.cpp。shaders/common/world_edge.hlsli)が読む。
// 外の柱は世界の箱の中の層ごとに基準の大気を読んだ静かなセル(MakeAtmosphereColumn)。仮の世界の気体の区画(ProbeGasField)の
// 層の基準と同じ道(SampleAtmosphere → 一番下の層の静水圧 AtmospherePressureMicroPascal → MakeHydrostaticReference)で、
// 同じ区画ならビット単位で同じセル(試験 multires_edge)。柱の外の高さ(上下の外)は端の層の基準を読む(world_edge.hlsli の WeColumnRow。
// 仮の世界の気体の開いた境界の外と同じ)。風は入れない(D-514)。
// 縦は y(上 = +y)。レベル 0 のセルの y = Y の高さは海面から Y × 0.5 m(セルの下の面。仮の世界の区画の床 0 と同じ)。
// 基準の大気が無い世界の開いた端は真空(柱が空。熱は伝わらない。ベイクが「注意」を出す。reaction_table.cpp の WarnWorldSetting)。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "common/reaction.hlsli"
#include "sim/multires_nest.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 世界の箱(根のレベル rootLevel のセルの座標。low を含み high を含まない。軸は x・y・z の順で y が上)
    struct MultiresWorldBox {
        int32_t rootLevel = 0;
        std::array<int64_t, 3> low = {0, 0, 0};
        std::array<int64_t, 3> high = {0, 0, 0};
    };

    // 外の柱の層の数の上限(レベル 0 の 0.5 m の層で 2^20 = 約 524 km。器の決まり)
    inline constexpr int64_t MULTIRES_COLUMN_LAYER_LIMIT = int64_t{1} << 20;

    // 基準の大気の柱: レベル 0 の y が firstY から layers 層の、静かな基準のセル(成分は物質 ID の順・0 の成分は載せない)。
    // 一番下の層の圧力はその高さの静水圧、上の層は層の重さの分だけ下げる(仮の世界の気体の区画の層の基準と同じ)。
    // 基準の大気が空なら空。断る: 基準の大気の物質がセルのインラインの数(RX_MAX_CELL_SPECIES)を超える
    [[nodiscard]] std::expected<std::vector<reaction::RxCell>, std::string> MakeAtmosphereColumn(
        const BakedReactionTable& table, int64_t firstY, uint32_t layers);

    // 環境(table.environment.edges・table.atmosphere)と世界の箱から世界の端を作る。外の柱は箱の中の層(レベル 0 の y)だけ。
    // 端が全部 wall か基準の大気が無ければ柱は空(開いた端は真空)。
    // 断る: 箱が空・層が MULTIRES_COLUMN_LAYER_LIMIT を超える・MakeAtmosphereColumn が断る
    [[nodiscard]] std::expected<MultiresWorldEdges, std::string> MakeMultiresWorldEdges(const BakedReactionTable& table,
                                                                                        const MultiresWorldBox& box);

    // 世界の端を替える(環境のホットリロード・箱を変えた時。刻みの境目で呼ぶ)。前と違えば、前か後で開いた端に接する本物のブロックを
    // 種にする(次の StepActive で刻み、外との流れが始まる・変わる。一様なブロックは流れがあれば頁に広がる)。種にしたブロックの数を返す
    uint32_t SetWorldEdges(MultiresNest& nest, MultiresWorldEdges edges);

}  // namespace bicameral::sim
