// box_collision.h — 直方体どうしの接触(試作、double。T-0016)。
// 分離軸(面 6 本 + 辺の組 9 本)で一番浅く重なる向きを選び、面なら参照面に相手の面を切り抜いて多点接触(最大 4 点)、
// 辺どうしなら最も近い 2 点を返す。接触点には特徴の番号を付け、刻みをまたいだ力の引き継ぎ(ウォームスタート)に使う。
#pragma once

#include <array>
#include <cstdint>

#include "lab_math.h"

namespace bicameral::lab {

    struct BoxShape {
        Vec3 center;
        Mat3 rotation;  // 局所 → 世界(列が局所の軸)
        Vec3 halfExtent;
    };

    struct ContactPointGeometry {
        Vec3 pointA;            // A の表面の点(世界)
        Vec3 pointB;            // B の表面の点(世界)
        double separation = 0;  // normal · (pointB − pointA)。負なら食い込み
        uint32_t feature = 0;
    };

    struct ContactGeometry {
        Vec3 normal;  // A → B の向き
        std::array<ContactPointGeometry, 4> points{};
        int count = 0;
    };

    // 離れている距離が margin 以下なら接触を作って true
    bool CollideBoxes(const BoxShape& a, const BoxShape& b, double margin, ContactGeometry& contact);

}  // namespace bicameral::lab
