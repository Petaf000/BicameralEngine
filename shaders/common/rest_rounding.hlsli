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

#ifdef __cplusplus
}  // namespace bicameral::rest
#endif

#endif  // BICAMERAL_REST_ROUNDING_HLSLI
