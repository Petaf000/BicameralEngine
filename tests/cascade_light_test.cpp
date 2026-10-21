// cascade_light_test.cpp — 世界の中の光: 距離ごとの段の CPU リファレンス(sim/cascade_light。T-0958・23 §10 R-R1)。
//   1. 木から写す: 石は 2 つの帯で通さない・照り返し 0.5(D-610)・空気は透明・温度はセルから
//   2. 保存: 帯ごとに 放った + 持ち越しを放った = 吸った + 返した + 外へ(ビット単位)。セルの和 = 帳簿・持ち越しの和 = 返した
//   3. 決定的: 放つ面と袋を逆の順に回しても同じ・2 回で同じ(持ち越しのハッシュも)
//   4. 壁の陰: 焚き火と壁の場面で、壁の裏の受け手が受ける量が壁の無い時の 5 % 未満(仮)
//   5. 筋(研究の記録): 焚き火から 8 m の円の 24 か所のばらつき(60 刻みの平均。合格の線 ±10 %〔仮〕)
//   6. 坂を下る(研究の記録): 同じ温度の閉じた箱で、セルごとの正味の出入り ÷ 放った量と、温度に直した動き(D-435 の 1 mK と比べる)
//   7. 解析解: 平行な黒い板(16 × 16 セル)の見え方の割合が解析解の ±10 %(T-0584 と同じ基準)
//   8. 費用(記録): 1 スレッドの CPU で、焚き火 1 つ・熱い床 16 × 16・48 × 48(熱いセル 2304)の光線の本数・歩いたセル・ms/刻み
//   9. 照り返しの持ち越しの大きさ(セーブの形の案のバイト/セル)
//  10. 誤り: 段の形が範囲の外
// 場面は CascadeGrid(段 0 の写し)を直接作る(1 の写しの道具は木から作る)。double を使うのは試験の参照値と表示だけ。
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <numbers>
#include <vector>

