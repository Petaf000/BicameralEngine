// sun_lens.h — 日の光を物で集める CPU リファレンス(研究 R-R2・T-0878・docs/design/23-radiation.md §5・§6・ADR-0425)。
// 本体は shaders/common/sun_lens.hlsli(LnTraceLens・LnWalkWorld)。ここはレンズの物の値を作り、太陽の影の柱に「レンズに入った量」を
// 取らせ(SweepSunColumnCaught)、取られた量をレンズの格子の点(物の細かさ)の光線へ分けて走らせるだけ。
//
// データの流れ:
//   レンズの形(SunLensShape: 物の箱の中心・口の半径・2 つの球面・厚さ・物質と量・物の細かさ)+ 物質の値(光の値・屈折の値)
//   → MakeSunLens(LnLens)→ FocusSunLight: 影の柱がレンズの面に届けた量(光線ごと)→ 口の中の格子の点ごとの光線 → 世界のセルへ
//   → 出力: セルごとの吸った・返した(レンズの光線の単位)・当たった面(セルの面の上の範囲)・帳簿。
// 世界は読むだけ(状態を変えない。セルのエネルギーへ当てる段は土台の共通の口。T-0910 の申し送り (1)〜(4))。
// レンズの物の箱は世界の軸に揃え、光の軸は太陽の向きの一番大きい軸(回る物は物の物理と組む時。T-0878 の申し送り)。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <vector>

#include "common/sun_lens.hlsli"
#include "sim/sun_column.h"

namespace bicameral::sim {

    // レンズの物(両凸。試験の場面が置く。物の箱の最小の形が来たら物の値から作る)
    struct SunLensShape {
        std::array<int64_t, 3> centreUm{};  // 物の箱の中心(世界の µm)
        int64_t apertureUm = 0;             // 口の半径
        int64_t frontRadiusUm = 0;          // 日の側の球面の半径
        int64_t backRadiusUm = 0;
        int64_t thicknessUm = 0;         // 真ん中の厚さ(2 つの球面の膨らみの和以上)
        uint32_t axis = 1;               // 光の軸(世界の軸)
        int32_t facing = 1;              // 日の側 = facing × 軸
        uint32_t species = 0;            // 物質(光の値・屈折の値を読む)
        uint64_t micromolesPerCell = 0;  // 0.5 m のセルを満たした時の量(µmol。密度。吸い込みの τ を出す)
        int32_t lensLevel = 9;           // 物の細かさ: 光線の間隔 = 0.5 m ÷ 2^lensLevel(光線の細かさ以上)
    };

    // 当たった面の升の鍵: [SunLensHitKey(枠・番号・入った軸), 升の U, 升の V](升は LN_SPOT_UM 角。世界の µm の格子)
    using SunLensSpot = std::array<int64_t, 3>;

    struct SunLensResult {
        multires::SunParams params{};
        std::vector<uint64_t> columnWords;  // 太陽の影の柱(sun_column.h の並び。光線の細かさの単位)
        std::vector<SunCaughtRay> caught;   // レンズに入った量(影の柱の光線ごと)
        std::vector<uint64_t>
            lensWords;  // 世界のセルの吸った・返した(SunCellWord の並び。レンズの光線の単位 = 上の単位 ÷ 2^16)
        std::map<SunLensSpot, uint64_t> hits;  // 升ごとのレンズの光の量(レンズの光線の単位)

        // --- 帳簿(レンズの光線の単位。[0] 短い帯・[1] 長い帯)---
        std::array<uint64_t, 2> lensEntered{};  // = Σ 取られた × 2^16
        std::array<uint64_t, 2> lensAbsorbed{};
        std::array<uint64_t, 2> lensReflected{};
        std::array<uint64_t, 2> worldAbsorbed{};
        std::array<uint64_t, 2> worldReflected{};
        std::array<uint64_t, 2> escaped{};
        uint64_t lensRays = 0;        // 走らせたレンズの光線の数(費用。日が当たらなければ 0)
        uint32_t unfinishedRays = 0;  // LN_MAX_STEPS で止めた光線
    };

    [[nodiscard]] constexpr uint64_t SunLensHitKey(uint32_t slot, uint32_t index, uint32_t axis) {
        return (((uint64_t{slot} * multires::MR_BLOCK_CELLS) + index) * 3) + axis;
    }

    // レンズの物の値(形を確かめ、物質の光の値・屈折の値と量から τ を出す)
    [[nodiscard]] std::expected<multires::LnLens, std::string> MakeSunLens(const BakedReactionTable& table,
                                                                           const SunLensShape& shape);

    // 太陽の影の柱 + レンズ。reverse ならレンズの光線を逆の順に(結果は順によらない: R3 の確かめ用)
    [[nodiscard]] std::expected<SunLensResult, std::string> FocusSunLight(const MultiresNest& nest,
                                                                          const BakedReactionTable& table,
                                                                          const SunLight& sun, int32_t rayLevel,
                                                                          const SunLensShape& shape,
                                                                          bool reverse = false);

    // セルの面に当たる熱の流れの強さ(W/m²。帯の和)= 日の光そのもの(セルの面の平均)+ レンズの光(升の量 ÷ 升の面の一番大きい所)。
    // 23 §6 の「当たる熱の流れの強さ」の導かれる値の試作(入れるのは放射だけ。ADR-0383 仮 1)
    [[nodiscard]] uint64_t SunLensFluxWatts(const MultiresNest& nest, const SunLensResult& result, uint32_t slot,
                                            uint32_t index, uint32_t axis);

    // 当たった面のうち強さが一番大きい所(鍵の小さい方を先に。無ければ強さ 0)
    struct SunLensPeak {
        uint64_t fluxWatts = 0;
        uint64_t key = 0;
    };

    [[nodiscard]] SunLensPeak FindSunLensPeak(const MultiresNest& nest, const SunLensResult& result);

}  // namespace bicameral::sim
