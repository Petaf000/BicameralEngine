// time_step_lab.cpp — 研究 R-T2 の CPU の試作(time_step_lab.h の先頭を見よ)。式は shaders/common/time_step.hlsli。
// 出来事(歩の終わり)の刻みだけを回すので、早送りの費用は歩の数で決まり、刻みの数によらない。浮動小数点は使わない。
#include "sim/time_step_lab.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <span>

#include "common/time_step.hlsli"

namespace bicameral::sim {

    using namespace bicameral::fx;
    using namespace bicameral::time_step;

    namespace {

        constexpr uint32_t HEAT = TIME_STEP_HEAT;
        constexpr uint32_t DISSOLVED = TIME_STEP_DISSOLVED;
        constexpr uint32_t REACTANT = TIME_STEP_REACTANT;
        constexpr uint32_t PRODUCT = TIME_STEP_PRODUCT;
        constexpr uint32_t CARRIED = TIME_STEP_CARRIED;
        constexpr uint64_t PARTS_PER_MILLION = 1000000;
        constexpr uint32_t VIEW_BITS = 16;
        constexpr uint64_t TIME_STEP_NO_ESTIMATE = ~0ull;

        // --- 番号と小さな道具 ---

        size_t At(const TimeStepLabSetup& setup, uint32_t quantity, uint32_t cell) {
            return static_cast<size_t>(quantity) * setup.cellCount + cell;
        }

        uint32_t BlockCount(const TimeStepLabSetup& setup) {
            return setup.cellCount / setup.blockCells;
        }

        uint32_t BlockOf(const TimeStepLabSetup& setup, uint32_t cell) {
            return cell / setup.blockCells;
        }

        uint64_t Ticks(int32_t exponent) {
            return 1ull << static_cast<uint32_t>(exponent);
        }

        // 値の見積もり y(× 2^16)。負の値は 0 として読む(見積もりだけ。流れは整数で両側に足すので保存は変わらない)
        int64_t ViewOf(const TimeStepLabSetup& setup, uint32_t quantity, uint32_t cell, int64_t value) {
            return TsTemperatureQ16(value, setup.carried[quantity].capacityQ16[cell]);
        }

        // 変化の大きさを精度の単位 × 2^16 に(熱 = mK・D = ppm・A と P = referenceAmount の 100 万分の 1)
        uint64_t PrecisionUnits(const TimeStepLabSetup& setup, uint32_t quantity, uint32_t cell, int64_t delta) {
            const uint64_t magnitude = FxAbsU64(delta);
            if (quantity < CARRIED) {
                const FxU128 numerator = {magnitude >> 32, magnitude << 32};

                return FxDivU128By64(numerator, setup.carried[quantity].capacityQ16[cell]).quotient;
            }

            const FxU128 scaled = FxMulU64Full(magnitude, PARTS_PER_MILLION << VIEW_BITS);

            return FxDivU128By64(scaled, setup.referenceAmount).quotient;
        }

        // 反応の端数を丸める決まった乱数(歩を始めた刻みとセルから。R6)
        constexpr uint32_t REACTION_ROUNDING_PURPOSE = 0x52543254;

        uint64_t ReactionRandom(uint64_t startTick, uint32_t cell) {
            return FxHash64(0, startTick, cell, REACTION_ROUNDING_PURPOSE);
        }

        // 反応の割合(歩の初めの温度で選ぶ)
        uint64_t ReactionRate(const TimeStepLabSetup& setup, uint32_t cell, int64_t temperatureQ16) {
            if (setup.fuel[cell] == TIME_STEP_FUEL_SLOW)
                return setup.rateColdQ32;

            if (setup.fuel[cell] == TIME_STEP_FUEL_BURNS && temperatureQ16 >= setup.ignitionTemperatureQ16)
                return setup.rateHotQ32;

            return 0;
        }

    }  // namespace

    namespace {

        // --- 列(同じ刻みに同じ e で始まる続きのブロック)の熱か D を後ろ向きの Euler で解く ---
        // 行 i: D_i y_i − L_i y_(i−1) − R_i y_(i+1) = r_i(係数は値 / y の単位 × 2^16・y は × 2^16・r は値 × 2^32)
        //   D = C + N (g左 + g右 + 流れ右 + 地面 + 流れ出し)・L = N (g左 + 流れ左)・R = N g右
        //   r = C y0 + N 地面 y地面 + 源。列の外の隣は値を固定して r へ(L・R を 0 に)。
        // 流れは下流へ減らない(flowQ16 が単調)ので D ≥ L + R(対角が勝つ)。Thomas の消去は比を Q62 で持つ。

        constexpr uint32_t RATIO_BITS = 62;

        struct Span {
            uint32_t firstBlock = 0;
            uint32_t blockCount = 0;
            uint32_t firstCell = 0;
            uint32_t cellCount = 0;
            int32_t exponent = 0;
            uint64_t start = 0;

            // 列の外の隣のセルがあるか(世界の端でない)・この列が面の流れを記録するか(隣が長い歩か、先に解いた長い列)
            bool leftExists = false;
            bool rightExists = false;
            bool leftOwned = false;
            bool rightOwned = false;
        };

        FxU128 AddWide(FxU128 a, FxU128 b) {
            const uint64_t low = a.lo + b.lo;

            return {a.hi + b.hi + (low < a.lo ? 1ull : 0ull), low};
        }

        uint64_t Scaled(uint64_t perTick, uint64_t ticks) {
            const FxU128 product = FxMulU64Full(perTick, ticks);
            FX_ASSERT(product.hi == 0);

            return product.lo;
        }

        uint64_t MulRatio(uint64_t value, uint64_t ratioQ62) {
            return FxShiftRightU128(FxMulU64Full(value, ratioQ62), RATIO_BITS);
        }

        uint64_t RatioQ62(uint64_t value, uint64_t divisor) {
            FX_ASSERT(value <= divisor);
            const FxU128 scaled = {value >> (64 - RATIO_BITS), value << RATIO_BITS};

            return FxDivU128By64(scaled, divisor).quotient;
        }

        constexpr uint32_t ARRIVAL_EXTRAPOLATE = 8;  // arrivalLook の印(短い隣の境を先へ伸ばす)
        constexpr uint32_t ARRIVAL_CLAMP = 16;
        constexpr uint32_t ARRIVAL_UPWIND = 32;

        uint64_t NonNegative(int64_t value) {
            return value > 0 ? static_cast<uint64_t>(value) : 0;
        }

