// time_step_lab.h — 研究 R-T2(刻みの長さを状態が変わる速さで選ぶ。ADR-0390・ADR-0410・20 §14.2。T-0856)の CPU の試作。
// 捨てる試作(CLAUDE.md §1-10 の調査)。C-0(釣り合う家族の部品)・C-1(欄の置き場)は使わず、1 本の列のセルで形を確かめる。
//
// 世界: 1 本の列のセル(断面は決まった広さ。セルの長さ dx。川の流れの向き = +x)。セルは 4 つの真実を持つ:
//   熱(mJ)・溶けた物 D・反応物 A・生まれた物 P(µmol)。熱と D は「運ぶ・広がる」の 1 本の式(量ごとの容量・伝導・流れ・地面)、
//   A → P は温度で 2 つの速さの決まった割合の反応(熱を出す)。外との出入り(流れ込み・流れ出し・地面・火を付ける)は
//   帳簿の「外への出入り」の列(20 §2.4・§14.2)。
// 刻み: ブロック(blockCells 個のセル)ごとに長さ 2^e 刻みの歩。歩の境目は 2^e の倍数の刻み(2 進の区間なので、隣どうしの歩は
//   入れ子か離れている)。歩の初めに、そのブロックの寄与(セルの源・持ち主の面の流れ)を全部計算して記録し、両側の「歩の変化」に
//   整数で足す(保存は厳密)。歩の終わりに真実へ足す(途中の真実は歩の初めの値)。
//   熱と D: 後ろ向きの Euler(同じ刻みに同じ e で始まる続きのセルを三重対角で解く。長い刻みでも安定で、一定に流れる所の
//   釣り合いを崩さない。本物では C-0 の部品に長さを C・g・源に入れて渡す形。T-0853)。面の流れは解いた値から作る。
//   A → P: 閉じた形(TsDecayedPart)。
//   長さの違うブロックの間の面は短い側が持ち主で、自分の歩ごとに長い側の歩の初めの値(真実)を読んで計算し、
//   長い側の歩の変化に溜める(Berger–Colella の時間の向きの版 = 面の帳簿)。長い側の解きは短い隣の今の値を境にする。
// 選び方: 歩を終えたブロックの変化(計器の精度の単位: 温度 1 mK・D は水の 100 万分の 1・A は referenceAmount の 100 万分の 1。
//   D-435)から次の e(time_step.hlsli の TsDesiredExponent)。隣のブロックとの差は 1 段まで: 長すぎる隣の歩はその刻みで
//   打ち切る(記録を経った刻みの割合に縮める。保存はそのまま)。
// 早送り: StepTimeStepLab に渡す刻みの数を増やすだけ。歩の終わりの刻みだけを回すので、分け方によらずビット一致で、
//   費用は歩の数で決まる。浮動小数点は使わない(04 §4)。
#pragma once

#include <cstdint>
#include <vector>

namespace bicameral::sim {

    enum class TimeStepRule : uint32_t {
        Fixed,     // e を変えない(短い刻みの答え合わせ・決まった長さ)
        Change,    // 試し 1(規則 A): 前の歩の変化の量(N に比例と見る)
        Estimate,  // 試し 2(規則 B): 前の歩の誤差の見積もり |半分 2 回 − 1 回|(N² に比例と見る)
    };

    inline constexpr uint32_t TIME_STEP_HEAT = 0;       // mJ(見積もりは温度 mK)
    inline constexpr uint32_t TIME_STEP_DISSOLVED = 1;  // µmol(見積もりは水の 100 万分の 1 = ppm)
    inline constexpr uint32_t TIME_STEP_REACTANT = 2;   // µmol
    inline constexpr uint32_t TIME_STEP_PRODUCT = 3;    // µmol
    inline constexpr uint32_t TIME_STEP_QUANTITIES = 4;
    inline constexpr uint32_t TIME_STEP_CARRIED = 2;  // 運ぶ・広がるの式で解く量(熱・D)

    inline constexpr uint8_t TIME_STEP_FUEL_NONE = 0;
    inline constexpr uint8_t TIME_STEP_FUEL_SLOW = 1;   // いつも rateColdQ32 で(発酵)
    inline constexpr uint8_t TIME_STEP_FUEL_BURNS = 2;  // 着火の温度以上で rateHotQ32(燃える)

