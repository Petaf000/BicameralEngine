// physics_collision.hlsli — 直方体どうしの接触(整数。08 §2 の 2。T-0016)。手順は試作(tools/physics_lab/box_collision.cpp)と同じ:
// 分離軸(面 6 本 + 辺の組 9 本)で一番浅く重なる向きを選び、面なら参照面に相手の面を切り抜いて最大 4 点、辺どうしなら最も近い 2 点。
// 切り抜きが空なら、ほかの軸で作り直す(T-0091)。
// 同点のときは番号の小さい方(決まった順)。HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。
//
// 単位: 位置・半分の辺・分離 2^-20 m / 回転・法線 Q1.30。計算は A の中心からの相対の位置で行う(値を小さく保つ)。
#ifndef BICAMERAL_PHYSICS_COLLISION_HLSLI
#define BICAMERAL_PHYSICS_COLLISION_HLSLI

#include "physics_math.hlsli"

PX_NAMESPACE_BEGIN

FX_CONST int64_t PX_FACE_RELATIVE_TOLERANCE_Q16 = 62259;  // 0.95: 面の軸を辺の軸より優先する
FX_CONST int64_t PX_FACE_ABSOLUTE_TOLERANCE = 5243;       // 5 mm(2^-20 m)
FX_CONST int64_t PX_PARALLEL_EPSILON = 8192;  // 辺の組の外積の長さがこれ未満(Q1.30 で約 7.6e-6)なら平行とみなす
FX_CONST uint32_t PX_MAX_CONTACT_POINTS = 4;
FX_CONST uint32_t PX_MAX_CLIP_VERTICES = 8;
FX_CONST int64_t PX_MOST_NEGATIVE = -(int64_t)FX_U64(0x7FFFFFFFu, 0xFFFFFFFFu);

struct PxBox {
    PxVec3 center;      // 2^-20 m
    PxMat3 rotation;    // 局所 → 世界(列が局所の軸)、Q1.30
    PxVec3 halfExtent;  // 2^-20 m
};

struct PxContactPointGeometry {
    PxVec3 pointA;       // A の表面の点(世界、2^-20 m)
    PxVec3 pointB;       // B の表面の点
    int64_t separation;  // normal · (pointB − pointA)(2^-20 m)。負なら食い込み
    uint32_t feature;
};

struct PxContactGeometry {
    PxVec3 normal;  // A → B(Q1.30)
    PxContactPointGeometry points[4];
    uint32_t count;
};

struct PxAxisCandidate {
    int64_t separation;
    PxVec3 normal;  // A → B
    uint32_t kind;  // 0 = A の面、1 = B の面、2 = 辺の組、3 = まだ無い
    uint32_t indexA;
    uint32_t indexB;
};

FX_FN PxAxisCandidate PxNoAxis() {
    PxAxisCandidate candidate = {PX_MOST_NEGATIVE, PxMakeVec3(0, 0, 0), 3, 0, 0};
    return candidate;
}

// 軸に投影した直方体の半分の幅(2^-20 m)
FX_FN int64_t PxProjectedRadius(PxBox box, PxVec3 axis) {
    int64_t radius = 0;
    for (uint32_t k = 0; k < 3; ++k) {
        radius += FxMulShiftS64(PxGet(box.halfExtent, k), PxAbs(PxDot(PxColumn(box.rotation, k), axis, PX_UNIT_SHIFT)),
                                PX_UNIT_SHIFT);
    }

    return radius;
}

// 軸 axis での分離(2^-20 m)。offset = B の中心 − A の中心
FX_FN PxAxisCandidate PxTestAxis(PxBox a, PxBox b, PxVec3 offset, PxVec3 axis, uint32_t kind, uint32_t indexA,
                                 uint32_t indexB) {
    const int64_t distance = PxDot(offset, axis, PX_UNIT_SHIFT);
    PxAxisCandidate candidate;
    candidate.separation = PxAbs(distance) - PxProjectedRadius(a, axis) - PxProjectedRadius(b, axis);
    candidate.normal = distance < 0 ? PxNegate(axis) : axis;
    candidate.kind = kind;
    candidate.indexA = indexA;
    candidate.indexB = indexB;

    return candidate;
}