        // 1 回の解き。y は始めの y(× 2^16)を受けて答えに。edgeLeft・edgeRight は列の外の隣の y、sources は値 × 2^32
        void SolveCarried(const TimeStepLabSetup& setup, const TimeStepCarried& law, const Span& span, uint64_t ticks,
                          int64_t edgeLeft, int64_t edgeRight, std::span<const FxU128> sources, std::span<int64_t> y) {
            const uint32_t last = setup.cellCount - 1;
            const uint32_t count = span.cellCount;
            std::vector<uint64_t> ratio(count);
            std::vector<uint64_t> partial(count);
            for (uint32_t k = 0; k < count; ++k) {
                const uint32_t cell = span.firstCell + k;
                const uint64_t conductLeft = cell > 0 ? Scaled(law.conductQ16[cell - 1], ticks) : 0;
                const uint64_t conductRight = cell < last ? Scaled(law.conductQ16[cell], ticks) : 0;
                const uint64_t flowLeft = cell > 0 ? Scaled(law.flowQ16[cell - 1], ticks) : 0;
                const uint64_t flowRight = cell < last ? Scaled(law.flowQ16[cell], ticks) : 0;
                const uint64_t ground = Scaled(law.groundQ16[cell], ticks);
                const uint64_t outflow = cell == setup.outflowCell ? Scaled(law.outflowQ16, ticks) : 0;
                const uint64_t capacity = law.capacityQ16[cell];
                const uint64_t diagonal = capacity + conductLeft + conductRight + flowRight + ground + outflow;
                uint64_t left = conductLeft + flowLeft;
                uint64_t right = conductRight;

                // --- 右辺(値 × 2^32)---
                FxU128 rhs = AddWide(FxMulU64Full(capacity, NonNegative(y[k])), sources[k]);
                rhs = AddWide(rhs, FxMulU64Full(ground, NonNegative(law.groundValueQ16)));
                if (k == 0 && span.leftExists) {
                    rhs = AddWide(rhs, FxMulU64Full(left, NonNegative(edgeLeft)));
                    left = 0;
                }

                if (k == count - 1 && span.rightExists) {
                    rhs = AddWide(rhs, FxMulU64Full(right, NonNegative(edgeRight)));
                    right = 0;
                }

                // --- 前から消去 ---
                const uint64_t previousRatio = k > 0 ? ratio[k - 1] : 0;
                const uint64_t previousPartial = k > 0 ? partial[k - 1] : 0;
                const uint64_t denominator = diagonal - MulRatio(left, previousRatio);
                const FxU128 numerator = AddWide(rhs, FxMulU64Full(left, previousPartial));
                ratio[k] = RatioQ62(right, denominator);
                partial[k] = FxDivU128By64(numerator, denominator).quotient;
            }

            // --- 後ろから代入 ---
            uint64_t next = 0;
            for (uint32_t k = count; k-- > 0;) {
                next = partial[k] + MulRatio(next, ratio[k]);
                y[k] = static_cast<int64_t>(next);
            }
        }

    }  // namespace

    namespace {

        // --- 記録を両側の歩の変化と帳簿へ ---

        void ApplyFaceRecord(const TimeStepLabSetup& setup, TimeStepLabState& state, uint32_t face,
                             const TimeStepLabFaceRecord& record, int64_t sign) {
            for (uint32_t q = 0; q < CARRIED; ++q) {
                state.stepDelta[At(setup, q, face)] -= sign * record.flow[q];
                state.stepDelta[At(setup, q, face + 1)] += sign * record.flow[q];
            }
        }

        void ApplyCellRecord(const TimeStepLabSetup& setup, TimeStepLabState& state, uint32_t cell,
                             const TimeStepLabCellRecord& record, int64_t sign) {
            TimeStepLabLedger& ledger = state.ledger;
            for (uint32_t q = 0; q < CARRIED; ++q) {
                const int64_t net = record.inflow[q] - record.outflow[q] - record.ground[q];
                state.stepDelta[At(setup, q, cell)] += sign * net;
                ledger.outside[q] += sign * net;
                ledger.outflow[q] += sign * record.outflow[q];
                ledger.ground[q] += sign * record.ground[q];
            }

            const int64_t reactionHeat = record.reacted * setup.heatPerAmount;
            state.stepDelta[At(setup, HEAT, cell)] += sign * reactionHeat;
            state.stepDelta[At(setup, REACTANT, cell)] -= sign * record.reacted;
            state.stepDelta[At(setup, PRODUCT, cell)] += sign * record.reacted;
            ledger.reactionHeat += sign * reactionHeat;
            ledger.reacted += sign * record.reacted;
        }

        // 記録が刻み tick をまたぐか(tick に始まった記録も。打ち切ると 0 になる)
        bool Crosses(uint64_t start, uint64_t ticks, uint64_t tick) {
            return start <= tick && tick < start + ticks;
        }

        int64_t Shrink(int64_t value, uint64_t kept, uint64_t ticks) {
            return FxMulDivS64(value, kept, ticks);
        }

        // 面の記録を刻み tick で切る(残りの分を両側の歩の変化から引く。保存はそのまま)
        void CutFaceRecord(const TimeStepLabSetup& setup, TimeStepLabState& state, uint32_t face, uint64_t tick) {
            TimeStepLabFaceRecord& record = state.faceRecords[face];
            if (!Crosses(record.start, record.ticks, tick))
                return;

            const uint64_t kept = tick - record.start;
            TimeStepLabFaceRecord scaled = record;
            scaled.ticks = kept;
            for (uint32_t q = 0; q < CARRIED; ++q)
                scaled.flow[q] = Shrink(record.flow[q], kept, record.ticks);

            ApplyFaceRecord(setup, state, face, record, -1);
            ApplyFaceRecord(setup, state, face, scaled, 1);
            record = scaled;
        }

        // --- 歩を閉じる(歩の変化を真実へ)。measured なら規則 A の変化を測って次の e を選ぶ ---
        // 係数(値 / y の単位 × 2^16 / 刻み)× y(× 2^16)÷ 容量 = 1 刻みにセルの y が変わる量(y × 2^16。符号付き)
        int64_t SignedRate(uint64_t coefficientQ16, int64_t viewQ16, uint64_t capacityQ16) {
            const FxU128 product = FxMulU64Full(coefficientQ16, FxAbsU64(viewQ16));
            const auto magnitude = static_cast<int64_t>(FxDivU128By64(product, capacityQ16).quotient);

            return viewQ16 < 0 ? -magnitude : magnitude;
        }

