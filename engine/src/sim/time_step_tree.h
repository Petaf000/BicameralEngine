// time_step_tree.h — 研究 R-T2 の 2(T-0950。ADR-0410・ADR-0423・20 §14.2)。試し 1 の刻みの長さの選び方(ブロックの歩 2^e・
// 伸ばすのは 1 歩に 1 段まで・隣の差 1 段・境は短い側が持ち主・後ろ向きの Euler)を、多重解像度の木のブロックの上の熱(伝導)へ。
// 捨てる試作(CPU。CLAUDE.md §1-10 の調査)。C-0・C-1 の部品は変えずに使う:
//   セル = MultiresNest のセル(MultiresCellStore の口 LoadNestCell / StoreNestCell)。エネルギー = 熱(mJ。そのレベルの単位 = 8^-k)、
//          成分 TIME_TREE_REACTANT = 反応物 A・TIME_TREE_PRODUCT = 生まれた物 P(µmol。そのレベルの単位)
//   解き = C-0 の SolveEquilibrium(長さ N を C・g・源に入れる。外の隣と地面は値を固定した面。部品は時間を知らない)
//   導かれる値 = 解いた温度を登録した導かれる値の欄(LoadNestField)に。眠った所(一様のブロック)には書かない(T-0854 の申し送り)
//   歩の記録(ブロックの e・区間・セルの歩の変化・セルと面の記録)= この試作の仮の口 TimeTreeSteps(C-1 の型は変えない。
//   要る口は docs/tickets/T-0950)
// 面は木の隣(MrFindFaceNeighbor)から 1 回だけ作る(同じレベル = − 側のセルから・違うレベル = 細かい側から)。面の流れは細かい側の単位で、
//   粗い側へは 8^差 で割った値を決まった乱数で丸めて送る(広がる家族の丸め D-552。細かい側は送った分 × 8^差 を失う = 保存は厳密)。
// 刻みの流れは time_step_lab(1 本の列)と同じ: 出来事(歩の終わり)の刻みだけを回す・歩の終わりに歩の変化を真実へ・
//   長すぎる隣の歩は打ち切る。浮動小数点は使わない(04 §4)。
#pragma once

#include <cstdint>
#include <vector>

#include "common/fixed.hlsli"
#include "sim/multires_nest.h"
#include "sim/time_step_lab.h"

namespace bicameral::sim {

    inline constexpr uint32_t TIME_TREE_REACTANT = 1;  // 成分の物質 ID(A)
    inline constexpr uint32_t TIME_TREE_PRODUCT = 2;   // 成分の物質 ID(P)

    struct TimeTreeSetup {
        // --- 熱(どれもレベルによらない数。面の係数だけレベル k で × 4^k)---
        uint64_t capacityQ32 = 0;          // セルの熱容量(2^-32 × そのレベルの単位 / mK)
        fx::FxU128 conductLevel0Q32{};     // レベル 0 の面の係数(2^-32 × 単位 / mK / 刻み)
        uint64_t groundQ32 = 0;            // 地面(空気)への係数(2^-32 × 単位 / mK / 刻み)
        int64_t groundTemperatureQ16 = 0;  // 地面の温度(mK × 2^16)

        // --- 反応 A → P ---
        uint8_t fuel = TIME_STEP_FUEL_NONE;  // TIME_STEP_FUEL_*(どのセルも同じ)
        uint64_t rateColdQ32 = 0;
        uint64_t rateHotQ32 = 0;
        int64_t ignitionTemperatureQ16 = 0;
        int64_t heatPerAmount = 0;  // 1 µmol 反応した時に出る熱(mJ)

        // --- 刻みの長さを選ぶ(time_step_lab と同じ)・解き ---
        TimeStepRule rule = TimeStepRule::Change;
        int32_t fixedExponent = 0;
        int32_t maxExponent = 14;
        uint64_t budgetQ16 = 1ull << 16;  // 1 歩の変化の許す大きさ(mK × 2^16)
        uint32_t cycles = 4;              // C-0 の V サイクルの回数(法則ごとの定数)
        bool galerkin = false;            // C-0 の粗い段の面の係数を子の和のまま(EquilibriumOptions::galerkinCoarse)
    };

    // 面 1 つ(a = 細かい側か、同じレベルなら − 側。b = 相手)。係数は a の単位 / 刻み
    struct TimeTreeFace {
        uint32_t a = 0;  // セルの番号(枠 × MR_BLOCK_CELLS + セル)
        uint32_t b = 0;
        uint32_t blockA = 0;
        uint32_t blockB = 0;
        uint32_t gap = 0;
        fx::FxU128 coefficient{};
    };

    // 木の形から 1 回だけ作る(木を変えたら作り直す。眠る・起こす〔頁を畳む・広げる〕は形を変えない)
    struct TimeTreeLayout {
        std::vector<uint32_t> blocks;  // 刻む世界の本物のブロックの枠(枠の順)
        std::vector<uint8_t> stepped;  // セルごと: 刻むセル(覆われていない)
        std::vector<TimeTreeFace> faces;
        std::vector<std::vector<uint32_t>> blockFaces;  // 枠ごと: 触れる面
        std::vector<std::vector<uint32_t>> neighbors;   // 枠ごと: 面でつながる枠(枠の順)
        int32_t finestLevel = 0;
    };

