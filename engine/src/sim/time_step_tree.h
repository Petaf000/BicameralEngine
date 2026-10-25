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

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "common/fixed.hlsli"
#include "sim/equilibrium.h"
#include "sim/multires_nest.h"
#include "sim/time_step_lab.h"

namespace bicameral::sim {

    inline constexpr uint32_t TIME_TREE_REACTANT = 1;  // 成分の物質 ID(A)
    inline constexpr uint32_t TIME_TREE_PRODUCT = 2;   // 成分の物質 ID(P)

    // 刻みの長さの選び方(T-0985。研究 R-T2 の打ち切り条件の 2 通り)
    enum class TimeTreeSelect : uint8_t {
        Previous,  // 1 通り目(T-0950): 前の歩の変化だけで選ぶ
        Retake,    // 2 通り目(T-0985): 前の歩で選び、解いた歩の変化が予算を超えた枠は縮めて取り直す(真実へ書く前に)
        // 退く道の 1 本目(T-1013・ADR-0437。寝ている間だけ使う。起きている間は毎刻み = Fixed): e は 0 か上限だけ。
        // 上限の倍数の刻みごとに、全部の枠を上限の長さで試しに解き、歩の変化が予算の内の枠(状態がほぼ一定 = 川など)だけ一気に進める。
        // ほかは毎刻み。毎刻みの枠の隣は毎刻み(前線が長い歩の枠に入らないための 1 枠の緩衝)。隣の差 1 段の決まりは使わない
        Jump,
    };

    // 列の解きを見る口(T-1111。GPU の試験が同じ系を GPU で解き直す)。系は δ = 0 から V を cycles 回で解く(SolveSpan と同じ)。
    // 残差で止める形(residualStop)の時は呼ばない
    struct TimeTreeSpanSolve {
        const EquilibriumSystem& system;
        int32_t exponent = 0;  // 列の刻みの長さ e(2^e 刻み)
        uint64_t start = 0;    // 列の歩の初めの刻み
        uint32_t cycles = 0;   // V の回数(e ごとの表。TimeTreeSetup の cycles・cyclesAtZero・shortExponent)
    };

    using TimeTreeSpanObserver = std::function<void(const TimeTreeSpanSolve&)>;

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
        // 1 歩の変化の許す大きさ(mK × 2^16)。既定 0.1 K(D-642。T-1063)
        uint64_t budgetQ16 = uint64_t{100} << 16;
        // C-0 の V サイクルの回数(法則ごとの定数)。長い刻み(e > shortExponent)の列。静かな夜で V 2 は V 4 と同じ精度の内(T-1108)
        uint32_t cycles = 2;
        // 短い刻み(e ≤ shortExponent)の列の V の回数(0 = cycles と同じ)。短い刻みの系は 1 回の V で解け切る
        // (T-1090。火事で V 4 とビット一致。長い刻みは解け切らない)
        uint32_t cyclesAtZero = 1;
        // 2^8 刻み ≈ 4.3 秒まで(静かな夜で e ≤ 8 は V 4 と同じ差・e ≤ 10 から増える。T-1090)
        int32_t shortExponent = 8;
        // 残差で止める(T-1108。0 = 決まった回数): 列ごとに V を 1 回ずつ、残差の計器の最大(式の単位)がこれ以下になるまで
        // (上限は上の回数)
        uint64_t residualStop = 0;
        bool galerkin = false;  // C-0 の粗い段の面の係数を子の和のまま(EquilibriumOptions::galerkinCoarse)
        TimeTreeSelect select = TimeTreeSelect::Previous;
        // 同じレベルで 2^これ ブロック角の枠は同じ e に揃える(細かさを同じ物の長さのブロックで比べる。0 = ブロックごと)
        uint32_t groupShift = 0;
        // 刻みを選ぶ単位の物の長さ(D-640。一旦 4 m): レベル unitLevel のブロックの長さ(0 = 4 m)。それより細かいレベルは
        // 同じ長さの組に揃える(2 m のブロックなら 2³ 枠)。粗いレベル(8 m のブロック)はブロックのまま(短くする形は T-1089)。
        // false なら groupShift だけ(T-0985・T-1074 の比べ)
        bool unitByLength = true;
        int32_t unitLevel = 0;
        // 前線の緩衝(T-1014 の見立ての確かめ): 望みの e が 0 の枠(歩の変化が予算の 1/4 を超えた・触られた)の隣も e = 0 に。
        // 隣の差 1 段の決まりだけだと、前線の隣は予算と関係なく e = 1 になれる
        bool frontBuffer = false;
        // 前線の緩衝を揃える単位(groupShift の枠の組)の数で(T-1074。0 = 上の frontBuffer の形): 動いている単位(どれかの枠の
        // 望みの e が 0)から面の隣をたどってこの数の単位までを e = 0 に。単位の物の長さ × これ が緩衝の幅
        uint32_t frontBufferUnits = 0;

        // --- 見る口(結果に効かない)---
        const TimeTreeSpanObserver* spanObserver = nullptr;  // 列の解き(T-1111)
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
        std::vector<std::vector<uint32_t>> mates;       // 枠ごと: 同じ e に揃える枠(groupShift。自分は入れない)
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
        uint64_t vCycles = 0;        // 回した V の数(T-1108)
        uint64_t worstResidual = 0;  // 部品の残差の計器の最大(式の単位。解けきらなかった所)
        uint64_t events = 0;
        uint64_t truncations = 0;
        uint64_t retakes = 0;          // 取り直した枠の数(2 通り目)
        uint64_t zeroCellSteps = 0;    // e = 0 の列で解いたセル(T-1074)
        uint64_t forcedCellSteps = 0;  // そのうち自分の望みの e は 0 でない枠(緩衝・隣の差・揃える相手で毎刻みにされた)

