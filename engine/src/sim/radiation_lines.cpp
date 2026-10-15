// radiation_lines.cpp — 放射の対を平行な線の束で見つける(T-0584・ADR-0262)。説明は radiation_lines.h
#include "sim/radiation_lines.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include "common/radiation.hlsli"
#include "common/radiation_directions.hlsli"

namespace bicameral::sim {

    namespace rx = ::bicameral::reaction;

    namespace {

        constexpr int64_t ONE_Q16 = 65536;
        constexpr int64_t LINE_SPACING_Q16 = 2 * ONE_Q16 +
                                             1;             // 線の間隔(セル 2 + 2^-16)。板の中で線が掃く幅 ≤ 2 より広い
        constexpr uint32_t DROP_TRANSMITTANCE_Q16 = 4096;   // 透過が 1/16 未満になったら、その先の対は作らない
        constexpr uint32_t PURPOSE_GROUP = 0x52440001u;     // R6 の用途: 向きの組を選ぶ
        constexpr uint32_t PURPOSE_OFFSET_B = 0x52440002u;  // 格子のずれ(2 つ目の軸)
        constexpr uint32_t PURPOSE_OFFSET_C = 0x52440003u;  // 格子のずれ(3 つ目の軸)

        // 線の向き: 一番大きい軸 A(正の向きにそろえる)と、ほかの 2 軸 B・C
        struct LineDirection {
            std::array<uint32_t, 3> axes{};  // A, B, C(格子の軸の番号 0 = x・1 = y・2 = z)
            std::array<int64_t, 3> steps{};  // D_A(> 0)・D_B・D_C(|D_B|・|D_C| ≤ D_A)
            uint32_t viewQ16 = 0;            // 線 1 本の見え方の割合(面 A = セルの面の単位)
            int64_t reachSlabs = 0;          // 16 m に当たる A の板の数
        };

        // 床の割り算(負の数も下へ)
        int64_t FloorDivide(int64_t numerator, int64_t denominator) {
            const int64_t quotient = numerator / denominator;
            const bool inexact = quotient * denominator != numerator;

            return inexact && ((numerator < 0) != (denominator < 0)) ? quotient - 1 : quotient;
        }

        int64_t Absolute(int64_t value) {
            return value < 0 ? -value : value;
        }

        LineDirection MakeDirection(uint32_t entry) {
            const int32_t* row = &rx::RD_DIRECTIONS[entry * rx::RD_DIRECTION_WORDS];
            std::array<int64_t, 3> d = {row[0], row[1], row[2]};

            uint32_t axis = 0;
            for (uint32_t k = 1; k < 3; ++k) {
                if (Absolute(d[k]) > Absolute(d[axis]))
                    axis = k;
            }

            // 線は両向きなので、A の向きを正にそろえる
            if (d[axis] < 0) {
                for (int64_t& component : d)
                    component = -component;
            }

            LineDirection direction;
            direction.axes = {axis, axis == 0 ? 1u : 0u, axis == 2 ? 1u : 2u};
            for (uint32_t k = 0; k < 3; ++k)
                direction.steps[k] = d[direction.axes[k]];

            direction.viewQ16 = static_cast<uint32_t>(row[3]);
            direction.reachSlabs = row[4];

            return direction;
        }

        // A の板 k の入口の位置(単位 1 / (2^16 × D_A))から、その板の中の最初のセル。動く向きが負なら、ちょうど境の点は下のセル
        int64_t CellOfPosition(int64_t position, int64_t denominator, int64_t step) {
            if (step >= 0)
                return FloorDivide(position, denominator);

            return -FloorDivide(-position, denominator) - 1;
        }

        // 線 1 本を歩いて対を作る
        class LineWalker {
        public:
            LineWalker(RadiationGrid grid, std::span<const uint32_t> absorptance, const LineDirection& direction,
                       std::vector<RadiationPair>& pairs)
                : m_absorptance(absorptance), m_direction(direction), m_pairs(pairs) {
                const std::array<uint32_t, 3> sizes = {grid.sizeX, grid.sizeY, grid.sizeZ};
                for (uint32_t k = 0; k < 3; ++k)
                    m_sizes[k] = sizes[direction.axes[k]];

                m_strides = {1, grid.sizeX, int64_t{grid.sizeX} * grid.sizeY};
                m_shareDivisor = 2 * rx::RD_GROUP_DIRECTIONS;
            }

            int64_t SizeOfAxis(uint32_t k) const { return m_sizes[k]; }

