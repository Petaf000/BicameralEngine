// radiation_lines_test.cpp — 放射の対を平行な線の束で見つける(T-0584・ADR-0262。sim/radiation_lines)。
// CPU だけで走る。確かめること:
//   - 黒い平行な板の見え方の割合が解析解の ±10%(60 刻みの平均)
//   - 完全に遮る壁の向こうの受け手との対はちょうど 0(陰)
//   - どのセルも、その刻みの対の分け前の合計 ≤ 1(受け手が多くの送り手から受けても行き過ぎない。T-0583 の上限の前提)
//   - 保存がビット単位・対の並びによらない・同じ刻みと種なら同じ対
//   - 熱い板の前の熱容量の小さい膜が、板の温度を超えない(坂を下る)
// double を使うのは試験の参照値だけ。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <random>
#include <vector>

#include "sim/radiation_lines.h"
#include "sim/radiation_reference.h"

namespace {

    using namespace bicameral::sim;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr uint32_t ONE_Q16 = 65536;

    struct Box {
        uint32_t size = 0;
        std::vector<uint32_t> absorptance;

        explicit Box(uint32_t side) : size(side), absorptance(size_t{side} * side * side, 0) {}

        uint32_t Index(uint32_t x, uint32_t y, uint32_t z) const { return (z * size + y) * size + x; }
        RadiationGrid Grid() const { return {.sizeX = size, .sizeY = size, .sizeZ = size}; }

        // y の層の正方形(x・z が first から side 個)
        void FillLayer(uint32_t y, uint32_t first, uint32_t side, uint32_t value) {
            for (uint32_t z = first; z < first + side; ++z) {
                for (uint32_t x = first; x < first + side; ++x)
                    absorptance[Index(x, y, z)] = value;
            }
        }
    };

    // 1 辺 s の正方形が距離 d で向かい合う時の見え方の割合(平行・同軸。Incropera の表の式)
    double ParallelSquaresView(double side, double distance) {
        const double x = side / distance;
        const double x2 = x * x;
        const double term1 = std::log(std::sqrt((1 + x2) * (1 + x2) / (1 + 2 * x2)));
        const double term2 = 2 * x * std::sqrt(1 + x2) * std::atan(x / std::sqrt(1 + x2));
        const double term3 = 2 * x * std::atan(x);

        return 2 / (std::numbers::pi * x2) * (term1 + term2 - term3);
    }

    bool InLayer(const Box& box, uint32_t cell, uint32_t y) {
        return (cell / box.size) % box.size == y;
    }

    // --- 平行な板 ---

    void TestParallelPlates() {
        constexpr uint32_t SIDE = 16;
        for (const uint32_t gap : {2u, 4u, 8u}) {
            Box box(40);
            const uint32_t lower = 8;
            const uint32_t upper = lower + gap + 1;
            box.FillLayer(lower, 12, SIDE, ONE_Q16);
            box.FillLayer(upper, 12, SIDE, ONE_Q16);

            double total = 0;
            constexpr uint64_t TICKS = 60;
            for (uint64_t tick = 0; tick < TICKS; ++tick) {
                for (const RadiationPair& pair : FindRadiationPairs(box.Grid(), box.absorptance, tick, 7)) {
                    const bool across = (InLayer(box, pair.first, lower) && InLayer(box, pair.second, upper)) ||
                                        (InLayer(box, pair.first, upper) && InLayer(box, pair.second, lower));
                    if (across)
                        total += pair.viewQ16 / static_cast<double>(ONE_Q16);
                }
            }

            const double measured = total / TICKS / (SIDE * SIDE);
            const double expected = ParallelSquaresView(SIDE, gap);
            std::printf("平行な板 16×16 セル・隙間 %u セル: F = %.4f(解析解 %.4f・%+.1f%%)\n", gap, measured, expected,
                        (measured / expected - 1) * 100);
            EXPECT(std::abs(measured / expected - 1) < 0.10);
        }
    }

    // --- 陰 ---

