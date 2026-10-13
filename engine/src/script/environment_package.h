// environment_package.h — 世界のパッケージの「環境」の値(分類 environment)を、世界の物理の値 sim::WorldPhysics に読む
// (T-0400・ADR-0176・ADR-0171・D-480)。
//
// データの流れ: パッケージのフォルダ → LoadPackages(合わせた「分類 → 鍵 → 値」)→ ReadWorldEnvironment → sim::WorldPhysics
//   → 物理(PhysicsParametersFor)・気体(ApplyWorldPhysics)。
// 書き方(1 つの鍵 = 1 つの値。値は整数で、単位は鍵の名前に書く。Mod・環境ミキサーは鍵ごとに足す・上書きする):
//   environment = {
//       gravity_um_per_s2 = 9806650,    -- 重力の強さ(µm/s²)。0〜100 m/s²
//       sound_speed_mm_per_s = 340000,  -- 気体の音速の基準(mm/s)。0〜気体の小刻みの安定の上限(既定の小刻み 24 で 432 m/s)
//   }
// 書かない値は 0 = 無い(ブランクの世界と同じ。D-455)。知らない鍵・整数でない数・範囲の外は誤り(綴りの誤りを黙って通さない)。
// 合わせた表の分類の 1 つなので、表の版(TableVersion)と再生ファイルの表の中身に入る(環境を変えた世界は版が変わり、
// 再生は記録の表の中身から同じ環境を作り直す。14 §2「環境ミキサー」)。
// 欄を変えたら、型の定義 data/types/bicameral.d.luau の Environment も変える。ここは CPU だけ(原則 2)。
#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "script/luau_package.h"
#include "sim/world_physics.h"

namespace bicameral::script {

    // 環境の分類の名前(パッケージの entry が返す表の鍵)と、値の鍵
    constexpr std::string_view ENVIRONMENT_CATEGORY = "environment";
    constexpr std::string_view ENVIRONMENT_GRAVITY_KEY = "gravity_um_per_s2";
    constexpr std::string_view ENVIRONMENT_SOUND_SPEED_KEY = "sound_speed_mm_per_s";

    // 合わせた表から世界の物理の値を作る。分類が無ければブランク(全部 0)。誤りは「パッケージ X: environment.鍵: 何が違うか」。
    // ReadReactionTableDefinition がこれを呼び、ベイクした表の environment に入る(T-0398 の共有の決め事)
    [[nodiscard]] std::expected<sim::WorldPhysics, std::string> ReadWorldEnvironment(const PackageSetResult& packages);

}  // namespace bicameral::script
