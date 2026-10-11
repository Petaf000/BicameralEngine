// probe_surface.hlsli — 筆の「面に沿って置く」(T-0332・docs/design/14 §2「実装(T-0332)」)が物とみなすセルの見分け方と、印の並び。
// GPU(shaders/sim/probe_surface.hlsl が 64³ の印を作る)と CPU リファレンス(engine/src/sim/probe_surface.cpp の BuildProbeSurfaceMask)が
// 同じ関数を使う。View のためだけ(世界の結果にも再生ファイルにも入らない)。
//
// 物とみなす: セルの物質量の合計が PROBE_SURFACE_THRESHOLD_MICROMOLES 以上。反応表に相(固体・液体)の情報が無いので物質量の濃さで見分ける
// (空気のセル 0.125 m³ は約 5.08 mol・初めの世界の木箱の壁〔セルロース 1 割〕は約 43.3 mol)。しきい値は空気の約 4 倍の 20 mol
// (仮。相を表に持たせる時に置き換える。気体の材料〔窒素・二酸化炭素〕を 1 気圧で置いても面にならない)。
// 印の並び: 語 w のビット b = セルの番号 32w + b(番号は x + 64y + 4096z。probe_sim.hlsli の世界の並び)。
#ifndef BICAMERAL_PROBE_SURFACE_HLSLI
#define BICAMERAL_PROBE_SURFACE_HLSLI

#include "probe_sim.hlsli"
#include "reaction.hlsli"

#ifdef __cplusplus
#define PROBE_SURFACE_NAMESPACE_BEGIN \
    namespace bicameral::sim {        \
        using namespace ::bicameral::reaction;
#define PROBE_SURFACE_NAMESPACE_END }
#else
#define PROBE_SURFACE_NAMESPACE_BEGIN
#define PROBE_SURFACE_NAMESPACE_END
#endif

PROBE_SURFACE_NAMESPACE_BEGIN

FX_CONST uint64_t PROBE_SURFACE_THRESHOLD_MICROMOLES = 20000000;  // 仮(上の説明)
FX_CONST uint32_t PROBE_SURFACE_WORDS = PROBE_CELL_COUNT / 32;

// セルの物質量の合計がしきい値以上か(足す前に残りと比べるので、64bit の和は溢れない)
FX_FN bool ProbeIsSolidCell(RxCell cell, uint64_t threshold) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        if (cell.amounts[i] >= threshold - total)
            return true;

        total += cell.amounts[i];
    }

    return false;
}

PROBE_SURFACE_NAMESPACE_END

#endif  // BICAMERAL_PROBE_SURFACE_HLSLI