        // 今の状態での 1 刻みの変化の速さ(法則の右辺を今の値で。y × 2^16 / 刻み。T-1169 の rateLook)。
        // 運ぶ(風上の値)・伝わる・地面・流れ込み・流れ出し。反応は閉じた形でゆっくりなので入れない
        int64_t CellRate(const TimeStepLabSetup& setup, const TimeStepLabState& state, uint32_t q, uint32_t cell) {
            const TimeStepCarried& law = setup.carried[q];
            const uint64_t capacity = law.capacityQ16[cell];
            const int64_t self = ViewOf(setup, q, cell, state.values[At(setup, q, cell)]);
            int64_t rate = SignedRate(law.groundQ16[cell], law.groundValueQ16 - self, capacity);
            if (cell > 0) {
                const int64_t up = ViewOf(setup, q, cell - 1, state.values[At(setup, q, cell - 1)]);
                rate += SignedRate(law.flowQ16[cell - 1], up, capacity);
                rate += SignedRate(law.conductQ16[cell - 1], up - self, capacity);
            }

            if (cell + 1 < setup.cellCount) {
                const int64_t down = ViewOf(setup, q, cell + 1, state.values[At(setup, q, cell + 1)]);
                rate -= SignedRate(law.flowQ16[cell], self, capacity);
                rate += SignedRate(law.conductQ16[cell], down - self, capacity);
            }

            if (cell == setup.inflowCell)
                rate += SignedRate(uint64_t{1} << VIEW_BITS, law.inflowQ16, capacity);

            if (cell == setup.outflowCell)
                rate -= SignedRate(law.outflowQ16, self, capacity);

            return rate;
        }

        // 今の変化の速さで決まる e の上限: 枠のどのセルも 2^e 刻みの変化の見積もりが予算の内(T-1169。rateLook)。
        // 前の歩の変化で選ぶと、流れが運んでくる前線が静かな枠に入る直前に長い歩を取る。一定に流れる所は右辺が 0 なので長いまま
        int32_t RateCap(const TimeStepLabSetup& setup, const TimeStepLabState& state, uint32_t block, uint64_t budget) {
            const uint32_t first = block * setup.blockCells;
            uint64_t fastest = 0;
            for (uint32_t q = 0; q < CARRIED; ++q) {
                for (uint32_t cell = first; cell < first + setup.blockCells; ++cell)
                    fastest = std::max(fastest, FxAbsU64(CellRate(setup, state, q, cell)));
            }

            int32_t cap = TS_MAX_EXPONENT;
            while (cap > 0 && fastest > (budget >> static_cast<uint32_t>(cap)))
                --cap;

            return cap;
        }

        void CloseBlock(const TimeStepLabSetup& setup, TimeStepLabState& state, uint32_t block, bool measured) {
            uint64_t change = 0;
            int64_t edge[2][CARRIED] = {};
            const uint32_t first = block * setup.blockCells;
            const uint32_t last = first + setup.blockCells - 1;
            for (uint32_t q = 0; q < TIME_STEP_QUANTITIES; ++q) {
                for (uint32_t cell = first; cell < first + setup.blockCells; ++cell) {
                    int64_t& delta = state.stepDelta[At(setup, q, cell)];
                    // 反応の進みは閉じた形で長さによらないので、変化の量で測るのは運ぶ・広がるの量(熱・D)だけ
                    if (q < CARRIED) {
                        const uint64_t units = PrecisionUnits(setup, q, cell, delta);
                        change = std::max(change, units);
                        const int64_t signedUnits = delta < 0 ? -static_cast<int64_t>(units)
                                                              : static_cast<int64_t>(units);
                        if (cell == first)
                            edge[0][q] = signedUnits;

                        if (cell == last)
                            edge[1][q] = signedUnits;
                    }

                    int64_t& value = state.values[At(setup, q, cell)];
                    value += delta;
                    delta = 0;
                    if (value < 0)
                        ++state.negativeValues;
                }
            }

            state.open[block] = 0;
            if (!measured)
                return;

            // --- 次の e(規則 B は誤差の見積もり。見積もりが無い e = 0 は規則 A)---
            const uint64_t budget = setup.budget << VIEW_BITS;
            const int32_t e = state.exponent[block];
            const uint64_t estimate = state.lastMeasure[block];
            if (setup.rule == TimeStepRule::Fixed)
                state.desired[block] = setup.fixedExponent;
            else if (setup.rule == TimeStepRule::Change || estimate == TIME_STEP_NO_ESTIMATE)
                state.desired[block] = TsDesiredExponent(TS_RULE_CHANGE, e, change, budget);
            else
                state.desired[block] = TsDesiredExponent(TS_RULE_ESTIMATE, e, estimate, budget);

            if (setup.rateLook && setup.rule == TimeStepRule::Change)
                state.desired[block] = std::min(state.desired[block], RateCap(setup, state, block, budget));

            state.lastRate[block] = change >> static_cast<uint32_t>(e);
            for (uint32_t side = 0; side < 2; ++side) {
                for (uint32_t q = 0; q < CARRIED; ++q)
                    state.lastEdgeRate[(block * 2 + side) * CARRIED + q] = edge[side][q] /
                                                                           static_cast<int64_t>(Ticks(e));
            }
        }

        // --- 歩を刻み tick で打ち切る(記録を経った刻みの割合に縮めてから閉じる。保存はそのまま)---
        void TruncateBlock(const TimeStepLabSetup& setup, TimeStepLabState& state, uint32_t block, uint64_t tick) {
            const uint32_t first = block * setup.blockCells;
            const uint32_t last = first + setup.blockCells - 1;
            for (uint32_t cell = first; cell <= last; ++cell) {
                TimeStepLabCellRecord& record = state.cellRecords[cell];
                if (!Crosses(record.start, record.ticks, tick))
                    continue;

                const uint64_t kept = tick - record.start;
                TimeStepLabCellRecord scaled = record;
                scaled.ticks = kept;
                for (uint32_t q = 0; q < CARRIED; ++q) {
                    scaled.ground[q] = Shrink(record.ground[q], kept, record.ticks);
                    scaled.inflow[q] = Shrink(record.inflow[q], kept, record.ticks);
                    scaled.outflow[q] = Shrink(record.outflow[q], kept, record.ticks);
                }

                // 反応は閉じた形で経った刻みの分を出し直す(真実はまだ歩の初めの値)
                const int64_t temperature = ViewOf(setup, HEAT, cell, state.values[At(setup, HEAT, cell)]);
                const int64_t reactant = static_cast<int64_t>(NonNegative(state.values[At(setup, REACTANT, cell)]));
                const uint64_t random = ReactionRandom(record.start, cell);
                const uint64_t rate = ReactionRate(setup, cell, temperature);
                scaled.reacted = kept == 0 ? 0 : TsDecayedPart(reactant, rate, kept, random);
                ApplyCellRecord(setup, state, cell, record, -1);
                ApplyCellRecord(setup, state, cell, scaled, 1);
                record = scaled;
            }

            // --- 面: 刻み tick をまたぐ記録は持ち主によらず全部切る。歩の途中の隣の記録も、ここまでの分はこのブロックの
            // 閉じる変化に入り、ここから先はこのブロックの次の歩(隣が歩の途中なので、こちらが短い側として持つ)が記録し直す ---
            const uint32_t firstFace = first > 0 ? first - 1 : 0;
            const uint32_t lastFace = std::min(last, setup.cellCount - 2);
            for (uint32_t face = firstFace; face <= lastFace; ++face)
                CutFaceRecord(setup, state, face, tick);

            CloseBlock(setup, state, block, false);
            state.desired[block] = state.touched[block] != 0 ? 0 : state.exponent[block];
            ++state.truncations;
        }

    }  // namespace

