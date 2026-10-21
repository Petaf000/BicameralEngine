// sun_column.h — 太陽の影の柱の CPU リファレンス(docs/design/23-radiation.md §3.1・ADR-0396。T-0910)。
// 本体は shaders/common/sun_column.hlsli の SunWalkRay(GPU の sim/gpu_sun_column と同じ関数)。ここは木・セル・物質の光の値を読む道具と、
// 光線を全部走らせて出力の語(SunColumnResult::words)に足すだけ。
//
// データの流れ:
//   太陽(SunLight: 光の進む向き・帯ごとの強さ。天候の層・環境の値から呼ぶ側が渡す)+ 世界の木(MultiresNest。世界の箱 = nest.edges.world)
//   + 物質の光の値(BakedReactionTable::speciesLight)→ MakeSunParams → SweepSunColumn
//   → 出力の語: 帯ごとの帳簿(入った・吸った・返した・外へ)+ 世界の枠のセルごとの「吸った・返した」(光線の細かさのエネルギーの単位)。
// 出力はセルを書き換えない(当たる面へ熱として渡す口。足し方・粗い葉への換算は使う側。チケット T-0910 の申し送り)。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "common/sun_column.hlsli"
#include "sim/multires_nest.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 太陽(1 刻みの値)。向きは光の進む向き(太陽から地面へ。世界の軸 x・y・z。上は +y)。強さは光に垂直な面の mW/m²
    struct SunLight {
        std::array<int32_t, 3> direction = {0, -1, 0};
        uint32_t irradianceShortMilliwatts = 0;  // 短い帯(見える光)
        uint32_t irradianceLongMilliwatts = 0;   // 長い帯(赤外)
    };

    struct SunColumnResult {
        multires::SunParams params{};
        // [帳簿 SUN_LEDGER_WORDS][世界の枠 × 512 × SUN_CELL_WORDS](番地は SunLedgerWord・SunCellWord)
        std::vector<uint64_t> words;
    };

    // 掃き方を決める。rayLevel = 光線の細かさ(そのレベルのセルの間隔の格子・そのレベルの単位のエネルギー)。
    // 世界の箱(nest.edges.world)が空・向きが 0 か大きすぎる・rayLevel が根より粗いか木の最も細かい本物のブロックより粗い・量が大きすぎるなら誤り
    [[nodiscard]] std::expected<multires::SunParams, std::string> MakeSunParams(const MultiresNest& nest,
                                                                                const SunLight& sun, int32_t rayLevel);

    // 出力の語の数(世界の枠の数から)
    [[nodiscard]] size_t SunColumnWordCount(const MultiresNest& nest);

    // 光線を全部走らせる(reverse なら逆の順。結果は順によらない: R3 の確かめ用)
    [[nodiscard]] std::vector<uint64_t> SweepSunColumn(const MultiresNest& nest, const BakedReactionTable& table,
                                                       const multires::SunParams& params, bool reverse = false);

    // MakeSunParams + SweepSunColumn
    [[nodiscard]] std::expected<SunColumnResult, std::string> SweepSunColumn(const MultiresNest& nest,
                                                                             const BakedReactionTable& table,
                                                                             const SunLight& sun, int32_t rayLevel);

}  // namespace bicameral::sim