FX_FN bool PxPrefer(PxAxisCandidate candidate, PxAxisCandidate best) {
    return candidate.separation >
           FxMulShiftS64(best.separation, PX_FACE_RELATIVE_TOLERANCE_Q16, 16) + PX_FACE_ABSOLUTE_TOLERANCE;
}

// --- 面の接触: 相手の面の 4 頂点を、参照面の 4 辺の平面で切り抜く ---
// 点の番号(特徴)は、点が乗っている 2 つの境界(相手の面の辺 0〜3・参照面の横の面 4〜7)の組 = 小さい方 × 8 + 大きい方(T-0091)。
// 元の頂点 k は辺 k−1 と辺 k(辺 k = 頂点 k → k+1)、交点は「切られた辺の境界」と「切った面」。番号は点の位置で一意になる
FX_FN uint32_t PxClipId(uint32_t boundaryA, uint32_t boundaryB) {
    return boundaryA < boundaryB ? boundaryA * 8 + boundaryB : boundaryB * 8 + boundaryA;
}

// 隣り合う 2 点(番号 idA・idB)が共に乗っている境界(2 点を結ぶ辺の境界)
FX_FN uint32_t PxSharedBoundary(uint32_t idA, uint32_t idB) {
    const uint32_t firstA = idA >> 3;
    if (firstA == (idB >> 3) || firstA == (idB & 7u))
        return firstA;

    return idA & 7u;
}

struct PxPolygon {
    PxVec3 position[8];
    uint32_t id[8];
    uint32_t count;
};

// 平面 normal · p ≤ offset の側を残す(Sutherland-Hodgman)
FX_FN PxPolygon PxClipPolygon(PxPolygon polygon, PxVec3 normal, int64_t offset, uint32_t planeIndex) {
    PxPolygon result;
    result.count = 0;
    for (uint32_t i = 0; i < polygon.count; ++i) {
        const uint32_t nextIndex = i + 1 == polygon.count ? 0 : i + 1;
        const PxVec3 current = polygon.position[i];
        const PxVec3 next = polygon.position[nextIndex];
        const int64_t currentDistance = PxDot(normal, current, PX_UNIT_SHIFT) - offset;
        const int64_t nextDistance = PxDot(normal, next, PX_UNIT_SHIFT) - offset;
        const bool currentInside = currentDistance <= 0;

        if (currentInside && result.count < PX_MAX_CLIP_VERTICES) {
            result.position[result.count] = current;
            result.id[result.count] = polygon.id[i];
            result.count += 1;
        }

        if (currentInside == (nextDistance <= 0) || result.count >= PX_MAX_CLIP_VERTICES)
            continue;

        // 交点 = current + (next − current) × d_c / (d_c − d_n)
        const PxVec3 delta = PxSub(next, current);
        const int64_t denominator = currentDistance - nextDistance;
        const PxVec3 step = PxMakeVec3(FxDivS64(delta.x * currentDistance, denominator),
                                       FxDivS64(delta.y * currentDistance, denominator),
                                       FxDivS64(delta.z * currentDistance, denominator));
        result.position[result.count] = PxAdd(current, step);
        result.id[result.count] = PxClipId(PxSharedBoundary(polygon.id[i], polygon.id[nextIndex]), 4 + planeIndex);
        result.count += 1;
    }

    return result;
}

// 3 点が作る向きつきの面積 × 2(法線の向きに投影。2^-40 m²)
FX_FN int64_t PxSignedArea(PxVec3 origin, PxVec3 p, PxVec3 q, PxVec3 normal) {
    return PxDot(PxCross(PxSub(p, origin), PxSub(q, origin), 0), normal, PX_UNIT_SHIFT);
}

