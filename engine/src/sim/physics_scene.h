// physics_scene.h — 物理の原理の確認(T-0016、08 §6 の場面 A・C の小さい版)に使う場面の定義。
// 値はすべて整数(04 §2 の単位)で決め、浮動小数点の試作(tools/physics_lab)と整数のソルバーの両方が同じ場面を読む。
// 試験用の場面で、ゲームの中身ではない(D-006)。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

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

    // 仮の世界の積み木(T-0098): 場面 A と同じ 10 段を、仮の世界の格子の中(物理の座標 x = 10 m・z = 16.25 m。格子の左寄り、
    // 既定の断面を通る位置)に積む。地面は格子の床。窓のクリックで押して崩す
    [[nodiscard]] PhysicsScene MakeProbeStackScene();

    // 質量比: 1 kg の箱の上に 100 kg の箱。10 s
    [[nodiscard]] PhysicsScene MakeMassRatioScene();

    // C 小: 囲い(内側 3 m 四方)の中に大きさ 0.25〜1 m の箱 50 個を 0.25 s ごとに落として山にし、16 s で囲いの壁を 1 枚消して崩す。30 s
    [[nodiscard]] PhysicsScene MakePileScene(uint64_t seed);

}  // namespace bicameral::sim
