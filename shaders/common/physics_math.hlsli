// physics_math.hlsli — 物理(08。T-0016 研究 R-PHYS-1)の整数の数学: 3 次元のベクトル・回転・6×6 の連立方程式。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束に従う)。CPU のソルバー(engine/src/sim/physics_world.cpp)と
// GPU のカーネル(T-0090)が同じ関数を呼ぶので、同じ入力からはビット単位で同じ結果になる。
//
// 単位(04 §2 の「物理」の行):
//   位置・腕 2^-20 m / 刻みの中の変位 Δp・拘束の値 C 2^-32 m / 回転 Δθ 2^-32 rad / 単位ベクトル・回転行列・四元数 Q1.30 /
//   角のヤコビアン r × b 2^-30 m / 硬さ・M/h²・H の要素 2^-8 N/m(回転は N·m/rad)/ 力・λ・勾配 2^-16 N(回転は N·m)
//
// 6×6 の解き方(研究 R-PHYS-1 の中心): 硬さ(1〜10^12 N/m)と質量の項(数 N/m〜10^9 N/m)が 1 つの行列に混ざり、
// 値の幅が 64bit の固定小数点の 1 つの Q 形式には収まらない。そこで
//   1. 行と列を同じ 2 の冪で対称にずらし、対角を [2^60, 2^62) に揃える(行列の大きさに関係なく 62bit の精度を使える)。
//   2. 対称正定値で対角 < 1 の行列の Cholesky の因子は |L| ≤ 1(行のノルム² = 対角)なので、Q62 で溢れずに分解できる。
//   3. 右辺もまとめて 2 の冪でずらし、解の桁は PxSolveResult::maxBits で記録する(条件数が悪いと解が大きくなる)。
#ifndef BICAMERAL_PHYSICS_MATH_HLSLI
#define BICAMERAL_PHYSICS_MATH_HLSLI

#include "fixed.hlsli"

// 全部 0 の構造体(C++ は値初期化、HLSL は 0 からのキャスト)
#ifdef __cplusplus
#define PX_ZERO(Type) \
    Type {}
#else
#define PX_ZERO(Type) ((Type)0)
#endif

