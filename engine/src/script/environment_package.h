// environment_package.h — 世界のパッケージの「環境」の値(分類 environment)を、世界の物理の値 sim::WorldPhysics に読む
// (T-0400・ADR-0176・ADR-0171・D-480)。
//
// データの流れ: パッケージのフォルダ → LoadPackages(合わせた「分類 → 鍵 → 値」)→ ReadWorldEnvironment → sim::WorldPhysics
//   → 物理(PhysicsParametersFor)・気体(ApplyWorldPhysics)。
// 書き方(1 つの鍵 = 1 つの値。値は整数で、単位は鍵の名前に書く。Mod・環境ミキサーは鍵ごとに足す・上書きする):
//   environment = {
//       gravity_um_per_s2 = 9806650,    -- 重力の強さ(µm/s²)。0〜100 m/s²
//       sound_speed_mm_per_s = 340000,  -- 気体の音速の基準(mm/s)。0〜気体の小刻みの安定の上限(既定の小刻み 24 で 432 m/s)
//       atmosphere_surface_pressure_pa = 101325,  -- 基準の大気の一番低い点の圧力(Pa。ほかの高さはエンジンが静水圧で出す)
//       atmosphere_profile = {                    -- 基準の大気の高さの点の列(低い順。点の間は直線。T-0499・ADR-0182 の改訂)
//           { height_mm = 0, temperature_mk = 288150, composition = { nitrogen = 780780, oxygen = 209460, ... } },
//           { height_mm = 11000000, temperature_mk = 216650, composition = { ... } },
//       },
//   }
// 一様な大気の書き方(T-0373。高さ 0 の点 1 つとして読む。atmosphere_profile とは一緒に書けない):
//   environment = { atmosphere_temperature_mk = 288150, atmosphere_surface_pressure_pa = 101325 }
//   atmosphere = { nitrogen = 780780, oxygen = 209460, ... }  -- 成分(物質 → ppm。足して 1000000。1 つの鍵 = 1 つの物質)
// 書かない値は 0 = 無い(ブランクの世界と同じ。D-455)。知らない鍵・整数でない数・範囲の外は誤り(綴りの誤りを黙って通さない)。
// 合わせた表の分類の 1 つなので、表の版(TableVersion)と再生ファイルの表の中身に入る(環境を変えた世界は版が変わり、
// 再生は記録の表の中身から同じ環境を作り直す。14 §2「環境ミキサー」)。
// 欄を変えたら、型の定義 data/types/bicameral.d.luau の Environment も変える。ここは CPU だけ(原則 2)。
#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "script/luau_package.h"
#include "sim/world_atmosphere.h"
#include "sim/world_physics.h"

namespace bicameral::script {

    // 環境の分類の名前(パッケージの entry が返す表の鍵)と、値の鍵
    constexpr std::string_view ENVIRONMENT_CATEGORY = "environment";
    constexpr std::string_view ENVIRONMENT_GRAVITY_KEY = "gravity_um_per_s2";
    constexpr std::string_view ENVIRONMENT_SOUND_SPEED_KEY = "sound_speed_mm_per_s";
    constexpr std::string_view
        ENVIRONMENT_GAS_EDDY_VISCOSITY_KEY = "gas_eddy_viscosity_mm2_per_s";  // T-0758・D-516(仮)
    constexpr std::string_view ENVIRONMENT_ATMOSPHERE_TEMPERATURE_KEY = "atmosphere_temperature_mk";
    constexpr std::string_view ENVIRONMENT_ATMOSPHERE_PRESSURE_KEY = "atmosphere_surface_pressure_pa";
    constexpr std::string_view ENVIRONMENT_ATMOSPHERE_PROFILE_KEY = "atmosphere_profile";
    constexpr std::string_view
        ENVIRONMENT_WORLD_EDGES_KEY = "world_edges";  // { top, bottom, sides } = "open" か "wall"(T-0710)
    constexpr std::string_view ATMOSPHERE_CATEGORY = "atmosphere";

    // 合わせた表から世界の物理の値を作る。分類が無ければブランク(全部 0)。誤りは「パッケージ X: environment.鍵: 何が違うか」。
    // ReadReactionTableDefinition がこれを呼び、ベイクした表の environment に入る(T-0398 の共有の決め事)
    [[nodiscard]] std::expected<sim::WorldPhysics, std::string> ReadWorldEnvironment(const PackageSetResult& packages);

    // 基準の大気を名前のまま読む(T-0373・T-0499・ADR-0182)。高さの点の列(environment.atmosphere_profile)か、
    // 一様な書き方(分類 atmosphere と atmosphere_temperature_mk = 高さ 0 の点 1 つ)。両方書いたら誤り。無ければ空。
    // 欄の形(列であること・知らない欄・整数・範囲)はここ、物質の名前・割合の合計・高さの順の検査はベイク(sim::BakeWorldAtmosphere)
    [[nodiscard]] std::expected<sim::WorldAtmosphereDefinition, std::string> ReadWorldAtmosphere(
        const PackageSetResult& packages);

}  // namespace bicameral::script
