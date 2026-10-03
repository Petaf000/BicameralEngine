// physics_solve6_wave.hlsli — 物の 6×6(PxSolveSymmetric6)を 1 ウェーブの 27 レーンで分けて解く(T-0095)。
// 入力は共有メモリの和(physics_bindings.hlsli の g_systemSum。慣性の項も入っている)、出力は解 x(全部のレーンが同じ値を持つ)。
//
// レーン t < 21 は下三角の要素 (r, c)(t = r(r+1)/2 + c)、t = 21〜26 は右辺の成分 r = t − 21 を持つ(レジスタ。共有メモリもバリアも使わない)。
// 1 つの要素に対する演算(尺度合わせ・Cholesky・前進と後退の代入)の式と項は PxSolveSymmetric6(shaders/common/physics_math.hlsli)と同じで、
// 違うのは「どのレーンがいつ計算するか」だけ:
//   - Cholesky は右から見る形(列 j を作ったら、その右下の要素から L_rj × L_cj を引く)。引く項は CPU と同じで、引く順だけが違う。
//     64bit の整数の引き算は 2^64 を法として順番に依存しないので、結果はビット一致する。
//   - 前進の代入は Cholesky の列と一緒に進める(z_j は L_jj ができた時点で決まる)。後退の代入は右辺のレーンが下から順に。
// 1 回の解の待ち時間: 割り算 27 → 12 段・平方根 6 段・64bit の積 約 70 → 11 段(T-0093: 1 スレッドで約 42 µs だった)。
// 使えるのは、ウェーブが 32 レーン以上で、1 物を解くスレッド(SOLVE_LANES)がウェーブの幅の倍数のとき(組の先頭がウェーブの先頭に揃う)。
// それ以外(WARP の狭いウェーブ・島の方式の 16 本)は、呼ぶ側が 1 スレッドで PxSolveSymmetric6 を解く。
#ifndef BICAMERAL_PHYSICS_SOLVE6_WAVE_HLSLI
#define BICAMERAL_PHYSICS_SOLVE6_WAVE_HLSLI

#include "common/physics_math.hlsli"

static const uint32_t SOLVE6_LOWER_LANES = 21;  // 下三角(対角を含む)
static const uint32_t SOLVE6_RHS_LANE = 21;     // 右辺の成分 0 のレーン
static const uint32_t SOLVE6_USED_LANES = 27;
static const uint32_t SOLVE6_MIN_WAVE = 32;

// 下三角の要素 (row, column) を持つレーン
uint32_t Solve6LaneOf(uint32_t row, uint32_t column) {
    return row * (row + 1) / 2 + column;
}

// 1 レーンが持つ要素
struct Solve6Element {
    // --- どの要素か ---
    uint32_t row;
    uint32_t column;  // 右辺と空きのレーンは 6
    bool lower;       // 下三角の要素
    bool rhs;         // 右辺の成分(右辺が 0 のときは倒す)

    // --- 尺度 ---
    int32_t shiftRow;     // s_row
    int32_t shiftColumn;  // s_column(下三角だけ)
};

Solve6Element MakeSolve6Element(uint32_t laneIndex) {
    Solve6Element element;
    element.row = 0;
    element.column = 6;
    element.lower = laneIndex < SOLVE6_LOWER_LANES;
    element.rhs = !element.lower && laneIndex < SOLVE6_USED_LANES;
    element.shiftRow = 0;
    element.shiftColumn = 0;
    if (element.rhs)
        element.row = laneIndex - SOLVE6_RHS_LANE;

    if (!element.lower)
        return element;

    while (laneIndex >= Solve6LaneOf(element.row + 1, 0))
        ++element.row;

    element.column = laneIndex - Solve6LaneOf(element.row, 0);
    return element;
}

// --- floor(sqrt(value))(value < 2^126)をウェーブの 32 レーンで 5 ビットずつ決める ---
// PxSqrtU128(1 スレッドの Newton 法。1 回に 128÷64 の割り算 約 3 回と 64bit の平方根の 32 回のループ)と同じ値(floor の平方根は 1 つに決まる)。
// 決めた上の桁 r に、レーン t が候補 r + t × 2^shift を足し、その 2 乗が value 以下のレーンの数 − 1 が次の 5 ビット
// (候補は t について単調なので、条件を満たすレーンは 0 から続く)。結果は 63 ビット以下なので 13 回
uint64_t WaveSqrtU128(FxU128 value) {
    const uint32_t laneIndex = WaveGetLaneIndex();
    uint64_t root = 0;
    [unroll] for (uint32_t digit = 0; digit < 13; ++digit) {
        const uint32_t shift = 60 - 5 * digit;
        const uint64_t candidate = root | ((uint64_t)laneIndex << shift);
        const FxU128 square = FxMulU64Full(candidate, candidate);
        const uint32_t laneLimit = digit == 0 ? 8 : 32;  // 一番上の桁はビット 60〜62 だけ(候補が 2^64 を超えないように)
        const bool fits = laneIndex < laneLimit &&
                          (square.hi < value.hi || (square.hi == value.hi && square.lo <= value.lo));
        root |= (uint64_t)(WaveActiveCountBits(fits) - 1) << shift;
    }

    return root;
}

