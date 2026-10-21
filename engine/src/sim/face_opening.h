// face_opening.h — セルの面の開き具合の欄(21 §2.7 の塞ぐ面・23 §7 の表面の細かい形・T-0937)を多重解像度の世界に持つ CPU。
//
// データの流れ: 真実(セルの固い物の量 + 表面の細かい形の差分。D-615)→ 形の関数(common/surface_detail.hlsli の SdCellOpennessAt)
//   → 面の塞ぐ割合(この欄。導かれる値)→ 運ぶ家族が面を通る量に掛ける開き(common/face_opening.hlsli の FoFaceOpen)。
// 欄は登録した欄 1 つ(SF_PLACE_FACE・U32・導かれる値。ADR-0399)。導かれる値なのでハッシュ・保存点に入らず、保存点から戻した後と
// 形が変わった時(掘った・割れた。刻みの段 3「占める層と塞ぐ面」)に DeriveFaceBlocked で作り直す。無い頁は 0 = 塞がない。
// 今の使い手: 多重解像度の 1 段の液体(sim/multires_liquid。欄を名前で引き、あれば読む)。気体・熱・案 B は後(T-0976)。
// 仮(T-0937): 根のレベルのブロックだけ(液体の 1 段と同じ)。区画の外のセルは塞がない。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <cstdint>

#include "common/face_opening.hlsli"
#include "sim/multires_nest.h"
#include "sim/surface_detail.h"

namespace bicameral::sim {

    // 欄の名前(運ぶ家族は FindNestField でこの名前を引く)
    inline constexpr const char* FACE_BLOCKED_FIELD_NAME = "face.blocked";

    // 表面の細かい形の区画を置く所: 区画の角(x・z = 0、高さ 0)= 世界の根のレベルのセル (originX, originUp, originZ) の下の角
    struct SurfacePlacement {
        int64_t originX = 0;
        int64_t originUp = 0;
        int64_t originZ = 0;
    };

    // 面の塞ぐ割合の欄を登録する(根を置く前に)。返すのは欄の番号(登録できなければ MULTIRES_NO_FIELD)
    [[nodiscard]] uint32_t RegisterFaceBlockedField(MultiresNest& nest);

    // 区画の形から全部の根のブロックの面の塞ぐ割合を作り直す(区画の外は 0)。
    // 断る(false): 根のレベル以外のブロックがある・セルが 1 µm より細かい・頁が足りない
    [[nodiscard]] bool DeriveFaceBlocked(MultiresNest& nest, uint32_t field, const SurfaceDetailScene& scene,
                                         uint32_t mode, const SurfacePlacement& placement);

    // 枠 slot のセル index の + 側の面 axis の塞ぐ割合(Q16)
    [[nodiscard]] uint32_t LoadFaceBlockedQ16(const MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t index,
                                              uint32_t axis);

}  // namespace bicameral::sim