            // 板 0 の入口で B・C の位置が startB・startC(Q16)の線
            void Walk(int64_t startB, int64_t startC) {
                m_open.clear();
                const int64_t stepA = m_direction.steps[0];
                const int64_t denominator = ONE_Q16 * stepA;
                const int64_t positionB = startB * stepA;
                const int64_t positionC = startC * stepA;

                // 線が格子(と 1 セルの余白)の中にいる板だけ歩く
                const auto rangeB = SlabRange(positionB, m_direction.steps[1], m_sizes[1], denominator);
                const auto rangeC = SlabRange(positionC, m_direction.steps[2], m_sizes[2], denominator);
                const int64_t first = std::max<int64_t>({0, rangeB[0], rangeC[0]});
                const int64_t last = std::min<int64_t>({m_sizes[0] - 1, rangeB[1], rangeC[1]});
                for (int64_t slab = first; slab <= last; ++slab) {
                    const int64_t entryB = positionB + slab * m_direction.steps[1] * ONE_Q16;
                    const int64_t entryC = positionC + slab * m_direction.steps[2] * ONE_Q16;
                    WalkSlab(slab, entryB, entryC, denominator);
                }
            }

        private:
            std::span<const uint32_t> m_absorptance;
            const LineDirection& m_direction;
            std::vector<RadiationPair>& m_pairs;
            std::array<int64_t, 3> m_sizes{};
            std::array<int64_t, 3> m_strides{};
            uint32_t m_shareDivisor = 1;

            struct OpenAbsorber {
                uint32_t cell = 0;
                uint32_t absorptanceQ16 = 0;
                uint32_t transmittanceQ16 = 0;  // このセルから今の位置までの途中の透過
                int64_t slab = 0;
            };
            std::vector<OpenAbsorber> m_open;

            // 板の入口の位置が −1 〜 size + 1 セルにある板の範囲(位置 = position + 板 × step × 2^16)
            static std::array<int64_t, 2> SlabRange(int64_t position, int64_t step, int64_t size, int64_t denominator) {
                const int64_t low = -denominator;
                const int64_t high = (size + 1) * denominator;
                if (step == 0) {
                    const bool inside = position >= low && position <= high;
                    return inside ? std::array<int64_t, 2>{0, INT64_MAX} : std::array<int64_t, 2>{1, 0};
                }

                const int64_t perSlab = Absolute(step) * ONE_Q16;
                if (step > 0)
                    return {-FloorDivide(position - low, perSlab), FloorDivide(high - position, perSlab)};

                return {-FloorDivide(high - position, perSlab), FloorDivide(position - low, perSlab)};
            }

            // 境を越える所(板の中の割合 s の分子)。越えなければ −1
            static int64_t Crossing(int64_t entry, int64_t exit, int64_t cell, int64_t denominator, int64_t step) {
                if (step > 0) {
                    const int64_t boundary = (cell + 1) * denominator;
                    return boundary < exit ? boundary - entry : -1;
                }

                if (step < 0) {
                    const int64_t boundary = cell * denominator;
                    return exit < boundary ? entry - boundary : -1;
                }

                return -1;
            }

            void WalkSlab(int64_t slab, int64_t entryB, int64_t entryC, int64_t denominator) {
                const int64_t stepB = m_direction.steps[1];
                const int64_t stepC = m_direction.steps[2];
                const int64_t exitB = entryB + stepB * ONE_Q16;
                const int64_t exitC = entryC + stepC * ONE_Q16;
                int64_t cellB = CellOfPosition(entryB, denominator, stepB);
                int64_t cellC = CellOfPosition(entryC, denominator, stepC);
                const int64_t crossB = Crossing(entryB, exitB, cellB, denominator, stepB);
                const int64_t crossC = Crossing(entryC, exitC, cellC, denominator, stepC);

                Visit(slab, cellB, cellC);

                // 2 つの境を越える板は、先に越える方から(割合 = 分子 ÷ |D|。掛け算で比べる。同じなら B から)
                const bool bothCross = crossB >= 0 && crossC >= 0;
                const bool firstB = crossB >= 0 && (!bothCross || crossB * Absolute(stepC) <= crossC * Absolute(stepB));
                const int64_t moveB = stepB > 0 ? 1 : -1;
                const int64_t moveC = stepC > 0 ? 1 : -1;
                if (firstB) {
                    cellB += moveB;
                    Visit(slab, cellB, cellC);
                }

                if (crossC >= 0) {
                    cellC += moveC;
                    Visit(slab, cellB, cellC);
                }

                if (crossB >= 0 && !firstB) {
                    cellB += moveB;
                    Visit(slab, cellB, cellC);
                }
            }

            void Visit(int64_t slab, int64_t cellB, int64_t cellC) {
                if (cellB < 0 || cellB >= m_sizes[1] || cellC < 0 || cellC >= m_sizes[2])
                    return;  // 格子の外は何も無い(透明)

                std::array<int64_t, 3> coordinates{};
                coordinates[m_direction.axes[0]] = slab;
                coordinates[m_direction.axes[1]] = cellB;
                coordinates[m_direction.axes[2]] = cellC;
                const auto cell = static_cast<uint32_t>(coordinates[0] * m_strides[0] + coordinates[1] * m_strides[1] +
                                                        coordinates[2] * m_strides[2]);
                const uint32_t absorptance = m_absorptance[cell];
                if (absorptance == 0)
                    return;

                AddPairs(cell, absorptance, slab);
            }