    void TestShadowIsExactlyZero() {
        Box box(32);
        box.FillLayer(6, 8, 16, ONE_Q16);   // 送り手
        box.FillLayer(12, 0, 32, ONE_Q16);  // 格子の端まで届く壁
        box.FillLayer(18, 8, 16, ONE_Q16);  // 受け手

        uint64_t behind = 0;
        uint64_t wall = 0;
        for (uint64_t tick = 0; tick < 60; ++tick) {
            for (const RadiationPair& pair : FindRadiationPairs(box.Grid(), box.absorptance, tick, 3)) {
                const bool first6 = InLayer(box, pair.first, 6);
                const bool second6 = InLayer(box, pair.second, 6);
                const bool first18 = InLayer(box, pair.first, 18);
                const bool second18 = InLayer(box, pair.second, 18);
                if ((first6 && second18) || (first18 && second6))
                    behind += pair.viewQ16;
                if ((first6 && InLayer(box, pair.second, 12)) || (second6 && InLayer(box, pair.first, 12)))
                    wall += pair.viewQ16;
            }
        }

        std::printf("壁の陰: 送り手と受け手の対の見え方の和 %llu(壁へは %llu)\n",
                    static_cast<unsigned long long>(behind), static_cast<unsigned long long>(wall));
        EXPECT(behind == 0);
        EXPECT(wall > 0);
    }

    // --- 受け手の分け前 ---

    void TestShareSumAtMostOne() {
        std::mt19937_64 random(11);
        uint64_t worst = 0;
        size_t pairCount = 0;
        for (int world = 0; world < 6; ++world) {
            Box box(24);
            std::uniform_int_distribution<uint32_t> kind(0, 3);
            std::uniform_int_distribution<uint32_t> partial(1, ONE_Q16);
            for (uint32_t& value : box.absorptance) {
                const uint32_t pick = kind(random);
                value = pick == 0 ? 0 : (pick == 1 ? ONE_Q16 : partial(random) >> (pick == 2 ? 0 : 4));
            }

            for (uint64_t tick = 0; tick < 8; ++tick) {
                std::vector<uint64_t> shares(box.absorptance.size(), 0);
                const std::vector<RadiationPair> pairs = FindRadiationPairs(box.Grid(), box.absorptance, tick, world);
                pairCount += pairs.size();
                for (const RadiationPair& pair : pairs) {
                    shares[pair.first] += pair.shareQ16;
                    shares[pair.second] += pair.shareQ16;
                }

                worst = std::max(worst, *std::ranges::max_element(shares));
            }
        }

        std::printf("分け前の合計の最大 %llu / 65536(対 %zu 個)\n", static_cast<unsigned long long>(worst), pairCount);
        EXPECT(worst <= ONE_Q16);
        EXPECT(pairCount > 0);
    }

    // --- 保存・並び・決定性 ---