    namespace {

        // --- 隣の値を読む(真実 = 隣の歩の初めの値)---
        // 長い隣の歩の終わりの見積もりを時刻で内挿する形も試したが、その見積もりは長い側が古い境で解いた値なので、
        // 前線が近づくと大きく外れた(2 m のセルの川で 24 mK)。歩の初めの値は外れない(T-0856 の作業ログ)
        int64_t EdgeView(const TimeStepLabSetup& setup, const TimeStepLabState& state, uint32_t quantity,
                         uint32_t cell) {
            return ViewOf(setup, quantity, cell, state.values[At(setup, quantity, cell)]);
        }

        // 1 つの解きの値(値の単位): 面・地面・流れ出し
        struct SolveFlows {
            std::vector<int64_t> faces;    // 列の左の面から右の面まで(cellCount + 1 個。端の面は持ち主の時だけ使う)
            std::vector<int64_t> ground;   // セルごと
            std::vector<int64_t> outflow;  // セルごと
        };

        void MeasureFlows(const TimeStepLabSetup& setup, const TimeStepCarried& law, const Span& span, uint64_t ticks,
                          int64_t edgeLeft, int64_t edgeRight, std::span<const int64_t> y, SolveFlows& flows) {
            const uint32_t count = span.cellCount;
            flows.faces.assign(count + 1, 0);
            flows.ground.assign(count, 0);
            flows.outflow.assign(count, 0);
            for (uint32_t k = 0; k <= count; ++k) {
                const bool atLeft = k == 0;
                const bool atRight = k == count;
                if ((atLeft && !span.leftExists) || (atRight && !span.rightExists))
                    continue;

                const uint32_t face = span.firstCell + k - 1;
                const int64_t upwind = atLeft ? edgeLeft : y[k - 1];
                const int64_t downwind = atRight ? edgeRight : y[k];
                flows.faces[k] = TsFaceFlow(law.conductQ16[face], ticks, upwind, downwind) +
                                 TsProductTerm(static_cast<int64_t>(NonNegative(upwind)), ticks, law.flowQ16[face], 32);
            }

            for (uint32_t k = 0; k < count; ++k) {
                const uint32_t cell = span.firstCell + k;
                flows.ground[k] = TsFaceFlow(law.groundQ16[cell], ticks, y[k], law.groundValueQ16);
                if (cell == setup.outflowCell)
                    flows.outflow[k] = TsProductTerm(static_cast<int64_t>(NonNegative(y[k])), ticks, law.outflowQ16,
                                                     32);
            }
        }

    }  // namespace

    namespace {

        // --- 列の歩を始める: 反応・流れ込み・解き・記録 ---

        struct SpanSolution {
            std::vector<int64_t> end;        // 歩の終わりの y(× 2^16。反応の誤差の見積もりに使う)
            SolveFlows flows;                // 記録する流れ(1 回の解きの値)
            std::vector<uint64_t> estimate;  // セルごとの |半分 2 回 − 1 回|(y × 2^16。半分に分けた時だけ)
            uint32_t solves = 1;             // 解いた回数(費用)
        };

        // 1 つの量の流れを解く(1 回。規則 B は誤差の見積もりのために半分 2 回も解く。進めるのは 1 回の解き)
        SpanSolution SolveQuantity(const TimeStepLabSetup& setup, const TimeStepLabState& state, const Span& span,
                                   uint32_t quantity, std::span<const int64_t> sourceValues) {
            const TimeStepCarried& law = setup.carried[quantity];
            const uint64_t ticks = Ticks(span.exponent);
            const uint32_t count = span.cellCount;
            int64_t left = span.leftExists ? EdgeView(setup, state, quantity, span.firstCell - 1) : 0;
            int64_t right = span.rightExists ? EdgeView(setup, state, quantity, span.firstCell + count) : 0;
            if ((setup.arrivalLook & ARRIVAL_EXTRAPOLATE) != 0) {
                // 短い隣(この出来事で先に解かない側)は自分の歩の間に変わり続ける。その変化を歩の半ばまで見込む(T-1170)
                // 印 16 = 見込みを予算までに抑える・印 32 = 風上の隣だけ(流れが入ってくる面)
                const auto budgetView = static_cast<int64_t>(setup.budget << VIEW_BITS);
                const auto ahead = [&](uint32_t neighborBlock, uint32_t edge) {
                    const int64_t rate = state.lastEdgeRate[(neighborBlock * 2 + edge) * CARRIED + quantity];
                    const int64_t change = rate * static_cast<int64_t>(ticks / 2);
                    if ((setup.arrivalLook & ARRIVAL_CLAMP) == 0)
                        return change;

                    return std::clamp(change, -budgetView, budgetView);
                };
                const bool upwindOnly = (setup.arrivalLook & ARRIVAL_UPWIND) != 0;
                if (span.leftExists && !span.leftOwned && (!upwindOnly || law.flowQ16[span.firstCell - 1] > 0))
                    left += ahead(span.firstBlock - 1, 1);

                if (span.rightExists && !span.rightOwned && !upwindOnly)
                    right += ahead(span.firstBlock + span.blockCount, 0);
            }

            SpanSolution solution;
            std::vector<int64_t> start(count);
            for (uint32_t k = 0; k < count; ++k)
                start[k] = EdgeView(setup, state, quantity, span.firstCell + k);

            // --- 1 回 ---
            std::vector<FxU128> sources(count);
            for (uint32_t k = 0; k < count; ++k) {
                const uint64_t value = static_cast<uint64_t>(sourceValues[k]);
                sources[k] = {value >> 32, value << 32};
            }

            solution.end = start;
            SolveCarried(setup, law, span, ticks, left, right, sources, solution.end);
            MeasureFlows(setup, law, span, ticks, left, right, solution.end, solution.flows);
            if (span.exponent == 0 || setup.rule != TimeStepRule::Estimate)
                return solution;

            // --- 半分 2 回(源は半分ずつ。値 × 2^32 なので割り切れる)---
            for (FxU128& source : sources)
                source = {source.hi >> 1, (source.lo >> 1) | (source.hi << 63)};

            std::vector<int64_t> halves = start;
            SolveCarried(setup, law, span, ticks >> 1, left, right, sources, halves);
            SolveCarried(setup, law, span, ticks >> 1, left, right, sources, halves);
            solution.solves = 3;
            solution.estimate.resize(count);
            for (uint32_t k = 0; k < count; ++k)
                solution.estimate[k] = FxAbsU64(halves[k] - solution.end[k]);

            return solution;
        }

    }  // namespace

