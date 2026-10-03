// physics_scene.cpp — 物理の原理の確認の場面(physics_scene.h)。乱数は FxHash64(R6)なので、同じ種からは同じ場面になる。
#include "sim/physics_scene.h"

#include "common/fixed.hlsli"
#include "common/units.hlsli"

namespace bicameral::sim {

    namespace {

        using namespace bicameral::fx;

        constexpr int64_t METER = (int64_t)1 << POSITION_FRACTION_BITS;
        constexpr uint64_t MILLIGRAMS_PER_KG = 1000000;

        // --- 乱数の用途(FxHash64)---
        constexpr uint32_t PURPOSE_SIZE = 0x50480001u;
        constexpr uint32_t PURPOSE_DENSITY = 0x50480002u;
        constexpr uint32_t PURPOSE_ANGLE = 0x50480003u;
        constexpr uint32_t PURPOSE_JITTER = 0x50480004u;

        // 辺の長さ(mm)から半分の辺(2^-20 m)
        int64_t HalfExtentFromMillimeters(int64_t sideMillimeters) {
            return (sideMillimeters * METER) / 2000;
        }

        // 地面: 100 m 四方・厚さ 1 m、上の面が y = 0
        PhysicsSceneBody MakeGround() {
            return {
                .halfExtent = {50 * METER, METER / 2, 50 * METER},
                .position = {0, -METER / 2, 0},
            };
        }

        // 立方体(辺 1 m)を、底が y = bottom(2^-20 m)になる高さに置く
        PhysicsSceneBody MakeMeterCube(uint64_t massKg, int64_t bottom) {
            return {
                .halfExtent = {METER / 2, METER / 2, METER / 2},
                .massMilligrams = massKg * MILLIGRAMS_PER_KG,
                .position = {0, bottom + METER / 2, 0},
            };
        }

        // Q1.30 の四元数の積 a × b
        std::array<int32_t, 4> MultiplyQuaternion(const std::array<int32_t, 4>& a, const std::array<int32_t, 4>& b) {
            const auto product = [](int32_t x, int32_t y) {
                return (int64_t)x * y;
            };
            const int64_t x = product(a[3], b[0]) + product(a[0], b[3]) + product(a[1], b[2]) - product(a[2], b[1]);
            const int64_t y = product(a[3], b[1]) - product(a[0], b[2]) + product(a[1], b[3]) + product(a[2], b[0]);
            const int64_t z = product(a[3], b[2]) + product(a[0], b[1]) - product(a[1], b[0]) + product(a[2], b[3]);
            const int64_t w = product(a[3], b[3]) - product(a[0], b[0]) - product(a[1], b[1]) - product(a[2], b[2]);

            return {(int32_t)(x >> 30), (int32_t)(y >> 30), (int32_t)(z >> 30), (int32_t)(w >> 30)};
        }

        // 軸(0 = x, 1 = y, 2 = z)まわりに angle(1 周 = 2^32)回す四元数
        std::array<int32_t, 4> AxisRotation(uint32_t axis, uint32_t angle) {
            const FxSinCos halfAngle = FxSinCosTurn32(angle / 2);
            std::array<int32_t, 4> rotation{0, 0, 0, halfAngle.cosine};
            rotation[axis] = halfAngle.sine;

            return rotation;
        }

        // 乱数の向き(y → x → z の順に回す)
        std::array<int32_t, 4> RandomRotation(uint64_t seed, uint64_t id) {
            const auto angle = [&](uint32_t axis) {
                return (uint32_t)FxHash64(seed, axis, id, PURPOSE_ANGLE);
            };
            const auto yaw = AxisRotation(1, angle(1));
            const auto pitch = AxisRotation(0, angle(0));
            const auto roll = AxisRotation(2, angle(2));

            return MultiplyQuaternion(roll, MultiplyQuaternion(pitch, yaw));
        }

        // 囲い: 内側 3 m 四方、壁は厚さ 0.2 m・高さ 3 m。番号 0 の壁(+x)を後で消す
        void AddPen(PhysicsScene& scene, uint64_t removeTick) {
            constexpr int64_t INNER_HALF = 3 * METER / 2;
            constexpr int64_t THICK_HALF = METER / 10;
            constexpr int64_t HEIGHT_HALF = 3 * METER / 2;
            constexpr int64_t OFFSET = INNER_HALF + THICK_HALF;
            constexpr int64_t LONG_HALF = INNER_HALF + 2 * THICK_HALF;

            const std::array<PhysicsSceneBody, 4> walls{{
                {.halfExtent = {THICK_HALF, HEIGHT_HALF, LONG_HALF}, .position = {OFFSET, HEIGHT_HALF, 0}},
                {.halfExtent = {THICK_HALF, HEIGHT_HALF, LONG_HALF}, .position = {-OFFSET, HEIGHT_HALF, 0}},
                {.halfExtent = {LONG_HALF, HEIGHT_HALF, THICK_HALF}, .position = {0, HEIGHT_HALF, OFFSET}},
                {.halfExtent = {LONG_HALF, HEIGHT_HALF, THICK_HALF}, .position = {0, HEIGHT_HALF, -OFFSET}},
            }};

            for (const PhysicsSceneBody& wall : walls)
                scene.bodies.push_back(wall);

            scene.bodies[scene.bodies.size() - 4].removeTick = removeTick;
        }