        // --- 段の重みを使い回せるか(T-1090。測りだけ。結果にも要約にも効かない)---
        // e = 0 の解きのうち、枠の組が前の e = 0 の解きと違った回(段の形が変わる = 段の重みを作り直す)と、組は同じでも
        // どれかのセルの温度の帯(1 K・10 K・100 K ごと)が変わった回(伝導率が温度で変わる D-639 なら係数が変わる見込み)
        uint64_t zeroSolves = 0;
        uint64_t zeroReshaped = 0;
        std::array<uint64_t, 3> zeroRebanded{};
        std::vector<uint32_t> lastZeroSlots;
        std::array<std::vector<int64_t>, 3> cellBand;
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

    // --- 1 出来事の列を 1 本の系にまとめる(T-1141。GPU の早送りで起動の数と固定費を減らす)---
    // 同じ刻みに解く列どうしは面を持たない(列の外は値 0 を固定した面)ので、節を並べただけの系(塊の対角)は列ごとに解いたのと
    // ビット一致する。ただし部品の段の作り方は系全体で決まる(粗い段を足すかは「どれかの節の重み < 1/2」・縮約は座標の親)ので、
    // まとめてよいのは V の回数・段の数・節のレベルが同じで、縮約がブロック(8 セル角)の中で閉じる(粗い段 3 つまで)列だけ。
    // ほかは列ごとの系のまま(組を分ける)。CPU の試作の解き(列ごと)はこのまとめで変わらない(試験 event-merge で確かめる)
    struct TimeTreeEventSpan {
        EquilibriumSystem system;
        int32_t exponent = 0;
        uint32_t cycles = 0;  // V の回数(e ごとの表)
    };

    struct TimeTreeEventGroup {
        EquilibriumSystem system;  // 列の系を入力の順に並べた(節・面・固定した面・右辺の項の番号をずらす)
        uint32_t cycles = 0;
        uint32_t levels = 0;               // 部品の段の数
        std::vector<uint32_t> spans;       // まとめた列(入力の番号)
        std::vector<uint32_t> nodeStarts;  // 列ごとの節の始まり(spans の順)
    };

    // 1 出来事の列(SolveSpans の順)を組に分けてまとめる。組は最初の列の順
    std::vector<TimeTreeEventGroup> GroupTimeTreeEvent(std::span<const TimeTreeEventSpan> spans);

    // --- 早送りの待ちの見積もり(D-650・T-1111。ゲームはこれを読んで眠れる条件などを決める。法則は変えない)---
    // 1 出来事(その刻みに解く列を 1 本の系にまとめた形。T-0951)の GPU の費用の式。既定は RTX 3070 Ti・release の測り
    // (gpu_time_step_span_test --measure・記録の形は前の解きから。docs/perf.md の T-1111)。浮動小数点を使わないので ps の整数
    struct TimeTreeSolveCost {
        // 解くセルがこれ以下の出来事は V を 1 グループで回す(ImTail)
        uint32_t smallCells = 1024;
        uint64_t smallEventNs = 13300;  // 小さい出来事の段 0 + 段の重み(512 セル)
        uint64_t smallCycleNs = 26000;  // 小さい出来事の V 1 回
        // 大きい出来事: 固定費 + セルの数 × 傾き。ただし下限(8,192〜6.6 万セルはほぼこの値。測りで 0.06〜0.52 ms と揺れた:
        // 512 セルの系を 16 並べた 8,192 は 0.06 ms・広げた木の 8,192 は 0.26〜0.52 ms。原因は未確認 = T-1140)
        uint64_t eventNs = 120000;
        uint64_t eventFloorNs = 260000;
        uint64_t cellPs = 2530;       // 段 0 + 段の重みのセル 1 つ
        uint64_t cellCyclePs = 1940;  // V 1 回のセル 1 つ
    };

    // 1 出来事の GPU の待ち(ns)。cells = 解くセル、cellCycles = Σ セル × V の回数
    uint64_t TimeTreeEventNs(const TimeTreeSolveCost& cost, uint64_t cells, uint64_t cellCycles);

    struct TimeTreeForwardEstimate {
        uint64_t events = 0;      // 出来事(解く刻み)の数
        uint64_t cellSolves = 0;  // セル × 解く回数
        uint64_t cellCycles = 0;  // セル × V の回数(e ごとの表)
        uint64_t waitMicroseconds = 0;
    };

    // 今の枠ごとの刻みの長さ e が、この先 ticks 刻みの間そのまま続くと見なして数える(見積もり。結果にも状態にも効かない)。
    // 出来事は最も短い歩の終わりごと(長い歩の終わりはそれに重なり、同じ刻みの列は 1 本の系にまとめる)。
    // 前線は動くが毎刻みの範囲の大きさは保たれる見込み(T-1089)。眠る・起こす・前線の広がりは数に入れない
    TimeTreeForwardEstimate EstimateTimeTreeForward(const TimeTreeSetup& setup, const TimeTreeWorld& world,
                                                    uint64_t ticks, const TimeTreeSolveCost& cost);

}  // namespace bicameral::sim
