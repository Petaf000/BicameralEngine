// object_flow_cpu.h — 物の箱の中の気体の流れ(D-667 の 2 段目・T-1143)の CPU リファレンス。式は shaders/common/object_heat.hlsli の「層の流れ」
// (OhFlowCellOf・OhFlowExcessQ16・OhFlowBuoyancy・OhFlowFaceWeight・OhFlowShareQ16・OhFlowMoved)。GPU の形は後(T-1143 の作業ログ)。
// データの流れ(流れの小刻みごと。どれも小刻みの初めの値から計算する = Jacobi。圧力だけ赤黒の反復):
//   見立て(ボクセルごと: 気体の質量・pV・気体の割合・膨らみたい体積)→ 面の速さ q = M ÷ m_f
//   → ① 運動量を移した質量に比例して同じ軸の隣の面へ(D-663。箱の外の面の向こう = 囲むセル = 止まった空気)
//   → ② 縦の面に浮力 → ③ 圧力の射影(箱の外 = 囲むセルの圧力。膨らみたい体積をこの小刻みで出す)
//   → ④ 風上の気体を体積の割合で移す(1 小刻みに出るのは半分まで)。箱の外へ出る・入る分は対の端数を通してセルへ(帳簿「物との受け渡し」)。
// 骨格のボクセル: 既定は壁(骨格に面する面は運動量を持たず、骨格の気体の膨らみ・縮みだけが圧力の射影で出入りする = 吹き出す多孔の壁)。
// skeletonWalls = false なら骨格の中も気体として流れる(気体の割合 = 凝縮相の体積を除いた分。比べ)。
// 真実: 面の運動量(OhShellFlow::momentum。層を作った時に 0 = 止まった空気、返す時に捨てる)。運動のエネルギーは数えない(音より遅い極限)。
// 圧力の反復(T-1201): 既定は毎小刻み 0 から解き始め(pressureWarmStart で前の小刻みの値から。層の状態)、1 回の赤黒で値が 1 つも変わらなければ止める
//   (= 整数の反復の動かない点。上限 pressureSweeps)。止まった時の値はその整数の式の答えそのもの(決まった回数で打ち切らない。D-428)。
//   回数は状態だけで決まるので決定的(GPU も同じ規則。T-1201)。pressureUntilQuiet = false・pressureWarmStart = false で T-1200 までの形(0 から決まった回数)。
//   pressureMultigrid(T-1202)で解き始めを多重格子で作る(PressureMultigrid。最後は同じ赤黒を収まるまで。収まるまでの赤黒の費用が 1/3〜1/9)。
// 保存: 移すのは対の流れだけ(片側から引いた量を反対側に足す)なので、セル + ボクセル + 対の端数はビット単位で変わらない。
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <vector>

#include "object_shell_cpu.h"

namespace bicameral::test {

    struct OhShellFlow {
        uint32_t substeps = 4;  // 1 刻みの流れの小刻み(1/240 s。速さ 1 m/s で 1 小刻みに 0.53 ボクセル)
        uint32_t pressureSweeps =
            1024;  // 圧力の赤黒の反復の上限(pressureUntilQuiet = false なら決まった回数。T-1200 までは 24)
        bool pressureUntilQuiet = true;  // 1 回の赤黒で圧力の値が 1 つも変わらなければ止める(T-1201)
        // 前の小刻みの圧力の値から解き始める(T-1201。既定 false = 毎小刻み 0 から。温めても回数は減らず 1〜2 割増えた)
        bool pressureWarmStart = false;
        std::vector<int64_t> potential;                // 圧力の解き始めの値(層の状態。層を作った時に 0、返す時に捨てる)
        std::array<std::vector<int64_t>, 3> momentum;  // 軸ごとの面(+ 向きが正。質量 × q)
        uint64_t limitedShares = 0;                    // 数えるだけ: 出る上限で縮めた(ボクセル × 小刻み)
        int64_t fastestQ16 = 0;                        // 数えるだけ: 一番速い面(Q16 ボクセル / 小刻み)
        // 数えるだけ(T-1201): 圧力を解いた回数・赤黒の回数の和・一番多い回数・上限で止まった回数
        uint64_t pressureSolves = 0;
        uint64_t pressureSweepsUsed = 0;
        uint32_t pressureMostSweeps = 0;
        uint64_t pressureCapped = 0;
        // 解き始めを多重格子で作る(T-1202。最後は同じ赤黒を収まるまで)・その費用の和(細かい格子の赤黒 1 回 = 1000)
        bool pressureMultigrid = false;
        // 調べるだけ(T-1201): 「収まった」とみなす 1 回の赤黒の値の変わりの上限(0 = 1 つも変わらない。GPU は 0 だけ)
        int64_t pressureQuietTolerance = 0;
        uint64_t pressureMultigridWork = 0;
        // 数えるだけ(T-1202): 収まるまでの赤黒を「隣が変わったボクセルだけ」回した時の計算の数(細かい格子の赤黒 1 回 = 1000。答えは同じ)
        uint64_t pressureActiveWork = 0;
        // 調べるだけ(T-1201。0 = しない): 0 から解く時、この回数の時の値と止まった時の値の、面の速さの差の最大と速さの最大(Q16)
        uint32_t gapSweeps = 0;
        int64_t gapSpeedQ16 = 0;
        int64_t gapReferenceQ16 = 0;
        // 骨格に面する面: true = 壁(運動量を持たず、骨格の膨らみ・縮みの分だけ圧力の射影で出入りする = 吹き出す多孔の壁)/
        // false = 骨格の中も気体として流れる(紙のボクセルの気体と固体は 1 つの温度なので、流れ込んだ炎の気体と紙がすぐ同じ温度になる)
        bool skeletonWalls = true;
        // 調べるだけ(T-1202): pressureDumpEvery 回に 1 回、圧力の問題(箱・面の重み・残り)と止まった答え・回数を書き出す(多重格子の研究用)
        std::ostream* pressureDump = nullptr;
        uint32_t pressureDumpEvery = 0;
    };

