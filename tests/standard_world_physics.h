// standard_world_physics.h — 地球の値で書いた試験の場面が使う「標準の値」(重力 9.80665 m/s²・気体の音速の基準 340 m/s)。
// エンジンは中身を持たない(D-454)ので、地球の値は標準パッケージ data/standard/earth_environment にあり、ここは試験だけの写し
// (T-0373 で engine/src/sim/world_physics.h から移した)。standard_packages_test がパッケージの値と同じことを確かめる。
// サンプルの世界(data/packages/combustion_test/environment.luau)も同じ値(environment_package_test)。
#pragma once

#include <cstdint>

#include "sim/world_physics.h"

namespace bicameral::sim {

    inline constexpr uint32_t STANDARD_GRAVITY_MICROMETERS_PER_SECOND2 = 9806650;    // 標準の重力の定義値
    inline constexpr uint32_t STANDARD_SOUND_SPEED_MILLIMETERS_PER_SECOND = 340000;  // D-440: 地面の近くの空気の音速
    // earth_environment の気体の乱れの粘り(仮の値。T-0775・Q121 ②)。StandardWorldPhysics() には入れない: 気体の試験の場面は
    // 粘りの無い世界で取った基準(gas_reference・probe_gas・rest_return など)と比べるので、粘りを試す場面だけが値を書く
    inline constexpr uint32_t STANDARD_GAS_EDDY_VISCOSITY_MM2_PER_S = 200000;

    [[nodiscard]] constexpr WorldPhysics StandardWorldPhysics() {
        return {.gravityMicrometersPerSecond2 = STANDARD_GRAVITY_MICROMETERS_PER_SECOND2,
                .soundSpeedMillimetersPerSecond = STANDARD_SOUND_SPEED_MILLIMETERS_PER_SECOND};
    }

}  // namespace bicameral::sim
