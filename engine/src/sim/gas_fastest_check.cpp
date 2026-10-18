// gas_fastest_check.cpp — 最速(投影)× 小さい空きの検査(T-0788)。何をするかは gas_fastest_check.h。
// 1. 気体の通れるセルの面を、大きい同士(部分)・小さい同士(小さいセルのまとまり)の 2 つの互いに素な集合でつなぐ(開いた境界の外は部分の 1 つ)
// 2. 小さいセルと大きいセル(か外)が接する面ごとに「まとまり → 部分」の組を集める
// 3. 2 つ以上の部分に接するまとまりが、小さい空きだけでつながった組(部分同士は大きいセルでつながらないので、孔を通るしかない)
#include "sim/gas_fastest_check.h"

#include <algorithm>
#include <format>
#include <map>
#include <numeric>
#include <utility>

#include "core/aliases.h"

namespace bicameral::sim {

    namespace {

        // 互いに素な集合(根は小さい番号。並びによらず同じ根になる)
        struct DisjointSets {
            std::vector<uint32_t> parents;

            explicit DisjointSets(uint32_t count) : parents(count) { std::iota(parents.begin(), parents.end(), 0u); }

            uint32_t Find(uint32_t node) {
                while (parents[node] != node) {
                    parents[node] = parents[parents[node]];
                    node = parents[node];
                }

                return node;
            }

            void Join(uint32_t a, uint32_t b) {
                a = Find(a);
                b = Find(b);
                if (a == b)
                    return;

                if (a < b)
                    parents[b] = a;
                else
                    parents[a] = b;
            }
        };

        std::array<uint32_t, GAS_AXES> Sizes(const GasConfig& config) {
            return {config.sizeX, config.sizeY, config.sizeZ};
        }

        std::array<uint32_t, GAS_AXES> Coordinate(const std::array<uint32_t, GAS_AXES>& sizes, uint32_t index) {
            return {index % sizes[0], (index / sizes[0]) % sizes[1], index / (sizes[0] * sizes[1])};
        }

        // 箱の中の面ごとに visit(左のセル, 右のセル)。周期の境界は端の面(最後のセル → 最初のセル)も
        template <typename Visit>
        void ForEachInnerFace(const GasConfig& config, Visit&& visit) {
            const std::array<uint32_t, GAS_AXES> sizes = Sizes(config);
            const std::array<uint32_t, GAS_AXES> strides = {1, sizes[0], sizes[0] * sizes[1]};
            const uint32_t cells = sizes[0] * sizes[1] * sizes[2];
            for (uint32_t index = 0; index < cells; ++index) {
                const std::array<uint32_t, GAS_AXES> at = Coordinate(sizes, index);
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                    if (at[axis] + 1 < sizes[axis])
                        visit(index, index + strides[axis]);
                    else if (config.boundary[axis] == GasBoundary::Periodic && sizes[axis] > 1)
                        visit(index, index - (at[axis] * strides[axis]));
                }
            }
        }