    namespace {

        // 列の歩を始める(刻み span.start から 2^e 刻み)。反応・流れ込み・解き → 記録を両側の歩の変化へ
        void StartSpan(const TimeStepLabSetup& setup, TimeStepLabState& state, const Span& span) {
            const uint64_t ticks = Ticks(span.exponent);
            const uint32_t count = span.cellCount;

            // --- 反応(歩の初めの温度と A から閉じた形)と流れ込み ---
            std::vector<TimeStepLabCellRecord> cells(count);
            std::vector<int64_t> sources[CARRIED];
            for (uint32_t q = 0; q < CARRIED; ++q)
                sources[q].assign(count, 0);

            for (uint32_t k = 0; k < count; ++k) {
                const uint32_t cell = span.firstCell + k;
                TimeStepLabCellRecord& record = cells[k];
                record.start = span.start;
                record.ticks = ticks;
                const int64_t temperature = ViewOf(setup, HEAT, cell, state.values[At(setup, HEAT, cell)]);
                const int64_t reactant = static_cast<int64_t>(NonNegative(state.values[At(setup, REACTANT, cell)]));
                const uint64_t random = ReactionRandom(span.start, cell);
                record.reacted = TsDecayedPart(reactant, ReactionRate(setup, cell, temperature), ticks, random);
                sources[HEAT][k] += record.reacted * setup.heatPerAmount;
                if (cell != setup.inflowCell)
                    continue;

                for (uint32_t q = 0; q < CARRIED; ++q) {
                    record.inflow[q] = TsProductTerm(setup.carried[q].inflowQ16, ticks, 1, 16);
                    sources[q][k] += record.inflow[q];
                }
            }

            // --- 熱と D を解く ---
            SpanSolution solutions[CARRIED];
            for (uint32_t q = 0; q < CARRIED; ++q) {
                solutions[q] = SolveQuantity(setup, state, span, q, sources[q]);
                state.cellSteps += static_cast<uint64_t>(count) * solutions[q].solves;
            }

            // --- セルの記録と見積もり ---
            for (uint32_t k = 0; k < count; ++k) {
                const uint32_t cell = span.firstCell + k;
                TimeStepLabCellRecord& record = cells[k];
                for (uint32_t q = 0; q < CARRIED; ++q) {
                    record.ground[q] = solutions[q].flows.ground[k];
                    record.outflow[q] = solutions[q].flows.outflow[k];
                }

                state.cellRecords[cell] = record;
                ApplyCellRecord(setup, state, cell, record, 1);
            }

            // --- 面の記録(中の面と、持ち主の端の面)---
            for (uint32_t k = 0; k <= count; ++k) {
                const bool atLeft = k == 0;
                const bool atRight = k == count;
                if ((atLeft && !span.leftOwned) || (atRight && !span.rightOwned))
                    continue;

                // 前の持ち主(続いている隣の歩)の記録は、ここから先をこの列が持つので切る
                const uint32_t face = span.firstCell + k - 1;
                CutFaceRecord(setup, state, face, span.start);
                TimeStepLabFaceRecord record;
                record.start = span.start;
                record.ticks = ticks;
                record.owner = span.firstBlock;
                for (uint32_t q = 0; q < CARRIED; ++q)
                    record.flow[q] = solutions[q].flows.faces[k];

                state.faceRecords[face] = record;
                ApplyFaceRecord(setup, state, face, record, 1);
            }

            // --- ブロックの歩 ---
            for (uint32_t block = span.firstBlock; block < span.firstBlock + span.blockCount; ++block) {
                state.exponent[block] = span.exponent;
                state.start[block] = span.start;
                state.end[block] = span.start + ticks;
                state.open[block] = 1;
                state.lastMeasure[block] = TIME_STEP_NO_ESTIMATE;
                if (solutions[0].estimate.empty())
                    continue;

                // --- ブロックの誤差の見積もり(精度の単位 × 2^16。熱 = mK・D = ppm・反応 = 100 万分の 1)---
                // 反応: 歩の終わりの温度で選んだ速さで出し直した量との差の半分(温度で速さが変わる所の誤差。1 次)
                uint64_t estimate = 0;
                for (uint32_t k = (block - span.firstBlock) * setup.blockCells;
                     k < (block - span.firstBlock + 1) * setup.blockCells; ++k) {
                    for (uint32_t q = 0; q < CARRIED; ++q)
                        estimate = std::max(estimate, solutions[q].estimate[k]);

                    const uint32_t cell = span.firstCell + k;
                    const int64_t reactant = static_cast<int64_t>(NonNegative(state.values[At(setup, REACTANT, cell)]));
                    const uint64_t rateEnd = ReactionRate(setup, cell, solutions[HEAT].end[k]);
                    const int64_t reactedEnd = TsDecayedPart(reactant, rateEnd, ticks,
                                                             ReactionRandom(span.start, cell));
                    const int64_t gap = (reactedEnd - cells[k].reacted) / 2;
                    estimate = std::max(estimate, PrecisionUnits(setup, REACTANT, cell, gap));
                }

                state.lastMeasure[block] = estimate;
            }

            state.blockSteps += span.blockCount;
        }

    }  // namespace

    namespace {

        // --- これから来る変化(T-1170・T-1173。arrivalLook)---
        constexpr uint64_t NEVER = ~0ull;
        constexpr uint32_t ARRIVAL_FAR = 1;
        constexpr uint32_t ARRIVAL_SPREAD = 2;
        constexpr uint32_t ARRIVAL_PILE = 4;

        // 枠を渡る刻み(下流へ・上流へ)。速さ(セル / 刻み)= 流れ ÷ 容量(+ 2 × 広がり ÷ 容量 ÷ セルの数)。
        // 渡る刻み = セルの数 ÷ 速さ = n² × 容量 ÷ (n × 流れ + 2 × 広がり)。量(熱・D)のうち速い方
        struct Crossing {
            uint64_t down = NEVER;
            uint64_t up = NEVER;
        };

