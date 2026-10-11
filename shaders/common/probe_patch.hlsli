// probe_patch.hlsli — 仮の世界(common/probe_sim.hlsli)に魔法のパッチ(common/magic_patch.hlsli)を置くコマンドの形と、
// 範囲が重なるブロック(T-0323・ADR-0095)。HLSL と C++ で共通(fixed.hlsli の約束)。GPU(shaders/sim/probe_tick.hlsl の
// ApplyPatch・ApplyPatchBoundaries)と CPU リファレンス(engine/src/sim/probe_sim.cpp の ProbeReference)が同じ関数を呼ぶ。
//
// データの流れ:
//   魔法(エディタ・ゲームの Controller)→ sim::Command(type = PROBE_COMMAND_TYPE_PATCH。sim::MakePatchCommand)→ 再生ファイルに記録
//   → GPU のキュー → 刻み t の適用の単位: 先に、速さが変わる(始まり・終わり)パッチの範囲のブロックを起こして終わったパッチを除き
//   (ApplyPatchBoundaries)、コマンドを番号順に当てる(足せたら範囲のブロックを起こす)→ 同じ刻みの伝導と反応(probe_conduct.hlsl)が
//   セルを覆うパッチを表に重ねる(MpPatchTable)。パッチの列(u14)は世界の状態なので、保存点に写し、状態の要約に混ぜる(MpListHash)。
// 起こす = 「刻みの直前に変わった」(tc = 刻み t)にして起こす刻みを今にする(置くコマンドと同じ。待ちの丸め ADR-0018:
//   速さが変わる刻みに tc を書き直さないと、眠っていたブロックが古い速さで引いた待ちのまま眠り続ける)。
// 範囲はレベル 0 のセル(= 仮の世界のセル。0.5 m)の座標。格子の外にはみ出してよい(重なる所だけ)。
#ifndef BICAMERAL_PROBE_PATCH_HLSLI
#define BICAMERAL_PROBE_PATCH_HLSLI

#include "magic_patch.hlsli"
#include "probe_world.hlsli"

#ifdef __cplusplus
#define PROBE_PATCH_NAMESPACE_BEGIN      \
    namespace bicameral::sim {           \
        using namespace ::bicameral::fx; \
        using namespace ::bicameral::reaction;
#define PROBE_PATCH_NAMESPACE_END }
#else
#define PROBE_PATCH_NAMESPACE_BEGIN
#define PROBE_PATCH_NAMESPACE_END
#endif

PROBE_PATCH_NAMESPACE_BEGIN

// パッチの列のバイト数(u14。MP_LIST_WORDS 語)
PROBE_CONST uint32_t PROBE_PATCH_LIST_BYTES = MP_LIST_WORDS * 4;

// コマンドの payload(MP_COMMAND_* の語)
struct ProbePatchPayload {
    uint32_t words[MP_COMMAND_WORDS];
};

// payload からパッチを作る(始まり = 当てる刻み tick)
FX_FN MpPatch ProbeDecodePatch(ProbePatchPayload payload, uint64_t tick) {
    return MpPatchFromCommand(FX_U64(payload.words[MP_COMMAND_RULE_KEY + 1], payload.words[MP_COMMAND_RULE_KEY]),
                              payload.words[MP_COMMAND_EFFECT], payload.words[MP_COMMAND_DURATION],
                              (int32_t)payload.words[MP_COMMAND_LOW], (int32_t)payload.words[MP_COMMAND_LOW + 1],
                              (int32_t)payload.words[MP_COMMAND_LOW + 2], (int32_t)payload.words[MP_COMMAND_HIGH],
                              (int32_t)payload.words[MP_COMMAND_HIGH + 1], (int32_t)payload.words[MP_COMMAND_HIGH + 2],
                              tick);
}

// パッチの範囲が重なるブロックの直方体(ブロックの座標。両端を含む)。格子と重ならなければ valid = 0
struct ProbePatchBlocks {
    uint32_t valid;
    uint32_t beginX;
    uint32_t beginY;
    uint32_t beginZ;
    uint32_t endX;
    uint32_t endY;
    uint32_t endZ;
};

// 1 軸: [low, high] を格子 [0, PROBE_GRID_SIZE − 1] に切り詰めた端(重ならなければ low > high のまま)
FX_FN int32_t ProbePatchClampLow(int32_t low) {
    return low > 0 ? low : 0;
}

FX_FN int32_t ProbePatchClampHigh(int32_t high) {
    const int32_t last = (int32_t)PROBE_GRID_SIZE - 1;
    return high < last ? high : last;
}

FX_FN ProbePatchBlocks ProbePatchBlockRange(MpPatch patch) {
    const int32_t lowX = ProbePatchClampLow(patch.lowX);
    const int32_t lowY = ProbePatchClampLow(patch.lowY);
    const int32_t lowZ = ProbePatchClampLow(patch.lowZ);
    const int32_t highX = ProbePatchClampHigh(patch.highX);
    const int32_t highY = ProbePatchClampHigh(patch.highY);
    const int32_t highZ = ProbePatchClampHigh(patch.highZ);

    ProbePatchBlocks blocks;
    blocks.valid = (lowX <= highX && lowY <= highY && lowZ <= highZ) ? 1u : 0u;
    blocks.beginX = blocks.valid != 0 ? (uint32_t)lowX / PROBE_BLOCK_SIZE : 0u;
    blocks.beginY = blocks.valid != 0 ? (uint32_t)lowY / PROBE_BLOCK_SIZE : 0u;
    blocks.beginZ = blocks.valid != 0 ? (uint32_t)lowZ / PROBE_BLOCK_SIZE : 0u;
    blocks.endX = blocks.valid != 0 ? (uint32_t)highX / PROBE_BLOCK_SIZE : 0u;
    blocks.endY = blocks.valid != 0 ? (uint32_t)highY / PROBE_BLOCK_SIZE : 0u;
    blocks.endZ = blocks.valid != 0 ? (uint32_t)highZ / PROBE_BLOCK_SIZE : 0u;

    return blocks;
}

PROBE_PATCH_NAMESPACE_END

#endif  // BICAMERAL_PROBE_PATCH_HLSLI