    // 箱の面の並び(軸 a の面は、その軸だけ 1 つ多い)
    struct OhFlowBox {
        uint32_t size[3];

        [[nodiscard]] size_t FaceCount(uint32_t axis) const {
            size_t count = 1;
            for (uint32_t a = 0; a < 3; ++a)
                count *= size[a] + (a == axis ? 1u : 0u);

            return count;
        }

        [[nodiscard]] size_t Face(uint32_t axis, const uint32_t c[3]) const {
            const uint32_t sx = size[0] + (axis == 0 ? 1u : 0u);
            const uint32_t sy = size[1] + (axis == 1 ? 1u : 0u);

            return c[0] + (static_cast<size_t>(sx) * (c[1] + (static_cast<size_t>(sy) * c[2])));
        }

        [[nodiscard]] size_t Voxel(const uint32_t c[3]) const {
            return c[0] + (static_cast<size_t>(size[0]) * (c[1] + (static_cast<size_t>(size[1]) * c[2])));
        }
    };

    inline OhFlowBox FlowBoxOf(const reaction::OhObject& object) {
        return {.size = {object.sizeX, object.sizeY, object.voxelCount / (object.sizeX * object.sizeY)}};
    }

    // 層を作った・返した後に運動量を 0 から(止まった空気)
    inline void ResetShellFlow(const OhWorld& world, uint32_t o, OhShellFlow& flow) {
        const OhFlowBox box = FlowBoxOf(world.objects[o]);
        for (uint32_t a = 0; a < 3; ++a)
            flow.momentum[a].assign(box.FaceCount(a), 0);

        flow.potential.assign(world.objects[o].voxelCount, 0);
    }

    // 調べるだけ(T-1201): 決まった回数 gapSweeps の時の圧力と止まった時の圧力の、面の速さ(Ψ_i − Ψ_j。基準の質量の面)の差の最大と速さの最大
    inline void MeasurePressureGap(const OhFlowBox& box, const std::vector<int64_t>& early,
                                   const std::vector<int64_t>& settled, OhShellFlow& flow) {
        for (uint32_t z = 0; z < box.size[2]; ++z) {
            for (uint32_t y = 0; y < box.size[1]; ++y) {
                for (uint32_t x = 0; x < box.size[0]; ++x) {
                    const uint32_t c[3] = {x, y, z};
                    const size_t v = box.Voxel(c);
                    for (uint32_t a = 0; a < 3; ++a) {
                        // + 側の面(箱の外は Ψ = 0)
                        uint32_t plus[3] = {x, y, z};
                        ++plus[a];
                        const bool inside = plus[a] < box.size[a];
                        const int64_t earlyPlus = inside ? early[box.Voxel(plus)] : 0;
                        const int64_t settledPlus = inside ? settled[box.Voxel(plus)] : 0;
                        const int64_t earlySpeed = early[v] - earlyPlus;
                        const int64_t settledSpeed = settled[v] - settledPlus;
                        flow.gapSpeedQ16 = std::max(flow.gapSpeedQ16, std::abs(earlySpeed - settledSpeed));
                        flow.gapReferenceQ16 = std::max(flow.gapReferenceQ16, std::abs(settledSpeed));
                    }
                }
            }
        }
    }

    // --- 圧力の多重格子(T-1202。道 B のやり直し)---
    // 圧力の式 Σ β_f (Ψ_i − Ψ_j) = 残り(箱の外 Ψ = 0)の答えは「1 回の赤黒で値が 1 つも変わらない」整数の点(T-1201)。
    // 0 から赤黒だけで探すと、滑らかな誤差が赤黒 1 回に 0.94〜0.97 しか減らず、平均 50〜220 回かかった。ここでは解き始めの値を多重格子で作り、
    // 最後は同じ赤黒を収まるまで回す(止める判定は T-1201 と同じ。答えは同じ整数の式の動かない点の 1 つ)。
    //   ① 細かい単位(Ψ × 2^8)で V 字 1 回(線形の誤差は V 字 1 回で 1/10 に減る。T-1202 で測った)
    //   ② 動かない点では各ボクセルの残りが [0, 重みの和) か (−重みの和, 0] の幅に入る(割り算を 0 へ切るため)。その幅の真ん中へ寄せる
    //      (右辺から 符号 × (重みの和 − 1) ÷ 2 を引く)V 字をもう 1 回。寄せないと、整数の赤黒が切り捨ての偏りを 1 ずつ運ぶのに 30〜70 回かかった
    //   ③ Ψ = 四捨五入(細かい値 ÷ 2^8)から、ふつうの赤黒を収まるまで
    // 粗い格子: 2 × 2 × 2 をまとめる(奇数の端は 1 つ)。粗い面の重み = まとめた面の重みの和 × 細かい中心の距離 ÷ 粗い中心の距離。
    // 戻し: セル中心の三線形(軸ごとに 12 分の重み。壁の向こうは 0)。残りの戻しは和。赤黒・割り算は細かい格子と同じ形(0 へ切る)。
    struct OhMgLevel {
        uint32_t size[3];
        std::vector<std::array<uint64_t, 6>>
            weight;  // ボクセルごと: −x・+x・−y・+y・−z・+z の面(箱の外の面は Ψ = 0 に向く)
        std::vector<uint64_t> total;
        std::vector<int64_t> rhs;  // 残り × 65536 の単位(① ② は × 2^8 も)
        std::vector<int64_t> value;

