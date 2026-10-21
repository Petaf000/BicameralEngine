// face_opening.hlsli — セルの面の開き具合を法則が直接読む口(21 §2.7 の塞ぐ面・23 §7 の表面の細かい形・T-0937)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU(engine/src/sim/face_opening.cpp・multires_liquid.cpp)と
// GPU(後で。T-0975)が同じ関数を呼ぶ。
//
// 欄(登録は sim/face_opening.h の RegisterFaceBlockedField): 面の塞ぐ割合 b(Q16。セルの + 側の 3 面。導かれる値 = 真実から作る)。
//   真実はセルの固い物の量と表面の細かい形の差分(D-615)。b はその形から「面のうち固い物が塞ぐ割合」として導く(SdCellOpennessAt)。
//   無い頁は 0 = 塞がない(どの面も今までどおり)。
// 運ぶ家族が面を通る量に掛ける開き: 面の開き = min(両側のセルの空きの小さい方, 1 − b)(FoFaceOpen)。
//   形とセルの固い物の量は同じ固い物の 2 つの見え方なので、掛けずに小さい方を取る(掛けると同じ固い物を 2 回数える。仮。T-0937)。
//   b = 0 なら今の開きとビット単位で同じ。セルより細い溝は、溝の面だけが開き、溝の外の地面の面は 0 になる。
// 粒が穴を通る口(21 §2.7 S8。1 周目は口だけ): FoGrainFaceOpen が上の開きと穴の大きさ・粒の大きさ・比(世界のデータ)を読む。
// 浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_FACE_OPENING_HLSLI
#define BICAMERAL_FACE_OPENING_HLSLI

#include "common/fixed.hlsli"

#ifdef __cplusplus
#define FO_NAMESPACE_BEGIN              \
    namespace bicameral::face_opening { \
        using namespace ::bicameral::fx;
#define FO_NAMESPACE_END }
#else
#define FO_NAMESPACE_BEGIN
#define FO_NAMESPACE_END
#endif

FO_NAMESPACE_BEGIN

FX_CONST uint32_t FO_OPEN_FULL = 65536;  // 割合の 1(Q16。LQ_OPEN_FULL・CV_OPEN_FULL と同じ)

// 開き(Q16)→ 塞ぐ割合(欄に書く値。開きが 1 なら 0 = 欄の頁を作らない)
FX_FN uint32_t FoBlockedFromOpen(uint32_t openQ16) {
    return openQ16 >= FO_OPEN_FULL ? 0u : FO_OPEN_FULL - openQ16;
}

// 塞ぐ割合 → 面の開きの上限
FX_FN uint32_t FoOpenLimit(uint32_t blockedQ16) {
    return blockedQ16 >= FO_OPEN_FULL ? 0u : FO_OPEN_FULL - blockedQ16;
}

// 運ぶ家族が面を通す開き = min(両側のセルの空きから作った開き, 1 − 塞ぐ割合)。b = 0 なら cellsOpenQ16 のまま
FX_FN uint32_t FoFaceOpen(uint32_t cellsOpenQ16, uint32_t blockedQ16) {
    const uint32_t limit = FoOpenLimit(blockedQ16);

    return cellsOpenQ16 < limit ? cellsOpenQ16 : limit;
}

// 粒の相の面の開き(S8): 粒の大きさ ÷ 穴の大きさが passRatioPct ÷ 100 より大きいと通らない(0)。
// 穴の大きさ 0(物の塞ぐ面が無い・形から導いていない)か比 0(世界のデータに書いていない)は制限しない。
// 1 周目は口だけ(案 B の粒の流れが呼ぶのは 2 周目。21 §2.7)
FX_FN uint32_t FoGrainFaceOpen(uint32_t faceOpenQ16, uint32_t holeSizeUm, uint32_t grainSizeUm, uint32_t passRatioPct) {
    if (holeSizeUm == 0 || passRatioPct == 0)
        return faceOpenQ16;

    const uint64_t grainScaled = (uint64_t)grainSizeUm * 100;
    const uint64_t holeScaled = (uint64_t)holeSizeUm * passRatioPct;

    return grainScaled > holeScaled ? 0u : faceOpenQ16;
}

FO_NAMESPACE_END

#endif  // BICAMERAL_FACE_OPENING_HLSLI