// 4 点を超えたら 4 点に減らす: 一番深い点 → それから一番遠い点 → 三角形を最大にする点 → 外に一番はみ出す点
FX_FN PxContactGeometry PxReducePoints(PxContactGeometry contact, PxContactPointGeometry points[8], uint32_t count) {
    if (count <= PX_MAX_CONTACT_POINTS) {
        for (uint32_t i = 0; i < count; ++i)
            contact.points[i] = points[i];

        contact.count = count;
        return contact;
    }

    uint32_t chosen[4] = {0, 0, 0, 0};
    for (uint32_t i = 1; i < count; ++i) {
        if (points[i].separation < points[chosen[0]].separation)
            chosen[0] = i;
    }

    const PxVec3 p0 = points[chosen[0]].pointA;
    int64_t best = -1;
    for (uint32_t i = 0; i < count; ++i) {
        const PxVec3 diff = PxSub(points[i].pointA, p0);
        const int64_t value = PxDot(diff, diff, 0);
        if (value > best) {
            best = value;
            chosen[1] = i;
        }
    }

    const PxVec3 p1 = points[chosen[1]].pointA;
    best = -1;
    for (uint32_t i = 0; i < count; ++i) {
        const int64_t value = PxAbs(PxSignedArea(p0, p1, points[i].pointA, contact.normal));
        if (value > best) {
            best = value;
            chosen[2] = i;
        }
    }

    const PxVec3 p2 = points[chosen[2]].pointA;
    const int64_t orientation = PxSignedArea(p0, p1, p2, contact.normal) < 0 ? -1 : 1;
    best = PX_MOST_NEGATIVE;
    for (uint32_t i = 0; i < count; ++i) {
        const PxVec3 p = points[i].pointA;
        const int64_t e01 = PxSignedArea(p0, p1, p, contact.normal) * orientation;
        const int64_t e12 = PxSignedArea(p1, p2, p, contact.normal) * orientation;
        const int64_t e20 = PxSignedArea(p2, p0, p, contact.normal) * orientation;
        const int64_t value = -PxMin(e01, PxMin(e12, e20));
        if (value > best) {
            best = value;
            chosen[3] = i;
        }
    }

    for (uint32_t k = 0; k < 4; ++k)
        contact.points[k] = points[chosen[k]];

    contact.count = 4;
    return contact;
}

// 相手の面: 参照面の法線と一番逆を向く面(同点は番号の小さい方、+ を先に)
struct PxFace {
    uint32_t axis;
    int64_t sign;
};

FX_FN PxFace PxFindIncidentFace(PxBox incident, PxVec3 referenceNormal) {
    PxFace face = {0, 1};
    int64_t mostOpposite = FX_U64(0x7FFFFFFFu, 0xFFFFFFFFu);
    for (uint32_t k = 0; k < 3; ++k) {
        const int64_t d = PxDot(PxColumn(incident.rotation, k), referenceNormal, PX_UNIT_SHIFT);
        if (d < mostOpposite) {
            mostOpposite = d;
            face.axis = k;
            face.sign = 1;
        }

        if (-d < mostOpposite) {
            mostOpposite = -d;
            face.axis = k;
            face.sign = -1;
        }
    }

    return face;
}