        [[nodiscard]] size_t Count() const { return static_cast<size_t>(size[0]) * size[1] * size[2]; }

        [[nodiscard]] size_t Index(uint32_t x, uint32_t y, uint32_t z) const {
            return x + (static_cast<size_t>(size[0]) * (y + (static_cast<size_t>(size[1]) * z)));
        }
    };

    inline constexpr uint32_t OH_MG_FINE_BITS = 8;          // ① ② の細かい単位(Ψ × 2^8)
    inline constexpr uint32_t OH_MG_SMOOTH_SWEEPS = 2;      // 段ごとの前と後の赤黒
    inline constexpr uint32_t OH_MG_COARSEST_SWEEPS = 16;   // 一番粗い段(どの向きも 2 以下)の赤黒
    inline constexpr int64_t OH_MG_PROLONG_DIVISOR = 1728;  // 三線形の重み 12 × 12 × 12

    // 隣の値の重みつきの和(箱の外は 0)
    inline int64_t MgNeighbourSum(const OhMgLevel& level, uint32_t x, uint32_t y, uint32_t z) {
        const size_t v = level.Index(x, y, z);
        const uint32_t c[3] = {x, y, z};
        int64_t sum = 0;
        for (uint32_t a = 0; a < 3; ++a) {
            uint32_t m[3] = {x, y, z};
            uint32_t p[3] = {x, y, z};
            if (c[a] > 0) {
                --m[a];
                sum += static_cast<int64_t>(level.weight[v][2 * a]) * level.value[level.Index(m[0], m[1], m[2])];
            }

            if (c[a] + 1 < level.size[a]) {
                ++p[a];
                sum += static_cast<int64_t>(level.weight[v][(2 * a) + 1]) * level.value[level.Index(p[0], p[1], p[2])];
            }
        }

        return sum;
    }

    inline void MgSweep(OhMgLevel& level) {
        namespace rx = bicameral::reaction;
        for (uint32_t colour = 0; colour < 2; ++colour) {
            for (uint32_t z = 0; z < level.size[2]; ++z) {
                for (uint32_t y = 0; y < level.size[1]; ++y) {
                    for (uint32_t x = 0; x < level.size[0]; ++x) {
                        if (((x + y + z) & 1u) != colour)
                            continue;

                        const size_t v = level.Index(x, y, z);
                        const int64_t sum = level.rhs[v] + MgNeighbourSum(level, x, y, z);
                        level.value[v] = rx::OhMulDivToward0(sum, 1, std::max<uint64_t>(level.total[v], 1));
                    }
                }
            }
        }
    }

    // 軸の粗い番号 I の中心(細かいセル i の中心を 2i + 1 とする単位。まとめた細かいセル a = 2I・b = min(2I + 1, n − 1) の中点)
    inline uint32_t MgCenter2(uint32_t fineCount, uint32_t coarse) {
        const uint32_t first = 2 * coarse;
        const uint32_t last = std::min(first + 1, fineCount - 1);

        return first + last + 1;
    }

    // 粗い面の重み: まとめた面の重みの和 × 細かい中心の距離(内 2・壁まで 1)÷ 粗い中心の距離(同じ単位)
    inline uint64_t MgCoarseFaceWeight(uint64_t sum, uint32_t fineCount, uint32_t coarseCount, uint32_t coarse,
                                       uint32_t side) {
        const uint32_t me = MgCenter2(fineCount, coarse);
        if (side == 0) {
            if (coarse == 0)
                return sum / me;

            return (sum * 2) / (me - MgCenter2(fineCount, coarse - 1));
        }

        if (coarse + 1 == coarseCount)
            return sum / ((2 * fineCount) - me);

        return (sum * 2) / (MgCenter2(fineCount, coarse + 1) - me);
    }

