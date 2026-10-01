// box_collision.cpp — 直方体どうしの接触(box_collision.h)。
// 整数版(shaders/common/physics_*.hlsli)に同じ手順を移すので、分岐と順番は決まった形(番号の小さい順・同点は先のもの)にしている。
#include "box_collision.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace bicameral::lab {

    namespace {

        // 面の軸を辺の軸より優先する(ほぼ同じ深さなら面にする。面の多点接触の方が積み重ねが安定する)
        constexpr double RELATIVE_TOLERANCE = 0.95;
        constexpr double ABSOLUTE_TOLERANCE = 0.005;
        constexpr double PARALLEL_EPSILON = 1e-6;

        double SignOf(double value) {
            return value < 0 ? -1.0 : 1.0;
        }

        // 軸 axis に投影した直方体の半分の幅
        double ProjectedRadius(const BoxShape& box, Vec3 axis) {
            double radius = 0;
            for (int k = 0; k < 3; ++k)
                radius += box.halfExtent[k] * std::abs(Dot(box.rotation.Column(k), axis));

            return radius;
        }

        struct AxisCandidate {
            double separation = -1e300;
            Vec3 normal;    // A → B
            int kind = -1;  // 0 = A の面、1 = B の面、2 = 辺の組
            int indexA = 0;
            int indexB = 0;
        };

        // 軸 axis で調べ、離れていれば false。最大の分離を best に残す
        bool TestAxis(const BoxShape& a, const BoxShape& b, Vec3 axis, double margin, AxisCandidate candidate,
                      AxisCandidate& best) {
            const Vec3 offset = b.center - a.center;
            const double distance = Dot(offset, axis);
            const double separation = std::abs(distance) - ProjectedRadius(a, axis) - ProjectedRadius(b, axis);
            if (separation > margin)
                return false;

            if (separation > best.separation) {
                candidate.separation = separation;
                candidate.normal = axis * SignOf(distance);
                best = candidate;
            }

            return true;
        }

        // --- 面の接触: 相手の面を参照面の 4 辺で切り抜く ---
        // 切り抜きの点の特徴: 点が乗っている 2 つの境界(相手の面の辺 0〜3・参照面の横の面 4〜7)の組(T-0091)。
        // 元の頂点 k は辺 k−1 と辺 k(辺 k = 頂点 k → k+1)、交点は「切られた辺の境界」と「切った面」。
        // 番号は点の位置で一意になる(前は面の番号と頂点の番号のビットが重なり、別の点が同じ番号になっていた)
        struct ClipVertex {
            Vec3 position;
            uint32_t first = 0;   // 小さい方の境界
            uint32_t second = 0;  // 大きい方の境界

            [[nodiscard]] uint32_t Id() const { return first * 8 + second; }
        };

        ClipVertex MakeClipVertex(Vec3 position, uint32_t boundaryA, uint32_t boundaryB) {
            return {position, std::min(boundaryA, boundaryB), std::max(boundaryA, boundaryB)};
        }

        // 隣り合う 2 点が共に乗っている境界(2 点を結ぶ辺の境界)
        uint32_t SharedBoundary(const ClipVertex& a, const ClipVertex& b) {
            if (a.first == b.first || a.first == b.second)
                return a.first;

            return a.second;
        }

        std::vector<ClipVertex> ClipPolygon(const std::vector<ClipVertex>& polygon, Vec3 planeNormal,
                                            double planeOffset, uint32_t planeIndex) {
            std::vector<ClipVertex> result;
            const size_t count = polygon.size();
            for (size_t i = 0; i < count; ++i) {
                const ClipVertex& current = polygon[i];
                const ClipVertex& next = polygon[(i + 1) % count];
                const double currentDistance = Dot(planeNormal, current.position) - planeOffset;
                const double nextDistance = Dot(planeNormal, next.position) - planeOffset;

                if (currentDistance <= 0)
                    result.push_back(current);

                if ((currentDistance <= 0) == (nextDistance <= 0))
                    continue;

                const double t = currentDistance / (currentDistance - nextDistance);
                result.push_back(MakeClipVertex(current.position + (next.position - current.position) * t,
                                                SharedBoundary(current, next), 4 + planeIndex));
            }

            return result;
        }

        // 4 点を超えたら 4 点に減らす: 一番深い点 → それから一番遠い点 → 三角形を最大にする点 → 外に一番はみ出す点
        void ReducePoints(ContactGeometry& contact, const std::vector<ContactPointGeometry>& points) {
            if (points.size() <= 4) {
                for (const ContactPointGeometry& point : points)
                    contact.points[contact.count++] = point;

                return;
            }

            std::array<size_t, 4> chosen{};
            const auto score = [&](auto&& measure) {
                size_t bestIndex = 0;
                double bestValue = -1e300;
                for (size_t i = 0; i < points.size(); ++i) {
                    const double value = measure(points[i].pointA);
                    if (value > bestValue) {
                        bestValue = value;
                        bestIndex = i;
                    }
                }

                return bestIndex;
            };

            for (size_t i = 1; i < points.size(); ++i) {
                if (points[i].separation < points[chosen[0]].separation)
                    chosen[0] = i;
            }

            const Vec3 p0 = points[chosen[0]].pointA;
            chosen[1] = score([&](Vec3 p) { return Dot(p - p0, p - p0); });
            const Vec3 p1 = points[chosen[1]].pointA;
            const Vec3 n = contact.normal;
            chosen[2] = score([&](Vec3 p) { return std::abs(Dot(Cross(p1 - p0, p - p0), n)); });
            const Vec3 p2 = points[chosen[2]].pointA;
            const double orientation = SignOf(Dot(Cross(p1 - p0, p2 - p0), n));
            chosen[3] = score([&](Vec3 p) {
                const double e01 = Dot(Cross(p1 - p0, p - p0), n) * orientation;
                const double e12 = Dot(Cross(p2 - p1, p - p1), n) * orientation;
                const double e20 = Dot(Cross(p0 - p2, p - p2), n) * orientation;
                return -std::min(e01, std::min(e12, e20));
            });

            for (size_t index : chosen)
                contact.points[contact.count++] = points[index];
        }

        void MakeFaceContact(const BoxShape& a, const BoxShape& b, const AxisCandidate& axis, double margin,
                             ContactGeometry& contact) {
            const bool referenceIsA = axis.kind == 0;
            const BoxShape& reference = referenceIsA ? a : b;
            const BoxShape& incident = referenceIsA ? b : a;
            const int referenceAxis = referenceIsA ? axis.indexA : axis.indexB;
            const Vec3 referenceNormal = referenceIsA ? axis.normal : -axis.normal;  // 参照面の外向き(相手の方)

            // 相手の面: 参照面の法線と一番逆を向く面
            int incidentAxis = 0;
            double incidentSign = 1;
            double mostOpposite = 1e300;
            for (int k = 0; k < 3; ++k) {
                const double d = Dot(incident.rotation.Column(k), referenceNormal);
                if (d < mostOpposite) {
                    mostOpposite = d;
                    incidentAxis = k;
                    incidentSign = 1;
                }

                if (-d < mostOpposite) {
                    mostOpposite = -d;
                    incidentAxis = k;
                    incidentSign = -1;
                }
            }

            const int i1 = (incidentAxis + 1) % 3;
            const int i2 = (incidentAxis + 2) % 3;
            const Vec3 incidentCenter = incident.center + incident.rotation.Column(incidentAxis) *
                                                              (incidentSign * incident.halfExtent[incidentAxis]);
            const Vec3 e1 = incident.rotation.Column(i1) * incident.halfExtent[i1];
            const Vec3 e2 = incident.rotation.Column(i2) * incident.halfExtent[i2];
            std::vector<ClipVertex> polygon{
                MakeClipVertex(incidentCenter + e1 + e2, 3, 0), MakeClipVertex(incidentCenter - e1 + e2, 0, 1),
                MakeClipVertex(incidentCenter - e1 - e2, 1, 2), MakeClipVertex(incidentCenter + e1 - e2, 2, 3)};

            // 参照面の 4 辺で切り抜く
            const int r1 = (referenceAxis + 1) % 3;
            const int r2 = (referenceAxis + 2) % 3;
            const Vec3 u1 = reference.rotation.Column(r1);
            const Vec3 u2 = reference.rotation.Column(r2);
            polygon = ClipPolygon(polygon, u1, Dot(u1, reference.center) + reference.halfExtent[r1], 0);
            polygon = ClipPolygon(polygon, -u1, -Dot(u1, reference.center) + reference.halfExtent[r1], 1);
            polygon = ClipPolygon(polygon, u2, Dot(u2, reference.center) + reference.halfExtent[r2], 2);
            polygon = ClipPolygon(polygon, -u2, -Dot(u2, reference.center) + reference.halfExtent[r2], 3);

            // 参照面より下(margin 以内)の点を接触点にする
            const Vec3 faceCenter = reference.center + referenceNormal * reference.halfExtent[referenceAxis];
            const double referenceSign = SignOf(Dot(reference.rotation.Column(referenceAxis), referenceNormal));
            const auto referenceFace = (uint32_t)(referenceAxis * 2 + (referenceSign > 0 ? 0 : 1));
            const auto incidentFace = (uint32_t)(incidentAxis * 2 + (incidentSign > 0 ? 0 : 1));
            const uint32_t faceBits = ((referenceIsA ? 1u : 0u) << 31) | (referenceFace << 26) | (incidentFace << 23);
            std::vector<ContactPointGeometry> points;
            for (const ClipVertex& vertex : polygon) {
                const double separation = Dot(referenceNormal, vertex.position - faceCenter);
                if (separation > margin)
                    continue;

                const Vec3 onReference = vertex.position - referenceNormal * separation;
                const Vec3 pointA = referenceIsA ? onReference : vertex.position;
                const Vec3 pointB = referenceIsA ? vertex.position : onReference;
                points.push_back({pointA, pointB, separation, faceBits | vertex.Id()});
            }

            ReducePoints(contact, points);
        }

        void MakeEdgeContact(const BoxShape& a, const BoxShape& b, const AxisCandidate& axis,
                             ContactGeometry& contact) {
            const Vec3 n = axis.normal;
            Vec3 edgeA = a.center;
            Vec3 edgeB = b.center;
            uint32_t signBits = 0;
            for (int k = 0; k < 3; ++k) {
                if (k != axis.indexA) {
                    const double s = SignOf(Dot(a.rotation.Column(k), n));
                    edgeA += a.rotation.Column(k) * (s * a.halfExtent[k]);
                    signBits |= (s > 0 ? 1u : 0u) << k;
                }

                if (k != axis.indexB) {
                    const double s = -SignOf(Dot(b.rotation.Column(k), n));
                    edgeB += b.rotation.Column(k) * (s * b.halfExtent[k]);
                    signBits |= (s > 0 ? 1u : 0u) << (k + 3);
                }
            }

            // 2 本の直線の最も近い点(線分の範囲に切る)
            const Vec3 directionA = a.rotation.Column(axis.indexA);
            const Vec3 directionB = b.rotation.Column(axis.indexB);
            const Vec3 r = edgeA - edgeB;
            const double cosine = Dot(directionA, directionB);
            const double c = Dot(directionA, r);
            const double f = Dot(directionB, r);
            const double denominator = std::max(1 - cosine * cosine, 1e-12);
            const double limitA = a.halfExtent[axis.indexA];
            const double limitB = b.halfExtent[axis.indexB];
            const double s = std::clamp((cosine * f - c) / denominator, -limitA, limitA);
            const double t = std::clamp(f + s * cosine, -limitB, limitB);

            const Vec3 pointA = edgeA + directionA * s;
            const Vec3 pointB = edgeB + directionB * t;
            const uint32_t feature = (1u << 30) | (signBits << 8) | (uint32_t)(axis.indexA * 3 + axis.indexB);
            contact.points[0] = {pointA, pointB, Dot(n, pointB - pointA), feature};
            contact.count = 1;
        }

    }  // namespace

    bool CollideBoxes(const BoxShape& a, const BoxShape& b, double margin, ContactGeometry& contact) {
        AxisCandidate bestFaceA;
        AxisCandidate bestFaceB;
        AxisCandidate bestEdge;

        for (int i = 0; i < 3; ++i) {
            if (!TestAxis(a, b, a.rotation.Column(i), margin, {.kind = 0, .indexA = i}, bestFaceA))
                return false;
        }

        for (int j = 0; j < 3; ++j) {
            if (!TestAxis(a, b, b.rotation.Column(j), margin, {.kind = 1, .indexB = j}, bestFaceB))
                return false;
        }

        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                const Vec3 axis = Cross(a.rotation.Column(i), b.rotation.Column(j));
                const double length = Length(axis);
                if (length < PARALLEL_EPSILON)
                    continue;

                if (!TestAxis(a, b, axis * (1 / length), margin, {.kind = 2, .indexA = i, .indexB = j}, bestEdge))
                    return false;
            }
        }

        // 面を優先して選ぶ(A の面 → B の面 → 辺)
        AxisCandidate best = bestFaceA;
        if (bestFaceB.separation > RELATIVE_TOLERANCE * best.separation + ABSOLUTE_TOLERANCE)
            best = bestFaceB;

        if (bestEdge.kind == 2 && bestEdge.separation > RELATIVE_TOLERANCE * best.separation + ABSOLUTE_TOLERANCE)
            best = bestEdge;

        // 選んだ軸で点が無ければ(相手の面が参照面の外にはみ出して切り抜きが空)、ほかの軸で作る: 辺の組 → B の面 → A の面
        const std::array<AxisCandidate, 4> order{best, bestEdge, bestFaceB, bestFaceA};
        for (const AxisCandidate& axis : order) {
            if (axis.kind < 0)
                continue;

            contact = {};
            contact.normal = axis.normal;
            if (axis.kind == 2)
                MakeEdgeContact(a, b, axis, contact);
            else
                MakeFaceContact(a, b, axis, margin, contact);

            if (contact.count > 0)
                return true;
        }

        return false;
    }

}  // namespace bicameral::lab