        // 開いた境界に接するセルごとに visit(セル)
        template <typename Visit>
        void ForEachOpenEdgeCell(const GasConfig& config, Visit&& visit) {
            const std::array<uint32_t, GAS_AXES> sizes = Sizes(config);
            const uint32_t cells = sizes[0] * sizes[1] * sizes[2];
            for (uint32_t index = 0; index < cells; ++index) {
                const std::array<uint32_t, GAS_AXES> at = Coordinate(sizes, index);
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                    if (config.boundary[axis] == GasBoundary::Open && (at[axis] == 0 || at[axis] + 1 == sizes[axis])) {
                        visit(index);
                        break;
                    }
                }
            }
        }

        // まとまり(touches の first。並びは根の順)ごとに接する部分を数え、2 つ以上なら組に。
        std::vector<GasSmallOpenJoint> MakeJoints(std::span<const std::pair<uint32_t, uint32_t>> touches,
                                                  std::span<const uint32_t> partCells, uint32_t outsideRoot,
                                                  std::vector<uint32_t>& jointOfCluster) {
            // 同じ部分の組をつなぐまとまり(孔のある壁の孔それぞれ)は 1 つの組にまとめる(並びはまとまりの根の順 = 決定的)
            std::vector<GasSmallOpenJoint> joints;
            std::map<std::vector<uint32_t>, uint32_t> jointOfParts;
            for (size_t first = 0; first < touches.size();) {
                size_t last = first;
                std::vector<uint32_t> touched;
                while (last < touches.size() && touches[last].first == touches[first].first)
                    touched.push_back(touches[last++].second);

                if (touched.size() >= 2) {
                    const auto [found, added] = jointOfParts.try_emplace(touched, static_cast<uint32_t>(joints.size()));
                    if (added) {
                        GasSmallOpenJoint joint{.parts = static_cast<uint32_t>(touched.size()),
                                                .smallestPartCells = UINT32_MAX};
                        for (const uint32_t part : touched) {
                            if (part == outsideRoot)
                                joint.outside = true;
                            else
                                joint.smallestPartCells = std::min(joint.smallestPartCells, partCells[part]);
                        }

                        joints.push_back(joint);
                    }

                    jointOfCluster[touches[first].first] = found->second;
                }

                first = last;
            }

            return joints;
        }

        // 空き(Q16)を百分率の文に(小数 2 桁。切り捨て)
        std::string OpenPercentText(uint32_t openQ16) {
            const uint64_t basisPoints = (uint64_t)openQ16 * 10000 / GAS_OPEN_FULL;
            return std::format("{}.{:02}%", basisPoints / 100, basisPoints % 100);
        }

    }  // namespace

    std::vector<GasSmallOpenJoint> FindGasSmallOpenJoints(const GasConfig& config, std::span<const uint32_t> openQ16,
                                                          uint32_t smallOpenQ16) {
        if (openQ16.empty())
            return {};

        const auto cells = static_cast<uint32_t>(openQ16.size());
        const uint32_t outside = cells;  // 開いた境界の外(部分の 1 つ)
        const auto isBig = [&](uint32_t index) {
            return openQ16[index] >= smallOpenQ16;
        };
        const auto isSmall = [&](uint32_t index) {
            return openQ16[index] != 0 && openQ16[index] < smallOpenQ16;
        };

        // --- 1. 部分(大きい同士)と小さいセルのまとまり(小さい同士)---
        DisjointSets parts(cells + 1);
        DisjointSets clusters(cells);
        ForEachInnerFace(config, [&](uint32_t left, uint32_t right) {
            if (isBig(left) && isBig(right))
                parts.Join(left, right);
            else if (isSmall(left) && isSmall(right))
                clusters.Join(left, right);
        });
        ForEachOpenEdgeCell(config, [&](uint32_t index) {
            if (isBig(index))
                parts.Join(index, outside);
        });

        // --- 2. まとまり → 接する部分の組 ---
        std::vector<std::pair<uint32_t, uint32_t>> touches;
        const auto touch = [&](uint32_t smallCell, uint32_t part) {
            touches.emplace_back(clusters.Find(smallCell), parts.Find(part));
        };
        ForEachInnerFace(config, [&](uint32_t left, uint32_t right) {
            if (isSmall(left) && isBig(right))
                touch(left, right);
            else if (isBig(left) && isSmall(right))
                touch(right, left);
        });
        ForEachOpenEdgeCell(config, [&](uint32_t index) {
            if (isSmall(index))
                touch(index, outside);
        });

        rng::sort(touches);
        touches.erase(std::unique(touches.begin(), touches.end()), touches.end());

        // --- 3. 2 つ以上の部分に接するまとまり ---
        std::vector<uint32_t> partCells(cells + 1, 0);
        for (uint32_t index = 0; index < cells; ++index) {
            if (isBig(index))
                ++partCells[parts.Find(index)];
        }

        const uint32_t outsideRoot = parts.Find(outside);
        std::vector<uint32_t> jointOfCluster(cells, UINT32_MAX);
        std::vector<GasSmallOpenJoint> joints = MakeJoints(touches, partCells, outsideRoot, jointOfCluster);

        // --- まとまりのセルの数・最も小さい空き・範囲 ---
        const std::array<uint32_t, GAS_AXES> sizes = Sizes(config);
        for (uint32_t index = 0; index < cells; ++index) {
            if (!isSmall(index) || jointOfCluster[clusters.Find(index)] == UINT32_MAX)
                continue;

            GasSmallOpenJoint& joint = joints[jointOfCluster[clusters.Find(index)]];
            const std::array<uint32_t, GAS_AXES> at = Coordinate(sizes, index);
            if (joint.smallCells == 0) {
                joint.smallestOpenQ16 = openQ16[index];
                joint.minCell = at;
                joint.maxCell = at;
            }

            ++joint.smallCells;
            joint.smallestOpenQ16 = std::min(joint.smallestOpenQ16, openQ16[index]);
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                joint.minCell[axis] = std::min(joint.minCell[axis], at[axis]);
                joint.maxCell[axis] = std::max(joint.maxCell[axis], at[axis]);
            }
        }

        // 外のほかに部分が無い組は無い(2 つ以上に接する)ので、UINT32_MAX は残らない。念のため 0 に
        for (GasSmallOpenJoint& joint : joints) {
            if (joint.smallestPartCells == UINT32_MAX)
                joint.smallestPartCells = 0;
        }

        return joints;
    }

    uint32_t GasSmallOpenBreakPercent(uint32_t openQ16) {
        // T-0788 の調べ(16³ の板・層の全部が同じ空き・最速・40 刻み。板の下の圧力 = 上 × 102 / 120 / 150 / 200 / 500 %)で壊れた最も小さい差
        if (openQ16 <= GAS_OPEN_FULL / 256)
            return 102;

        if (openQ16 <= GAS_OPEN_FULL / 16)
            return 120;

        if (openQ16 <= GAS_OPEN_FULL / 8)
            return 150;

        if (openQ16 <= GAS_OPEN_FULL / 4)
            return 500;

        return 0;
    }

    std::string GasSmallOpenJointWarning(const GasSmallOpenJoint& joint, std::string_view where,
                                         std::string_view range) {
        const uint32_t breakPercent = GasSmallOpenBreakPercent(joint.smallestOpenQ16);
        std::string measured;
        if (breakPercent == 0)
            measured = "測った板では空き 1/4 が 5 倍の圧力の差で壊れた";
        else if (breakPercent < 200)
            measured = std::format("測った板では、この空きの層は片側の圧力が {}% 高いだけで壊れた", breakPercent - 100);
        else
            measured = std::format("測った板では、この空きの層は片側の圧力が {} 倍で壊れた", breakPercent / 100);

        return std::format(
            "{}{}の {} の {} セル(空き {} 以上・{} 未満)だけが、気体を {} つの部分{}に分けてつないでいる"
            "(最も小さい部分は {} セル)。最速(投影)の圧力はこの孔を 1 "
            "刻みで釣り合わせようとして、圧力と速さが壊れることがある"
            "({}。ADR-0346)。値はそのまま流す。直すには: この気体を本物の音速で解く(陽解法か、投影に音速を書く)/ "
            "孔のセルの空きを {} 以上にする / 孔を通らずに行き来できる道を開ける(D-519)",
            GAS_FASTEST_SMALL_OPEN_WARNING_PREFIX, where, range, joint.smallCells,
            OpenPercentText(joint.smallestOpenQ16), OpenPercentText(GAS_FASTEST_SMALL_OPEN_Q16), joint.parts,
            joint.outside ? "(開いた境界の外を含む)" : "", joint.smallestPartCells, measured,
            OpenPercentText(GAS_FASTEST_SMALL_OPEN_Q16));
    }

}  // namespace bicameral::sim