    // 細かい段の残りと重みから粗い段を作る
    inline OhMgLevel MgCoarsen(const OhMgLevel& fine) {
        OhMgLevel coarse;
        for (uint32_t a = 0; a < 3; ++a)
            coarse.size[a] = (fine.size[a] + 1) / 2;

        const size_t count = coarse.Count();
        std::vector<std::array<uint64_t, 6>> sums(count);
        coarse.weight.assign(count, {});
        coarse.total.assign(count, 0);
        coarse.rhs.assign(count, 0);
        coarse.value.assign(count, 0);
        for (uint32_t z = 0; z < fine.size[2]; ++z) {
            for (uint32_t y = 0; y < fine.size[1]; ++y) {
                for (uint32_t x = 0; x < fine.size[0]; ++x) {
                    const size_t v = fine.Index(x, y, z);
                    const size_t parent = coarse.Index(x / 2, y / 2, z / 2);
                    const int64_t applied = (static_cast<int64_t>(fine.total[v]) * fine.value[v]) -
                                            MgNeighbourSum(fine, x, y, z);
                    coarse.rhs[parent] += fine.rhs[v] - applied;
                    const uint32_t c[3] = {x, y, z};
                    for (uint32_t a = 0; a < 3; ++a) {
                        // まとまりの外へ向く面だけ(− 側は偶数の座標・+ 側は奇数の座標か箱の端)
                        if ((c[a] & 1u) == 0)
                            sums[parent][2 * a] += fine.weight[v][2 * a];

                        if ((c[a] & 1u) == 1 || c[a] + 1 == fine.size[a])
                            sums[parent][(2 * a) + 1] += fine.weight[v][(2 * a) + 1];
                    }
                }
            }
        }

        for (uint32_t z = 0; z < coarse.size[2]; ++z) {
            for (uint32_t y = 0; y < coarse.size[1]; ++y) {
                for (uint32_t x = 0; x < coarse.size[0]; ++x) {
                    const size_t v = coarse.Index(x, y, z);
                    const uint32_t c[3] = {x, y, z};
                    for (uint32_t face = 0; face < 6; ++face) {
                        const uint32_t a = face / 2;
                        coarse.weight[v][face] = MgCoarseFaceWeight(sums[v][face], fine.size[a], coarse.size[a], c[a],
                                                                    face & 1u);
                        coarse.total[v] += coarse.weight[v][face];
                    }
                }
            }
        }

        return coarse;
    }

    // 三線形の戻しの軸ごとの重み(12 分): 細かいセル i が使う粗い番号 2 つ(UINT32_MAX = 壁の向こう = 0)
    struct OhMgTap {
        uint32_t coarse[2];
        int64_t weight[2];
    };

    inline OhMgTap MgProlongTap(uint32_t fineCount, uint32_t coarseCount, uint32_t i) {
        const uint32_t position = (2 * i) + 1;
        const uint32_t closest = i / 2;
        const uint32_t closeCenter = MgCenter2(fineCount, closest);
        if (position == closeCenter)
            return {.coarse = {closest, UINT32_MAX}, .weight = {12, 0}};

        // 反対側の中心(壁なら壁の位置)
        uint32_t other = UINT32_MAX;
        uint32_t otherCenter = 0;
        if (position < closeCenter) {
            if (closest > 0) {
                other = closest - 1;
                otherCenter = MgCenter2(fineCount, other);
            }
        } else if (closest + 1 < coarseCount) {
            other = closest + 1;
            otherCenter = MgCenter2(fineCount, other);
        } else {
            otherCenter = 2 * fineCount;
        }

        const uint32_t toClose = position > closeCenter ? position - closeCenter : closeCenter - position;
        const uint32_t toOther = position > otherCenter ? position - otherCenter : otherCenter - position;
        const auto closeWeight = static_cast<int64_t>((12 * toOther) / (toClose + toOther));

        return {.coarse = {closest, other}, .weight = {closeWeight, 12 - closeWeight}};
    }

    inline void MgProlongAdd(OhMgLevel& fine, const OhMgLevel& coarse) {
        namespace rx = bicameral::reaction;
        std::array<std::vector<OhMgTap>, 3> taps;
        for (uint32_t a = 0; a < 3; ++a) {
            taps[a].resize(fine.size[a]);
            for (uint32_t i = 0; i < fine.size[a]; ++i)
                taps[a][i] = MgProlongTap(fine.size[a], coarse.size[a], i);
        }

        for (uint32_t z = 0; z < fine.size[2]; ++z) {
            for (uint32_t y = 0; y < fine.size[1]; ++y) {
                for (uint32_t x = 0; x < fine.size[0]; ++x) {
                    int64_t sum = 0;
                    for (uint32_t k = 0; k < 8; ++k) {
                        const OhMgTap& tx = taps[0][x];
                        const OhMgTap& ty = taps[1][y];
                        const OhMgTap& tz = taps[2][z];
                        const uint32_t cx = tx.coarse[k & 1u];
                        const uint32_t cy = ty.coarse[(k >> 1) & 1u];
                        const uint32_t cz = tz.coarse[(k >> 2) & 1u];
                        if (cx == UINT32_MAX || cy == UINT32_MAX || cz == UINT32_MAX)
                            continue;

                        const int64_t weight = tx.weight[k & 1u] * ty.weight[(k >> 1) & 1u] * tz.weight[(k >> 2) & 1u];
                        sum += weight * coarse.value[coarse.Index(cx, cy, cz)];
                    }

                    fine.value[fine.Index(x, y, z)] += rx::OhMulDivToward0(sum, 1, OH_MG_PROLONG_DIVISOR);
                }
            }
        }
    }

    // V 字 1 回。戻り値 = 費用(細かい格子の赤黒 1 回 = 1000。粗い段は数の割合・残りを求めるのは赤黒 1 回・戻しは 0.5 回)
    inline uint64_t MgCycle(OhMgLevel& level, uint64_t fineCount) {
        const uint64_t count = level.Count();
        const bool coarsest = level.size[0] <= 2 && level.size[1] <= 2 && level.size[2] <= 2;
        if (coarsest) {
            for (uint32_t s = 0; s < OH_MG_COARSEST_SWEEPS; ++s)
                MgSweep(level);

            return (OH_MG_COARSEST_SWEEPS * 1000 * count) / fineCount;
        }

        for (uint32_t s = 0; s < OH_MG_SMOOTH_SWEEPS; ++s)
            MgSweep(level);

        OhMgLevel coarse = MgCoarsen(level);
        uint64_t work = MgCycle(coarse, fineCount);
        MgProlongAdd(level, coarse);
        for (uint32_t s = 0; s < OH_MG_SMOOTH_SWEEPS; ++s)
            MgSweep(level);

        return work + ((((2 * OH_MG_SMOOTH_SWEEPS) + 1) * 1000 * count) + (500 * count)) / fineCount;
    }