        Crossing BlockCrossing(const TimeStepLabSetup& setup, uint32_t block) {
            const uint64_t n = setup.blockCells;
            const uint32_t cell = block * setup.blockCells;
            const uint32_t face = std::min(cell, setup.cellCount - 2);
            const bool spread = (setup.arrivalLook & ARRIVAL_SPREAD) != 0;
            Crossing crossing;
            for (uint32_t q = 0; q < CARRIED; ++q) {
                const TimeStepCarried& law = setup.carried[q];
                const uint64_t numerator = n * n * law.capacityQ16[cell];
                const uint64_t flow = n * law.flowQ16[face];
                const uint64_t widen = spread ? 2 * law.conductQ16[face] : 0;
                if (flow + widen > 0)
                    crossing.down = std::min(crossing.down, numerator / (flow + widen));

                if (widen > flow)
                    crossing.up = std::min(crossing.up, numerator / (widen - flow));
            }

            return crossing;
        }

        uint64_t SaturatingAdd(uint64_t a, uint64_t b) {
            return a > NEVER - b ? NEVER : a + b;
        }

        // 歩を始める枠(next ≥ 0)の e を、ほかの枠の変化が届いて予算を積むまでの刻みに収める。
        // 源の枠 u の届く刻みの元 = 予算 ÷ u の 1 刻みの変化(u の中の前線は u の端にあると見て、u 自身を渡る刻みは足さない)
        // 面で積もる(ARRIVAL_PILE): 短い隣が持ち主の面の流れは、長い側の歩の変化の境のセルに積もる(長い側の解きは歩の初めの
        // 隣の値を境にするので、歩の間に隣が変わった分は中へ運ばれない)。積もる量 ≈ 面の係数 ÷ 容量 × 隣の 1 刻みの変化 × N² / 2
        // (N = 自分の歩の刻み。流れの速さが 1 歩にセルを C 個渡るなら、届いた変化の C / 2 倍が 1 セルに集まる。T-1170)。
        // これが予算の内の e まで。面をはさむ 2 つのセルが同じ速さで変わる所(一定に流れる・一様に冷める)には掛からない。
        // 隣が今は自分より短くない面も見る(歩の間に隣が短くなって積もり始める。隣の e で外すと遅い川で 567 mK に戻った)
        int32_t PileCap(const TimeStepLabSetup& setup, const TimeStepLabState& state, uint32_t block, uint64_t budget) {
            const uint32_t blocks = BlockCount(setup);
            const uint32_t first = block * setup.blockCells;
            const uint32_t last = first + setup.blockCells - 1;
            uint64_t bound = NEVER;  // N² の上限
            for (const int32_t side : {-1, 1}) {
                const int64_t other = static_cast<int64_t>(block) + side;
                if (other < 0 || other >= static_cast<int64_t>(blocks))
                    continue;

                const uint32_t neighbor = static_cast<uint32_t>(other);
                const uint32_t cell = side < 0 ? first : last;
                const uint32_t face = side < 0 ? first - 1 : last;
                const uint32_t ownEdge = side < 0 ? 0 : 1;
                for (uint32_t q = 0; q < CARRIED; ++q) {
                    // 面をはさむ 2 つのセルの変化の速さの差(一様に冷める・一定に流れる所は 0)
                    const int64_t mine = state.lastEdgeRate[(block * 2 + ownEdge) * CARRIED + q];
                    const int64_t theirs = state.lastEdgeRate[(neighbor * 2 + (1 - ownEdge)) * CARRIED + q];
                    const uint64_t rate = FxAbsU64(theirs - mine);
                    if (rate == 0)
                        continue;

                    const TimeStepCarried& law = setup.carried[q];
                    const uint64_t coefficient = law.conductQ16[face] + (side < 0 ? law.flowQ16[face] : 0);
                    if (coefficient == 0)
                        continue;

                    const FxU128 numerator = FxMulU64Full(2 * budget * setup.pileCells, law.capacityQ16[cell]);
                    if (numerator.hi >= coefficient)
                        continue;

                    bound = std::min(bound, FxDivU128By64(numerator, coefficient).quotient / rate);
                }
            }

            int32_t longest = 0;
            while (longest < TS_MAX_EXPONENT && (uint64_t{1} << (2 * static_cast<uint32_t>(longest + 1))) <= bound)
                ++longest;

            return longest;
        }

        void CapByArrival(const TimeStepLabSetup& setup, const TimeStepLabState& state, std::vector<int32_t>& next) {
            const uint32_t blocks = BlockCount(setup);
            if ((setup.arrivalLook & ARRIVAL_PILE) != 0) {
                const uint64_t pileBudget = setup.budget << VIEW_BITS;
                for (uint32_t b = 0; b < blocks; ++b) {
                    if (next[b] > 0)
                        next[b] = std::min(next[b], PileCap(setup, state, b, pileBudget));
                }
            }

            if ((setup.arrivalLook & (ARRIVAL_FAR | ARRIVAL_SPREAD)) == 0)
                return;

            const uint64_t budget = setup.budget << VIEW_BITS;
            std::vector<uint64_t> arrival(blocks, NEVER);
            std::vector<Crossing> crossing(blocks);
            std::vector<uint64_t> source(blocks, NEVER);
            for (uint32_t b = 0; b < blocks; ++b) {
                crossing[b] = BlockCrossing(setup, b);
                source[b] = state.lastRate[b] == 0 ? NEVER : budget / state.lastRate[b];
            }

            // --- 下流へ(左 → 右)と上流へ(右 → 左)の 2 回の掃き。届く刻みの最小 ---
            uint64_t carried = NEVER;
            for (uint32_t b = 0; b < blocks; ++b) {
                arrival[b] = std::min(arrival[b], carried);
                carried = std::min(SaturatingAdd(carried, crossing[b].down), source[b]);
            }

            carried = NEVER;
            for (uint32_t b = blocks; b-- > 0;) {
                arrival[b] = std::min(arrival[b], carried);
                carried = std::min(SaturatingAdd(carried, crossing[b].up), source[b]);
            }

            for (uint32_t b = 0; b < blocks; ++b) {
                if (next[b] <= 0 || arrival[b] == NEVER)
                    continue;

                int32_t longest = 0;
                while (longest < TS_MAX_EXPONENT && Ticks(longest + 1) <= arrival[b])
                    ++longest;

                next[b] = std::min(next[b], longest);
            }
        }

        // --- 滑る窓(T-1174。time_step_lab.h の windowBlocks)---
        // 単位の格子の境を持たず、各枠がまわりの決まった長さの中の最も短い望みに合わせる。置く場所で分け方が変わらない(T-1173 の
        // 帯の根)まま、窓と坂を長さで決めれば細かさにも依らない見込み。局所の状態(まわりの望み)だけを読む(N1)

