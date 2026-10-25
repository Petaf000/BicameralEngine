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
#include <numeric>
#include <utility>
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

    // 運び方の試験は毎刻み全部の形(組 1)で 1 刻みずつ確かめる(既定の 15 組・熱も待つ〔D-658〕は持ち越しを続けて渡す時だけ同じ量になる。
    // 組の形は Slotted の試験と筋の試験の組の欄で見る)
    CascadeParams Params(uint32_t baseTexels = 4, uint32_t levels = 5) {
        return {.baseTexels = baseTexels,
                .levels = levels,
                .intervalBlocks = 1,
                .seed = 7,
                .quietBelowMk = QUIET_MK,
                .emitSlots = 1,
                .thermalInSlots = false};
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

            // 放つ面の組(T-1010)で待つ光は持ち越しの表にある: 持ち越しの和 = 返した + 待たせた熱 + そのまま送った
            ok = ok &&
                 ledger.emitted + ledger.carriedIn ==
                     ledger.absorbed + ledger.reflected + ledger.escaped + ledger.held &&
                 cellEmitted == ledger.emitted && cellAbsorbed == ledger.absorbed &&
                 carried == ledger.reflected + ledger.held + ledger.kept;
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

        // 放つ面の組(T-1010): 待った光も表から消えずに次の刻みへ(前の表の和 = 放った持ち越し + そのまま送った)
        CascadeParams slotted = Params();
        slotted.emitSlots = 3;
        slotted.thermalInSlots = true;
        carry = {};
        for (uint64_t tick = 0; tick < 7; ++tick) {
            const CascadeTickResult result = StepCascadeLight(grid, carry, slotted, tick);
            EXPECT(Conserves(result));
            for (uint32_t band = 0; band < rx::LT_BANDS; ++band) {
                uint64_t before = 0;
                for (const CascadeCarryEntry& entry : carry.entries) {
                    if (entry.band == band)
                        before += entry.amount;
                }

                EXPECT(before == result.ledger[band].carriedIn + result.ledger[band].kept);
            }

            carry = result.carry;
        }

        // 既定の形(15 組・熱も待つ。D-658): 持ち越しを渡し続ければ保存はそのまま、熱で放った量は毎刻み同じ(待つのは届く時だけ)
        const CascadeParams defaults{.seed = 7, .quietBelowMk = QUIET_MK};
        EXPECT(defaults.emitSlots == 15 && defaults.thermalInSlots);
        const CascadeTickResult whole = StepCascadeLight(grid, {}, Params(), 0);
        carry = {};
        uint64_t heldTotal = 0;
        for (uint64_t tick = 0; tick < 16; ++tick) {
            const CascadeTickResult result = StepCascadeLight(grid, carry, defaults, tick);
            EXPECT(Conserves(result));
            EXPECT(result.ledger[rx::LT_BAND_LONG].emitted == whole.ledger[rx::LT_BAND_LONG].emitted);
            heldTotal += result.ledger[rx::LT_BAND_LONG].held;
            carry = result.carry;
        }

        EXPECT(heldTotal != 0);
        std::printf("保存: 3 刻み(持ち越しつき)・放つ面の組 3 で 7 刻み・既定の 15 組で 16 刻み、帯ごとにビット単位\n");
    }

    // --- 2b. 放つ面の組の釣り合い(T-1146): 軸に揃った面のブロック n × n を組に配った数の偏り ---

    void TestSlotBalance() {
        constexpr uint32_t SLOTS = 15;
        rx::RcStepConstants constants{};
        constants.levels = 1;  // ブロック = 1 セル(組の式だけを見る)
        constants.emitSlots = SLOTS;
        uint32_t worstExcess = 0;
        for (int64_t n = 2; n <= 16; ++n) {
            for (int axis = 0; axis < 3; ++axis) {
                for (const int64_t fixed : {int64_t{0}, int64_t{1}, int64_t{5}, int64_t{-3}}) {
                    std::array<uint32_t, SLOTS> counts{};
                    for (int64_t u = 0; u < n; ++u) {
                        for (int64_t v = 0; v < n; ++v) {
                            const std::array<int64_t, 3> cell = axis == 0   ? std::array<int64_t, 3>{fixed, u, v}
                                                                : axis == 1 ? std::array<int64_t, 3>{u, fixed, v}
                                                                            : std::array<int64_t, 3>{u, v, fixed};
                            for (uint32_t slot = 0; slot < SLOTS; ++slot) {
                                constants.tick = slot;
                                if (rx::RcEmitSlotActive(constants, cell[0], cell[1], cell[2]))
                                    ++counts[slot];
                            }
                        }
                    }

                    const auto [low, high] = std::ranges::minmax(counts);
                    const uint32_t ideal = (n * n) % SLOTS == 0 ? 0u : 1u;
                    worstExcess = std::max(worstExcess, high - low - ideal);
                    EXPECT(std::accumulate(counts.begin(), counts.end(), 0u) == static_cast<uint32_t>(n * n));
                }
            }
        }

        // 前の式(bx + 3 bz + 5 by)は 16 × 16 で 9 個の差・8 × 8 で 2〜6 個。今の式は 2 × 2〜16 × 16 のどの軸の面も、割り切れる時の最小の差 + 3 以内
        EXPECT(worstExcess <= 3);

        // 光源の近く: どの軸の面でも隣り合う 3 × 3 のブロックは違う組
        bool distinct = true;
        for (int axis = 0; axis < 3; ++axis) {
            std::array<uint32_t, SLOTS> counts{};
            for (int64_t u = -1; u <= 1; ++u) {
                for (int64_t v = -1; v <= 1; ++v) {
                    const std::array<int64_t, 3> cell = axis == 0   ? std::array<int64_t, 3>{0, u, v}
                                                        : axis == 1 ? std::array<int64_t, 3>{u, 0, v}
                                                                    : std::array<int64_t, 3>{u, v, 0};
                    for (uint32_t slot = 0; slot < SLOTS; ++slot) {
                        constants.tick = slot;
                        if (rx::RcEmitSlotActive(constants, cell[0], cell[1], cell[2]))
                            ++counts[slot];
                    }
                }
            }

            distinct = distinct && std::ranges::max(counts) == 1;
        }

        EXPECT(distinct);
        std::printf(
            "組の釣り合い: 15 組・軸に揃った面 2×2〜16×16 で組ごとの数の差は最小 + %u 以内・隣り合う 3×3 は違う組\n",
            worstExcess);
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
        double low = 0;  // 全部の刻みの平均の、平均からのずれ
        double high = 0;
        std::array<double, 3> averageWorst{};  // 60・250・500 刻みの平均の |ずれ| の最大(そこまで回した時)
        double instantWorstMean = 0;           // 1 刻みごとの |ずれ| の最大の、刻みの平均(瞬間の筋)
        double instantRms = 0;                 // 1 刻みごとの二乗平均のずれの、刻みの平均
        uint64_t raysPerTick = 0;
        double msPerTick = 0;

        // --- 正しい値(焚き火の面から受け手の面への遮る物の無い G の面の積分)からのずれ(ADR-0453・T-1081)---
        std::array<double, 3> averageExactWorst{};  // 60・250・500 刻みの平均
        double exactAll = 0;                        // 全部の刻みの平均
        double instantExactWorstMean = 0;           // 瞬間(1 刻み)の |ずれ| の最大の、刻みの平均
        double instantExactRms = 0;
    };

    // 面(セル・向き dir = 軸 × 2 + 負なら 1)の上の点(u, v は −0.5〜0.5)
    std::array<double, 3> FacePoint(const std::array<int64_t, 3>& cell, int dir, double u, double v) {
        const int axis = dir / 2;
        std::array<double, 3> p = {static_cast<double>(cell[0]) + 0.5, static_cast<double>(cell[1]) + 0.5,
                                   static_cast<double>(cell[2]) + 0.5};
        p[axis] += (dir % 2) != 0 ? -0.5 : 0.5;
        p[(axis + 1) % 3] += u;
        p[(axis + 2) % 3] += v;

        return p;
    }

    // 遮る物の無い面と面の G(セルの面積 = 1)。mm × mm の点どうしの積分(T-1062 の試作 GeometricExchange と同じ)
    double FaceExchange(const std::array<int64_t, 3>& cellA, int dirA, const std::array<int64_t, 3>& cellB, int dirB,
                        int mm) {
        const double signA = (dirA % 2) != 0 ? -1.0 : 1.0;
        const double signB = (dirB % 2) != 0 ? -1.0 : 1.0;
        const double weight = 1.0 / (static_cast<double>(mm) * mm * mm * mm);
        double g = 0;
        for (int i = 0; i < mm * mm; ++i) {
            const auto pa = FacePoint(cellA, dirA, ((i % mm) + 0.5) / mm - 0.5, ((i / mm) + 0.5) / mm - 0.5);
            for (int j = 0; j < mm * mm; ++j) {
                const auto pb = FacePoint(cellB, dirB, ((j % mm) + 0.5) / mm - 0.5, ((j / mm) + 0.5) / mm - 0.5);
                const std::array<double, 3> d = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
                const double r2 = (d[0] * d[0]) + (d[1] * d[1]) + (d[2] * d[2]);
                const double cosA = std::max(0.0, signA * d[dirA / 2]);
                const double cosB = std::max(0.0, -signB * d[dirB / 2]);
                g += weight * cosA * cosB / (std::numbers::pi * r2 * r2);
            }
        }

        return g;
    }

    // 受け手ごとの正しい値の重み(焚き火の 6 面 × 受け手の 6 面。焚き火の 6 面は同じだけ放つ)
    std::vector<double> ExactRingWeights(const std::array<int64_t, 3>& fire,
                                         const std::vector<std::array<int64_t, 3>>& receivers) {
        std::vector<double> weights;
        for (const auto& receiver : receivers) {
            double sum = 0;
            for (int dirA = 0; dirA < 6; ++dirA) {
                for (int dirB = 0; dirB < 6; ++dirB)
                    sum += FaceExchange(fire, dirA, receiver, dirB, 8);
            }

            weights.push_back(sum);
        }

        return weights;
    }

    // 受け手ごとの量(見かけの面と距離で割ったもの)の平均からのずれの |最大| と二乗平均
    std::pair<double, double> Deviation(const std::vector<double>& values) {
        double mean = 0;
        for (const double value : values)
            mean += value / static_cast<double>(values.size());

        double worst = 0;
        double squares = 0;
        for (const double value : values) {
            const double deviation = (value / mean) - 1;
            worst = std::max(worst, std::fabs(deviation));
            squares += deviation * deviation / static_cast<double>(values.size());
        }

        return {worst, std::sqrt(squares)};
    }

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
        // 受け手の見かけの面と距離の重み。焚き火も受け手も立方体なので、向き θ の強さ ∝ 放つ側の見かけの面 × 受ける側の見かけの面 ÷ 距離²
        // (どちらも |cos θ| + |sin θ|)。T-0958 の試験は受ける側だけで割っていて、軸の向きが 30 % 低く見えていた(T-0959 で直した。
        // T-0972 の試作の測りは両方で割っていた)
        std::vector<double> scale(COUNT, 0);
        for (uint32_t k = 0; k < COUNT; ++k) {
            const double dx = static_cast<double>(receivers[k][0] - CENTER);
            const double dz = static_cast<double>(receivers[k][2] - CENTER);
            const double distanceSquared = (dx * dx) + (dz * dz);
            const double projected = (std::fabs(dx) + std::fabs(dz)) / std::sqrt(distanceSquared);
            scale[k] = distanceSquared / (projected * projected);
        }

        const std::vector<double> exact = ExactRingWeights({CENTER, CENTER, CENTER}, receivers);
        const auto exactDeviation = [&](const std::vector<double>& raw) {
            std::vector<double> ratio(COUNT, 0);
            for (uint32_t k = 0; k < COUNT; ++k)
                ratio[k] = raw[k] / exact[k];

            return Deviation(ratio);
        };

        std::vector<double> sums(COUNT, 0);
        std::vector<double> rawSums(COUNT, 0);
        CascadeCarry carry;
        RingResult ring;
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            const CascadeTickResult result = StepCascadeLight(grid, carry, params, tick);
            std::vector<double> one(COUNT, 0);
            std::vector<double> raw(COUNT, 0);
            for (uint32_t k = 0; k < COUNT; ++k) {
                raw[k] = static_cast<double>(
                    Received(result, grid.IndexOf(receivers[k][0], receivers[k][1], receivers[k][2])));
                one[k] = raw[k] * scale[k];
                sums[k] += one[k];
                rawSums[k] += raw[k];
            }

            const auto [worst, rms] = Deviation(one);
            ring.instantWorstMean += worst / ticks;
            ring.instantRms += rms / ticks;
            const auto [exactWorst, exactRms] = exactDeviation(raw);
            ring.instantExactWorstMean += exactWorst / ticks;
            ring.instantExactRms += exactRms / ticks;
            for (size_t at = 0; at < 3; ++at) {
                if (tick + 1 == std::array<uint32_t, 3>{60, 250, 500}[at]) {
                    ring.averageWorst[at] = Deviation(sums).first;
                    ring.averageExactWorst[at] = exactDeviation(rawSums).first;
                }
            }

            ring.raysPerTick += result.rays;
            carry = result.carry;
        }

        ring.msPerTick = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() /
                         ticks;
        ring.raysPerTick /= ticks;
        double mean = 0;
        for (const double sum : sums)
            mean += sum / COUNT;

        ring.exactAll = exactDeviation(rawSums).first;
        ring.low = (*std::ranges::min_element(sums) / mean) - 1;
        ring.high = (*std::ranges::max_element(sums) / mean) - 1;

        return ring;
    }

    // 筋を 60 刻みの平均・250・500 刻みの平均・瞬間(1 刻み)で見る。形: 今の形(n0 = 4・5 段・袋 1 本)、光線を約 8 倍にした 2 つ
    // (袋 8 本・n0 = 8)、段なしで向きを細かくした形(n0 = 128・1 段。瞬間の筋が光線の数でどう減るかの目安)。T-0959
    void RecordRing() {
        struct Form {
            const char* name;
            uint32_t baseTexels;
            uint32_t levels;
            uint32_t raysPerBag;
            uint32_t ticks;
            uint32_t emitSlots = 1;
        };
        const std::array<Form, 5> forms = {Form{"放つ面の組 7(熱も)", 4, 5, 1, 504, 7},
                                           Form{"今の形 n0 = 4・5 段", 4, 5, 1, 500},
                                           Form{"袋 8 本 n0 = 4・5 段", 4, 5, 8, 30}, Form{"n0 = 8・5 段", 8, 5, 1, 30},
                                           Form{"段なし n0 = 128・1 段", 128, 1, 1, 4}};
        for (const Form& form : forms) {
            CascadeParams params = Params(form.baseTexels, form.levels);
            params.raysPerBag = form.raysPerBag;
            params.emitSlots = form.emitSlots;
            params.thermalInSlots = form.emitSlots > 1;
            const RingResult ring = MeasureRing(params, form.ticks);
            std::printf(
                "筋: %s: 8 m の円 瞬間 |ずれ|の最大 %.1f %%・二乗平均 %.1f %%(%u 刻みの平均)/ 平均 60 刻み %.1f "
                "%%・250 刻み "
                "%.1f %%・500 刻み %.1f %%(線 ±10 %%。0 は回していない)/ %u 刻みの平均 %+.1f 〜 %+.1f %%・%llu "
                "本/刻み・"
                "%.1f ms/刻み\n",
                form.name, ring.instantWorstMean * 100, ring.instantRms * 100, form.ticks, ring.averageWorst[0] * 100,
                ring.averageWorst[1] * 100, ring.averageWorst[2] * 100, form.ticks, ring.low * 100, ring.high * 100,
                static_cast<unsigned long long>(ring.raysPerTick), ring.msPerTick);
            std::printf(
                "  正しい値(面の積分)から: 瞬間 |ずれ|の最大 %.1f %%・二乗平均 %.1f %% / 平均 60 刻み %.1f %%・250 "
                "刻み %.1f %%・"
                "500 刻み %.1f %%(0 は回していない)/ %u 刻みの平均 %.1f %%\n",
                ring.instantExactWorstMean * 100, ring.instantExactRms * 100, ring.averageExactWorst[0] * 100,
                ring.averageExactWorst[1] * 100, ring.averageExactWorst[2] * 100, form.ticks, ring.exactAll * 100);
            // 研究の記録(合格の線は Q147 の答え待ち): 平均が悪くなっていないかだけを見る
            EXPECT(form.ticks < 500 || form.emitSlots > 1 || ring.averageWorst[2] < 0.15);
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
        struct Run {
            uint32_t levels;
            uint32_t ticks;
            uint32_t emitSlots = 1;
        };
        for (const Run run : {Run{1, 20}, Run{2, 200}, Run{5, 60}, Run{5, 105, 7}}) {
            const uint32_t levels = run.levels;
            const uint32_t ticks = run.ticks;
            std::vector<double> net(grid.CellCount(), 0);
            std::vector<double> emitted(grid.CellCount(), 0);
            CascadeCarry carry;
            for (uint32_t tick = 0; tick < ticks; ++tick) {
                CascadeParams params = Params(4, levels);
                params.emitSlots = run.emitSlots;
                params.thermalInSlots = run.emitSlots > 1;
                const CascadeTickResult result = StepCascadeLight(grid, carry, params, tick);
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
                worstMk = std::max(worstMk, std::fabs(net[cell]) / ticks / 65536.0 / 170000.0);
            }

            std::printf(
                "坂を下る(研究の記録): n0 = 4・%u 段・放つ面の組 %u: 正味 ÷ 放った 最大 %.1f %%・温度に直して最大 %.3f "
                "mK/刻み(%u 刻み)\n",
                levels, run.emitSlots, worstShare * 100, worstMk, ticks);
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
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // 途中で切れても(時間切れ)出した行が残るように
    TestGridFromTree();
    TestConservation();
    TestSlotBalance();
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