    void TestConservationOrderDeterminism() {
        std::mt19937_64 random(5);
        Box box(20);
        std::uniform_int_distribution<uint32_t> partial(0, ONE_Q16);
        for (uint32_t& value : box.absorptance)
            value = random() % 3 == 0 ? partial(random) : 0;

        std::uniform_int_distribution<uint32_t> temperature(250000, 2500000);
        std::vector<RadiationSurface> surfaces(box.absorptance.size());
        for (size_t index = 0; index < surfaces.size(); ++index) {
            surfaces[index] = {.temperatureMk = temperature(random),
                               .heatCapacityNanojoulesPerKelvin = 1ull << (20 + random() % 20),
                               .absorptanceQ16 = box.absorptance[index]};
        }

        std::vector<RadiationPair> pairs = FindRadiationPairs(box.Grid(), box.absorptance, 9, 1);
        const std::vector<RadiationPair> again = FindRadiationPairs(box.Grid(), box.absorptance, 9, 1);
        const std::vector<RadiationPair> other = FindRadiationPairs(box.Grid(), box.absorptance, 10, 1);
        const auto same = [](const std::vector<RadiationPair>& a, const std::vector<RadiationPair>& b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& p, const auto& q) {
                       return p.first == q.first && p.second == q.second && p.viewQ16 == q.viewQ16 &&
                              p.shareQ16 == q.shareQ16;
                   });
        };
        EXPECT(same(pairs, again));
        EXPECT(!same(pairs, other));

        const std::vector<int64_t> changes = ExchangeRadiation(surfaces, pairs);
        int64_t sum = 0;
        for (const int64_t change : changes)
            sum += change;
        EXPECT(sum == 0);

        std::ranges::reverse(pairs);
        EXPECT(ExchangeRadiation(surfaces, pairs) == changes);
    }

    // --- 熱い板の前の小さい膜 ---

    void TestFilmStaysBelowSource() {
        Box box(24);
        box.FillLayer(4, 4, 16, ONE_Q16);      // 熱い板(1500 K・大きい熱容量)
        box.FillLayer(6, 4, 16, ONE_Q16 / 3);  // 薄い膜(熱容量が小さい)
        box.FillLayer(8, 4, 16, ONE_Q16 / 2);  // 2 枚目の膜
        box.FillLayer(14, 4, 16, ONE_Q16);     // 冷たい板(300 K)

        std::vector<int64_t> energies(box.absorptance.size(), 0);
        std::vector<uint64_t> capacities(box.absorptance.size(), 0);
        for (size_t index = 0; index < energies.size(); ++index) {
            const uint32_t y = static_cast<uint32_t>((index / box.size) % box.size);
            capacities[index] = (y == 4 || y == 14) ? (1ull << 50) : 2000000;  // nJ/K: 板は無限・膜は 2 mJ/K
            const int64_t kelvin = y == 4 ? 1500 : 300;
            energies[index] = kelvin * static_cast<int64_t>(capacities[index] / 1000000);  // mJ = K × mJ/K
        }

        int64_t startSum = 0;
        for (const int64_t energy : energies)
            startSum += energy;

        bool ordered = true;
        uint32_t hottestFilm = 0;
        for (uint64_t tick = 0; tick < 600; ++tick) {
            std::vector<RadiationSurface> surfaces(energies.size());
            for (size_t index = 0; index < energies.size(); ++index) {
                const uint64_t milliJoulesPerKelvin = capacities[index] / 1000000;
                surfaces[index] = {.temperatureMk = static_cast<uint32_t>(energies[index] * 1000 /
                                                                          static_cast<int64_t>(milliJoulesPerKelvin)),
                                   .heatCapacityNanojoulesPerKelvin = capacities[index],
                                   .absorptanceQ16 = box.absorptance[index]};
            }

            const std::vector<int64_t> changes = ExchangeRadiation(
                surfaces, FindRadiationPairs(box.Grid(), box.absorptance, tick, 2));
            for (size_t index = 0; index < energies.size(); ++index) {
                energies[index] += changes[index];
                const uint32_t y = static_cast<uint32_t>((index / box.size) % box.size);
                if (y == 6 || y == 8) {
                    const int64_t temperatureMk = energies[index] * 1000 / 2;
                    ordered = ordered && temperatureMk <= 1500000 && temperatureMk >= 299000;
                    hottestFilm = std::max(hottestFilm, static_cast<uint32_t>(temperatureMk));
                }
            }
        }

        int64_t endSum = 0;
        for (const int64_t energy : energies)
            endSum += energy;

        std::printf("膜の最高温度 %u mK(板 1,500,000 mK)\n", hottestFilm);
        EXPECT(ordered);
        EXPECT(hottestFilm > 300000);
        EXPECT(endSum == startSum);
    }

}  // namespace

int main() {
    TestParallelPlates();
    TestShadowIsExactlyZero();
    TestShareSumAtMostOne();
    TestConservationOrderDeterminism();
    TestFilmStaysBelowSource();
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("radiation_lines: すべて通過\n");

    return 0;
}