// 相手の面の 4 頂点(番号は境界の組。PxClipId)
FX_FN PxPolygon PxFacePolygon(PxBox box, PxFace face) {
    const uint32_t i1 = (face.axis + 1) % 3;
    const uint32_t i2 = (face.axis + 2) % 3;
    const PxVec3 toFace = PxScale(PxColumn(box.rotation, face.axis), face.sign * PxGet(box.halfExtent, face.axis),
                                  PX_UNIT_SHIFT);
    const PxVec3 center = PxAdd(box.center, toFace);
    const PxVec3 e1 = PxScale(PxColumn(box.rotation, i1), PxGet(box.halfExtent, i1), PX_UNIT_SHIFT);
    const PxVec3 e2 = PxScale(PxColumn(box.rotation, i2), PxGet(box.halfExtent, i2), PX_UNIT_SHIFT);

    PxPolygon polygon = PX_ZERO(PxPolygon);
    polygon.count = 4;
    polygon.position[0] = PxAdd(PxAdd(center, e1), e2);
    polygon.position[1] = PxAdd(PxSub(center, e1), e2);
    polygon.position[2] = PxSub(PxSub(center, e1), e2);
    polygon.position[3] = PxSub(PxAdd(center, e1), e2);
    polygon.id[0] = PxClipId(3, 0);
    polygon.id[1] = PxClipId(0, 1);
    polygon.id[2] = PxClipId(1, 2);
    polygon.id[3] = PxClipId(2, 3);

    return polygon;
}

// 参照面の 4 辺の平面で切り抜く
FX_FN PxPolygon PxClipToReferenceSides(PxPolygon polygon, PxBox reference, uint32_t referenceAxis) {
    const uint32_t r1 = (referenceAxis + 1) % 3;
    const uint32_t r2 = (referenceAxis + 2) % 3;
    const PxVec3 u1 = PxColumn(reference.rotation, r1);
    const PxVec3 u2 = PxColumn(reference.rotation, r2);
    const int64_t c1 = PxDot(u1, reference.center, PX_UNIT_SHIFT);
    const int64_t c2 = PxDot(u2, reference.center, PX_UNIT_SHIFT);
    polygon = PxClipPolygon(polygon, u1, c1 + PxGet(reference.halfExtent, r1), 0);
    polygon = PxClipPolygon(polygon, PxNegate(u1), -c1 + PxGet(reference.halfExtent, r1), 1);
    polygon = PxClipPolygon(polygon, u2, c2 + PxGet(reference.halfExtent, r2), 2);

    return PxClipPolygon(polygon, PxNegate(u2), -c2 + PxGet(reference.halfExtent, r2), 3);
}

// 面の接触(a は原点に置いた A、b は A の中心からの相対の B)
FX_FN PxContactGeometry PxMakeFaceContact(PxBox a, PxBox b, PxAxisCandidate axis, int64_t margin) {
    const bool referenceIsA = axis.kind == 0;
    const PxBox reference = referenceIsA ? a : b;
    const PxBox incident = referenceIsA ? b : a;
    const uint32_t referenceAxis = referenceIsA ? axis.indexA : axis.indexB;
    const PxVec3 referenceNormal = referenceIsA ? axis.normal : PxNegate(axis.normal);  // 参照面の外向き(相手の方)
    const PxFace incidentFace = PxFindIncidentFace(incident, referenceNormal);
    const PxPolygon polygon = PxClipToReferenceSides(PxFacePolygon(incident, incidentFace), reference, referenceAxis);

    // 特徴の番号: 参照が A か・参照面・相手の面・頂点(切り抜きでできた点は辺と平面から)
    const int64_t referenceSign = PxDot(PxColumn(reference.rotation, referenceAxis), referenceNormal, PX_UNIT_SHIFT) < 0
                                      ? -1
                                      : 1;
    const uint32_t referenceFace = referenceAxis * 2 + (referenceSign > 0 ? 0u : 1u);
    const uint32_t incidentFaceId = incidentFace.axis * 2 + (incidentFace.sign > 0 ? 0u : 1u);
    const uint32_t faceBits = ((referenceIsA ? 1u : 0u) << 31) | (referenceFace << 26) | (incidentFaceId << 23);

    // 参照面より下(margin 以内)の点を接触点にする
    const PxVec3 faceCenter = PxAdd(
        reference.center, PxScale(referenceNormal, PxGet(reference.halfExtent, referenceAxis), PX_UNIT_SHIFT));
    PxContactPointGeometry points[8];
    uint32_t count = 0;
    for (uint32_t v = 0; v < polygon.count; ++v) {
        const PxVec3 position = polygon.position[v];
        const int64_t separation = PxDot(referenceNormal, PxSub(position, faceCenter), PX_UNIT_SHIFT);
        if (separation > margin)
            continue;

        const PxVec3 onReference = PxSub(position, PxScale(referenceNormal, separation, PX_UNIT_SHIFT));
        points[count].pointA = referenceIsA ? onReference : position;
        points[count].pointB = referenceIsA ? position : onReference;
        points[count].separation = separation;
        points[count].feature = faceBits | polygon.id[v];
        count += 1;
    }

    PxContactGeometry contact = PX_ZERO(PxContactGeometry);
    contact.normal = axis.normal;

    return PxReducePoints(contact, points, count);
}