        bool UsesWindow(const TimeStepLabSetup& setup) {
            return setup.windowBlocks != 0 || setup.slopeBlocks > 1;
        }

        // 枠 block の上限 = min(まわりの枠の望み + 窓の外の坂)。wishes: 始める枠は窓の前の値・歩の途中の枠は歩を始めた時の望み
        int32_t WindowBound(const TimeStepLabSetup& setup, const std::vector<int32_t>& wishes, uint32_t block) {
            const int64_t window = setup.windowBlocks;
            const int64_t slope = std::max<uint32_t>(setup.slopeBlocks, 1);
            const int64_t reach = window + slope * (TS_MAX_EXPONENT + 1);
            const int64_t self = block;
            const int64_t from = std::max<int64_t>(0, self - reach);
            const int64_t to = std::min<int64_t>(static_cast<int64_t>(wishes.size()) - 1, self + reach);

            int64_t bound = TS_MAX_EXPONENT;
            for (int64_t other = from; other <= to; ++other) {
                const int64_t beyond = std::max<int64_t>(0, std::abs(other - self) - window);
                const int64_t rise = (beyond + slope - 1) / slope;
                bound = std::min(bound, wishes[static_cast<size_t>(other)] + rise);
            }

            return static_cast<int32_t>(bound);
        }

        // 始める枠の e を窓の上限まで縮める。歩の途中の枠が上限より長ければ打ち切って始め直す(上限が変わらなくなるまで)。
        // 望み(free)は局所の状態の上限(面で積もる分など)までで、世界の時計にそろえる上限(cap)は入れない: 入れると、奇数の刻みに
        // 打ち切った枠の望みが 0 になり、窓の中の枠を次々と打ち切って全体が毎刻みに張り付く(T-1174 の 1 回目の測り)。
        // 返すのは窓の前の望み(隣の差の打ち切りでも使う)
        std::vector<int32_t> ApplyWindow(const TimeStepLabSetup& setup, TimeStepLabState& state, uint64_t tick,
                                         int32_t cap, const std::vector<int32_t>& free, std::vector<int32_t>& next) {
            const uint32_t blocks = BlockCount(setup);
            std::vector<int32_t> local(blocks);
            for (uint32_t b = 0; b < blocks; ++b)
                local[b] = next[b] >= 0 ? free[b] : state.wish[b];

            bool changed = true;
            while (changed) {
                changed = false;
                for (uint32_t b = 0; b < blocks; ++b) {
                    if (next[b] >= 0 || state.exponent[b] <= WindowBound(setup, local, b))
                        continue;

                    TruncateBlock(setup, state, b, tick);
                    local[b] = std::clamp(state.desired[b], 0, setup.maxExponent);
                    next[b] = std::clamp(state.desired[b], 0, cap);
                    changed = true;
                }
            }

            for (uint32_t b = 0; b < blocks; ++b) {
                if (next[b] < 0)
                    continue;

                state.wish[b] = local[b];
                next[b] = std::min(next[b], WindowBound(setup, local, b));
            }

            return local;
        }

        // --- 出来事(刻み tick): 終えた歩を閉じる → e を選ぶ(隣の差 1 段・長すぎる隣は打ち切る)→ 列ごとに歩を始める ---

        std::vector<int32_t> ChooseExponents(const TimeStepLabSetup& setup, TimeStepLabState& state, uint64_t tick) {
            const uint32_t blocks = BlockCount(setup);
            const int32_t cap = std::min(TsAlignedCap(tick), setup.maxExponent);
            std::vector<int32_t> next(blocks, -1);  // -1 = 歩の途中
            for (uint32_t b = 0; b < blocks; ++b) {
                if (state.open[b] == 0)
                    next[b] = std::clamp(state.desired[b], 0, cap);
            }

            if (setup.arrivalLook != 0 && setup.rule == TimeStepRule::Change)
                CapByArrival(setup, state, next);

            std::vector<int32_t> local;
            if (UsesWindow(setup)) {
                std::vector<int32_t> free(blocks, -1);
                for (uint32_t b = 0; b < blocks; ++b) {
                    if (state.open[b] == 0)
                        free[b] = std::clamp(state.desired[b], 0, setup.maxExponent);
                }

                if (setup.arrivalLook != 0 && setup.rule == TimeStepRule::Change)
                    CapByArrival(setup, state, free);

                local = ApplyWindow(setup, state, tick, cap, free, next);
            }

            const auto current = [&](uint32_t b) {
                return next[b] >= 0 ? next[b] : state.exponent[b];
            };
            bool changed = true;
            while (changed) {
                changed = false;
                for (uint32_t b = 0; b < blocks; ++b) {
                    for (const int32_t side : {-1, 1}) {
                        const int64_t other = static_cast<int64_t>(b) + side;
                        if (other < 0 || other >= static_cast<int64_t>(blocks))
                            continue;

                        const uint32_t neighbor = static_cast<uint32_t>(other);
                        if (current(b) <= current(neighbor) + 1)
                            continue;

                        // --- 長すぎる: 始める側なら縮め、歩の途中なら打ち切って始め直す ---
                        if (next[b] < 0) {
                            TruncateBlock(setup, state, b, tick);
                            next[b] = std::clamp(state.desired[b], 0, cap);
                            if (!local.empty()) {
                                local[b] = std::clamp(state.desired[b], 0, setup.maxExponent);
                                state.wish[b] = local[b];
                                next[b] = std::min(next[b], WindowBound(setup, local, b));
                            }
                        }

                        next[b] = std::min(next[b], current(neighbor) + 1);
                        changed = true;
                    }
                }
            }

            return next;
        }

