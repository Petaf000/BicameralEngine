// cascade_face_test.cpp — 熱の放射の段(RC)の面の受け渡しの形の研究の測り(T-1365・ADR-0535 (b))。
// CascadeParams::faceCoupling(0 = 今の崖〔RcMaskAcross〕・1 = Rosseland の上限付き・2 = 二流束)を CPU リファレンスで比べる:
//   1. 形 0 は RcMaskAcross とビット単位で同じ・係数は τ について単調・どの形も両側で同じ(形 0 は T-1367 から)
//   2. 保存: どの形でも 放った + 持ち越しを放った = 吸った + 返した + 外へ(ビット単位)
//   3. 拡散(記録): 一様な物の板の中で T⁴ を 2 次の形にした時、真ん中のセルの正味 ÷ Rosseland の解析解(4σ Δ²(T⁴) ÷ 3τ)
//   4. 釣り合い: 同じ温度の閉じた箱(石の殻・τ 2 の物・τ 40 の板・石の塊)で、セルの正味の最大(W)と殻の内の和。
//      どの形も殻の内の和 ÷ 放った が向きの粗さの残りの大きさ以下(T-1367: 形 0 は片側だけ通さない面で +5.9 % だった)
//   5. 2 つのセルの受け渡し(記録): 熱い 800 K と冷たい 300 K の隣どうしが放射で渡す W(木・砂は既定の灰色の固体の a。伝導と比べる)
//   6. 費用(記録): 3 の場面の 1 刻みの ms
// 場面は CascadeGrid を直接作る。double は参照値と表示だけ。
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
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

    constexpr std::array<uint32_t, 3> FORMS = {rx::RC_FACE_CLIFF, rx::RC_FACE_ROSSELAND, rx::RC_FACE_TWO_FLUX};
    constexpr std::array<const char*, 3> FORM_NAMES = {"0 崖", "1 Rosseland", "2 二流束"};
    constexpr double UNITS_TO_WATTS = 60.0 / 65536.0 / 1000.0;  // 2^−16 mJ/刻み → W

    // τ(double)→ Q32
    uint64_t DepthQ32(double depth) {
        return static_cast<uint64_t>(depth * 4294967296.0);
    }

    rx::RcCellLight Medium(uint64_t depthQ32) {
        return {.depthShortQ32 = depthQ32, .depthLongQ32 = depthQ32, .reflectanceShortQ16 = 0, .reflectanceLongQ16 = 0};
    }

    CascadeParams Params(uint32_t form) {
        return {.baseTexels = 4,
                .levels = 2,
                .intervalBlocks = 1,
                .seed = 7,
                .quietBelowMk = 0,
                .emitSlots = 1,
                .thermalInSlots = false,
                .faceCoupling = form};
    }

    int64_t Net(const CascadeTickResult& result, size_t cell) {
        int64_t net = 0;
        for (uint32_t band = 0; band < rx::LT_BANDS; ++band) {
            net += static_cast<int64_t>(result.absorbed[(cell * rx::LT_BANDS) + band]) -
                   static_cast<int64_t>(result.emitted[(cell * rx::LT_BANDS) + band]);
        }

        return net;
    }

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

            ok = ok &&
                 ledger.emitted + ledger.carriedIn ==
                     ledger.absorbed + ledger.reflected + ledger.escaped + ledger.held &&
                 cellEmitted == ledger.emitted && cellAbsorbed == ledger.absorbed;
        }

        return ok;
    }

    CascadeGrid MakeGrid(int64_t sizeX, int64_t sizeY, int64_t sizeZ,
                         const std::function<std::pair<rx::RcCellLight, uint32_t>(int64_t, int64_t, int64_t)>& shape) {
        CascadeGrid grid;
        grid.size = {sizeX, sizeY, sizeZ};
        grid.lights.resize(static_cast<size_t>(sizeX * sizeY * sizeZ));
        grid.temperatureMk.resize(grid.lights.size());
        for (int64_t z = 0; z < sizeZ; ++z) {
            for (int64_t y = 0; y < sizeY; ++y) {
                for (int64_t x = 0; x < sizeX; ++x) {
                    const auto [light, temperature] = shape(x, y, z);
                    grid.lights[grid.IndexOf(x, y, z)] = light;
                    grid.temperatureMk[grid.IndexOf(x, y, z)] = temperature;
                }
            }
        }

        return grid;
    }

    // --- 1. 係数 ---

    void TestCoupling() {
        const std::array<uint64_t, 9> depths = {0,
                                                DepthQ32(0.5),
                                                DepthQ32(4.0 / 3.0),
                                                DepthQ32(8),
                                                rx::RD_DEPTH_SATURATED_Q32 - 1,
                                                rx::RD_DEPTH_SATURATED_Q32,
                                                DepthQ32(52.8),
                                                DepthQ32(100000),
                                                rx::RC_FACE_DEPTH_LIMIT_Q32};
        const rx::RcBands bands{.shortBand = 123456789, .longBand = 987654321};
        for (const uint64_t own : depths) {
            for (const uint64_t across : depths) {
                const rx::RcBands cliff = rx::RcCoupleAcross(bands, Medium(own), Medium(across), rx::RC_FACE_CLIFF);
                const rx::RcBands mask = rx::RcMaskAcross(bands, Medium(own), Medium(across));
                EXPECT(cliff.shortBand == mask.shortBand && cliff.longBand == mask.longBand);
                // 形 0 は両側とも飽和の時だけ 0(T-1367)
                const bool sealed = own >= rx::RD_DEPTH_SATURATED_Q32 && across >= rx::RD_DEPTH_SATURATED_Q32;
                EXPECT(sealed ? (mask.shortBand == 0 && mask.longBand == 0)
                              : (mask.shortBand == bands.shortBand && mask.longBand == bands.longBand));
                for (const uint32_t form : FORMS) {
                    const rx::RcBands there = rx::RcCoupleAcross(bands, Medium(own), Medium(across), form);
                    const rx::RcBands back = rx::RcCoupleAcross(bands, Medium(across), Medium(own), form);
                    EXPECT(there.longBand == back.longBand);
                }
            }
        }

        for (const uint32_t form : {rx::RC_FACE_ROSSELAND, rx::RC_FACE_TWO_FLUX}) {
            uint64_t previous = rx::RcFaceCouplingQ16(form, 0);
            EXPECT(previous == rx::LT_ONE_Q16);
            for (const uint64_t depth : depths) {
                const uint64_t coupling = rx::RcFaceCouplingQ16(form, depth);
                EXPECT(coupling <= previous);
                previous = coupling;
            }
        }

        std::printf("係数 g(τ)(Q16 → 割合):\n  τ        Rosseland  二流束\n");
        for (const double depth : {0.1, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 28.8, 32.0, 52.8, 100.0, 1000.0, 1.0e6}) {
            std::printf("  %-8g %.4f     %.4f\n", depth,
                        static_cast<double>(rx::RcFaceCouplingQ16(rx::RC_FACE_ROSSELAND, DepthQ32(depth))) / 65536.0,
                        static_cast<double>(rx::RcFaceCouplingQ16(rx::RC_FACE_TWO_FLUX, DepthQ32(depth))) / 65536.0);
        }
    }

    // --- 3・6. 拡散と費用 ---

    constexpr int64_t SLAB_X = 24;
    constexpr int64_t SLAB_SIDE = 11;
    constexpr int64_t SLAB_CENTER = 12;
    constexpr double SLAB_BASE_MK = 1000000;
    constexpr double SLAB_CURVE = 0.5 / 144.0;  // T⁴ = T0⁴ (1 + 曲がり (x − 真ん中)²)。端で 1.5 倍

    uint32_t SlabMk(int64_t x, bool curved) {
        const double offset = static_cast<double>(x - SLAB_CENTER);
        const double factor = curved ? 1.0 + (SLAB_CURVE * offset * offset) : 1.0;

        return static_cast<uint32_t>(std::lround(SLAB_BASE_MK * std::pow(factor, 0.25)));
    }

    // 真ん中の列(x = 真ん中・y, z の真ん中 3 × 3)の正味の平均(刻みの平均)と保存
    struct SlabRun {
        double net = 0;
        double milliseconds = 0;
        bool conserves = true;
    };

    SlabRun RunSlab(uint64_t depthQ32, uint32_t form, bool curved, uint32_t ticks) {
        const CascadeGrid grid = MakeGrid(SLAB_X, SLAB_SIDE, SLAB_SIDE, [&](int64_t x, int64_t, int64_t) {
            return std::pair{Medium(depthQ32), SlabMk(x, curved)};
        });
        SlabRun run;
        CascadeCarry carry;
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            const auto before = std::chrono::steady_clock::now();
            const CascadeTickResult result = StepCascadeLight(grid, carry, Params(form), tick);
            run.milliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before)
                                    .count();
            run.conserves = run.conserves && Conserves(result);
            for (int64_t z = SLAB_SIDE / 2 - 1; z <= SLAB_SIDE / 2 + 1; ++z) {
                for (int64_t y = SLAB_SIDE / 2 - 1; y <= SLAB_SIDE / 2 + 1; ++y)
                    run.net += static_cast<double>(Net(result, grid.IndexOf(SLAB_CENTER, y, z)));
            }

            carry = result.carry;
        }

        run.net /= 9.0 * ticks;
        run.milliseconds /= ticks;

        return run;
    }

    void RecordDiffusion() {
        // Rosseland: 正味 = 4 ÷ 3τ × (B(x+1) − 2B(x) + B(x−1))(B = 黒い面 1 つが 1 刻みに放つ量。帯の和)
        const double second = static_cast<double>(rx::RcFaceBlackEmission(SlabMk(SLAB_CENTER + 1, true))) * 2.0 -
                              2.0 * static_cast<double>(rx::RcFaceBlackEmission(SlabMk(SLAB_CENTER, true)));
        constexpr uint32_t TICKS = 4;
        std::printf(
            "拡散(記録): 一様な板 %lld × %lld × %lld・T⁴ を 2 次に(真ん中 1000 K)。真ん中の正味 ÷ Rosseland(%u "
            "刻みの平均)\n",
            static_cast<long long>(SLAB_X), static_cast<long long>(SLAB_SIDE), static_cast<long long>(SLAB_SIDE),
            TICKS);
        std::printf("  τ        形 0 崖   形 1 Rosseland  形 2 二流束   (1 刻みの ms: 形 0 / 1 / 2)\n");
        for (const double depth : {0.5, 2.0, 8.0, 16.0, 31.0, 52.8, 200.0}) {
            std::array<double, 3> ratio{};
            std::array<double, 3> milliseconds{};
            for (size_t f = 0; f < FORMS.size(); ++f) {
                const SlabRun curved = RunSlab(DepthQ32(depth), FORMS[f], true, TICKS);
                const SlabRun flat = RunSlab(DepthQ32(depth), FORMS[f], false, TICKS);
                EXPECT(curved.conserves && flat.conserves);
                ratio[f] = (curved.net - flat.net) / (4.0 / (3.0 * depth) * second);
                milliseconds[f] = (curved.milliseconds + flat.milliseconds) / 2;
            }

            std::printf("  %-8g %8.3f   %8.3f        %8.3f      (%.0f / %.0f / %.0f)\n", depth, ratio[0], ratio[1],
                        ratio[2], milliseconds[0], milliseconds[1], milliseconds[2]);
        }
    }

    // --- 4. 釣り合い ---

    constexpr double BALANCE_SHARE_LIMIT = 0.03;  // 殻の内の和 ÷ 放った の上限(T-1367)

    // 場面: 0 = τ 2 の物だけ・1 = τ 40 の物だけ・2 = τ 2 の物の中に τ 40 の板と石の塊
    void RecordBalance(uint32_t scene) {
        constexpr int64_t SIZE = 12;
        const uint64_t opaque = rx::RC_FACE_DEPTH_LIMIT_Q32;
        const CascadeGrid grid = MakeGrid(SIZE, SIZE, SIZE, [&](int64_t x, int64_t y, int64_t z) {
            const bool shell = x < 1 || y < 1 || z < 1 || x >= SIZE - 1 || y >= SIZE - 1 || z >= SIZE - 1;
            const bool block = scene == 2 && x >= 7 && x < 9 && y >= 1 && y < 4 && z >= 7 && z < 9;
            const bool slab = scene == 2 && y == 5 && x >= 2 && x < 6;
            const uint64_t medium = scene == 1 ? DepthQ32(40) : DepthQ32(2);
            const uint64_t depth = shell || block ? opaque : (slab ? DepthQ32(40) : medium);

            return std::pair{Medium(depth), uint32_t{1000000}};
        });
        constexpr uint32_t TICKS = 8;
        constexpr std::array<const char*, 3> SCENES = {"τ 2 の物だけ", "τ 40 の物だけ", "τ 2 の物に τ 40 の板と石の塊"};
        std::printf("  場面 %s\n", SCENES[scene]);
        for (size_t f = 0; f < FORMS.size(); ++f) {
            std::vector<double> net(grid.CellCount(), 0);
            double emitted = 0;
            CascadeCarry carry;
            bool conserves = true;
            for (uint32_t tick = 0; tick < TICKS; ++tick) {
                const CascadeTickResult result = StepCascadeLight(grid, carry, Params(FORMS[f]), tick);
                conserves = conserves && Conserves(result);
                for (size_t cell = 0; cell < grid.CellCount(); ++cell)
                    net[cell] += static_cast<double>(Net(result, cell));
                for (const CascadeBandLedger& band : result.ledger)
                    emitted += static_cast<double>(band.emitted);

                carry = result.carry;
            }

            EXPECT(conserves);
            double worst = 0;
            int64_t worstCell = 0;
            double inside = 0;
            for (size_t cell = 0; cell < grid.CellCount(); ++cell) {
                const auto flat = static_cast<int64_t>(cell);
                const std::array<int64_t, 3> at = {flat % SIZE, (flat / SIZE) % SIZE, flat / (SIZE * SIZE)};
                if (std::ranges::any_of(at, [](int64_t v) { return v == 0 || v == SIZE - 1; }))
                    continue;

                inside += net[cell];
                if (std::fabs(net[cell]) > worst) {
                    worst = std::fabs(net[cell]);
                    worstCell = flat;
                }
            }

            // 殻の内側の面(τ 2 の物と石が触れる面)の正味の和: 崖が向こうだけを見ていた時は石だけが放ち、τ 2 の物が温まった(T-1367)
            std::printf(
                "  形 %-12s 正味の最大 %8.1f W(セル %lld,%lld,%lld)・殻の内の和 %+9.1f W・放った %.0f W・保存 %s\n",
                FORM_NAMES[f], worst / TICKS * UNITS_TO_WATTS, static_cast<long long>(worstCell % SIZE),
                static_cast<long long>((worstCell / SIZE) % SIZE), static_cast<long long>(worstCell / (SIZE * SIZE)),
                inside / TICKS * UNITS_TO_WATTS, emitted / TICKS * UNITS_TO_WATTS, conserves ? "OK" : "NG");
            // 向きの粗さの残り(n0 4・2 段・8 刻み。T-1365 の形 1・2 は τ 2 の箱で −2.3 %・−2.0 %)の大きさ以下
            EXPECT(std::fabs(inside) <= BALANCE_SHARE_LIMIT * emitted);
        }
    }

    // --- 5. 2 つのセルの受け渡し ---

    uint64_t OpaqueDepth(double micromoles) {
        // 既定の灰色の固体の a(LT_OPAQUE_ABSORPTION_MM2_PER_MOL)× 量
        const double sum = static_cast<double>(rx::LT_OPAQUE_ABSORPTION_MM2_PER_MOL) * micromoles;

        return rx::RcFaceDepthQ32({.hi = 0, .lo = static_cast<uint64_t>(sum)});
    }

    void RecordPair() {
        struct Material {
            const char* name;
            uint64_t depthQ32;
            const char* conduction;
        };
        // 木: 0.125 m³ × 600 kg/m³ ÷ 0.162 kg/mol = 463 mol・砂: 0.125 m³ × 0.6 × 2650 kg/m³ ÷ 0.060 kg/mol = 3312 mol(既定の灰色の a)
        const std::array<Material, 7> materials = {
            Material{"木(既定の灰色)", OpaqueDepth(463.0e6), "伝導 k 0.15 で 37.5 W"},
            Material{"砂(既定の灰色)", OpaqueDepth(3312.0e6), "伝導 k 0.3 で 75 W"},
            Material{"草 表の値 τ 52.8", DepthQ32(52.8), "伝導 0.06 kJ/秒 = 60 W(T-1156。1000 K 差)"},
            Material{"草 τ 28.8", DepthQ32(28.8), ""},
            Material{"τ 8", DepthQ32(8), ""},
            Material{"τ 2", DepthQ32(2), ""},
            Material{"τ 0.5", DepthQ32(0.5), ""},
        };
        std::printf("2 つのセル(記録): 800 K と 300 K の隣どうしが放射で渡す正味(W。σ(T1⁴ − T2⁴) × 0.25 m² = %.0f W)\n",
                    5.670374e-8 * (std::pow(800.0, 4) - std::pow(300.0, 4)) * 0.25);
        for (const Material& material : materials) {
            std::array<double, 3> watts{};
            for (size_t f = 0; f < FORMS.size(); ++f) {
                const CascadeGrid grid = MakeGrid(2, 1, 1, [&](int64_t x, int64_t, int64_t) {
                    return std::pair{Medium(material.depthQ32), x == 0 ? uint32_t{800000} : uint32_t{300000}};
                });
                CascadeCarry carry;
                double exchanged = 0;
                constexpr uint32_t TICKS = 4;
                for (uint32_t tick = 0; tick < TICKS; ++tick) {
                    const CascadeTickResult result = StepCascadeLight(grid, carry, Params(FORMS[f]), tick);
                    EXPECT(Conserves(result));
                    const auto received = [&](size_t cell) {
                        return static_cast<double>(result.absorbed[(cell * rx::LT_BANDS) + rx::LT_BAND_SHORT] +
                                                   result.absorbed[(cell * rx::LT_BANDS) + rx::LT_BAND_LONG]);
                    };
                    exchanged += received(1) - received(0);
                    carry = result.carry;
                }

                watts[f] = exchanged / TICKS * UNITS_TO_WATTS;
            }

            std::printf("  %-22s τ %-10.4g 形 0 %8.1f W・形 1 %8.1f W・形 2 %8.1f W  %s\n", material.name,
                        static_cast<double>(material.depthQ32) / 4294967296.0, watts[0], watts[1], watts[2],
                        material.conduction);
        }
    }

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestCoupling();
    RecordPair();
    std::printf(
        "釣り合い(記録): 同じ温度 1000 K の閉じた箱(12³・外の 1 層が石の殻)。殻を除くセルの正味(8 刻みの平均)\n");
    for (uint32_t scene = 0; scene < 3; ++scene)
        RecordBalance(scene);
    RecordDiffusion();
    if (failureCount != 0) {
        std::printf("%d 件 失敗\n", failureCount);

        return 1;
    }

    std::printf("OK\n");

    return 0;
}
