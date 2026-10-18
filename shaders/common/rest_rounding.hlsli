// rest_rounding.hlsli — 静止の丸めの 1 つの形(気体のセル・液体の面が同じ関数を呼ぶ。T-0758・D-516)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。
//
// 「眠る = 遅く、しかもこの刻みで速くなっていない」ものだけ運動量を 0 にする(運動のエネルギーは熱へ・消した運動量は帳簿の「静止の丸め」)。
// T-0723 の形(遅ければ止める)は、押されて動き出したばかりの遅い流れまで毎刻み止め、浅い池の 2 層目の薄い膜が抜けなくなった
// (T-0757 の申し送り: 60 秒で面の運動量 5.9 × 10^6 残る。丸めなしは 4.9 × 10^3)。速くなっている流れは押されている = 止めない。
// 遅くなっているものは止めても次の刻みの力がまた作り直すだけなので、釣り合いでは 0 に着いて眠る。
// 浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_REST_ROUNDING_HLSLI
#define BICAMERAL_REST_ROUNDING_HLSLI

#include "common/fixed.hlsli"

#ifdef __cplusplus
namespace bicameral::rest {
    using namespace ::bicameral::fx;
#endif

    // 大きさ(運動量の絶対値。比べる 2 つは同じ単位)。after < limit かつ after ≤ before なら丸める
    FX_FN bool RrShouldRest(uint64_t after, uint64_t before, uint64_t limit) {
        return after != 0u && after < limit && after <= before;
    }

    // 運動量 1 つ(液体の面)の丸めの結果。置き場は当てる段の後(上限・当てる・運ぶの後、運動のエネルギーを熱へ直す所。T-0777)
    struct RrRested {
        int64_t momentum;  // 丸めの後の運動量
        int64_t kinetic;   // 丸めの後の運動のエネルギー
        // 帳簿の「静止の丸め」に載せる運動量(外から入った向きが正 = 消した運動量の符号を返したもの)
        int64_t ledgerMomentum;
        int64_t heat;     // 熱になる運動のエネルギー(帳簿の「静止の丸め」でエネルギーの列へ)
        uint32_t rested;  // 1 = 丸めた
    };

    // 運動量 momentum(刻みの初め before・運動のエネルギー kinetic)を閾値 limit(運動量の単位。0 = 切る)で丸める。
    // 閾値を種類の単位へ直すのは種類(液体の面: 速さ × 刻みの数 × 両側の質量 ÷ 重力)。気体のセルは 3 軸の最大で RrShouldRest を呼ぶ
    FX_FN RrRested RrRestMomentum(int64_t momentum, int64_t before, int64_t kinetic, uint64_t limit) {
        RrRested result;
        result.momentum = momentum;
        result.kinetic = kinetic;
        result.ledgerMomentum = 0;
        result.heat = 0;
        result.rested = 0;
        if (!RrShouldRest(FxAbsU64(momentum), FxAbsU64(before), limit))
            return result;

        result.ledgerMomentum = -momentum;
        result.heat = kinetic;
        result.momentum = 0;
        result.kinetic = 0;
        result.rested = 1;

        return result;
    }

#ifdef __cplusplus
}  // namespace bicameral::rest
#endif

#endif  // BICAMERAL_REST_ROUNDING_HLSLI