// --- 1. 尺度合わせ: 対角から s_i、右辺から共通のずらし(PxDiagonalScaling・PxSolveSymmetric6 の前半)---
// 戻り値は右辺の一番上のビット(PX_SOLVE_NO_RHS なら解は 0)
int32_t ScaleSolve6(inout Solve6Element element, inout int64_t value, int32_t gainShift) {
    int32_t top = PX_SOLVE_NO_RHS;
    [unroll] for (uint32_t i = 0; i < 6; ++i) {
        const int32_t shift = PxDiagonalShift(WaveReadLaneAt(value, Solve6LaneOf(i, i)));
        const int64_t g = WaveReadLaneAt(value, SOLVE6_RHS_LANE + i);
        top = (int32_t)PxMax(top, PxRhsTopBit(g, gainShift, shift));
        if (element.row == i)
            element.shiftRow = shift;

        if (element.column == i)
            element.shiftColumn = shift;
    }

    const int32_t common = top - PX_SOLVE_RHS_BITS;
    element.rhs = element.rhs && top != PX_SOLVE_NO_RHS;
    if (element.lower)
        value = PxShift(value, element.shiftRow + element.shiftColumn);
    else if (element.rhs)
        value = PxShift(value, gainShift + element.shiftRow - common);

    return top;
}

// --- 2. Cholesky と前進の代入(PxFactorScaled6・PxSubstitute6 の前半)---
// 終わると下三角のレーンは L、右辺のレーンは z(L z = r)。戻り値は ok(全部のレーンで同じ)
bool FactorSolve6(Solve6Element element, inout int64_t value, inout uint32_t maxBits) {
    bool ok = true;
    [unroll] for (uint32_t j = 0; j < 6; ++j) {
        // 列 j の対角はもう前の列の寄与が引かれている
        const int64_t diagonal = WaveReadLaneAt(value, Solve6LaneOf(j, j));
        ok = ok && diagonal > 0;
        const int64_t root = (int64_t)WaveSqrtU128(PxCholeskyRadicand(diagonal));  // = PxCholeskyRoot(diagonal)
        const bool isDiagonal = element.lower && element.row == j && element.column == j;
        const bool belowDiagonal = element.lower && element.column == j && element.row > j;
        const bool rhsOfColumn = element.rhs && element.row == j;
        if (isDiagonal)
            value = root;
        else if (belowDiagonal || rhsOfColumn)
            value = FxDivShiftS64(value, root, 62);

        if (rhsOfColumn)
            maxBits = (uint32_t)PxMax(maxBits, PxBitsOf(value));

        // 列 j(L_mj と z_j)を配って、右下の要素から引く: a_rc −= L_rj × L_cj、r_r −= L_rj × z_j
        int64_t leftFactor = 0;
        int64_t rightFactor = WaveReadLaneAt(value, SOLVE6_RHS_LANE + j);
        [unroll] for (uint32_t m = j + 1; m < 6; ++m) {
            const int64_t entry = WaveReadLaneAt(value, Solve6LaneOf(m, j));
            if (element.row == m)
                leftFactor = entry;

            if (element.lower && element.column == m)
                rightFactor = entry;
        }

        const bool trailing = (element.lower && element.column > j) || (element.rhs && element.row > j);
        if (trailing)
            value -= FxMulShiftS64(leftFactor, rightFactor, 62);
    }

    return ok;
}

// --- 3. 後退の代入(Lᵀ y = z。PxSubstitute6 の後半)。右辺のレーンが下の成分から順に ---
void BackSubstituteSolve6(Solve6Element element, inout int64_t value, inout uint32_t maxBits) {
    [unroll] for (uint32_t step = 0; step < 6; ++step) {
        const uint32_t k = 5 - step;
        const int64_t diagonal = WaveReadLaneAt(value, Solve6LaneOf(k, k));
        if (element.rhs && element.row == k) {
            value = FxDivShiftS64(value, diagonal, 62);
            maxBits = (uint32_t)PxMax(maxBits, PxBitsOf(value));
        }

        // y_i −= L_ki × y_k(i < k)
        const int64_t solved = WaveReadLaneAt(value, SOLVE6_RHS_LANE + k);
        int64_t factor = 0;
        [unroll] for (uint32_t m = 0; m < k; ++m) {
            const int64_t entry = WaveReadLaneAt(value, Solve6LaneOf(k, m));
            if (element.row == m)
                factor = entry;
        }

        if (element.rhs && element.row < k)
            value -= FxMulShiftS64(factor, solved, 62);
    }
}

// PxSolveSymmetric6 と同じ解。value はこのレーンの要素(element の要素の A_rc か G_r。空きのレーンは 0)。
// ウェーブの全部のレーンが一様に呼ぶ(中で WaveReadLaneAt を使う)。解は全部のレーンに配る
PxSolveResult SolveSymmetric6InWave(Solve6Element element, int64_t value, int32_t gainShift) {
    uint32_t maxBits = 0;
    const int32_t top = ScaleSolve6(element, value, gainShift);
    const bool ok = FactorSolve6(element, value, maxBits);
    BackSubstituteSolve6(element, value, maxBits);

    // もとの尺度に戻す: X_i = y_i × 2^(s_i + common − 62)(右辺が 0 なら X = 0)
    const int32_t common = top - PX_SOLVE_RHS_BITS;
    const int64_t solution = element.rhs ? PxShift(value, element.shiftRow + common - 62) : 0;

    PxSolveResult result;
    [unroll] for (uint32_t i = 0; i < 6; ++i) result.x.v[i] = WaveReadLaneAt(solution, SOLVE6_RHS_LANE + i);

    result.maxBits = WaveActiveMax(maxBits);
    result.ok = ok;
    return result;
}

#endif  // BICAMERAL_PHYSICS_SOLVE6_WAVE_HLSLI
