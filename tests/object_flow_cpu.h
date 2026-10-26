// object_flow_cpu.h — 物の箱の中の気体の流れ(D-667 の 2 段目・T-1143)の CPU リファレンス。式は shaders/common/object_heat.hlsli の「層の流れ」
// (OhFlowCellOf・OhFlowExcessQ16・OhFlowBuoyancy・OhFlowFaceWeight・OhFlowShareQ16・OhFlowMoved)。GPU の形は後(T-1143 の作業ログ)。
// データの流れ(流れの小刻みごと。どれも小刻みの初めの値から計算する = Jacobi。圧力だけ赤黒の決まった回数の反復):
//   見立て(ボクセルごと: 気体の質量・pV・気体の割合・膨らみたい体積)→ 面の速さ q = M ÷ m_f
//   → ① 運動量を移した質量に比例して同じ軸の隣の面へ(D-663。箱の外の面の向こう = 囲むセル = 止まった空気)
//   → ② 縦の面に浮力 → ③ 圧力の射影(箱の外 = 囲むセルの圧力。膨らみたい体積をこの小刻みで出す)
//   → ④ 風上の気体を体積の割合で移す(1 小刻みに出るのは半分まで)。箱の外へ出る・入る分は対の端数を通してセルへ(帳簿「物との受け渡し」)。
// 骨格のボクセル: 既定は壁(骨格に面する面は運動量を持たず、骨格の気体の膨らみ・縮みだけが圧力の射影で出入りする = 吹き出す多孔の壁)。
// skeletonWalls = false なら骨格の中も気体として流れる(気体の割合 = 凝縮相の体積を除いた分。比べ)。
// 真実: 面の運動量(OhShellFlow::momentum。層を作った時に 0 = 止まった空気、返す時に捨てる)。運動のエネルギーは数えない(音より遅い極限)。
// 保存: 移すのは対の流れだけ(片側から引いた量を反対側に足す)なので、セル + ボクセル + 対の端数はビット単位で変わらない。
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "object_shell_cpu.h"

namespace bicameral::test {

    struct OhShellFlow {
        uint32_t substeps = 4;         // 1 刻みの流れの小刻み(1/240 s。速さ 1 m/s で 1 小刻みに 0.53 ボクセル)
        uint32_t pressureSweeps = 24;  // 圧力の赤黒の反復の回数(決まった数)
        std::array<std::vector<int64_t>, 3> momentum;  // 軸ごとの面(+ 向きが正。質量 × q)
        uint64_t limitedShares = 0;                    // 数えるだけ: 出る上限で縮めた(ボクセル × 小刻み)
        int64_t fastestQ16 = 0;                        // 数えるだけ: 一番速い面(Q16 ボクセル / 小刻み)
        // 骨格に面する面: true = 壁(運動量を持たず、骨格の膨らみ・縮みの分だけ圧力の射影で出入りする = 吹き出す多孔の壁)/
        // false = 骨格の中も気体として流れる(紙のボクセルの気体と固体は 1 つの温度なので、流れ込んだ炎の気体と紙がすぐ同じ温度になる)
        bool skeletonWalls = true;
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

        std::vector<int64_t> potential(count, 0);
        for (uint32_t sweep = 0; sweep < flow.pressureSweeps; ++sweep) {
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

                            potential[v] = rx::OhMulDivToward0(sum, 1, std::max<uint64_t>(total, 1));
                        }
                    }
                }
            }
        }

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