// 辺どうしの接触: 2 本の辺の最も近い 2 点(a は原点に置いた A、b は相対の B)
FX_FN PxContactGeometry PxMakeEdgeContact(PxBox a, PxBox b, PxAxisCandidate axis) {
    const PxVec3 n = axis.normal;
    PxVec3 edgeA = a.center;
    PxVec3 edgeB = b.center;
    uint32_t signBits = 0;
    for (uint32_t k = 0; k < 3; ++k) {
        if (k != axis.indexA) {
            const int64_t s = PxDot(PxColumn(a.rotation, k), n, PX_UNIT_SHIFT) < 0 ? -1 : 1;
            edgeA = PxAdd(edgeA, PxScale(PxColumn(a.rotation, k), s * PxGet(a.halfExtent, k), PX_UNIT_SHIFT));
            signBits |= (s > 0 ? 1u : 0u) << k;
        }

        if (k != axis.indexB) {
            const int64_t s = PxDot(PxColumn(b.rotation, k), n, PX_UNIT_SHIFT) < 0 ? 1 : -1;
            edgeB = PxAdd(edgeB, PxScale(PxColumn(b.rotation, k), s * PxGet(b.halfExtent, k), PX_UNIT_SHIFT));
            signBits |= (s > 0 ? 1u : 0u) << (k + 3);
        }
    }

    // 2 本の直線の最も近い点(線分の範囲に切る)
    const PxVec3 directionA = PxColumn(a.rotation, axis.indexA);
    const PxVec3 directionB = PxColumn(b.rotation, axis.indexB);
    const PxVec3 r = PxSub(edgeA, edgeB);
    const int64_t cosine = PxDot(directionA, directionB, PX_UNIT_SHIFT);
    const int64_t c = PxDot(directionA, r, PX_UNIT_SHIFT);
    const int64_t f = PxDot(directionB, r, PX_UNIT_SHIFT);
    const int64_t denominator = PxMax(PX_UNIT_ONE - FxMulShiftS64(cosine, cosine, PX_UNIT_SHIFT), 1);
    const int64_t limitA = PxGet(a.halfExtent, axis.indexA);
    const int64_t limitB = PxGet(b.halfExtent, axis.indexB);
    const int64_t s = PxClamp(FxDivShiftS64(FxMulShiftS64(cosine, f, PX_UNIT_SHIFT) - c, denominator, PX_UNIT_SHIFT),
                              -limitA, limitA);
    const int64_t t = PxClamp(f + FxMulShiftS64(s, cosine, PX_UNIT_SHIFT), -limitB, limitB);

    PxContactGeometry contact;
    contact.normal = n;
    contact.points[0].pointA = PxAdd(edgeA, PxScale(directionA, s, PX_UNIT_SHIFT));
    contact.points[0].pointB = PxAdd(edgeB, PxScale(directionB, t, PX_UNIT_SHIFT));
    contact.points[0].separation = PxDot(n, PxSub(contact.points[0].pointB, contact.points[0].pointA), PX_UNIT_SHIFT);
    contact.points[0].feature = (1u << 30) | (signBits << 8) | (axis.indexA * 3 + axis.indexB);
    contact.count = 1;

    return contact;
}