    // 運ぶ・広がるの量 1 つ(熱か D)。見積もりの単位 y(熱 = mK・D = ppm)× 2^16 で解く。係数はどれも
    // 値 ÷ y の単位(熱 = mJ/mK・D = µmol/ppm)× 2^16(伝導・流れ・地面・流れ出しは 1 刻みあたり)
    struct TimeStepCarried {
        std::vector<uint64_t> capacityQ16;  // セルごと
        std::vector<uint64_t> groundQ16;    // セルごと: 地面への伝導
        std::vector<uint64_t> conductQ16;   // 面ごと(cellCount − 1): 伝導
        std::vector<uint64_t> flowQ16;      // 面ごと: +x の流れが運ぶ(風上の y × 係数)。下流へ減らないこと
        uint64_t outflowQ16 = 0;            // outflowCell から外へ(風上の y × 係数)
        int64_t inflowQ16 = 0;              // inflowCell へ入る値 / 刻み × 2^16(0 以上)
        int64_t groundValueQ16 = 0;         // 地面の y × 2^16
    };

    struct TimeStepLabSetup {
        uint32_t cellCount = 0;
        uint32_t blockCells = 8;
        uint32_t inflowCell = 0;
        uint32_t outflowCell = 0;

        TimeStepCarried carried[TIME_STEP_CARRIED];

        // --- 反応 A → P(燃える・発酵の代わり)---
        std::vector<uint8_t> fuel;           // セルごと: TIME_STEP_FUEL_*
        uint64_t rateColdQ32 = 0;            // ゆっくりな反応(発酵)の速さ(割合 / 刻み)
        uint64_t rateHotQ32 = 0;             // 燃える物の着火の温度以上の速さ(下では 0)
        int64_t ignitionTemperatureQ16 = 0;  // mK × 2^16
        int64_t heatPerAmount = 0;           // 1 µmol 反応した時に出る熱(mJ)

        // --- 刻みの長さを選ぶ ---
        TimeStepRule rule = TimeStepRule::Change;
        int32_t fixedExponent = 0;     // Fixed の時の e
        int32_t maxExponent = 16;      // e の上限(time_step.hlsli の TS_MAX_EXPONENT 以下)
        uint64_t budget = 1;           // 1 歩の変化(規則 A)・曲がり(規則 B)の許す大きさ(精度の単位)
        uint64_t referenceAmount = 1;  // A の精度 = この量の 100 万分の 1
        // 今の状態での変化の速さ(法則の右辺を今の値で)で次の歩の長さに上限を付ける(T-1169。規則 A だけ。既定は外す):
        // 枠のどのセルも「今の速さ × 2^e」が予算の内の e まで。前の歩の変化だけで選ぶと、流れが運んでくる前線が静かな枠に
        // 入る直前に長い歩を取る(T-1169 の遅い川で毎刻みとの差 0.6 K)。一定に流れる所は右辺が 0 なので長い歩のまま
        bool rateLook = false;
        // これから来る変化で次の歩の長さに上限を付ける(T-1170・T-1173。規則 A だけ。既定は外す = 0):
        // 歩を始める枠の長さを「風上(と広がりの届く隣)の枠の変化が、その枠から自分まで運ばれて予算を積むまでの刻み」までにする。
        // 届く刻み = 間の枠を渡る刻みの和 + 変化の源の枠の 予算 ÷ 1 刻みの変化(前の歩の変化 ÷ 長さ)。一定に流れる所は源の変化が
        // 予算の 1/4 未満なので、上限は源の長さの 4 倍より長く、掛からない。
        // 印の和: 1 = 遠くから届く(枠を渡る刻み = セルの数 ÷ 流れの速さ)/ 2 = 1 の速さに広がりを足す(2 × 広がり ÷ 枠の長さ・
        // 逆向きにも届く)/ 4 = 面で積もる(time_step_lab.cpp の PileCap)/ 8 = 短い隣の境を、その隣の前の歩の 1 刻みの変化で
        // 自分の歩の半ばまで先へ伸ばして解く(長さは変えない。面の流れは今まで通り短い側が記録するので保存はそのまま)/
        // 16 = 8 の見込みを予算までに抑える / 32 = 8 を風上の隣だけに。
        // 既定は 4(仮・T-1170: 遅い川の毎刻みとの差 598 → 8.6 mK・今までの試験は通る。8 は 20 mK で費用もほぼ同じだが、
        // 0.5 m と 2 m のセルで選ぶ e が揃わなくなった〔細かさの試験が落ちる〕。1・2 は遅い川に効かなかった)
        uint32_t arrivalLook = 4;
    };

    // 世界の帳簿(外との出入り・反応。符号は世界の中へ入るのが正)
    struct TimeStepLabLedger {
        int64_t outside[TIME_STEP_CARRIED] = {};  // 流れ込み + 入れた熱 − 地面へ − 流れ出し
        int64_t reactionHeat = 0;                 // 反応が出した熱(mJ)
        int64_t reacted = 0;                      // A → P の量(比べる値)
        int64_t outflow[TIME_STEP_CARRIED] = {};  // 流れ出した熱・D(比べる値)
        int64_t ground[TIME_STEP_CARRIED] = {};   // 地面へ逃げた分(比べる値)
    };

