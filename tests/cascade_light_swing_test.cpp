// cascade_light_swing_test.cpp — 研究の測り(T-1100・T-1102。R-R1):
//   1. 距離ごとの段(RC)の筋が、物の温度をどれだけ揺らすか: 遠くの小さな火(焚き火 1 セル)から 8 m・16 m の円の 24 か所の受け手が
//      毎刻み受ける量を、石・草・紙の熱容量(仮の値)で温度に直し、毎刻みの揺れの幅と、平均の熱(正しい値 = 面の積分)との差を出す。
//   2. 釣り合いとの差だけを運ぶ形(CascadeParams::referenceMk。ADR-0461)の閉じた箱: 同じ温度の箱・熱い塊のある箱で、
//      正味の出入りと「冷たい側から熱い側へ」の流れを、今の形と比べる。
// どちらも記録の測り(合否は 1 mK の計器の精度〔D-435〕で見る所だけ)。データは sim/cascade_light の CPU リファレンスから。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <numbers>
#include <vector>

#include "sim/cascade_light.h"

namespace {

    using namespace bicameral;
    using namespace bicameral::sim;
    namespace rx = bicameral::reaction;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // --- 場面の道具(cascade_light_test と同じ値)---

    enum class Material : uint8_t {
        Air,
        Stone,
        Black,
    };

    struct Voxel {
        Material material = Material::Air;
        uint32_t temperatureMk = 0;
    };

    constexpr uint32_t FIRE_MK = 1200000;
    constexpr uint32_t QUIET_MK = 500000;
    constexpr double UNIT_PER_J = 1000.0 * 65536.0;  // 量は 2^−16 mJ

    rx::RcCellLight LightOf(Material material) {
        if (material == Material::Air)
            return {};

        const uint32_t reflectance = material == Material::Stone ? 32768u : 0u;

        return {.depthShortQ32 = rx::RD_DEPTH_SATURATED_Q32,
                .depthLongQ32 = rx::RD_DEPTH_SATURATED_Q32,
                .reflectanceShortQ16 = reflectance,
                .reflectanceLongQ16 = reflectance};
    }

    CascadeGrid MakeGrid(int64_t side, const std::function<Voxel(int64_t, int64_t, int64_t)>& shape) {
        CascadeGrid grid;
        grid.size = {side, side, side};
        grid.lights.resize(static_cast<size_t>(side * side * side));
        grid.temperatureMk.resize(grid.lights.size());
        for (int64_t z = 0; z < side; ++z) {
            for (int64_t y = 0; y < side; ++y) {
                for (int64_t x = 0; x < side; ++x) {
                    const Voxel voxel = shape(x, y, z);
                    const size_t at = grid.IndexOf(x, y, z);
                    grid.lights[at] = LightOf(voxel.material);
                    grid.temperatureMk[at] = voxel.temperatureMk;
                }
            }
        }

        return grid;
    }

    CascadeParams Params(uint32_t referenceMk = 0, uint32_t quietMk = QUIET_MK) {
        return {.baseTexels = 4,
                .levels = 5,
                .intervalBlocks = 1,
                .seed = 7,
                .quietBelowMk = quietMk,
                .referenceMk = referenceMk};
    }

    uint64_t Received(const CascadeTickResult& result, size_t cell) {
        return result.absorbed[(cell * rx::LT_BANDS) + rx::LT_BAND_SHORT] +
               result.absorbed[(cell * rx::LT_BANDS) + rx::LT_BAND_LONG];
    }

    uint64_t Emitted(const CascadeTickResult& result, size_t cell) {
        return result.emitted[(cell * rx::LT_BANDS) + rx::LT_BAND_SHORT] +
               result.emitted[(cell * rx::LT_BANDS) + rx::LT_BAND_LONG];
    }

    // 遮る物の無い面と面の G(cascade_light_test の FaceExchange と同じ積分)
    std::array<double, 3> FacePoint(const std::array<int64_t, 3>& cell, int dir, double u, double v) {
        const int axis = dir / 2;
        std::array<double, 3> p = {static_cast<double>(cell[0]) + 0.5, static_cast<double>(cell[1]) + 0.5,
                                   static_cast<double>(cell[2]) + 0.5};
        p[axis] += (dir % 2) != 0 ? -0.5 : 0.5;
        p[(axis + 1) % 3] += u;
        p[(axis + 2) % 3] += v;

        return p;
    }