        // 落とす箱 1 つ: 辺 250〜1000 mm、密度は木(300 kg/m³)か岩(2700 kg/m³)
        PhysicsSceneBody MakeFallingBox(uint64_t seed, uint64_t index) {
            std::array<int64_t, 3> sideMillimeters{};
            for (uint32_t axis = 0; axis < 3; ++axis)
                sideMillimeters[axis] = 250 + FxRandomBelow(FxHash64(seed, axis, index, PURPOSE_SIZE), 751);

            const bool isRock = FxRandomBelow(FxHash64(seed, 0, index, PURPOSE_DENSITY), 2) == 1;
            const uint64_t densityKgPerCubicMeter = isRock ? 2700 : 300;
            const auto volumeCubicMillimeters = (uint64_t)(sideMillimeters[0] * sideMillimeters[1] *
                                                           sideMillimeters[2]);

            // 落とす場所: 4 か所を対角に跳びながら回り、少しずらす(±5 cm)。
            // 続けて落とす 2 つは対角(1.98 m 離れる)、2 つ前は隣(1.4 m)だが 0.5 s 先に落ちているので、外接球(半径 0.87 m)が重ならない
            constexpr std::array<std::array<int64_t, 2>, 4> COLUMNS{{{1, 1}, {-1, -1}, {-1, 1}, {1, -1}}};
            const auto& column = COLUMNS[index % 4];
            const auto jitter = [&](uint32_t axis) {
                return (int64_t)FxRandomBelow(FxHash64(seed, axis, index, PURPOSE_JITTER), (uint32_t)(METER / 10)) -
                       METER / 20;
            };

            return {
                .halfExtent = {HalfExtentFromMillimeters(sideMillimeters[0]),
                               HalfExtentFromMillimeters(sideMillimeters[1]),
                               HalfExtentFromMillimeters(sideMillimeters[2])},
                .massMilligrams = densityKgPerCubicMeter * volumeCubicMillimeters / 1000,
                .position = {column[0] * 7 * METER / 10 + jitter(0), 7 * METER, column[1] * 7 * METER / 10 + jitter(2)},
                .rotation = RandomRotation(seed, index),
            };
        }

    }  // namespace

    PhysicsScene MakeStackScene() {
        PhysicsScene scene{.name = "stack", .tickCount = (uint64_t)60 * TICKS_PER_SECOND};
        scene.bodies.push_back(MakeGround());

        for (int64_t level = 0; level < 10; ++level)
            scene.bodies.push_back(MakeMeterCube(500, level * METER));

        return scene;
    }

    PhysicsScene MakeProbeStackScene() {
        // 格子の座標で x = 20・z = 32.5(既定の断面 z = 32 のセルの真ん中)の柱。地面は格子の床を覆う
        constexpr int64_t TOWER_X = 10 * METER;
        constexpr int64_t TOWER_Z = 16 * METER + METER / 4;

        PhysicsScene scene{.name = "probe_stack", .tickCount = (uint64_t)60 * TICKS_PER_SECOND};
        PhysicsSceneBody ground = MakeGround();
        ground.position = {16 * METER, -METER / 2, 16 * METER};
        scene.bodies.push_back(ground);

        for (int64_t level = 0; level < 10; ++level) {
            PhysicsSceneBody cube = MakeMeterCube(500, level * METER);
            cube.position[0] = TOWER_X;
            cube.position[2] = TOWER_Z;
            scene.bodies.push_back(cube);
        }

        return scene;
    }

    PhysicsScene MakeMassRatioScene() {
        PhysicsScene scene{.name = "mass_ratio", .tickCount = (uint64_t)10 * TICKS_PER_SECOND};
        scene.bodies.push_back(MakeGround());
        scene.bodies.push_back(MakeMeterCube(1, 0));
        scene.bodies.push_back(MakeMeterCube(100, METER));

        return scene;
    }

    PhysicsScene MakePileScene(uint64_t seed) {
        // 0.25 s ごとに 1 つ落とす(50 個で 12.5 s)→ 16 s で壁を消す → 30 s まで
        constexpr uint64_t BOX_COUNT = 50;
        constexpr uint64_t SPAWN_INTERVAL_TICKS = 15;
        constexpr uint64_t REMOVE_TICK = (uint64_t)16 * TICKS_PER_SECOND;

        PhysicsScene scene{.name = "pile", .tickCount = (uint64_t)30 * TICKS_PER_SECOND};
        scene.bodies.push_back(MakeGround());
        AddPen(scene, REMOVE_TICK);

        for (uint64_t index = 0; index < BOX_COUNT; ++index) {
            PhysicsSceneBody box = MakeFallingBox(seed, index);
            box.spawnTick = index * SPAWN_INTERVAL_TICKS;
            scene.bodies.push_back(box);
        }

        return scene;
    }

}  // namespace bicameral::sim
