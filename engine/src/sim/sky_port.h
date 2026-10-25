// sky_port.h — 空との交換の口: 世界の中の光(RC)が参照の温度との差だけを運ぶ時(ADR-0461)の、参照の分の空との出入り
// (docs/design/23-radiation.md §3.2・§3.3。T-1145・ADR-0474)。
//
// なぜ要るか: 参照の温度との差だけを運ぶ RC は、世界の箱の外を「参照の温度の物」として扱う(参照の温度の所は何も放たない)。
// 横と下の外(箱の外へ続く地面)はそれでよいが、上の外(空)は参照の温度ではない(夜の晴れた空はずっと冷たく、昼は散らばった光が来る)。
// その差 = 空から来る光 − 参照の黒体の光 を、上の境目の口として入れる。
//
// データの流れ:
//   天候の層(空から水平な面へ来る強さ: 帯ごとの mW/m²。今は呼ぶ側が渡す)+ 参照の温度(RC と同じ値)+ 空の向きの組(決めた数の向き × 水平な面への取り分)
//   → PlanSkyPort: 帯ごとの差(空 − 参照の黒体。RcBlackIrradianceMilliwatts)を、向きごとに「光に垂直な面の強さ」へ直した太陽の掃き(SunLight)の列。
//     差が正の帯は正の掃き、負の帯は負の掃き(大きさで掃き、吸った量を引く)に分ける
//   → SweepSkyPort: 掃きを全部、太陽の影の柱の本体(SweepSunColumn。GPU は GpuSunColumn で同じ語)で走らせ、正と負の語を別に足す
//   → 出力: セルごとの「吸った(正 − 負をセルのエネルギーへ)」「返した(正は RC の正の側、負は負の側の持ち越しへ。つなぐのは使う側)」と帯ごとの帳簿。
// 約束:
//   - 空の光 = 参照の黒体なら掃きは 0 本(口は何もしない。RC の差だけの形の「空も参照の温度」と同じ)。
//   - 保存: 掃きごとに 入った = 吸った + 返した + 外へ(太陽の影の柱と同じ。ビット単位)。足すのは整数の足し算だけ(順によらない)。
//   - 既定の向きの組は 4 つの斜め (±1, −1, ±1)(取り分 1/4 ずつ): 開けた水平な面は差のちょうど 1 倍、開けた縦の壁はちょうど 1/2 倍を受ける
//     (一様な空の見える割合と同じ)。穴の底のような所は 4 つの向きの筋になる(R-R1 の退く道の「決めた数の向きで掃く」と同じ性質。未確認の幅)。
// 出力はセルを書き換えない。浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "sim/multires_nest.h"
#include "sim/reaction_table.h"
#include "sim/sun_column.h"

namespace bicameral::sim {

    // 空の向き 1 つ(光の進む向き。下向き = y が負)と、水平な面へ来る強さのうちこの向きの取り分(Q16。組の和 = 2^16)
    struct SkyDirection {
        std::array<int32_t, 3> direction{};
        uint32_t shareQ16 = 0;
    };

    // 既定の向きの組(4 つの斜め。上の約束)
    [[nodiscard]] std::vector<SkyDirection> DefaultSkyDirections();

    struct SkyPortParams {
        uint32_t referenceMk = 0;         // RC の CascadeParams::referenceMk と同じ値
        uint32_t skyShortMilliwatts = 0;  // 空から水平な面へ来る短い帯(散らばった日の光)の mW/m²
        uint32_t skyLongMilliwatts = 0;   // 空から水平な面へ来る長い帯(空の温度で決まる赤外)の mW/m²
        int32_t rayLevel = 0;             // 掃きの光線の細かさ(太陽の影の柱と同じ)
        std::vector<SkyDirection> directions = DefaultSkyDirections();
    };

    // 掃き 1 本(negative なら吸った量をセルから引く側)
    struct SkyPortSweep {
        SunLight light;
        bool negative = false;
    };

    // 口の掃きの列を作る(向きが下を向いていない・取り分の和が 2^16 でない・強さが 32bit を超えるなら誤り)
    [[nodiscard]] std::expected<std::vector<SkyPortSweep>, std::string> PlanSkyPort(const SkyPortParams& params);

    // 空から来る長い帯を、温度 skyMk の黒い空として作る(試験と、天候の層がまだ無い間の値)
    [[nodiscard]] uint32_t SkyBlackLongMilliwatts(uint32_t skyMk);

    struct SkyPortResult {
        // [0] = 正の掃きの和・[1] = 負の掃きの和(どちらも SweepSunColumn の語の並び。番地は SunLedgerWord・SunCellWord)
        std::array<std::vector<uint64_t>, 2> words;
        uint32_t sweeps = 0;
    };

    // 口の掃きを全部走らせる(reverse なら掃きの順も光線の順も逆。結果は順によらない)
    [[nodiscard]] std::expected<SkyPortResult, std::string> SweepSkyPort(const MultiresNest& nest,
                                                                         const BakedReactionTable& table,
                                                                         const SkyPortParams& params,
                                                                         bool reverse = false);

    // セル 1 つ・帯 1 つの正味の吸った量(正 − 負。rayLevel のエネルギーの単位 8^−rayLevel mJ)
    [[nodiscard]] int64_t SkyPortAbsorbed(const SkyPortResult& result, uint32_t slot, uint32_t index, uint32_t band);

}  // namespace bicameral::sim