        void ProcessEvent(const TimeStepLabSetup& setup, TimeStepLabState& state) {
            const uint64_t tick = state.tick;
            const uint32_t blocks = BlockCount(setup);
            for (uint32_t b = 0; b < blocks; ++b) {
                if (state.open[b] != 0 && state.end[b] == tick)
                    CloseBlock(setup, state, b, true);
            }

            for (uint32_t b = 0; b < blocks; ++b) {
                if (state.touched[b] != 0)
                    state.desired[b] = 0;
            }

            const std::vector<int32_t> next = ChooseExponents(setup, state, tick);

            // --- 列: 続きの始めるブロックで e が同じもの。長い列から解く(短い隣は長い隣の見積もりを読む)---
            std::vector<Span> spans;
            for (uint32_t b = 0; b < blocks;) {
                if (next[b] < 0) {
                    ++b;
                    continue;
                }

                Span span;
                span.firstBlock = b;
                span.exponent = next[b];
                span.start = tick;
                while (b < blocks && next[b] == span.exponent) {
                    state.touched[b] = 0;
                    ++span.blockCount;
                    ++b;
                }

                span.firstCell = span.firstBlock * setup.blockCells;
                span.cellCount = span.blockCount * setup.blockCells;
                span.leftExists = span.firstBlock > 0;
                span.rightExists = span.firstBlock + span.blockCount < blocks;
                const uint32_t leftBlock = span.firstBlock - 1;
                const uint32_t rightBlock = span.firstBlock + span.blockCount;
                span.leftOwned = span.leftExists && (next[leftBlock] < 0 || next[leftBlock] > span.exponent);
                span.rightOwned = span.rightExists && (next[rightBlock] < 0 || next[rightBlock] > span.exponent);
                spans.push_back(span);
            }

            std::stable_sort(spans.begin(), spans.end(),
                             [](const Span& a, const Span& b) { return a.exponent > b.exponent; });
            for (const Span& span : spans)
                StartSpan(setup, state, span);

            for (uint32_t b = 0; b + 1 < blocks; ++b) {
                const int32_t gap = std::abs(state.exponent[b] - state.exponent[b + 1]);
                state.widestNeighborGap = std::max(state.widestNeighborGap, gap);
            }

            ++state.events;
        }

    }  // namespace

    // --- 外の口 ------------------------------------------------------------------------------------

    TimeStepLabState MakeTimeStepLabState(const TimeStepLabSetup& setup) {
        FX_ASSERT(setup.blockCells != 0 && setup.cellCount % setup.blockCells == 0);
        FX_ASSERT(setup.maxExponent <= TS_MAX_EXPONENT);
        const size_t values = static_cast<size_t>(TIME_STEP_QUANTITIES) * setup.cellCount;
        const uint32_t blocks = BlockCount(setup);
        const int32_t first = setup.rule == TimeStepRule::Fixed ? setup.fixedExponent : 0;

        TimeStepLabState state;
        state.values.assign(values, 0);
        state.stepDelta.assign(values, 0);
        state.cellRecords.assign(setup.cellCount, {});
        state.faceRecords.assign(setup.cellCount - 1, {});
        state.exponent.assign(blocks, first);
        state.open.assign(blocks, 0);
        state.touched.assign(blocks, 0);
        state.start.assign(blocks, 0);
        state.end.assign(blocks, 0);
        state.desired.assign(blocks, first);
        state.lastMeasure.assign(blocks, 0);
        state.lastRate.assign(blocks, 0);
        state.lastEdgeRate.assign(static_cast<size_t>(blocks) * 2 * CARRIED, 0);
        if (UsesWindow(setup))
            state.wish.assign(blocks, first);

        return state;
    }

    void StepTimeStepLab(const TimeStepLabSetup& setup, TimeStepLabState& state, uint64_t ticks) {
        const uint64_t target = state.tick + ticks;
        const uint32_t blocks = BlockCount(setup);
        for (;;) {
            if (!state.processed) {
                ProcessEvent(setup, state);
                state.processed = true;
            }

            uint64_t next = ~0ull;
            for (uint32_t b = 0; b < blocks; ++b)
                next = std::min(next, state.end[b]);

            if (next > target) {
                state.tick = target;
                return;
            }

            state.tick = next;
            state.processed = false;
            if (next == target)
                return;
        }
    }

    void InjectTimeStepLabHeat(const TimeStepLabSetup& setup, TimeStepLabState& state, uint32_t cell, int64_t heat) {
        const uint32_t block = BlockOf(setup, cell);
        state.touched[block] = 1;
        if (state.open[block] != 0 && state.end[block] == state.tick)
            CloseBlock(setup, state, block, true);
        else if (state.open[block] != 0)
            TruncateBlock(setup, state, block, state.tick);

        state.values[At(setup, HEAT, cell)] += heat;
        state.ledger.outside[HEAT] += heat;
        state.processed = false;
    }

    uint64_t HashTimeStepLab(const TimeStepLabState& state) {
        uint64_t hash = FxHash64(0, state.tick, state.processed ? 1 : 0, 0);
        const auto mix = [&hash](int64_t value) {
            hash = FxHashCombine(hash, static_cast<uint64_t>(value));
        };
        for (const auto* list : {&state.values, &state.stepDelta}) {
            for (const int64_t value : *list)
                mix(value);
        }

        for (size_t b = 0; b < state.exponent.size(); ++b) {
            mix(state.exponent[b]);
            mix(state.open[b]);
            mix(static_cast<int64_t>(state.start[b]));
            mix(static_cast<int64_t>(state.end[b]));
            mix(state.desired[b]);
            mix(static_cast<int64_t>(state.lastMeasure[b]));
            mix(static_cast<int64_t>(state.lastRate[b]));
            for (uint32_t i = 0; i < 2 * CARRIED; ++i)
                mix(state.lastEdgeRate[b * 2 * CARRIED + i]);
        }

        for (const int32_t wish : state.wish)
            mix(wish);

        for (const TimeStepLabFaceRecord& record : state.faceRecords) {
            mix(static_cast<int64_t>(record.start));
            mix(static_cast<int64_t>(record.ticks));
            mix(record.flow[0]);
            mix(record.flow[1]);
        }

        for (const TimeStepLabCellRecord& record : state.cellRecords) {
            mix(static_cast<int64_t>(record.start));
            mix(static_cast<int64_t>(record.ticks));
            mix(record.reacted);
        }

        mix(state.ledger.outside[0]);
        mix(state.ledger.outside[1]);
        mix(state.ledger.reactionHeat);

        return hash;
    }

    TimeStepLabTotals ConservedTimeStepLab(const TimeStepLabSetup& setup, const TimeStepLabState& state) {
        TimeStepLabTotals totals;
        for (uint32_t cell = 0; cell < setup.cellCount; ++cell) {
            totals.heat += TimeStepLabSettled(setup, state, HEAT, cell);
            totals.dissolved += TimeStepLabSettled(setup, state, DISSOLVED, cell);
            totals.reactant += TimeStepLabSettled(setup, state, REACTANT, cell);
            totals.reactant += TimeStepLabSettled(setup, state, PRODUCT, cell);
        }

        totals.heat -= state.ledger.outside[HEAT] + state.ledger.reactionHeat;
        totals.dissolved -= state.ledger.outside[DISSOLVED];

        return totals;
    }

    int64_t TimeStepLabSettled(const TimeStepLabSetup& setup, const TimeStepLabState& state, uint32_t quantity,
                               uint32_t cell) {
        const size_t index = At(setup, quantity, cell);

        return state.values[index] + state.stepDelta[index];
    }

}  // namespace bicameral::sim
