// equilibrium_block_scenes.h — 成分を持つ節(sim/equilibrium_block)の試験の場面: 違うレベルの節が混ざる箱(T-0969・ADR-0462)。
// equilibrium_block_test(CPU)と gpu_equilibrium_block_test(GPU 版と CPU のビット一致。T-1104)が同じ系を使う。
// 一辺 8 のレベル 0 の箱(下の面を固定)のうち、真ん中の 2 × 2 × 2 セルをレベル 1(4 × 4 × 4)に細かくした系(k は呼び手が選ぶ)。
// 面の係数はエネルギーの形(両側に同じブロック)。細かい節どうしは粗い節どうしの 1/2、細かい節と粗い節は 1/3(面積 ÷ 距離)。延ばし方は区分的に一定。
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

#include "sim/equilibrium_block.h"

namespace bicameral::test {

    inline constexpr int64_t UNIT = int64_t{1} << 32;
    inline constexpr uint32_t MIXED_EDGE = 8;

    using Key = std::tuple<int32_t, int64_t, int64_t, int64_t>;

    // 係数 scale ÷ divisor 倍の k × k(対角 (k + 1)・非対角 1)
    inline sim::EquilibriumBlock Coupling(uint32_t k, int64_t divisor, bool negative) {
        sim::EquilibriumBlock block{};
        for (uint32_t row = 0; row < k; ++row) {
            for (uint32_t column = 0; column < k; ++column) {
                const int64_t value = (row == column ? (int64_t)(k + 1) * UNIT : UNIT) / divisor;
                block[(row * sim::EQ_MAX_COMPONENTS) + column] = negative ? -value : value;
            }
        }

        return block;
    }

    // 粗いセル (x, y, z) が細かくした所か
    inline bool Refined(int64_t x, int64_t y, int64_t z) {
        return x >= 3 && x < 5 && y >= 3 && y < 5 && z >= 3 && z < 5;
    }

    // 節(レベル 0 は細かくしない所・レベル 1 は細かくした所)
    inline std::vector<sim::EquilibriumNode> MixedNodes(bool refine) {
        std::vector<sim::EquilibriumNode> nodes;
        for (int64_t z = 0; z < MIXED_EDGE; ++z) {
            for (int64_t y = 0; y < MIXED_EDGE; ++y) {
                for (int64_t x = 0; x < MIXED_EDGE; ++x) {
                    if (!refine || !Refined(x, y, z)) {
                        nodes.push_back({.level = 0, .x = x, .y = y, .z = z});
                        continue;
                    }

                    for (int64_t child = 0; child < 8; ++child)
                        nodes.push_back({.level = 1,
                                         .x = 2 * x + (child & 1),
                                         .y = 2 * y + ((child >> 1) & 1),
                                         .z = 2 * z + ((child >> 2) & 1)});
                }
            }
        }

        return nodes;
    }

    // 系を作る。reverse なら節と面の並びを逆にする
    inline sim::EquilibriumBlockSystem MixedSystem(uint32_t k, bool refine, bool reverse) {
        sim::EquilibriumBlockSystem system;
        system.components = k;
        system.coefficientShift = 32;
        for (uint32_t octant = 0; octant < sim::EQ_OCTANTS; ++octant) {
            for (uint32_t c = 0; c < k; ++c)
                system.prolongation[octant][(c * sim::EQ_MAX_COMPONENTS) + c] = int64_t{1} << sim::EQ_PROLONG_SHIFT;
        }

        system.nodes = MixedNodes(refine);
        if (reverse)
            std::ranges::reverse(system.nodes);

        std::map<Key, uint32_t> index;
        for (uint32_t i = 0; i < system.nodes.size(); ++i) {
            const sim::EquilibriumNode& node = system.nodes[i];
            index[{node.level, node.x, node.y, node.z}] = i;
        }

        // 面: 各節から +軸の向きの隣(同じレベル・細かい → 粗い・粗い → 細かい)。重ならないように low < high の向きだけ数える
        std::vector<sim::EquilibriumBlockFace> faces;
        const auto addFace = [&](uint32_t a, uint32_t b, int64_t divisor) {
            faces.push_back({.low = a,
                             .high = b,
                             .lowLow = Coupling(k, divisor, false),
                             .lowHigh = Coupling(k, divisor, true),
                             .highHigh = Coupling(k, divisor, false)});
        };
        for (uint32_t i = 0; i < system.nodes.size(); ++i) {
            const sim::EquilibriumNode& node = system.nodes[i];
            for (uint32_t axis = 0; axis < 3; ++axis) {
                for (const int64_t step : {int64_t{-1}, int64_t{1}}) {
                    const int64_t nx = node.x + (axis == 0 ? step : 0);
                    const int64_t ny = node.y + (axis == 1 ? step : 0);
                    const int64_t nz = node.z + (axis == 2 ? step : 0);
                    const auto same = index.find({node.level, nx, ny, nz});
                    if (same != index.end()) {
                        if (step > 0)
                            addFace(i, same->second, node.level == 0 ? 1 : 2);

                        continue;
                    }

                    // 細かい節の隣が粗い節(面は細かい側から 1 本)
                    if (node.level != 1)
                        continue;

                    const auto coarse = index.find({0, nx >> 1, ny >> 1, nz >> 1});
                    if (coarse != index.end())
                        addFace(i, coarse->second, 3);
                }
            }

            if (node.level == 0 && node.z == 0) {
                sim::EquilibriumBlockFixed fixed{.node = i, .block = Coupling(k, 1, false)};
                for (uint32_t c = 0; c < k; ++c)
                    fixed.value[c] = (int64_t)(c + 1) * 1000;

                system.fixedFaces.push_back(fixed);
            }

            const int64_t share = node.level == 0 ? 8 : 1;  // 重さは体積に比べて
            for (uint32_t c = 0; c < k; ++c) {
                const int64_t weight = -(int64_t)(c + 1) * share * ((node.x + node.y + 1) % 5 + 1) * (int64_t{1} << 18);
                system.sources.push_back({.node = i, .component = c, .value = weight});
            }
        }

        if (reverse)
            std::ranges::reverse(faces);

        system.faces = std::move(faces);

        return system;
    }

}  // namespace bicameral::test