    struct TimeTreeCellRecord {
        uint64_t start = 0;
        uint64_t ticks = 0;
        int64_t reacted = 0;  // A → P(そのレベルの単位)
        int64_t ground = 0;   // 地面へ(mJ。そのレベルの単位)
    };

    struct TimeTreeFaceRecord {
        uint64_t start = 0;
        uint64_t ticks = 0;
        int64_t flow = 0;   // a → b(a の単位)
        int64_t moved = 0;  // b が受けた値(b の単位。a は moved × 8^差 を失う)
    };

    // 歩の記録(仮の口。C-1 の欄ではなくこの試作が持つ。セーブに入れる = Q146 の一旦の決定 (A))
    struct TimeTreeSteps {
        uint64_t tick = 0;
        bool processed = false;

        // --- 枠ごと ---
        std::vector<int32_t> exponent;
        std::vector<int32_t> desired;
        std::vector<uint8_t> open;
        std::vector<uint8_t> touched;
        std::vector<uint64_t> start;
        std::vector<uint64_t> end;

        // --- セルごと(枠 × MR_BLOCK_CELLS + セル)。歩の変化(歩の終わりに真実へ)と記録 ---
        std::vector<int64_t> deltaEnergy;
        std::vector<int64_t> deltaReactant;  // P は −これ
        std::vector<TimeTreeCellRecord> cellRecords;
        std::vector<TimeTreeFaceRecord> faceRecords;  // layout.faces の順

        // --- 帳簿(最も細かいレベルの単位)---
        int64_t outsideHeat = 0;  // 入れた熱 − 地面へ
        int64_t reactionHeat = 0;
        int64_t groundHeat = 0;  // 地面へ逃げた分(比べる値)

        // --- 測り ---
        uint64_t cellSteps = 0;      // セル × 解いた回数
        uint64_t solves = 0;         // 部品を呼んだ回数
        uint64_t worstResidual = 0;  // 部品の残差の計器の最大(式の単位。解けきらなかった所)
        uint64_t events = 0;
        uint64_t truncations = 0;
        int32_t widestNeighborGap = 0;
        uint32_t negativeValues = 0;

        // --- 眠る・起こす(T-0854 の申し送りを確かめる)。sleepMean は眠った時の導かれる値の覆われていないセルの平均 ---
        uint64_t sleeps = 0;
        uint64_t wakes = 0;
        std::vector<int64_t> sleepMean;  // 枠ごと
    };

    struct TimeTreeWorld {
        MultiresNest nest;
        uint32_t temperatureField = 0;  // 導かれる値の欄(解いた温度。mK × 2^16)
        TimeTreeLayout layout;
        TimeTreeSteps steps;
    };

    // 木を作り終えた nest(導かれる値の欄 temperatureField を登録済み)から世界を作る。頁を持たない枠は広げる(導かれる値を書くため)
    TimeTreeWorld MakeTimeTreeWorld(const TimeTreeSetup& setup, MultiresNest nest, uint32_t temperatureField);

    // ticks 刻み進める(早送り = 大きい ticks。規則は同じ。分け方によらずビット一致)
    void StepTimeTree(const TimeTreeSetup& setup, TimeTreeWorld& world, uint64_t ticks);

    // 今の刻みにセルへ熱を入れる(火を付ける。帳簿の外への出入り)。その枠の歩を打ち切り、次の歩を e = 0 から
    void InjectTimeTreeHeat(const TimeTreeSetup& setup, TimeTreeWorld& world, uint32_t slot, uint32_t index,
                            int64_t heat);

    // 状態の要約(木の状態 HashWholeNest + 歩の記録)
    uint64_t HashTimeTree(const TimeTreeWorld& world);

    // 保存の検査: 最も細かいレベルの単位で Σ(真実 + 歩の変化)− 帳簿
    struct TimeTreeTotals {
        int64_t heat = 0;
        int64_t reactant = 0;  // A + P
        bool operator==(const TimeTreeTotals&) const = default;
    };
    TimeTreeTotals ConservedTimeTree(const TimeTreeWorld& world);

    // 歩の変化も足したセルの熱・A(比べる時に使う。そのレベルの単位)
    int64_t TimeTreeSettledEnergy(const TimeTreeWorld& world, uint32_t slot, uint32_t index);
    int64_t TimeTreeSettledReactant(const TimeTreeWorld& world, uint32_t slot, uint32_t index);

    // 温度(mK × 2^16)
    int64_t TimeTreeTemperatureQ16(const TimeTreeSetup& setup, int64_t energy);

    // 歩の記録をセーブに入れる時の大きさ(この刻みで生きている記録だけ。最小 = 枠の見出し 11 B + セルの記録 16 B + 面の記録 24 B。
    // 持ったまま = + セルの歩の変化 16 B/セル)
    struct TimeTreeRecordBytes {
        uint64_t minimal = 0;
        uint64_t held = 0;
        uint64_t cells = 0;  // 刻むセルの数
    };
    TimeTreeRecordBytes MeasureTimeTreeRecords(const TimeTreeWorld& world);

}  // namespace bicameral::sim