#include "sim/cascade_light.h"
#include "sun_column_scene.h"

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

    // --- 場面の道具 ---

    enum class Material : uint8_t {
        Air,
        Stone,  // 灰色で通さない・照り返し 0.5(D-610 の既定)
        Black,  // 通さない・照り返し 0(受け手・黒い板)
    };

    struct Voxel {
        Material material = Material::Air;
        uint32_t temperatureMk = 0;
    };

    using Shape = std::function<Voxel(int64_t x, int64_t y, int64_t z)>;

    constexpr uint32_t FIRE_MK = 1200000;
    constexpr uint32_t ROOM_MK = 293000;
    constexpr uint32_t QUIET_MK = 500000;  // 試験の場面では室温の物は放たない(焚き火の光だけを見る)

    rx::RcCellLight LightOf(Material material) {
        if (material == Material::Air)
            return {};

        const uint32_t reflectance = material == Material::Stone ? 32768u : 0u;

        return {.depthShortQ32 = rx::RD_DEPTH_SATURATED_Q32,
                .depthLongQ32 = rx::RD_DEPTH_SATURATED_Q32,
                .reflectanceShortQ16 = reflectance,
                .reflectanceLongQ16 = reflectance};
    }

    CascadeGrid MakeGrid(int64_t sizeX, int64_t sizeY, int64_t sizeZ, const Shape& shape) {
        CascadeGrid grid;
        grid.size = {sizeX, sizeY, sizeZ};
        grid.lights.resize(static_cast<size_t>(sizeX * sizeY * sizeZ));
        grid.temperatureMk.resize(grid.lights.size());
        for (int64_t z = 0; z < sizeZ; ++z) {
            for (int64_t y = 0; y < sizeY; ++y) {
                for (int64_t x = 0; x < sizeX; ++x) {
                    const Voxel voxel = shape(x, y, z);
                    const size_t at = grid.IndexOf(x, y, z);
                    grid.lights[at] = LightOf(voxel.material);
                    grid.temperatureMk[at] = voxel.temperatureMk;
                }
            }
        }

        return grid;
    }

    CascadeParams Params(uint32_t baseTexels = 4, uint32_t levels = 5) {
        return {.baseTexels = baseTexels, .levels = levels, .intervalBlocks = 1, .seed = 7, .quietBelowMk = QUIET_MK};
    }

    uint64_t Received(const CascadeTickResult& result, size_t cell) {
        return result.absorbed[(cell * rx::LT_BANDS) + rx::LT_BAND_SHORT] +
               result.absorbed[(cell * rx::LT_BANDS) + rx::LT_BAND_LONG];
    }

    uint64_t Emitted(const CascadeTickResult& result, size_t cell) {
        return result.emitted[(cell * rx::LT_BANDS) + rx::LT_BAND_SHORT] +
               result.emitted[(cell * rx::LT_BANDS) + rx::LT_BAND_LONG];
    }

    // 石の地面(y < 8)の上に、熱い石の床 hot × hot(y = 8)を真ん中に。48³
    CascadeGrid FireGround(int64_t hot) {
        constexpr int64_t SIZE = 48;
        constexpr int64_t CENTER = SIZE / 2;

        return MakeGrid(SIZE, SIZE, SIZE, [hot](int64_t x, int64_t y, int64_t z) {
            if (y < 8)
                return Voxel{Material::Stone, ROOM_MK};

            const int64_t low = CENTER - (hot / 2);
            const bool fire = y == 8 && x >= low && x < low + hot && z >= low && z < low + hot;

            return fire ? Voxel{Material::Stone, FIRE_MK} : Voxel{};
        });
    }

    // --- 1. 木から写す ---

    void TestGridFromTree() {
        const BakedReactionTable table = test::MakeSunTable();
        const MultiresNest nest = test::MakeSunWorld(table, 0, 2, 1, 2, test::FlatGround);
        const auto grid = MakeCascadeGrid(nest, table);
        EXPECT(grid.has_value());
        if (!grid)
            return;

        EXPECT(grid->size == (std::array<int64_t, 3>{16, 8, 16}));
        const rx::RcCellLight stone = grid->lights[grid->IndexOf(3, 0, 5)];
        const rx::RcCellLight air = grid->lights[grid->IndexOf(3, 4, 5)];
        EXPECT(stone.depthShortQ32 == rx::RD_DEPTH_SATURATED_Q32 && stone.depthLongQ32 == rx::RD_DEPTH_SATURATED_Q32);
        EXPECT(stone.reflectanceShortQ16 == 32768 && stone.reflectanceLongQ16 == 32768);
        EXPECT(air.depthShortQ32 == 0 && air.depthLongQ32 == 0);
        const uint32_t temperature = grid->temperatureMk[grid->IndexOf(3, 0, 5)];
        EXPECT(temperature + 2 >= static_cast<uint32_t>(test::SUN_ROOM_TEMPERATURE_MK) &&
               temperature <= static_cast<uint32_t>(test::SUN_ROOM_TEMPERATURE_MK) + 2);
        std::printf("木から写す: 石 τ 飽和・照り返し 0.5・空気は透明・温度 %u mK\n", temperature);
    }

    // --- 2. 保存 ---

    bool Conserves(const CascadeTickResult& result) {
        bool ok = true;
        for (uint32_t band = 0; band < rx::LT_BANDS; ++band) {
            const CascadeBandLedger& ledger = result.ledger[band];
            uint64_t cellEmitted = 0;
            uint64_t cellAbsorbed = 0;
            for (size_t cell = 0; cell * rx::LT_BANDS < result.emitted.size(); ++cell) {
                cellEmitted += result.emitted[(cell * rx::LT_BANDS) + band];
                cellAbsorbed += result.absorbed[(cell * rx::LT_BANDS) + band];
            }

            uint64_t carried = 0;
            for (const CascadeCarryEntry& entry : result.carry.entries) {
                if (entry.band == band)
                    carried += entry.amount;
            }

            ok = ok && ledger.emitted + ledger.carriedIn == ledger.absorbed + ledger.reflected + ledger.escaped &&
                 cellEmitted == ledger.emitted && cellAbsorbed == ledger.absorbed && carried == ledger.reflected;
        }

        return ok;
    }

    void TestConservation() {
        const CascadeGrid grid = FireGround(2);
        CascadeCarry carry;
        for (uint64_t tick = 0; tick < 3; ++tick) {
            const CascadeTickResult result = StepCascadeLight(grid, carry, Params(), tick);
            EXPECT(Conserves(result));
            EXPECT(result.ledger[rx::LT_BAND_LONG].emitted != 0 && result.ledger[rx::LT_BAND_SHORT].emitted != 0);
            EXPECT(result.ledger[rx::LT_BAND_LONG].reflected != 0);  // 石の地面が返す
            EXPECT(tick == 0 || result.ledger[rx::LT_BAND_LONG].carriedIn != 0);
            carry = result.carry;
        }

        std::printf("保存: 3 刻み(持ち越しつき)で帯ごとにビット単位\n");
    }

    // --- 3. 決定的 ---

    void TestDeterministic() {
        const CascadeGrid grid = FireGround(2);
        const CascadeTickResult first = StepCascadeLight(grid, {}, Params(), 0);
        const CascadeTickResult again = StepCascadeLight(grid, {}, Params(), 0);
        const CascadeTickResult reversed = StepCascadeLight(grid, {}, Params(), 0, true);
        EXPECT(first.absorbed == again.absorbed && first.carry == again.carry && first.ledger == again.ledger);
        EXPECT(first.absorbed == reversed.absorbed && first.emitted == reversed.emitted);
        EXPECT(first.carry == reversed.carry && first.ledger == reversed.ledger);
        EXPECT(HashCascadeCarry(first.carry) == HashCascadeCarry(reversed.carry));

        const CascadeTickResult second = StepCascadeLight(grid, first.carry, Params(), 1);
        const CascadeTickResult secondReversed = StepCascadeLight(grid, reversed.carry, Params(), 1, true);
        EXPECT(second.absorbed == secondReversed.absorbed && second.carry == secondReversed.carry);
        std::printf("決定的: 逆の順・2 回で同じ(持ち越しのハッシュ %016llx)\n",
                    static_cast<unsigned long long>(HashCascadeCarry(first.carry)));
    }

    // --- 4. 壁の陰 ---

    void TestWallShadow() {
        std::array<uint64_t, 2> received{};
        for (uint32_t wall = 0; wall < 2; ++wall) {
            const CascadeGrid grid = MakeGrid(32, 24, 24, [wall](int64_t x, int64_t y, int64_t z) {
                if (x == 8 && y == 12 && z == 12)
                    return Voxel{Material::Stone, FIRE_MK};

                if (wall != 0 && x == 14 && y >= 4 && y < 20 && z >= 4 && z < 20)
                    return Voxel{Material::Stone, ROOM_MK};

                return x == 20 && y == 12 && z == 12 ? Voxel{Material::Black, 0} : Voxel{};
            });
            CascadeCarry carry;
            for (uint64_t tick = 0; tick < 10; ++tick) {
                const CascadeTickResult result = StepCascadeLight(grid, carry, Params(), tick);
                received[wall] += Received(result, grid.IndexOf(20, 12, 12));
                carry = result.carry;
            }
        }

        std::printf("壁の陰: 受け手 壁なし %llu・壁あり %llu(10 刻み)\n", static_cast<unsigned long long>(received[0]),
                    static_cast<unsigned long long>(received[1]));
        EXPECT(received[0] != 0);
        EXPECT(received[1] * 20 < received[0]);
    }

    // --- 5. 筋(研究の記録)---

    struct RingResult {
        double low = 0;
        double high = 0;
        uint64_t raysPerTick = 0;
        double msPerTick = 0;
    };

    // 焚き火(1 セル)から 8 m(16 セル)の円の 24 か所の黒い受け手。受け手の見かけの面と距離で割って比べる
    RingResult MeasureRing(const CascadeParams& params, uint32_t ticks) {
        constexpr int64_t SIZE = 48;
        constexpr int64_t CENTER = SIZE / 2;
        constexpr int64_t RADIUS = 16;
        constexpr uint32_t COUNT = 24;
        std::vector<std::array<int64_t, 3>> receivers;
        for (uint32_t k = 0; k < COUNT; ++k) {
            const double angle = 2 * std::numbers::pi * k / COUNT;
            receivers.push_back({CENTER + std::lround(RADIUS * std::cos(angle)), CENTER,
                                 CENTER + std::lround(RADIUS * std::sin(angle))});
        }

        const CascadeGrid grid = MakeGrid(SIZE, SIZE, SIZE, [&](int64_t x, int64_t y, int64_t z) {
            if (x == CENTER && y == CENTER && z == CENTER)
                return Voxel{Material::Stone, FIRE_MK};

            const bool receiver = std::ranges::any_of(
                receivers, [&](const auto& r) { return r[0] == x && r[1] == y && r[2] == z; });

            return receiver ? Voxel{Material::Black, 0} : Voxel{};
        });
        std::vector<double> sums(COUNT, 0);
        CascadeCarry carry;
        RingResult ring;
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            const CascadeTickResult result = StepCascadeLight(grid, carry, params, tick);
            for (uint32_t k = 0; k < COUNT; ++k)
                sums[k] += static_cast<double>(
                    Received(result, grid.IndexOf(receivers[k][0], receivers[k][1], receivers[k][2])));

            ring.raysPerTick += result.rays;
            carry = result.carry;
        }

        ring.msPerTick = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() /
                         ticks;
        ring.raysPerTick /= ticks;
        double mean = 0;
        for (uint32_t k = 0; k < COUNT; ++k) {
            const double dx = static_cast<double>(receivers[k][0] - CENTER);
            const double dz = static_cast<double>(receivers[k][2] - CENTER);
            const double distanceSquared = (dx * dx) + (dz * dz);
            const double projected = (std::fabs(dx) + std::fabs(dz)) /
                                     std::sqrt(distanceSquared);  // 立方体の見かけの面
            sums[k] /= projected / distanceSquared;
            mean += sums[k] / COUNT;
        }

        ring.low = (*std::ranges::min_element(sums) / mean) - 1;
        ring.high = (*std::ranges::max_element(sums) / mean) - 1;

        return ring;
    }

    void RecordRing() {
        struct Form {
            uint32_t baseTexels;
            uint32_t levels;
        };
        for (const Form shape : {Form{4, 5}, Form{2, 5}}) {
            const RingResult ring = MeasureRing(Params(shape.baseTexels, shape.levels), 60);
            std::printf(
                "筋(研究の記録): n0 = %u・%u 段: 8 m の円 %+.1f 〜 %+.1f %%(60 刻みの平均・線 ±10 %%)・%llu "
                "本/刻み・%.1f ms/刻み\n",
                shape.baseTexels, shape.levels, ring.low * 100, ring.high * 100,
                static_cast<unsigned long long>(ring.raysPerTick), ring.msPerTick);
            // 研究が閉じるまでは悪くなっていないかだけを見る(種で ±15〜35 % と揺れる。T-0972)
            EXPECT(shape.baseTexels != 4 || (ring.low > -0.50 && ring.high < 0.50));
        }
    }

    // --- 6. 坂を下る(研究の記録)---

    void RecordSlope() {
        // 1 m 厚の石の殻(内のり 7 m)の中に石の塊。全部 1000 K。殻の内側の面の正味の出入りを見る
        constexpr int64_t SIZE = 18;
        const CascadeGrid grid = MakeGrid(SIZE, SIZE, SIZE, [](int64_t x, int64_t y, int64_t z) {
            const bool shell = x < 2 || y < 2 || z < 2 || x >= SIZE - 2 || y >= SIZE - 2 || z >= SIZE - 2;
            const bool block = x >= 7 && x < 10 && y >= 2 && y < 6 && z >= 7 && z < 10;

            return shell || block ? Voxel{Material::Stone, 1000000} : Voxel{};
        });
        for (const uint32_t levels : {1u, 5u}) {
            constexpr uint32_t TICKS = 20;
            std::vector<double> net(grid.CellCount(), 0);
            std::vector<double> emitted(grid.CellCount(), 0);
            CascadeCarry carry;
            for (uint32_t tick = 0; tick < TICKS; ++tick) {
                const CascadeTickResult result = StepCascadeLight(grid, carry, Params(4, levels), tick);
                for (size_t cell = 0; cell < grid.CellCount(); ++cell) {
                    emitted[cell] += static_cast<double>(Emitted(result, cell));
                    net[cell] += static_cast<double>(Received(result, cell)) -
                                 static_cast<double>(Emitted(result, cell));
                }

                carry = result.carry;
            }

            // 1 セル = 2 万 mol × 8.5 J/(mol·K)= 170 kJ/K。量は 2^−16 mJ
            double worstShare = 0;
            double worstMk = 0;
            // 殻の外の面は世界の外へ放つ(受けない)ので、殻の一番外の層は数えない
            for (size_t cell = 0; cell < grid.CellCount(); ++cell) {
                const auto flat = static_cast<int64_t>(cell);
                const std::array<int64_t, 3> at = {flat % SIZE, (flat / SIZE) % SIZE, flat / (SIZE * SIZE)};
                const bool outer = std::ranges::any_of(at, [](int64_t v) { return v == 0 || v == SIZE - 1; });
                if (emitted[cell] == 0 || outer)
                    continue;

                worstShare = std::max(worstShare, std::fabs(net[cell]) / emitted[cell]);
                worstMk = std::max(worstMk, std::fabs(net[cell]) / TICKS / 65536.0 / 170000.0);
            }

            std::printf(
                "坂を下る(研究の記録): n0 = 4・%u 段: 正味 ÷ 放った 最大 %.1f %%・温度に直して最大 %.3f mK/刻み(%u "
                "刻み)\n",
                levels, worstShare * 100, worstMk, TICKS);
            EXPECT(worstMk < 1.0);
        }
    }

    // --- 7. 解析解 ---

    double ParallelSquaresView(double side, double distance) {
        const double x = side / distance;
        const double x2 = x * x;
        const double term1 = std::log(std::sqrt((1 + x2) * (1 + x2) / (1 + 2 * x2)));
        const double term2 = 2 * x * std::sqrt(1 + x2) * std::atan(x / std::sqrt(1 + x2));
        const double term3 = 2 * x * std::atan(x);

        return 2 / (std::numbers::pi * x2) * (term1 + term2 - term3);
    }

    void TestParallelPlates() {
        constexpr int64_t SIDE = 16;
        for (const int64_t gap : {1, 4, 8}) {
            const CascadeGrid grid = MakeGrid(32, gap + 4, 32, [gap](int64_t x, int64_t y, int64_t z) {
                const bool inside = x >= 8 && x < 8 + SIDE && z >= 8 && z < 8 + SIDE;
                if (y == 0)
                    return Voxel{Material::Black, 0};

                if (y == 1)
                    return Voxel{Material::Black, inside ? 1000000u : 0u};

                return y == 2 + gap && inside ? Voxel{Material::Black, 0} : Voxel{};
            });
            double received = 0;
            double emitted = 0;
            for (uint64_t tick = 0; tick < 4; ++tick) {
                const CascadeTickResult result = StepCascadeLight(grid, {}, Params(), tick);
                for (int64_t z = 8; z < 8 + SIDE; ++z) {
                    for (int64_t x = 8; x < 8 + SIDE; ++x) {
                        received += static_cast<double>(Received(result, grid.IndexOf(x, 2 + gap, z)));
                        // 上の面だけが放つ(下の黒い床へは面の向こうが通さないので放たない)
                        emitted += static_cast<double>(Emitted(result, grid.IndexOf(x, 1, z)));
                    }
                }
            }

            // 板と板の間は gap セル(放つ面 y = 2 から受ける面 y = 2 + gap)
            const double expected = ParallelSquaresView(static_cast<double>(SIDE), static_cast<double>(gap));
            const double measured = received / emitted;
            std::printf("平行な黒い板 16 × 16・隙間 %lld セル: F = %.4f(解析解 %.4f・%+.1f %%)\n",
                        static_cast<long long>(gap), measured, expected, (measured / expected - 1) * 100);
            EXPECT(std::fabs(measured / expected - 1) < 0.10);
        }
    }

    // --- 8・9. 費用と持ち越しの大きさ(記録)---

    void RecordCost() {
        for (const int64_t hot : {1, 16, 48}) {
            const CascadeGrid grid = FireGround(hot);
            const CascadeTickResult warm = StepCascadeLight(grid, {}, Params(), 0);
            const auto start = std::chrono::steady_clock::now();
            const CascadeTickResult result = StepCascadeLight(grid, warm.carry, Params(), 1);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                                  .count();
            std::printf(
                "費用(1 スレッド): 熱いセル %lld: 光線 %llu 本/刻み(段ごと %llu・%llu・%llu・%llu・%llu)・歩いたセル "
                "%llu・%.0f ms/刻み\n",
                static_cast<long long>(hot * hot), static_cast<unsigned long long>(result.rays),
                static_cast<unsigned long long>(result.raysPerLevel[0]),
                static_cast<unsigned long long>(result.raysPerLevel[1]),
                static_cast<unsigned long long>(result.raysPerLevel[2]),
                static_cast<unsigned long long>(result.raysPerLevel[3]),
                static_cast<unsigned long long>(result.raysPerLevel[4]), static_cast<unsigned long long>(result.steps),
                ms);

            // 照り返しの持ち越し: 持つセルの数とバイト(セーブの形の案)
            size_t cells = 0;
            for (size_t at = 0; at < result.carry.entries.size(); ++at) {
                if (at == 0 || result.carry.entries[at - 1].cell != result.carry.entries[at].cell)
                    ++cells;
            }

            const uint64_t bytes = CascadeCarrySaveBytes(result.carry);
            std::printf(
                "  照り返しの持ち越し: 持つセル %zu(世界 %zu の %.2f %%)・値 %zu・%llu バイト(持つセルあたり "
                "%.1f・世界のセルあたり %.3f)\n",
                cells, grid.CellCount(), 100.0 * static_cast<double>(cells) / static_cast<double>(grid.CellCount()),
                result.carry.entries.size(), static_cast<unsigned long long>(bytes),
                cells == 0 ? 0.0 : static_cast<double>(bytes) / static_cast<double>(cells),
                static_cast<double>(bytes) / static_cast<double>(grid.CellCount()));
            EXPECT(Conserves(result));
        }
    }

    // --- 10. 誤り ---

    void TestRejects() {
        EXPECT(CheckCascadeParams(Params()).has_value());
        EXPECT(!CheckCascadeParams({.baseTexels = 0, .levels = 5}).has_value());
        EXPECT(!CheckCascadeParams({.baseTexels = 4, .levels = 0}).has_value());
        EXPECT(!CheckCascadeParams({.baseTexels = 64, .levels = 6}).has_value());
        EXPECT(!CheckCascadeParams({.baseTexels = 4, .levels = 5, .intervalBlocks = 0}).has_value());
    }

}  // namespace

int main() {
    TestGridFromTree();
    TestConservation();
    TestDeterministic();
    TestWallShadow();
    RecordRing();
    RecordSlope();
    TestParallelPlates();
    RecordCost();
    TestRejects();
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("cascade_light: すべて通過\n");

    return 0;
}
