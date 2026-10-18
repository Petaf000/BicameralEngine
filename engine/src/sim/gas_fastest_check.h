// gas_fastest_check.h — 最速(投影・音速 0)× 小さい空きの検査(T-0788・D-519・ADR-0346)。値は変えない(D-487 ②「あやしいのは警告して動かす」)。
// 最速の投影は、空きの小さいセル(孔・多孔の板)だけでつながった 2 つの気体の部分を 1 刻みで釣り合わせようとして、孔のセルに 1 刻みで
// 中身の何倍もを通す。運ぶ側は陽の風上のまま(中身で頭打ち)なので外側の反復が発散し、圧力と速さが壊れる(16³ の板。ADR-0346)。
// 本物の音速(陽解法・音速を書いた投影)は同じ置き方でも収まるので、検査は最速のときだけ。
// 「小さい」の閾値と、壊れた圧力の差は T-0788 の調べ(gas_projection_test --plates。チケットの表)から。
// データの流れ: GasBox の config(大きさ・境界)と openQ16(sim/probe_gas が世界のセルから作る)→ FindGasSmallOpenJoints
//   → 呼ぶ側が GasSmallOpenJointWarning で文にして作り手へ(sim/probe_gas の ProbeGasStepReport::warnings)。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sim/gas_reference.h"

namespace bicameral::sim {

    // 警告の文の頭(ほかの警告と見分けるため。試験が使う)
    inline constexpr std::string_view GAS_FASTEST_SMALL_OPEN_WARNING_PREFIX = "最速 × 小さい空き: ";

    // これより小さい空き(Q16)を「小さい」とする。測った 16³ の板(層の全部が同じ空き・最速・40 刻み)では、空き 1/2 は板の下が
    // 5 倍の圧力でも収まり、1/4 は 5 倍で、1/8 は 1.5 倍で、1/16 は 1.2 倍で、1/256 は 1.02 倍で壊れた(T-0788 のチケットの表)
    inline constexpr uint32_t GAS_FASTEST_SMALL_OPEN_Q16 = GAS_OPEN_FULL / 2;

    // 小さい空きのセルだけでつながった気体の部分の組 1 つ(同じ部分の組をつなぐ小さいセルのまとまりは 1 つにまとめる。孔のある壁の孔など)
    struct GasSmallOpenJoint {
        uint32_t parts = 0;              // つながる気体の部分の数(2 以上。開いた境界の外も 1 つと数える)
        bool outside = false;            // 部分の 1 つが開いた境界の外
        uint32_t smallCells = 0;         // つなぐ小さい空きのセルの数
        uint32_t smallestOpenQ16 = 0;    // その中の最も小さい空き
        uint32_t smallestPartCells = 0;  // 最も小さい部分のセルの数(外は数えない)
        // 小さい空きのセルの範囲(気体の箱の座標 x・y・z〔z が上〕。両端を含む)
        std::array<uint32_t, GAS_AXES> minCell{};
        std::array<uint32_t, GAS_AXES> maxCell{};
    };

    // 箱の気体の通れるセル(空き > 0)のうち、空きが smallOpenQ16 以上のセル(大きいセル)の面でつながった部分と、開いた境界の外を
    // 「部分」とし、小さい空きのセル(0 < 空き < smallOpenQ16)を通らないと行き来できない部分の組を返す(小さいセルの番号の順)。
    // 周期の境界は反対側とつながる。空きが空(全部のセルが全部)なら無い
    [[nodiscard]] std::vector<GasSmallOpenJoint> FindGasSmallOpenJoints(
        const GasConfig& config, std::span<const uint32_t> openQ16, uint32_t smallOpenQ16 = GAS_FASTEST_SMALL_OPEN_Q16);

    // 測った板で、この空きの層が壊れた最も小さい圧力の差(板の下の圧力 ÷ 上の圧力の %。0 = 測った範囲では壊れなかった)
    [[nodiscard]] uint32_t GasSmallOpenBreakPercent(uint32_t openQ16);

    // 作り手へ見せる文(頭は GAS_FASTEST_SMALL_OPEN_WARNING_PREFIX)。where は置き場の名前(「気体の区画」など)、
    // range は小さい空きのセルの範囲を作り手の座標で書いた文(座標の約束は呼ぶ側。sim/probe_gas は格子の座標)
    [[nodiscard]] std::string GasSmallOpenJointWarning(const GasSmallOpenJoint& joint, std::string_view where,
                                                       std::string_view range);

}  // namespace bicameral::sim
