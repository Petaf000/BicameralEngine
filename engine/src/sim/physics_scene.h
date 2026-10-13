// physics_scene.h — 物理の原理の確認(T-0016、08 §6 の場面 A・C の小さい版)に使う場面の定義。
// 値はすべて整数(04 §2 の単位)で決め、浮動小数点の試作(tools/physics_lab)と整数のソルバーの両方が同じ場面を読む。
// 試験用の場面で、ゲームの中身ではない(D-006)。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/world_layout.h"

namespace bicameral::sim {

    // 物の 1 つ。形は直方体(T-0016 の範囲。ボクセルの物は T-0043)
    struct PhysicsSceneBody {
        // --- 形 ---
        std::array<int64_t, 3> halfExtent{};  // 半分の辺(2^-20 m)

        // --- 質量(0 = 動かない物: 地面・壁)---
        uint64_t massMilligrams = 0;

        // --- 初めの状態 ---
        std::array<int64_t, 3> position{};                  // 重心(2^-20 m、y が上)
        std::array<int32_t, 4> rotation{0, 0, 0, 1 << 30};  // 四元数 (x, y, z, w)、Q1.30

        // --- 現れる・消える刻み(0 = 初めから / UINT64_MAX = 消えない)---
        uint64_t spawnTick = 0;
        uint64_t removeTick = UINT64_MAX;
    };

    struct PhysicsScene {
        std::string name;
        std::vector<PhysicsSceneBody> bodies;

        uint64_t tickCount = 0;
        uint32_t frictionQ16 = 1u << 15;  // 摩擦係数(Q16.16。全部の組で同じ)
    };

    // 場面 A: 1 m の立方体 10 段(各 500 kg)を地面に積む。60 s
    [[nodiscard]] PhysicsScene MakeStackScene();

    // 表の世界の組み立ての物(パッケージの world_bodies。T-0398・ADR-0172)の場面。60 s。物が無ければ空の場面。
    // 長さ(mm)は 2^-20 m に、質量(g)は mg に直す(割り切れない端は 0 の方へ切る)。向きは回さない。
    // サンプルの世界(data/packages/combustion_test/world_bodies.luau)は仮の世界の積み木(T-0098): 地面と 1 m の立方体 10 段
    [[nodiscard]] PhysicsScene MakeLayoutPhysicsScene(const WorldLayout& layout);

    // 質量比: 1 kg の箱の上に 100 kg の箱。10 s
    [[nodiscard]] PhysicsScene MakeMassRatioScene();

    // C 小: 囲い(内側 3 m 四方)の中に大きさ 0.25〜1 m の箱 50 個を 0.25 s ごとに落として山にし、16 s で囲いの壁を 1 枚消して崩す。30 s
    [[nodiscard]] PhysicsScene MakePileScene(uint64_t seed);

    // 大きな島(T-0094): 地面の上にれんが(1 × 0.5 × 1 m・500 kg)を 20 個 × 15 段、段ごとに半分ずらして積んだ壁。300 個が全部つながる。5 s
    [[nodiscard]] PhysicsScene MakeWallScene();

}  // namespace bicameral::sim
