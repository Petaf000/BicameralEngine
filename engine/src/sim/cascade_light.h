// cascade_light.h — 世界の中の光: 距離ごとの段(Radiance Cascades の型)の CPU リファレンス
// (docs/design/23-radiation.md §3.3・§3.4・ADR-0413。T-0958 R-R1)。光線 1 本の本体は shaders/common/cascade_light.hlsli の RcWalkRay。
//
// データの流れ:
//   世界の木(MultiresNest。今はレベル 0 = 0.5 m の根だけ)+ 物質の光の値(BakedReactionTable::speciesLight)
//   → MakeCascadeGrid(セルごとの温度と帯ごとの τ・照り返しの密な写し)
//   → StepCascadeLight(1 刻み): 段 0 = 各セルの 6 面が熱(σT⁴ × 帯の割合 × α(1 − ρ))と前の刻みに返された光を Lambert で升目へ分けて放つ
//     → 区間の終わりに残った量を、量が出た軸ごとに段 k + 1 の袋(ブロック × 升目)へ(RcPassToNextLevel)→ 袋の溜めが選んだ出どころから
//     段 k + 1 の光線(袋ごとに raysPerBag 本)→ … → 最後の段は世界の端まで。升目の中のずらしは刻みで巡る(T-0972 の 1b + 2。T-0959)
//   → 出力: セルごと・帯ごとの「放った(熱で。セルから引く量)」「吸った(セルへ足す量)」、次の刻みへ持ち越す照り返し(CascadeCarry)、帯ごとの帳簿。
// 出力はセルを書き換えない。吸った・放った分をセルのエネルギーへ当てる段・帳簿の理由の行・刻みの順(S42 の 7)は土台の仕事(T-0958 の申し送り)。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "common/cascade_light.hlsli"
#include "sim/multires_nest.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 段の形(ADR-0413)。既定は試作の見積もり(T-0877: n0 = 4・5 段・区間 1)
    struct CascadeParams {
        uint32_t baseTexels = 4;      // n0: 段 0 の向きの升目(立方体の 1 面の 1 辺)。段 k は n0 2^k
        uint32_t levels = 5;          // 段の数(最後の段は世界の端まで)
        uint32_t intervalBlocks = 1;  // 区間の長さ(段 k + 1 のブロックの数)
        uint64_t seed = 0;            // 世界の種(升目の中のずらし・余りの行き先。R6)
        // これより冷たいセルは熱で放たない(mK。0 = 全部放つ)。眠り(23 §4)を測るための口。返された光は温度によらず放つ
        uint32_t quietBelowMk = 0;
        // 段 k ≥ 1 の袋 1 つから出す光線の数(1 = 今の形。量を等分し、升目の中のずらしの巡りの続きを使う。T-0959 の費用の比べ)
        uint32_t raysPerBag = 1;
        // 放つ面を最上段のブロックごとに何組に分けて刻みで回すか(1 = 毎刻み全部。奇数。T-1010・ADR-0444)。
        // 自分の組でない刻みの光は持ち越しの表で待つ。thermalInSlots なら熱で放つ光も待つ(でなければ照り返しだけ)
        uint32_t emitSlots = 1;
        bool thermalInSlots = false;
        // 参照の温度との差だけを運ぶ(T-1102・ADR-0461。0 = 今のまま)。正の側(自分 − 参照)と負の側(参照 − 自分。negativePart)を
        // 別の刻みの組で運び、吸った・放ったの差を取る(StepCascadeLightRelative)。同じ温度の所は放たないので、坂を下るは構造で 0。
        // 0 でない時は quietBelowMk を使わない(冷たい物も負の側で放つ)。参照の温度 2^31 mK 未満
        uint32_t referenceMk = 0;
        bool negativePart = false;
    };

    // 段 0(レベル 0 のセル)の密な写し。添字 = IndexOf
    struct CascadeGrid {
        std::array<int64_t, 3> low{};
        std::array<int64_t, 3> size{};
        std::vector<reaction::RcCellLight> lights;
        std::vector<uint32_t> temperatureMk;

        [[nodiscard]] size_t CellCount() const { return lights.size(); }

        [[nodiscard]] bool Contains(int64_t x, int64_t y, int64_t z) const {
            return x >= low[0] && y >= low[1] && z >= low[2] && x < low[0] + size[0] && y < low[1] + size[1] &&
                   z < low[2] + size[2];
        }

        [[nodiscard]] size_t IndexOf(int64_t x, int64_t y, int64_t z) const {
            return static_cast<size_t>((((z - low[2]) * size[1]) + (y - low[1])) * size[0] + (x - low[0]));
        }
    };

    // 照り返しの持ち越し(刻みをまたぐ状態。23 §3.4)。0 でない (セル・面・帯) だけを、セル・面・帯の順に持つ。
    // 一旦の決定(司令塔): セーブとハッシュに入れる(HashCascadeCarry・CascadeCarrySaveBytes)
    struct CascadeCarryEntry {
        uint32_t cell = 0;  // CascadeGrid::IndexOf
        uint8_t face = 0;   // 光が入った面(0〜5。軸 × 2 + 負の面なら 1)
        uint8_t band = 0;   // LT_BAND_*
        uint64_t amount = 0;

        bool operator==(const CascadeCarryEntry&) const = default;
    };

    struct CascadeCarry {
        std::vector<CascadeCarryEntry> entries;

        bool operator==(const CascadeCarry&) const = default;
    };

    // 帯 1 つの帳簿(2^−16 mJ)。emitted + carriedIn = absorbed + reflected + escaped + held(ビット単位)。
    // 次の刻みへの持ち越しの和 = reflected + held + kept
    struct CascadeBandLedger {
        uint64_t emitted = 0;    // 熱で放った(セルから引く量)
        uint64_t carriedIn = 0;  // 前の刻みに返された光を放った
        uint64_t absorbed = 0;   // 吸った(セルへ足す量)
        uint64_t reflected = 0;  // 返した(次の刻みへ持ち越す)
        uint64_t escaped = 0;    // 世界の外へ
        uint64_t held = 0;       // 熱で放ったが、組の刻みまで持ち越しの表で待たせた(T-1010。emitted の内)
        uint64_t kept = 0;       // 持ち越しの表で、組の刻みでないので次の刻みへそのまま送った(carriedIn の外)

        bool operator==(const CascadeBandLedger&) const = default;
    };

    struct CascadeTickResult {
        // --- セルごと([セル × LT_BANDS])---
        std::vector<uint64_t> emitted;   // 熱で放った量(セルから引く)
        std::vector<uint64_t> absorbed;  // 吸った量(セルへ足す)

        // --- 次の刻みへ・帳簿 ---
        CascadeCarry carry;
        std::array<CascadeBandLedger, reaction::LT_BANDS> ledger{};

        // --- 費用の目安 ---
        uint64_t rays = 0;   // 歩いた光線の本数(段の全部)
        uint64_t steps = 0;  // 光線が通ったセルの数の和
        std::array<uint64_t, 8> raysPerLevel{};
    };

    // 世界の木 → 段 0 の写し。今はレベル 0 の根だけの木(細かい葉・粗い根を引く道具は T-0972)
    [[nodiscard]] std::expected<CascadeGrid, std::string> MakeCascadeGrid(const MultiresNest& nest,
                                                                          const BakedReactionTable& table);

    // 世界の箱を確かめる(1 辺 127 セルまで。溜めの出どころを 15bit × 3 に詰める。ADR-0432。細かい葉・粗い根を読む形は T-0973)
    [[nodiscard]] std::expected<void, std::string> CheckCascadeGrid(const CascadeGrid& grid);

    // 段の形を確かめる(升目は 1 辺 2^10 まで・段は 1〜8・区間は 1 以上・袋の光線は 1〜64)
    [[nodiscard]] std::expected<void, std::string> CheckCascadeParams(const CascadeParams& params);

    // 1 刻み。carry = 前の刻みに返された光。reverse なら放つセル・袋を逆の順に回す(結果は順によらない: R3 の確かめ用)
    [[nodiscard]] CascadeTickResult StepCascadeLight(const CascadeGrid& grid, const CascadeCarry& carry,
                                                     const CascadeParams& params, uint64_t tick, bool reverse = false);

    // 参照の温度との差を運ぶ 1 刻み(T-1102): 正の側と負の側の 2 つの結果。セルの正味 = (正.吸った − 正.放った) − (負.吸った − 負.放った)。
    // 持ち越しも側ごと(carry[0] = 正・carry[1] = 負)。params.referenceMk が 0 でないこと
    struct CascadeRelativeResult {
        std::array<CascadeTickResult, 2> sides;
    };

    [[nodiscard]] CascadeRelativeResult StepCascadeLightRelative(const CascadeGrid& grid,
                                                                 const std::array<CascadeCarry, 2>& carry,
                                                                 const CascadeParams& params, uint64_t tick,
                                                                 bool reverse = false);

    // 持ち越しのハッシュ(セーブとハッシュに入れる。エントリの順 = セル・面・帯)
    [[nodiscard]] uint64_t HashCascadeCarry(const CascadeCarry& carry);

    // セーブの形の大きさ(案): セルごとに 番号 4 B + (面 × 帯)の印 2 B + 0 でない値ごとに 4 B(1 刻みに 1 面へ返る量は 2^32 未満)
    [[nodiscard]] uint64_t CascadeCarrySaveBytes(const CascadeCarry& carry);

}  // namespace bicameral::sim