            // 開いている吸収するセル(手前)と、このセルの対。手前のセルの透過をこのセルの分だけ減らし、薄いもの・遠いものは閉じる
            void AddPairs(uint32_t cell, uint32_t absorptance, int64_t slab) {
                size_t kept = 0;
                for (OpenAbsorber& open : m_open) {
                    if (slab - open.slab > m_direction.reachSlabs)
                        continue;

                    const uint64_t transmittance = open.transmittanceQ16;
                    const auto view = static_cast<uint32_t>((uint64_t{m_direction.viewQ16} * transmittance) >> 16);
                    const uint64_t both = (uint64_t{open.absorptanceQ16} * absorptance) >> 16;
                    const auto share = static_cast<uint32_t>(((both * transmittance) >> 16) / m_shareDivisor);
                    if (view != 0)
                        m_pairs.push_back({.first = open.cell, .second = cell, .viewQ16 = view, .shareQ16 = share});

                    open.transmittanceQ16 = static_cast<uint32_t>(
                        (transmittance * (static_cast<uint64_t>(ONE_Q16) - absorptance)) >> 16);
                    if (open.transmittanceQ16 >= DROP_TRANSMITTANCE_Q16)
                        m_open[kept++] = open;
                }

                m_open.resize(kept);
                m_open.push_back({.cell = cell,
                                  .absorptanceQ16 = absorptance,
                                  .transmittanceQ16 = static_cast<uint32_t>(ONE_Q16),
                                  .slab = slab});
            }
        };

        // B か C の軸の線の始まり(Q16)の範囲: 線が板 0〜sizeA のどこかで格子(と 1 セルの余白)を通るもの
        std::array<int64_t, 2> StartRange(int64_t sizeOther, int64_t sizeA, int64_t step, int64_t stepA) {
            const int64_t drift = FloorDivide(sizeA * step * ONE_Q16, stepA);  // 板 sizeA までに動く量(Q16)
            const int64_t low = -ONE_Q16 - (drift > 0 ? drift : 0) - LINE_SPACING_Q16;
            const int64_t high = (sizeOther + 1) * ONE_Q16 - (drift < 0 ? drift : 0) + LINE_SPACING_Q16;

            return {low, high};
        }

        void WalkDirection(RadiationGrid grid, std::span<const uint32_t> absorptance, uint32_t entry, uint64_t tick,
                           uint64_t seed, std::vector<RadiationPair>& pairs) {
            const LineDirection direction = MakeDirection(entry);
            LineWalker walker(grid, absorptance, direction, pairs);

            // 格子のずれ(R6: 世界の種・刻み・向き・用途)
            const int64_t offsetB = fx::FxRandomBelow(fx::FxHash64(seed, tick, entry, PURPOSE_OFFSET_B),
                                                      static_cast<uint32_t>(LINE_SPACING_Q16));
            const int64_t offsetC = fx::FxRandomBelow(fx::FxHash64(seed, tick, entry, PURPOSE_OFFSET_C),
                                                      static_cast<uint32_t>(LINE_SPACING_Q16));
            const int64_t sizeA = walker.SizeOfAxis(0);
            const auto rangeB = StartRange(walker.SizeOfAxis(1), sizeA, direction.steps[1], direction.steps[0]);
            const auto rangeC = StartRange(walker.SizeOfAxis(2), sizeA, direction.steps[2], direction.steps[0]);

            const int64_t firstB = FloorDivide(rangeB[0] - offsetB, LINE_SPACING_Q16);
            const int64_t lastB = FloorDivide(rangeB[1] - offsetB, LINE_SPACING_Q16);
            const int64_t firstC = FloorDivide(rangeC[0] - offsetC, LINE_SPACING_Q16);
            const int64_t lastC = FloorDivide(rangeC[1] - offsetC, LINE_SPACING_Q16);
            for (int64_t lineB = firstB; lineB <= lastB; ++lineB) {
                for (int64_t lineC = firstC; lineC <= lastC; ++lineC)
                    walker.Walk(offsetB + lineB * LINE_SPACING_Q16, offsetC + lineC * LINE_SPACING_Q16);
            }
        }

    }  // namespace

    uint32_t RadiationDirectionsPerTick() {
        return rx::RD_GROUP_DIRECTIONS;
    }

    std::vector<RadiationPair> FindRadiationPairs(RadiationGrid grid, std::span<const uint32_t> absorptanceQ16,
                                                  uint64_t tick, uint64_t seed) {
        std::vector<RadiationPair> pairs;
        const size_t cellCount = size_t{grid.sizeX} * grid.sizeY * grid.sizeZ;
        if (cellCount == 0 || absorptanceQ16.size() != cellCount)
            return pairs;

        // 刻みの向きの組(R6)。1 組で半球の全部の向き
        const uint32_t group = fx::FxRandomBelow(fx::FxHash64(seed, tick, 0, PURPOSE_GROUP), rx::RD_GROUP_COUNT);
        for (uint32_t index = 0; index < rx::RD_GROUP_DIRECTIONS; ++index)
            WalkDirection(grid, absorptanceQ16, group * rx::RD_GROUP_DIRECTIONS + index, tick, seed, pairs);

        return pairs;
    }

}  // namespace bicameral::sim