#ifdef __cplusplus
#define PX_NAMESPACE_BEGIN         \
    namespace bicameral::physics { \
        using namespace ::bicameral::fx;
#define PX_NAMESPACE_END }
#else
#define PX_NAMESPACE_BEGIN
#define PX_NAMESPACE_END
#endif

PX_NAMESPACE_BEGIN

FX_CONST uint32_t PX_UNIT_SHIFT = 30;  // Q1.30
FX_CONST int64_t PX_UNIT_ONE = (int64_t)1 << 30;

// --- 符号つきのずらし(n > 0 は左。溢れはバグ。n < 0 は右で 0 方向に切り捨て)------------------
FX_FN int64_t PxShift(int64_t value, int32_t n) {
    if (n >= 0) {
        FX_ASSERT(n < 63 && FxAbsU64(value) < (FX_U64(0x80000000u, 0u) >> (uint32_t)n));
        return (int64_t)((uint64_t)value << (uint32_t)n);
    }

    if (n <= -64)
        return 0;

    return FxApplySign(FxAbsU64(value) >> (uint32_t)(-n), value < 0);
}

// --- 128bit の平方根: floor(sqrt(value))。value < 2^126 ---------------------------------------
// 上から近づく Newton 法(整数の floor で単調に減り、floor の平方根で止まる)
FX_FN uint64_t PxSqrtU128(FxU128 value) {
    if (value.hi == 0)
        return (uint64_t)FxSqrtU64(value.lo);

    FX_ASSERT(value.hi < FX_U64(0x40000000u, 0u));
    uint64_t x = ((uint64_t)FxSqrtU64(value.hi) + 1) << 32;
    for (uint32_t i = 0; i < 64; ++i) {
        const uint64_t quotient = FxDivU128By64(value, x).quotient;
        const uint64_t next = (x >> 1) + (quotient >> 1) + (x & quotient & 1);
        if (next >= x)
            break;

        x = next;
    }

    return x;
}

// --- 3 次元のベクトル(int64。単位は使う所で決める)------------------------------------------
struct PxVec3 {
    int64_t x;
    int64_t y;
    int64_t z;
};

FX_FN PxVec3 PxMakeVec3(int64_t x, int64_t y, int64_t z) {
    PxVec3 v = {x, y, z};
    return v;
}

FX_FN int64_t PxGet(PxVec3 v, uint32_t i) {
    return i == 0 ? v.x : (i == 1 ? v.y : v.z);
}

FX_FN PxVec3 PxSet(PxVec3 v, uint32_t i, int64_t value) {
    if (i == 0)
        v.x = value;
    else if (i == 1)
        v.y = value;
    else
        v.z = value;

    return v;
}

FX_FN PxVec3 PxAdd(PxVec3 a, PxVec3 b) {
    return PxMakeVec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

FX_FN PxVec3 PxSub(PxVec3 a, PxVec3 b) {
    return PxMakeVec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

// cond ? a : b(HLSL の ?: は構造体を返せない)
FX_FN PxVec3 PxSelect(bool cond, PxVec3 a, PxVec3 b) {
    if (cond)
        return a;

    return b;
}

FX_FN PxVec3 PxNegate(PxVec3 a) {
    return PxMakeVec3(-a.x, -a.y, -a.z);
}

// 各成分 × scale / 2^shift
FX_FN PxVec3 PxScale(PxVec3 v, int64_t scale, uint32_t shift) {
    return PxMakeVec3(FxMulShiftS64(v.x, scale, shift), FxMulShiftS64(v.y, scale, shift),
                      FxMulShiftS64(v.z, scale, shift));
}

FX_FN PxVec3 PxShiftVec3(PxVec3 v, int32_t n) {
    return PxMakeVec3(PxShift(v.x, n), PxShift(v.y, n), PxShift(v.z, n));
}

// Σ a_k b_k / 2^shift(項ごとに切り捨て)
FX_FN int64_t PxDot(PxVec3 a, PxVec3 b, uint32_t shift) {
    return FxMulShiftS64(a.x, b.x, shift) + FxMulShiftS64(a.y, b.y, shift) + FxMulShiftS64(a.z, b.z, shift);
}

FX_FN PxVec3 PxCross(PxVec3 a, PxVec3 b, uint32_t shift) {
    return PxMakeVec3(FxMulShiftS64(a.y, b.z, shift) - FxMulShiftS64(a.z, b.y, shift),
                      FxMulShiftS64(a.z, b.x, shift) - FxMulShiftS64(a.x, b.z, shift),
                      FxMulShiftS64(a.x, b.y, shift) - FxMulShiftS64(a.y, b.x, shift));
}

FX_FN int64_t PxAbs(int64_t value) {
    return value < 0 ? -value : value;
}

FX_FN int64_t PxMin(int64_t a, int64_t b) {
    return a < b ? a : b;
}

FX_FN int64_t PxMax(int64_t a, int64_t b) {
    return a > b ? a : b;
}

FX_FN int64_t PxClamp(int64_t value, int64_t low, int64_t high) {
    return PxMin(PxMax(value, low), high);
}

// ベクトルの長さ(同じ単位)。各成分 < 2^62
FX_FN uint64_t PxLength(PxVec3 v) {
    const FxU128 xx = FxMulU64Full(FxAbsU64(v.x), FxAbsU64(v.x));
    const FxU128 yy = FxMulU64Full(FxAbsU64(v.y), FxAbsU64(v.y));
    const FxU128 zz = FxMulU64Full(FxAbsU64(v.z), FxAbsU64(v.z));
    FxU128 sum = xx;
    sum.lo += yy.lo;
    sum.hi += yy.hi + (sum.lo < yy.lo ? (uint64_t)1 : (uint64_t)0);
    sum.lo += zz.lo;
    sum.hi += zz.hi + (sum.lo < zz.lo ? (uint64_t)1 : (uint64_t)0);

    return PxSqrtU128(sum);
}

// 長さ 1(Q1.30)にする。長さ 0 なら 0 を返す
FX_FN PxVec3 PxNormalize(PxVec3 v) {
    const uint64_t length = PxLength(v);
    if (length == 0)
        return PxMakeVec3(0, 0, 0);

    return PxMakeVec3(FxDivShiftS64(v.x, (int64_t)length, PX_UNIT_SHIFT),
                      FxDivShiftS64(v.y, (int64_t)length, PX_UNIT_SHIFT),
                      FxDivShiftS64(v.z, (int64_t)length, PX_UNIT_SHIFT));
}

// --- 3×3 の行列(行優先。回転行列は Q1.30)--------------------------------------------------
struct PxMat3 {
    PxVec3 row0;
    PxVec3 row1;
    PxVec3 row2;
};

FX_FN PxVec3 PxMatRow(PxMat3 m, uint32_t i) {
    if (i == 0)
        return m.row0;

    if (i == 1)
        return m.row1;

    return m.row2;
}

FX_FN PxVec3 PxColumn(PxMat3 m, uint32_t j) {
    return PxMakeVec3(PxGet(m.row0, j), PxGet(m.row1, j), PxGet(m.row2, j));
}

// m × v / 2^shift
FX_FN PxVec3 PxMulMat(PxMat3 m, PxVec3 v, uint32_t shift) {
    return PxMakeVec3(PxDot(m.row0, v, shift), PxDot(m.row1, v, shift), PxDot(m.row2, v, shift));
}

// mᵀ × v / 2^shift
FX_FN PxVec3 PxMulMatTransposed(PxMat3 m, PxVec3 v, uint32_t shift) {
    return PxMakeVec3(PxDot(PxColumn(m, 0), v, shift), PxDot(PxColumn(m, 1), v, shift),
                      PxDot(PxColumn(m, 2), v, shift));
}

// --- 四元数 (x, y, z, w)、Q1.30 ---------------------------------------------------------------
struct PxQuat {
    int64_t x;
    int64_t y;
    int64_t z;
    int64_t w;
};

FX_FN PxQuat PxMakeQuat(int64_t x, int64_t y, int64_t z, int64_t w) {
    PxQuat q = {x, y, z, w};
    return q;
}

FX_FN PxQuat PxQuatMultiply(PxQuat a, PxQuat b) {
    const uint32_t s = PX_UNIT_SHIFT;
    return PxMakeQuat(FxMulShiftS64(a.w, b.x, s) + FxMulShiftS64(a.x, b.w, s) + FxMulShiftS64(a.y, b.z, s) -
                          FxMulShiftS64(a.z, b.y, s),
                      FxMulShiftS64(a.w, b.y, s) - FxMulShiftS64(a.x, b.z, s) + FxMulShiftS64(a.y, b.w, s) +
                          FxMulShiftS64(a.z, b.x, s),
                      FxMulShiftS64(a.w, b.z, s) + FxMulShiftS64(a.x, b.y, s) - FxMulShiftS64(a.y, b.x, s) +
                          FxMulShiftS64(a.z, b.w, s),
                      FxMulShiftS64(a.w, b.w, s) - FxMulShiftS64(a.x, b.x, s) - FxMulShiftS64(a.y, b.y, s) -
                          FxMulShiftS64(a.z, b.z, s));
}

// 長さ 1 に直す(成分は |q| < 2 の範囲)
FX_FN PxQuat PxQuatNormalize(PxQuat q) {
    const uint64_t squared = (uint64_t)(q.x * q.x) + (uint64_t)(q.y * q.y) + (uint64_t)(q.z * q.z) +
                             (uint64_t)(q.w * q.w);
    const int64_t length = (int64_t)FxSqrtU64(squared);
    FX_ASSERT(length > 0);

    return PxMakeQuat(FxDivShiftS64(q.x, length, PX_UNIT_SHIFT), FxDivShiftS64(q.y, length, PX_UNIT_SHIFT),
                      FxDivShiftS64(q.z, length, PX_UNIT_SHIFT), FxDivShiftS64(q.w, length, PX_UNIT_SHIFT));
}

FX_FN PxMat3 PxRotationMatrix(PxQuat q) {
    const uint32_t s = PX_UNIT_SHIFT;
    const int64_t xx = FxMulShiftS64(q.x, q.x, s);
    const int64_t yy = FxMulShiftS64(q.y, q.y, s);
    const int64_t zz = FxMulShiftS64(q.z, q.z, s);
    const int64_t xy = FxMulShiftS64(q.x, q.y, s);
    const int64_t xz = FxMulShiftS64(q.x, q.z, s);
    const int64_t yz = FxMulShiftS64(q.y, q.z, s);
    const int64_t wx = FxMulShiftS64(q.w, q.x, s);
    const int64_t wy = FxMulShiftS64(q.w, q.y, s);
    const int64_t wz = FxMulShiftS64(q.w, q.z, s);
    const int64_t one = PX_UNIT_ONE;

    PxMat3 m;
    m.row0 = PxMakeVec3(one - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy));
    m.row1 = PxMakeVec3(2 * (xy + wz), one - 2 * (xx + zz), 2 * (yz - wx));
    m.row2 = PxMakeVec3(2 * (xz - wy), 2 * (yz + wx), one - 2 * (xx + yy));

    return m;
}

// 回転ベクトル θ(2^-32 rad)を q に積む: exp(θ/2) × q。|θ| < 2^31 rad
FX_CONST int64_t PX_TURN32_PER_RADIAN_Q32 = 683565276;  // 2^32 / (2π) を Q32 で(1 rad = この値 / 2^32 周 × 2^32)

FX_FN PxQuat PxIntegrateRotation(PxQuat q, PxVec3 rotation) {
    const uint64_t angle = PxLength(rotation);  // 2^-32 rad
    if (angle == 0)
        return q;

    // 半分の角を 1 周 = 2^32 の角度にする: (angle × 2^-32 rad) / 2 × 2^32 / (2π) = angle × (2^32/2π) / 2^33
    const uint32_t halfTurn = (uint32_t)FxShiftRightU128(FxMulU64Full(angle, (uint64_t)PX_TURN32_PER_RADIAN_Q32), 33);
    const FxSinCos halfAngle = FxSinCosTurn32(halfTurn);
    const PxVec3 axis = PxMakeVec3(FxDivShiftS64(rotation.x, (int64_t)angle, PX_UNIT_SHIFT),
                                   FxDivShiftS64(rotation.y, (int64_t)angle, PX_UNIT_SHIFT),
                                   FxDivShiftS64(rotation.z, (int64_t)angle, PX_UNIT_SHIFT));
    const PxQuat delta = PxMakeQuat(FxMulShiftS64(axis.x, halfAngle.sine, PX_UNIT_SHIFT),
                                    FxMulShiftS64(axis.y, halfAngle.sine, PX_UNIT_SHIFT),
                                    FxMulShiftS64(axis.z, halfAngle.sine, PX_UNIT_SHIFT), halfAngle.cosine);

    return PxQuatNormalize(PxQuatMultiply(delta, q));
}

// --- 6×6 の対称正定値の連立方程式 ------------------------------------------------------------
struct PxMat6 {
    int64_t m[36];  // 行優先
};

struct PxVec6 {
    int64_t v[6];
};

struct PxSolveResult {
    PxVec6 x;
    uint32_t maxBits;  // 途中の解(スケール後の Q62)の最大のビット数。調べる用(研究 R-PHYS-1)
    bool ok;           // 分解の途中で対角が 0 以下にならなかった
};

FX_CONST int32_t PX_SOLVE_RHS_BITS = 30;  // 右辺をこのビット数に揃える(残りの 32bit が条件数の余裕)

// 1. 対称な 2 の冪のずらしの量: A_ii × 2^(2 s_i) が [2^60, 2^62) に入る s_i
struct PxScaling {
    int32_t shift[6];
};

// 対角 1 つぶんのずらしの量(GPU のウェーブで解く形〔shaders/sim/physics_solve6_wave.hlsli〕も使う)
FX_FN int32_t PxDiagonalShift(int64_t diagonal) {
    FX_ASSERT(diagonal > 0);
    const int32_t room = 61 - (int32_t)FxMsbU64((uint64_t)diagonal);

    return room >= 0 ? room / 2 : -((1 - room) / 2);
}

FX_FN PxScaling PxDiagonalScaling(PxMat6 a) {
    PxScaling scaling;
    for (uint32_t i = 0; i < 6; ++i)
        scaling.shift[i] = PxDiagonalShift(a.m[i * 6 + i]);

    return scaling;
}

// 2. ずらした行列を Cholesky で分解する(Q62。下三角に L を入れて返す)。対角が 0 以下になったら ok = false
struct PxCholesky6 {
    PxMat6 l;
    bool ok;
};

// 列 j の対角(下の行の寄与を引いた後)から L_jj = floor(sqrt(対角 × 2^62))(Q62)を作る。対角が 0 以下なら 1 として続ける(呼ぶ側が ok を倒す)。
// 平方根を取る前の値(< 2^126)は GPU のウェーブで解く形(shaders/sim/physics_solve6_wave.hlsli)も使う
FX_FN FxU128 PxCholeskyRadicand(int64_t diagonal) {
    const uint64_t positive = diagonal > 0 ? (uint64_t)diagonal : 1u;
    const FxU128 shifted = {positive >> 2, positive << 62};

    return shifted;
}

FX_FN int64_t PxCholeskyRoot(int64_t diagonal) {
    return (int64_t)PxSqrtU128(PxCholeskyRadicand(diagonal));
}

FX_FN PxCholesky6 PxFactorScaled6(PxMat6 a, PxScaling scaling) {
    PxCholesky6 result;
    result.ok = true;
    for (uint32_t index = 0; index < 36; ++index)
        a.m[index] = PxShift(a.m[index], scaling.shift[index / 6] + scaling.shift[index % 6]);

    for (uint32_t j = 0; j < 6; ++j) {
        int64_t diagonal = a.m[j * 6 + j];
        for (uint32_t k = 0; k < j; ++k)
            diagonal -= FxMulShiftS64(a.m[j * 6 + k], a.m[j * 6 + k], 62);

        result.ok = result.ok && diagonal > 0;
        const int64_t root = PxCholeskyRoot(diagonal);
        a.m[j * 6 + j] = root;
        for (uint32_t i = j + 1; i < 6; ++i) {
            int64_t value = a.m[i * 6 + j];
            for (uint32_t k = 0; k < j; ++k)
                value -= FxMulShiftS64(a.m[i * 6 + k], a.m[j * 6 + k], 62);

            a.m[i * 6 + j] = FxDivShiftS64(value, root, 62);
        }
    }

    result.l = a;
    return result;
}

FX_FN uint32_t PxBitsOf(int64_t value) {
    return value == 0 ? 0u : FxMsbU64(FxAbsU64(value)) + 1;
}

// 3. 前進と後退の代入(L z = r、Lᵀ y = z)。y は Q62 の尺度
FX_FN PxSolveResult PxSubstitute6(PxMat6 l, PxVec6 y) {
    PxSolveResult result;
    result.maxBits = 0;
    result.ok = true;
    for (uint32_t i = 0; i < 6; ++i) {
        int64_t value = y.v[i];
        for (uint32_t k = 0; k < i; ++k)
            value -= FxMulShiftS64(l.m[i * 6 + k], y.v[k], 62);

        y.v[i] = FxDivShiftS64(value, l.m[i * 6 + i], 62);
        result.maxBits = (uint32_t)PxMax(result.maxBits, PxBitsOf(y.v[i]));
    }

    for (uint32_t step = 0; step < 6; ++step) {
        const uint32_t i = 5 - step;
        int64_t value = y.v[i];
        for (uint32_t k = i + 1; k < 6; ++k)
            value -= FxMulShiftS64(l.m[k * 6 + i], y.v[k], 62);

        y.v[i] = FxDivShiftS64(value, l.m[i * 6 + i], 62);
        result.maxBits = (uint32_t)PxMax(result.maxBits, PxBitsOf(y.v[i]));
    }

    result.x = y;
    return result;
}

// 右辺の成分 1 つの、ずらした後の一番上のビット(0 なら PX_SOLVE_NO_RHS)。成分の最大が右辺全体のずらしを決める
FX_CONST int32_t PX_SOLVE_NO_RHS = -1000;

FX_FN int32_t PxRhsTopBit(int64_t g, int32_t gainShift, int32_t shift) {
    return g == 0 ? PX_SOLVE_NO_RHS : (int32_t)FxMsbU64(FxAbsU64(g)) + gainShift + shift;
}

// A X = G × 2^gainShift を解く(A は対称正定値。A と G の単位は呼ぶ側が gainShift で合わせる)
FX_FN PxSolveResult PxSolveSymmetric6(PxMat6 a, PxVec6 g, int32_t gainShift) {
    const PxScaling scaling = PxDiagonalScaling(a);
    const PxCholesky6 factor = PxFactorScaled6(a, scaling);

    // 右辺: G_i × 2^(gainShift + s_i) を、一番大きい成分が 2^PX_SOLVE_RHS_BITS になるようにまとめてずらす
    int32_t top = PX_SOLVE_NO_RHS;
    for (uint32_t i = 0; i < 6; ++i)
        top = (int32_t)PxMax(top, PxRhsTopBit(g.v[i], gainShift, scaling.shift[i]));

    if (top == PX_SOLVE_NO_RHS) {
        PxSolveResult zero;
        zero.x = PX_ZERO(PxVec6);
        zero.maxBits = 0;
        zero.ok = factor.ok;
        return zero;
    }

    const int32_t common = top - PX_SOLVE_RHS_BITS;
    PxVec6 rhs;
    for (uint32_t i = 0; i < 6; ++i)
        rhs.v[i] = PxShift(g.v[i], gainShift + scaling.shift[i] - common);

    // もとの尺度に戻す: X_i = y_i × 2^(s_i + common − 62)
    PxSolveResult result = PxSubstitute6(factor.l, rhs);
    result.ok = factor.ok;
    for (uint32_t i = 0; i < 6; ++i)
        result.x.v[i] = PxShift(result.x.v[i], scaling.shift[i] + common - 62);

    return result;
}

PX_NAMESPACE_END

#endif  // BICAMERAL_PHYSICS_MATH_HLSLI