    double CubeExchange(const std::array<int64_t, 3>& a, const std::array<int64_t, 3>& b) {
        constexpr int MM = 6;
        double g = 0;
        for (int dirA = 0; dirA < 6; ++dirA) {
            for (int dirB = 0; dirB < 6; ++dirB) {
                const double signA = (dirA % 2) != 0 ? -1.0 : 1.0;
                const double signB = (dirB % 2) != 0 ? -1.0 : 1.0;
                for (int i = 0; i < MM * MM; ++i) {
                    const auto pa = FacePoint(a, dirA, ((i % MM) + 0.5) / MM - 0.5, ((i / MM) + 0.5) / MM - 0.5);
                    for (int j = 0; j < MM * MM; ++j) {
                        const auto pb = FacePoint(b, dirB, ((j % MM) + 0.5) / MM - 0.5, ((j / MM) + 0.5) / MM - 0.5);
                        const std::array<double, 3> d = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
                        const double r2 = (d[0] * d[0]) + (d[1] * d[1]) + (d[2] * d[2]);
                        const double cosA = std::max(0.0, signA * d[dirA / 2]);
                        const double cosB = std::max(0.0, -signB * d[dirB / 2]);
                        g += cosA * cosB / (std::numbers::pi * r2 * r2 * MM * MM * MM * MM);
                    }
                }
            }
        }

        return g;
    }

    // --- 1. 筋が物の温度を揺らす幅(T-1100)---

    struct Body {
        const char* name;
        double heatCapacityJPerK;  // 0.5 m のセル 1 つの熱容量(仮の値)
    };

    // 石: 0.125 m³ × 2.0 MJ/(m³·K)・草: 乾いた草と水 約 0.5 kg × 2 kJ/(kg·K)・紙: 0.5 m 角の紙 1 枚 80 g/m² × 0.25 m² × 1.3 kJ/(kg·K)
    constexpr std::array<Body, 3> BODIES = {Body{"石", 250000.0}, Body{"草", 1000.0}, Body{"紙", 26.0}};

    void RecordSwing(int64_t radius, uint32_t ticks) {
        const int64_t side = (2 * radius) + 16;
        const int64_t center = side / 2;
        constexpr int COUNT = 24;
        std::vector<std::array<int64_t, 3>> receivers;
        for (int k = 0; k < COUNT; ++k) {
            const double angle = 2 * std::numbers::pi * k / COUNT;
            receivers.push_back({center + std::lround(static_cast<double>(radius) * std::cos(angle)), center,
                                 center + std::lround(static_cast<double>(radius) * std::sin(angle))});
        }

        const CascadeGrid grid = MakeGrid(side, [&](int64_t x, int64_t y, int64_t z) {
            if (x == center && y == center && z == center)
                return Voxel{Material::Stone, FIRE_MK};

            const bool receiver = std::ranges::any_of(
                receivers, [&](const auto& r) { return r[0] == x && r[1] == y && r[2] == z; });

            return receiver ? Voxel{Material::Black, 0} : Voxel{};
        });

        std::vector<double> exact;
        double exactSum = 0;
        for (const auto& r : receivers) {
            exact.push_back(CubeExchange({center, center, center}, r));
            exactSum += exact.back();
        }

        // 受け手ごと・刻みごとの量(J)
        std::vector<std::vector<double>> joules(COUNT);
        CascadeCarry carry;
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            const CascadeTickResult result = StepCascadeLight(grid, carry, Params(), tick);
            for (int k = 0; k < COUNT; ++k)
                joules[k].push_back(static_cast<double>(Received(
                                        result, grid.IndexOf(receivers[k][0], receivers[k][1], receivers[k][2]))) /
                                    UNIT_PER_J);

            carry = result.carry;
        }

        // 平均の熱: RC の全部の受け手の和は正しい値の和とほぼ同じ(保存)なので、受け手ごとの正しい値は和を G の比で分けたもの
        double total = 0;
        for (const auto& series : joules) {
            for (const double value : series)
                total += value / ticks;
        }