    // 歩の寄与の記録(打ち切りで縮める)。セルの記録の持ち主はセルのブロック
    struct TimeStepLabCellRecord {
        uint64_t start = 0;
        uint64_t ticks = 0;
        int64_t ground[TIME_STEP_CARRIED] = {};   // 地面へ
        int64_t inflow[TIME_STEP_CARRIED] = {};   // 流れ込み
        int64_t outflow[TIME_STEP_CARRIED] = {};  // 流れ出し
        int64_t reacted = 0;                      // A → P
    };

    // 面の記録(左 → 右が正)。持ち主は歩を計算したブロック(短い側か同じ歩の列)
    struct TimeStepLabFaceRecord {
        uint64_t start = 0;
        uint64_t ticks = 0;
        uint32_t owner = 0;
        int64_t flow[TIME_STEP_CARRIED] = {};
    };

    struct TimeStepLabState {
        uint64_t tick = 0;       // 今の刻み(ここより前の出来事は済み)
        bool processed = false;  // 今の刻みの出来事を処理したか(触ると false に戻る)

        // --- セルごと。[量 × cellCount + セル] ---
        std::vector<int64_t> values;     // 真実(歩の途中のブロックは歩の初めの値)
        std::vector<int64_t> stepDelta;  // 歩の変化(歩の終わりに真実へ足す。面の帳簿もここ)
        std::vector<TimeStepLabCellRecord> cellRecords;
        std::vector<TimeStepLabFaceRecord> faceRecords;  // cellCount − 1 個

        // --- ブロックごと ---
        std::vector<int32_t> exponent;
        std::vector<uint8_t> open;          // 歩の途中か
        std::vector<uint8_t> touched;       // 外から触った(次の歩を e = 0 で)
        std::vector<uint64_t> start;        // 今の歩を始めた刻み
        std::vector<uint64_t> end;          // 今の歩を終える刻み(初めは 0 = 刻み 0 で始める)
        std::vector<int32_t> desired;       // 次の歩の e の望み
        std::vector<uint64_t> lastMeasure;  // 今の歩の誤差の見積もり(規則 B。精度の単位 × 2^16)
        std::vector<uint64_t> lastRate;     // 前の歩の 1 刻みの変化(精度の単位 × 2^16。arrivalLook)
        std::vector<int64_t>
            lastEdgeRate;  // [(枠 × 2 + 端) × 量]: 両端のセルの前の歩の 1 刻みの変化(符号付き。ARRIVAL_PILE)

        TimeStepLabLedger ledger;

        // --- 測り ---
        uint64_t cellSteps = 0;  // セル × 解いた回数(費用)
        uint64_t blockSteps = 0;
        uint64_t events = 0;
        uint64_t truncations = 0;       // 打ち切った歩の数
        int32_t widestNeighborGap = 0;  // 歩を始めた後の隣どうしの e の差の最大
        uint32_t negativeValues = 0;    // 真実が負になった回数
    };

    // 初めの状態。values(熱・D・A・P)は呼ぶ側が入れてから最初の StepTimeStepLab を呼ぶ(刻み 0 で全部の歩を始める。
    // StepTimeStepLab(…, 0) は刻み 0 の歩を始めるだけ)
    TimeStepLabState MakeTimeStepLabState(const TimeStepLabSetup& setup);

    // ticks 刻み進める(早送り = 大きい ticks。規則は同じ)。終わりの刻みの出来事は次の呼び出しで処理する
    // (だから分け方によらず同じ。InjectTimeStepLabHeat はその出来事の初めに入る)
    void StepTimeStepLab(const TimeStepLabSetup& setup, TimeStepLabState& state, uint64_t ticks);

    // 今の刻み(state.tick)にセルへ熱を入れる(火を付ける。帳簿の外への出入り)。そのブロックの歩をこの刻みで打ち切り、
    // 次の出来事(同じ刻み)で次の歩を e = 0 から始める
    void InjectTimeStepLabHeat(const TimeStepLabSetup& setup, TimeStepLabState& state, uint32_t cell, int64_t heat);

    // 状態の要約(真実・歩の変化・記録・e・帳簿)
    uint64_t HashTimeStepLab(const TimeStepLabState& state);

    // 保存の検査: 量ごとに Σ(真実 + 歩の変化)− 帳簿。毎刻み同じなら保存(熱・D・A + P)
    struct TimeStepLabTotals {
        int64_t heat = 0;
        int64_t dissolved = 0;
        int64_t reactant = 0;  // A + P
    };
    TimeStepLabTotals ConservedTimeStepLab(const TimeStepLabSetup& setup, const TimeStepLabState& state);

    // 歩の変化も足した値(比べる時に使う。歩の途中でも保存した量の置き場)
    int64_t TimeStepLabSettled(const TimeStepLabSetup& setup, const TimeStepLabState& state, uint32_t quantity,
                               uint32_t cell);

}  // namespace bicameral::sim