// 接触を作る軸の試す順(attempt 番目): 選んだ軸 → 辺の組 → B の面 → A の面
FX_FN PxAxisCandidate PxFallbackAxis(uint32_t attempt, PxAxisCandidate best, PxAxisCandidate edge,
                                     PxAxisCandidate faceB, PxAxisCandidate faceA) {
    if (attempt == 0)
        return best;

    if (attempt == 1)
        return edge;

    if (attempt == 2)
        return faceB;

    return faceA;
}

// 離れている距離が margin(2^-20 m)以下なら接触(count > 0)を返す。点は世界の位置
FX_FN PxContactGeometry PxCollideBoxes(PxBox a, PxBox b, int64_t margin) {
    PxContactGeometry contact;
    contact.normal = PxMakeVec3(0, 0, 0);
    contact.count = 0;

    // A の中心を原点にする
    const PxVec3 origin = a.center;
    b.center = PxSub(b.center, origin);
    a.center = PxMakeVec3(0, 0, 0);
    const PxVec3 offset = b.center;

    PxAxisCandidate bestFaceA = PxNoAxis();
    PxAxisCandidate bestFaceB = PxNoAxis();
    PxAxisCandidate bestEdge = PxNoAxis();
    bool separated = false;
    for (uint32_t i = 0; i < 3; ++i) {
        const PxAxisCandidate faceA = PxTestAxis(a, b, offset, PxColumn(a.rotation, i), 0, i, 0);
        const PxAxisCandidate faceB = PxTestAxis(a, b, offset, PxColumn(b.rotation, i), 1, 0, i);
        separated = separated || faceA.separation > margin || faceB.separation > margin;
        if (faceA.separation > bestFaceA.separation)
            bestFaceA = faceA;

        if (faceB.separation > bestFaceB.separation)
            bestFaceB = faceB;
    }

    for (uint32_t i = 0; i < 9 && !separated; ++i) {
        const PxVec3 cross = PxCross(PxColumn(a.rotation, i / 3), PxColumn(b.rotation, i % 3), PX_UNIT_SHIFT);
        if ((int64_t)PxLength(cross) < PX_PARALLEL_EPSILON)
            continue;

        const PxAxisCandidate edge = PxTestAxis(a, b, offset, PxNormalize(cross), 2, i / 3, i % 3);
        separated = separated || edge.separation > margin;
        if (edge.separation > bestEdge.separation)
            bestEdge = edge;
    }

    if (separated)
        return contact;

    // 面を優先して選ぶ(A の面 → B の面 → 辺)
    PxAxisCandidate best = bestFaceA;
    if (PxPrefer(bestFaceB, best))
        best = bestFaceB;

    if (bestEdge.kind == 2 && PxPrefer(bestEdge, best))
        best = bestEdge;

    // 選んだ軸で点が無ければ(相手の面が参照面の外にはみ出して切り抜きが空)、ほかの軸で作る: 辺の組 → B の面 → A の面(T-0091)
    for (uint32_t attempt = 0; attempt < 4 && contact.count == 0; ++attempt) {
        const PxAxisCandidate axis = PxFallbackAxis(attempt, best, bestEdge, bestFaceB, bestFaceA);
        if (axis.kind == 2)
            contact = PxMakeEdgeContact(a, b, axis);
        else if (axis.kind < 2)
            contact = PxMakeFaceContact(a, b, axis, margin);
    }

    // 世界の位置に戻す
    for (uint32_t k = 0; k < contact.count; ++k) {
        contact.points[k].pointA = PxAdd(contact.points[k].pointA, origin);
        contact.points[k].pointB = PxAdd(contact.points[k].pointB, origin);
    }

    return contact;
}

PX_NAMESPACE_END

#endif  // BICAMERAL_PHYSICS_COLLISION_HLSLI