        for (const Body& body : BODIES) {
            double meanRiseMk = 0;
            double worstSwingMk = 0;    // 自分の平均の上昇からの、温度のずれの最大(揺れの幅の半分)
            double worstStepMk = 0;     // 1 刻みの上昇の最大
            double worstBias1sMk = 0;   // 1 秒(60 刻み)の上昇の、正しい値との差の最大
            double worstBias500Mk = 0;  // 全部の刻みの平均の上昇の、正しい値との差の最大(1 秒あたり)
            for (int k = 0; k < COUNT; ++k) {
                const double expected = total * exact[k] / exactSum;
                double mean = 0;
                for (const double value : joules[k])
                    mean += value / ticks;

                double deviation = 0;
                for (uint32_t tick = 0; tick < ticks; ++tick) {
                    deviation += joules[k][tick] - mean;
                    worstSwingMk = std::max(worstSwingMk, std::fabs(deviation) / body.heatCapacityJPerK * 1000);
                    worstStepMk = std::max(worstStepMk, joules[k][tick] / body.heatCapacityJPerK * 1000);
                }

                double first = 0;
                for (uint32_t tick = 0; tick < std::min<uint32_t>(60, ticks); ++tick)
                    first += joules[k][tick];

                worstBias1sMk = std::max(worstBias1sMk,
                                         std::fabs(first - (expected * 60)) / body.heatCapacityJPerK * 1000);
                worstBias500Mk = std::max(worstBias500Mk,
                                          std::fabs(mean - expected) * 60 / body.heatCapacityJPerK * 1000);
                meanRiseMk += expected / body.heatCapacityJPerK * 1000 / COUNT;
            }

            std::printf(
                "温度の揺れ(焚き火 1 セル・%lld m の円・%s %.0f J/K): 平均の上昇 %.4f mK/刻み(%.2f mK/秒)・1 "
                "刻みの上昇の最大 %.4f mK・"
                "自分の平均の上昇からのずれの最大 %.4f mK(%u 刻み)・1 秒の上昇の正しい値との差の最大 %.4f mK・"
                "%u 刻みの平均の上昇の正しい値との差の最大 %.4f mK/秒\n",
                static_cast<long long>(radius / 2), body.name, body.heatCapacityJPerK, meanRiseMk, meanRiseMk * 60,
                worstStepMk, worstSwingMk, ticks, worstBias1sMk, ticks, worstBias500Mk);
        }
    }

    // --- 2. 釣り合い(参照の温度)との差だけを運ぶ形(T-1102・ADR-0461)---

    // 期待: Zero = どのセルも正味ちょうど 0・NoLoss = 参照の温度のセルは熱を失わない・NoGain = 参照の温度のセルは熱を得ない
    enum class Expect3 : uint8_t {
        Zero,
        NoLoss,
        NoGain,
    };

    // 場面を今の形(参照なし)と差だけを運ぶ形で回し、参照の温度のセル(殻の一番外の層を除く)の正味を比べる
    void RecordScene(const char* name, const CascadeGrid& grid, uint32_t referenceMk, uint32_t ticks, Expect3 expect) {
        const int64_t side = grid.size[0];
        const auto outer = [side](size_t cell) {
            const auto flat = static_cast<int64_t>(cell);
            const std::array<int64_t, 3> at = {flat % side, (flat / side) % side, flat / (side * side)};

            return std::ranges::any_of(at, [side](int64_t v) { return v == 0 || v == side - 1; });
        };

        for (const bool relative : {false, true}) {
            std::vector<double> net(grid.CellCount(), 0);
            std::array<CascadeCarry, 2> carry{};
            bool conserved = true;
            const auto conserve = [&conserved](const CascadeTickResult& result) {
                for (const auto& band : result.ledger)
                    conserved = conserved && band.emitted + band.carriedIn ==
                                                 band.absorbed + band.reflected + band.escaped + band.held;
            };
            for (uint32_t tick = 0; tick < ticks; ++tick) {
                if (!relative) {
                    const CascadeTickResult result = StepCascadeLight(grid, carry[0], Params(0, 0), tick);
                    conserve(result);
                    for (size_t cell = 0; cell < grid.CellCount(); ++cell)
                        net[cell] += static_cast<double>(Received(result, cell)) -
                                     static_cast<double>(Emitted(result, cell));

                    carry[0] = result.carry;
                    continue;
                }

                const CascadeRelativeResult result = StepCascadeLightRelative(grid, carry, Params(referenceMk, 0),
                                                                              tick);
                for (size_t at = 0; at < 2; ++at) {
                    const double sign = at == 0 ? 1.0 : -1.0;
                    conserve(result.sides[at]);
                    for (size_t cell = 0; cell < grid.CellCount(); ++cell)
                        net[cell] += sign * (static_cast<double>(Received(result.sides[at], cell)) -
                                             static_cast<double>(Emitted(result.sides[at], cell)));

                    carry[at] = result.sides[at].carry;
                }
            }

            double worstMk = 0;
            double worstLossMk = 0;
            double worstGainMk = 0;
            for (size_t cell = 0; cell < grid.CellCount(); ++cell) {
                if (outer(cell) || grid.temperatureMk[cell] != referenceMk)
                    continue;

                const double mk = net[cell] / ticks / UNIT_PER_J /
                                  170.0;  // 1 セル = 170 kJ/K(cascade_light_test と同じ)
                worstMk = std::max(worstMk, std::fabs(mk));
                worstLossMk = std::max(worstLossMk, -mk);
                worstGainMk = std::max(worstGainMk, mk);
            }

            std::printf(
                "%s(%s・%u 刻み): 参照の温度のセルの正味 最大 %.4f mK/刻み・失った最大 %.4f・得た最大 %.4f "
                "mK/刻み・保存 %s\n",
                name, relative ? "差だけを運ぶ" : "今の形", ticks, worstMk, std::max(0.0, worstLossMk),
                std::max(0.0, worstGainMk), conserved ? "ビット単位" : "崩れた");
            EXPECT(conserved);
            if (!relative)
                continue;

            if (expect == Expect3::Zero)
                EXPECT(worstMk == 0);
            else if (expect == Expect3::NoLoss)
                EXPECT(worstLossMk <= 0);
            else
                EXPECT(worstGainMk <= 0);
        }
    }

    // 1 m 厚の石の殻(内のり 7 m)の中に石の塊。殻は wallMk・塊は blockMk
    CascadeGrid ShellWithBlock(uint32_t wallMk, uint32_t blockMk) {
        constexpr int64_t SIZE = 18;

        return MakeGrid(SIZE, [=](int64_t x, int64_t y, int64_t z) {
            const bool shell = x < 2 || y < 2 || z < 2 || x >= SIZE - 2 || y >= SIZE - 2 || z >= SIZE - 2;
            const bool block = x >= 7 && x < 10 && y >= 2 && y < 6 && z >= 7 && z < 10;
            if (block)
                return Voxel{Material::Stone, blockMk};

            return shell ? Voxel{Material::Stone, wallMk} : Voxel{};
        });
    }

    // 空へ開いた石の板(24³ の世界の高さ 4〜5。世界の外 = 空)。hotMk が 0 でなければ板の上に 1 セルの熱い石
    CascadeGrid OpenSlab(uint32_t slabMk, uint32_t hotMk) {
        return MakeGrid(24, [=](int64_t x, int64_t y, int64_t z) {
            if (y >= 4 && y < 6)
                return Voxel{Material::Stone, slabMk};

            if (hotMk != 0 && y == 6 && x == 12 && z == 12)
                return Voxel{Material::Stone, hotMk};

            return Voxel{};
        });
    }

}  // namespace

int main() {
    RecordSwing(16, 500);
    RecordSwing(32, 250);
    RecordScene("閉じた箱 1000 K", ShellWithBlock(1000000, 1000000), 1000000, 60, Expect3::Zero);
    RecordScene("閉じた箱 1000 K + 熱い塊 1100 K", ShellWithBlock(1000000, 1100000), 1000000, 60, Expect3::NoLoss);
    RecordScene("閉じた箱 1000 K + 熱い塊 1010 K", ShellWithBlock(1000000, 1010000), 1000000, 60, Expect3::NoLoss);
    RecordScene("閉じた箱 1000 K + 冷たい塊 900 K", ShellWithBlock(1000000, 900000), 1000000, 60, Expect3::NoGain);
    RecordScene("空へ開いた板 1000 K(空も参照の温度)", OpenSlab(1000000, 0), 1000000, 60, Expect3::Zero);
    RecordScene("空へ開いた板 1000 K + 熱い石 1100 K", OpenSlab(1000000, 1100000), 1000000, 60, Expect3::NoLoss);
    if (failureCount != 0) {
        std::printf("FAILED: %d\n", failureCount);
        return 1;
    }

    std::printf("通過\n");
    return 0;
}