    // ① ② ③ の解き始め(戻り値 = 費用)
    inline uint64_t PressureMultigrid(const OhFlowBox& box, const std::array<std::vector<uint64_t>, 3>& weight,
                                      const std::vector<int64_t>& residual, std::vector<int64_t>& potential) {
        OhMgLevel fine;
        for (uint32_t a = 0; a < 3; ++a)
            fine.size[a] = box.size[a];

        const size_t count = fine.Count();
        fine.weight.assign(count, {});
        fine.total.assign(count, 0);
        fine.rhs.assign(count, 0);
        fine.value.assign(count, 0);
        for (uint32_t z = 0; z < box.size[2]; ++z) {
            for (uint32_t y = 0; y < box.size[1]; ++y) {
                for (uint32_t x = 0; x < box.size[0]; ++x) {
                    const uint32_t c[3] = {x, y, z};
                    const size_t v = box.Voxel(c);
                    for (uint32_t a = 0; a < 3; ++a) {
                        uint32_t plus[3] = {x, y, z};
                        ++plus[a];
                        fine.weight[v][2 * a] = weight[a][box.Face(a, c)];
                        fine.weight[v][(2 * a) + 1] = weight[a][box.Face(a, plus)];
                        fine.total[v] += fine.weight[v][2 * a] + fine.weight[v][(2 * a) + 1];
                    }

                    fine.rhs[v] = residual[v] * (int64_t{65536} << OH_MG_FINE_BITS);
                }
            }
        }

        // ①
        uint64_t work = MgCycle(fine, count);

        // ② 動かない点の残りの幅の真ん中へ寄せる
        for (size_t v = 0; v < count; ++v) {
            const auto middle = static_cast<int64_t>((std::max<uint64_t>(fine.total[v], 1) - 1) / 2) << OH_MG_FINE_BITS;
            fine.rhs[v] -= fine.value[v] >= 0 ? middle : -middle;
        }

        work += MgCycle(fine, count);

        // ③ 四捨五入(0 から遠い方へ)
        const int64_t halfUnit = int64_t{1} << (OH_MG_FINE_BITS - 1);
        for (size_t v = 0; v < count; ++v) {
            const int64_t value = fine.value[v];
            potential[v] = value >= 0 ? (value + halfUnit) >> OH_MG_FINE_BITS
                                      : -((-value + halfUnit) >> OH_MG_FINE_BITS);
        }

        return work;
    }

    // 調べるだけ(T-1202): 圧力の問題を 1 つ書き出す(並び: 箱 3 語・軸ごとの面の重み・残り・止まった答え・回数)
    inline void DumpPressureProblem(std::ostream& out, const OhFlowBox& box,
                                    const std::array<std::vector<uint64_t>, 3>& weight,
                                    const std::vector<int64_t>& residual, const std::vector<int64_t>& potential,
                                    uint32_t used) {
        const auto write = [&](const void* data, size_t bytes) {
            out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
        };
        write(box.size, sizeof(box.size));
        for (uint32_t a = 0; a < 3; ++a)
            write(weight[a].data(), weight[a].size() * sizeof(uint64_t));

        write(residual.data(), residual.size() * sizeof(int64_t));
        write(potential.data(), potential.size() * sizeof(int64_t));
        write(&used, sizeof(used));
        out.flush();
    }

    // 1 つの流れの小刻み。戻り値 = 量が負・枠があふれなかった
    inline bool StepShellFlowSubstep(const OhCpuTable& table, const OhGasList& gases, OhWorld& world, uint32_t o,
                                     OhShellFlow& flow, std::vector<uint8_t>& changed) {
        namespace rx = bicameral::reaction;
        const rx::OhObject object = world.objects[o];
        const OhFlowBox box = FlowBoxOf(object);
        const size_t count = object.voxelCount;
        const rx::RxCell air = world.cells[object.cell];
        const rx::OhFlowCell cellFlow = rx::OhFlowCellOf(table, air);
        const rx::OhVoxelGas cellGas = rx::OhPrepareGas(gases, air);

        // --- 見立て ---
        std::vector<rx::OhFlowCell> cells(count);
        std::vector<rx::OhVoxelGas> gas(count);
        std::vector<int64_t> excess(count);
        for (size_t v = 0; v < count; ++v) {
            const rx::RxCell& voxel = world.voxels[object.voxelBegin + v];
            cells[v] = rx::OhFlowCellOf(table, voxel);
            gas[v] = rx::OhPrepareGas(gases, voxel);
            excess[v] = rx::OhFlowExcessQ16(cells[v], cellFlow);
        }

        // 面の両側(箱の外 = SIZE_MAX = 囲むセル)
        struct Sides {
            size_t minus;
            size_t plus;
        };
        const auto sidesOf = [&](uint32_t axis, const uint32_t c[3]) {
            uint32_t m[3] = {c[0], c[1], c[2]};
            Sides sides{.minus = SIZE_MAX, .plus = SIZE_MAX};
            if (c[axis] < box.size[axis])
                sides.plus = box.Voxel(c);

            if (c[axis] > 0) {
                --m[axis];
                sides.minus = box.Voxel(m);
            }

            return sides;
        };
        const auto cellOf = [&](size_t v) -> const rx::OhFlowCell& {
            return v == SIZE_MAX ? cellFlow : cells[v];
        };
        const auto forFaces = [&](uint32_t axis, auto&& body) {
            const uint32_t limit[3] = {box.size[0] + (axis == 0 ? 1u : 0u), box.size[1] + (axis == 1 ? 1u : 0u),
                                       box.size[2] + (axis == 2 ? 1u : 0u)};
            for (uint32_t z = 0; z < limit[2]; ++z) {
                for (uint32_t y = 0; y < limit[1]; ++y) {
                    for (uint32_t x = 0; x < limit[0]; ++x) {
                        const uint32_t c[3] = {x, y, z};
                        body(box.Face(axis, c), sidesOf(axis, c), c);
                    }
                }
            }
        };

        // --- 面の質量と速さ(骨格に面する壁の面は運動量 0)---
        const auto skeleton = [&](size_t v) {
            return v != SIZE_MAX && (world.infos[object.voxelBegin + v].shape & rx::OH_SHELL_BIT) == 0;
        };
        std::array<std::vector<uint64_t>, 3> faceMass;
        std::array<std::vector<int64_t>, 3> speed;
        std::array<std::vector<uint8_t>, 3> wall;
        for (uint32_t a = 0; a < 3; ++a) {
            faceMass[a].resize(box.FaceCount(a));
            speed[a].resize(box.FaceCount(a));
            wall[a].resize(box.FaceCount(a));
            forFaces(a, [&](size_t f, Sides s, const uint32_t*) {
                faceMass[a][f] = rx::OhFlowFaceMass(cellOf(s.minus), cellOf(s.plus));
                wall[a][f] = flow.skeletonWalls && (skeleton(s.minus) || skeleton(s.plus)) ? 1 : 0;
                if (wall[a][f] != 0)
                    flow.momentum[a][f] = 0;

                speed[a][f] = rx::OhFlowSpeed(flow.momentum[a][f], faceMass[a][f]);
            });
        }

        // --- ① 運動量を運ぶ(ボクセルの中心を通る質量 × 風上の面の速さ。D-663)。箱の外の中心 = 止まった空気 ---
        std::array<std::vector<int64_t>, 3> next = flow.momentum;
        for (uint32_t a = 0; a < 3; ++a) {
            const uint32_t limit[3] = {box.size[0] + (a == 0 ? 2u : 0u), box.size[1] + (a == 1 ? 2u : 0u),
                                       box.size[2] + (a == 2 ? 2u : 0u)};
            // 軸 a の中心の座標 0 = 箱の外の手前・1..size = ボクセル・size+1 = 箱の外の奥
            for (uint32_t z = 0; z < limit[2]; ++z) {
                for (uint32_t y = 0; y < limit[1]; ++y) {
                    for (uint32_t x = 0; x < limit[0]; ++x) {
                        uint32_t c[3] = {x, y, z};
                        const uint32_t center = c[a];
                        const bool outsideMinus = center == 0;
                        const bool outsidePlus = center == box.size[a] + 1;
                        uint32_t minusFace[3] = {c[0], c[1], c[2]};
                        uint32_t plusFace[3] = {c[0], c[1], c[2]};
                        minusFace[a] = center - 1;
                        plusFace[a] = center;
                        const int64_t qMinus = outsideMinus ? 0 : speed[a][box.Face(a, minusFace)];
                        const int64_t qPlus = outsidePlus ? 0 : speed[a][box.Face(a, plusFace)];
                        const int64_t centerSpeed = std::clamp<int64_t>((qMinus + qPlus) / 2, -32768, 32768);
                        if (centerSpeed == 0)
                            continue;

                        uint64_t mass = cellFlow.mass;
                        if (!outsideMinus && !outsidePlus) {
                            c[a] = center - 1;
                            mass = cells[box.Voxel(c)].mass;
                        }

                        const int64_t carried = (static_cast<int64_t>(mass) * std::abs(centerSpeed)) >> 16;
                        const bool forward = centerSpeed > 0;
                        const int64_t upwindSpeed = forward ? qMinus : qPlus;
                        const int64_t moved = rx::OhMulDivToward0(upwindSpeed, static_cast<uint64_t>(carried), 1);
                        if (forward) {
                            if (!outsideMinus)
                                next[a][box.Face(a, minusFace)] -= moved;
                            if (!outsidePlus)
                                next[a][box.Face(a, plusFace)] += moved;
                        } else {
                            if (!outsidePlus)
                                next[a][box.Face(a, plusFace)] -= moved;
                            if (!outsideMinus)
                                next[a][box.Face(a, minusFace)] += moved;
                        }
                    }
                }
            }
        }

        // --- ② 縦の面(z)に浮力 ---
        const uint64_t gravity = rx::OhFlowGravityQ16(object.bodyLevel, flow.substeps);
        forFaces(2, [&](size_t f, Sides s, const uint32_t*) {
            next[2][f] += rx::OhFlowBuoyancy(gravity, cellOf(s.minus), cellOf(s.plus), cellFlow);
        });
        for (uint32_t a = 0; a < 3; ++a) {
            forFaces(a, [&](size_t f, Sides, const uint32_t*) {
                if (wall[a][f] != 0)
                    next[a][f] = 0;
            });
        }

        // --- ③ 圧力の射影: Σ β_f (Ψ_i − Ψ_j) >> 16 = 膨らみたい体積 − 予測の出る体積(箱の外 Ψ = 0)---
        std::array<std::vector<uint64_t>, 3> weight;
        std::vector<int64_t> residual = excess;
        for (uint32_t a = 0; a < 3; ++a) {
            weight[a].resize(box.FaceCount(a));
            forFaces(a, [&](size_t f, Sides s, const uint32_t*) {
                weight[a][f] = rx::OhFlowFaceWeight(cellFlow.mass, faceMass[a][f]);
                const int64_t q = rx::OhFlowSpeed(next[a][f], faceMass[a][f]);
                if (s.minus != SIZE_MAX)
                    residual[s.minus] -= q;
                if (s.plus != SIZE_MAX)
                    residual[s.plus] += q;
            });
        }

        // 解き始め: 温めた始まりなら前の小刻みの値(層の状態)、でなければ 0
        std::vector<int64_t> potential(count, 0);
        if (flow.pressureWarmStart && flow.potential.size() == count)
            potential = flow.potential;

        if (flow.pressureMultigrid)
            flow.pressureMultigridWork += PressureMultigrid(box, weight, residual, potential);

        // 隣が変わったボクセル(数えるだけ。最初は全部)
        std::vector<uint8_t> dirty(count, 1);
        uint64_t activeEvaluations = 0;
        const auto markNeighbours = [&](const uint32_t c[3]) {
            for (uint32_t a = 0; a < 3; ++a) {
                uint32_t m[3] = {c[0], c[1], c[2]};
                uint32_t p[3] = {c[0], c[1], c[2]};
                if (c[a] > 0) {
                    --m[a];
                    dirty[box.Voxel(m)] = 1;
                }

                if (c[a] + 1 < box.size[a]) {
                    ++p[a];
                    dirty[box.Voxel(p)] = 1;
                }
            }
        };

        std::vector<int64_t> atGap;
        uint32_t used = 0;
        bool quiet = false;
        for (uint32_t sweep = 0; sweep < flow.pressureSweeps; ++sweep) {
            bool moved = false;
            for (uint32_t colour = 0; colour < 2; ++colour) {
                for (uint32_t z = 0; z < box.size[2]; ++z) {
                    for (uint32_t y = 0; y < box.size[1]; ++y) {
                        for (uint32_t x = 0; x < box.size[0]; ++x) {
                            if (((x + y + z) & 1u) != colour)
                                continue;

                            const uint32_t c[3] = {x, y, z};
                            const size_t v = box.Voxel(c);
                            int64_t sum = residual[v] * 65536;
                            uint64_t total = 0;
                            for (uint32_t a = 0; a < 3; ++a) {
                                uint32_t plus[3] = {x, y, z};
                                ++plus[a];
                                const size_t minusFace = box.Face(a, c);
                                const size_t plusFace = box.Face(a, plus);
                                total += weight[a][minusFace] + weight[a][plusFace];
                                if (c[a] > 0) {
                                    uint32_t m[3] = {x, y, z};
                                    --m[a];
                                    sum += static_cast<int64_t>(weight[a][minusFace]) * potential[box.Voxel(m)];
                                }

                                if (c[a] + 1 < box.size[a])
                                    sum += static_cast<int64_t>(weight[a][plusFace]) * potential[box.Voxel(plus)];
                            }

                            const int64_t value = rx::OhMulDivToward0(sum, 1, std::max<uint64_t>(total, 1));
                            moved = moved || std::abs(value - potential[v]) > flow.pressureQuietTolerance;
                            activeEvaluations += dirty[v];
                            dirty[v] = 0;
                            if (value != potential[v])
                                markNeighbours(c);

                            potential[v] = value;
                        }
                    }
                }
            }

            ++used;
            if (flow.gapSweeps != 0 && used == flow.gapSweeps)
                atGap = potential;

            if (flow.pressureUntilQuiet && !moved) {
                quiet = true;
                break;
            }
        }

        if (flow.pressureWarmStart)
            flow.potential = potential;

        ++flow.pressureSolves;
        flow.pressureSweepsUsed += used;
        flow.pressureActiveWork += (activeEvaluations * 1000) / count;
        flow.pressureMostSweeps = std::max(flow.pressureMostSweeps, used);
        flow.pressureCapped += flow.pressureUntilQuiet && !quiet ? 1u : 0u;
        if (!atGap.empty())
            MeasurePressureGap(box, atGap, potential, flow);

        if (flow.pressureDump != nullptr && flow.pressureDumpEvery != 0 &&
            flow.pressureSolves % flow.pressureDumpEvery == 0)
            DumpPressureProblem(*flow.pressureDump, box, weight, residual, potential, used);

        // 運動量に圧力の力積(ΔM = (Ψ_i − Ψ_j)× 基準の質量)→ 速さ
        for (uint32_t a = 0; a < 3; ++a) {
            forFaces(a, [&](size_t f, Sides s, const uint32_t*) {
                const int64_t minus = s.minus == SIZE_MAX ? 0 : potential[s.minus];
                const int64_t plus = s.plus == SIZE_MAX ? 0 : potential[s.plus];
                next[a][f] += (minus - plus) * static_cast<int64_t>(cellFlow.mass);
                speed[a][f] = rx::OhFlowSpeed(next[a][f], faceMass[a][f]);
                flow.fastestQ16 = std::max(flow.fastestQ16, std::abs(speed[a][f]));
            });
        }

        // 壁の面の運動量はこの小刻みの膨らみの分だけ(持ち越さない)
        flow.momentum = next;
        for (uint32_t a = 0; a < 3; ++a) {
            forFaces(a, [&](size_t f, Sides, const uint32_t*) {
                if (wall[a][f] != 0)
                    flow.momentum[a][f] = 0;
            });
        }

        // --- ④ 運ぶ: 風上の気体を体積の割合で(ボクセルから出るのは 1 小刻みに半分まで)---
        std::vector<uint64_t> outShare(count, 0);
        for (uint32_t a = 0; a < 3; ++a) {
            forFaces(a, [&](size_t f, Sides s, const uint32_t*) {
                const int64_t q = speed[a][f];
                const size_t upwind = q > 0 ? s.minus : s.plus;
                if (q != 0 && upwind != SIZE_MAX)
                    outShare[upwind] += rx::OhFlowShareQ16(q, cells[upwind]);
            });
        }

        std::vector<rx::OhFlow> delta(count, rx::OhZeroFlow());
        rx::OhFlow toCell = rx::OhZeroFlow();
        for (uint32_t a = 0; a < 3; ++a) {
            forFaces(a, [&](size_t f, Sides s, const uint32_t*) {
                const int64_t q = speed[a][f];
                if (q == 0)
                    return;

                const size_t upwind = q > 0 ? s.minus : s.plus;
                const size_t downwind = q > 0 ? s.plus : s.minus;
                rx::OhFlow moved;
                if (upwind == SIZE_MAX) {
                    moved = rx::OhFlowMoved(table, gases, cellGas, cellFlow.temperature,
                                            rx::OhFlowShareQ16(q, cellFlow));
                    toCell.energy -= moved.energy;
                    for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g)
                        toCell.amounts[g] -= moved.amounts[g];
                } else {
                    uint64_t share = rx::OhFlowShareQ16(q, cells[upwind]);
                    if (outShare[upwind] > rx::OH_FLOW_SHARE_LIMIT_Q16)
                        share = rx::OhDivideU64(share * rx::OH_FLOW_SHARE_LIMIT_Q16, outShare[upwind]);

                    moved = rx::OhFlowMoved(table, gases, gas[upwind], cells[upwind].temperature, share);
                    delta[upwind].energy -= moved.energy;
                    for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g)
                        delta[upwind].amounts[g] -= moved.amounts[g];
                }

                if (downwind == SIZE_MAX) {
                    toCell = rx::OhAddFlow(toCell, moved);
                } else {
                    delta[downwind] = rx::OhAddFlow(delta[downwind], moved);
                }
            });
        }

        for (size_t v = 0; v < count; ++v)
            flow.limitedShares += outShare[v] > rx::OH_FLOW_SHARE_LIMIT_Q16 ? 1u : 0u;

        // --- 当てる ---
        bool ok = true;
        for (size_t v = 0; v < count; ++v) {
            if (rx::OhFlowIsZero(delta[v]))
                continue;

            rx::RxCell voxel = world.voxels[object.voxelBegin + v];
            voxel.energy += delta[v].energy;
            for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g) {
                const rx::OhAmountChange change = rx::OhAddAmount(voxel, gases.GasSpecies(g), delta[v].amounts[g]);
                voxel = change.cell;
                ok = ok && change.ok != 0;
            }

            world.voxels[object.voxelBegin + v] = voxel;
            changed[v] = 1;
        }

        if (!rx::OhFlowIsZero(toCell))
            ok = ExchangeThroughCarry(gases, world, o, toCell) && ok;

        return ok;
    }

    // 1 刻みの流れ(層のある物だけ。刻み tick の反応の後 = 次の刻みの受け渡し)。変わったボクセルの待ちとブロックを起こす
    inline bool StepShellFlow(const OhCpuTable& table, const OhGasList& gases, OhWorld& world, uint32_t o,
                              OhShellFlow& flow, uint64_t tick) {
        namespace rx = bicameral::reaction;
        if (world.objects[o].shellLayers == 0)
            return true;

        std::vector<uint8_t> changed(world.objects[o].voxelCount, 0);
        bool ok = true;
        for (uint32_t sub = 0; sub < flow.substeps; ++sub)
            ok = StepShellFlowSubstep(table, gases, world, o, flow, changed) && ok;

        ++world.pass;
        const rx::OhObject object = world.objects[o];
        for (size_t b = 0; b < world.blocks.size(); ++b) {
            if (world.blocks[b].object != o)
                continue;

            bool any = false;
            for (uint32_t local = 0; local < rx::OH_BLOCK_VOXELS; ++local) {
                const uint32_t voxel = rx::OhBlockVoxel(object, world.blocks[b], local);
                if (voxel == rx::OH_NO_BLOCK || changed[voxel - object.voxelBegin] == 0)
                    continue;

                world.waits[voxel].changedTick = tick;  // 次の刻みの受け渡しで変わった扱い
                any = true;
            }

            if (!any)
                continue;

            rx::OhBlockState& state = world.blockStates[b];
            state.changedPass = world.pass;
            state.quiet = 0;
            for (uint32_t& face : state.facePass)
                face = world.pass;
        }

        return ok;
    }

}  // namespace bicameral::test
