// sun_column.cpp — 太陽の影の柱の CPU リファレンス(sun_column.h)。光線 1 本の本体は shaders/common/sun_column.hlsli の SunWalkRay。
#include "sim/sun_column.h"

#include <format>

namespace bicameral::sim {

    namespace mr = ::bicameral::multires;
    namespace rx = ::bicameral::reaction;

    namespace {

        constexpr int64_t DIRECTION_LIMIT = mr::SUN_DIRECTION_LIMIT;
        constexpr uint64_t RAY_ENERGY_LIMIT = uint64_t{1} << 47;  // × 2^16 が 64bit に収まる(SunPassSegment)
        constexpr int64_t RAY_COUNT_LIMIT = int64_t{1} << 31;     // GPU のスレッドの番号が 32bit に収まる

        // --- SunWalkRay に渡す道具(sun_column.hlsli の約束)---

        struct CpuTree {
            const MultiresNest* nest = nullptr;

            [[nodiscard]] mr::MrBlock Block(uint32_t slot) const { return nest->blocks[slot]; }

            [[nodiscard]] uint32_t Lookup(int32_t level, int64_t originX, int64_t originY, int64_t originZ) const {
                return LookupBlock(*nest, level, originX, originY, originZ);
            }
        };

        struct CpuCells {
            const MultiresNest* nest = nullptr;

            [[nodiscard]] rx::RxCell Cell(uint32_t slot, uint32_t index) const {
                return LoadNestCell(*nest, slot, index);
            }
        };

        struct CpuLights {
            const std::vector<rx::LtSpeciesLight>* lights = nullptr;

            [[nodiscard]] rx::LtSpeciesLight Light(uint32_t species) const { return (*lights)[species]; }
        };

        struct CpuSink {
            std::vector<uint64_t>* words = nullptr;

            void Deposit(uint32_t slot, uint32_t index, uint32_t band, uint64_t absorbed, uint64_t reflected) const {
                (*words)[mr::SunCellWord(slot, index, band, mr::SUN_KIND_ABSORBED)] += absorbed;
                (*words)[mr::SunCellWord(slot, index, band, mr::SUN_KIND_REFLECTED)] += reflected;
            }
        };

        struct CpuCatcher {
            const SunCatcher* catcher = nullptr;

            [[nodiscard]] mr::SunCatch Catch(uint32_t rayB, uint32_t rayC) const { return (*catcher)(rayB, rayC); }
        };

        // 光線 1 本の帳簿を語に足す(帯ごと)
        void AddLedger(std::vector<uint64_t>& words, const mr::SunRay& walked) {
            words[mr::SunLedgerWord(rx::LT_BAND_SHORT, mr::SUN_LEDGER_ENTERED)] += walked.enteredShort;
            words[mr::SunLedgerWord(rx::LT_BAND_SHORT, mr::SUN_LEDGER_ABSORBED)] += walked.absorbedShort;
            words[mr::SunLedgerWord(rx::LT_BAND_SHORT, mr::SUN_LEDGER_REFLECTED)] += walked.reflectedShort;
            words[mr::SunLedgerWord(rx::LT_BAND_SHORT, mr::SUN_LEDGER_ESCAPED)] += walked.escapedShort;
            words[mr::SunLedgerWord(rx::LT_BAND_LONG, mr::SUN_LEDGER_ENTERED)] += walked.enteredLong;
            words[mr::SunLedgerWord(rx::LT_BAND_LONG, mr::SUN_LEDGER_ABSORBED)] += walked.absorbedLong;
            words[mr::SunLedgerWord(rx::LT_BAND_LONG, mr::SUN_LEDGER_REFLECTED)] += walked.reflectedLong;
            words[mr::SunLedgerWord(rx::LT_BAND_LONG, mr::SUN_LEDGER_ESCAPED)] += walked.escapedLong;
        }

        int64_t Absolute(int64_t value) {
            return value < 0 ? -value : value;
        }

        // 切り上げの割り算(どちらも 0 以上・分母は正)
        int64_t CeilDivide(int64_t numerator, int64_t denominator) {
            return (numerator + denominator - 1) / denominator;
        }

        int32_t FinestRealLevel(const MultiresNest& nest) {
            int32_t finest = nest.capacity.rootLevel;
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                const mr::MrBlock& block = nest.blocks[slot];
                if (block.kind == mr::MR_BLOCK_REAL && block.level > finest)
                    finest = block.level;
            }

            return finest;
        }

        // 光線の格子の 1 軸(箱の低い端からの番号の始まりと数)。横へずれる光線も入るように、ずれる側へ広げる
        std::array<int64_t, 2> RayRange(int64_t size, int64_t sizeA, int32_t step, int32_t stepA) {
            const int64_t drift = CeilDivide(sizeA * Absolute(step), stepA) + 1;
            if (step > 0)
                return {-drift, size};

            if (step < 0)
                return {0, size + drift};

            return {0, size};
        }

        // 向きの軸の並べ替え: A = 成分の絶対値が一番大きい軸(同じなら番号の小さい方)
        void SetAxes(const SunLight& sun, mr::SunParams& params) {
            const std::array<int64_t, 3> d = {sun.direction[0], sun.direction[1], sun.direction[2]};
            uint32_t axisA = 0;
            for (uint32_t k = 1; k < 3; ++k) {
                if (Absolute(d[k]) > Absolute(d[axisA]))
                    axisA = k;
            }

            params.axisA = axisA;
            params.axisB = axisA == 0 ? 1u : 0u;
            params.axisC = axisA == 2 ? 1u : 2u;
            params.signA = d[axisA] > 0 ? 1 : -1;
            params.stepA = static_cast<int32_t>(Absolute(d[axisA]));
            params.stepB = static_cast<int32_t>(d[params.axisB]);
            params.stepC = static_cast<int32_t>(d[params.axisC]);

            const auto squared = static_cast<uint64_t>((d[0] * d[0]) + (d[1] * d[1]) + (d[2] * d[2]));
            params.lengthQ15 = fx::FxSqrtU64(squared << (2 * mr::SUN_LENGTH_SHIFT));
        }

        std::expected<void, std::string> CheckSun(const MultiresNest& nest, const SunLight& sun, int32_t rayLevel) {
            const bool zero = sun.direction[0] == 0 && sun.direction[1] == 0 && sun.direction[2] == 0;
            const bool large = Absolute(sun.direction[0]) > DIRECTION_LIMIT ||
                               Absolute(sun.direction[1]) > DIRECTION_LIMIT ||
                               Absolute(sun.direction[2]) > DIRECTION_LIMIT;
            if (zero || large)
                return std::unexpected("太陽の向きが 0 か、成分が 32768 を超える");

            const mr::WeWorld& world = nest.edges.world;
            if (world.highX <= world.lowX || world.highY <= world.lowY || world.highZ <= world.lowZ)
                return std::unexpected("世界の箱(nest.edges.world)が空");

            if (world.rootLevel != nest.capacity.rootLevel)
                return std::unexpected("世界の箱のレベルが木の根のレベルと違う");

            // 光線の細かさは 0 以上(1 本の量を 2^L で数える)・木の最も細かい本物のブロック以上(細かい葉を飛ばさない)・20 以下
            const int32_t finest = FinestRealLevel(nest);
            const auto limit = static_cast<int32_t>(mr::SUN_RAY_LEVEL_LIMIT);
            if (rayLevel < 0 || rayLevel < finest || rayLevel > limit || rayLevel - nest.capacity.rootLevel > limit)
                return std::unexpected(std::format("光線の細かさ {} は 0 以上・木の最も細かいレベル {} 以上・20 以下で",
                                                   rayLevel, finest));

            return {};
        }

        // 世界の箱を光線の細かさのセルで、A・B・C の順に
        void SetBox(const MultiresNest& nest, int32_t rayLevel, mr::SunParams& params) {
            const mr::WeWorld& world = nest.edges.world;
            const auto shift = static_cast<uint32_t>(rayLevel - world.rootLevel);
            const std::array<int64_t, 3> low = {world.lowX << shift, world.lowY << shift, world.lowZ << shift};
            const std::array<int64_t, 3> size = {(world.highX - world.lowX) << shift,
                                                 (world.highY - world.lowY) << shift,
                                                 (world.highZ - world.lowZ) << shift};
            params.lowA = low[params.axisA];
            params.lowB = low[params.axisB];
            params.lowC = low[params.axisC];
            params.sizeA = size[params.axisA];
            params.sizeB = size[params.axisB];
            params.sizeC = size[params.axisC];
        }

    }  // namespace

    std::expected<mr::SunParams, std::string> MakeSunParams(const MultiresNest& nest, const SunLight& sun,
                                                            int32_t rayLevel) {
        if (auto checked = CheckSun(nest, sun, rayLevel); !checked)
            return std::unexpected(checked.error());

        mr::SunParams params{};
        params.rayLevel = rayLevel;
        params.rootLevel = nest.capacity.rootLevel;
        SetAxes(sun, params);
        SetBox(nest, rayLevel, params);

        // --- 光線の格子 ---
        const auto rangeB = RayRange(params.sizeB, params.sizeA, params.stepB, params.stepA);
        const auto rangeC = RayRange(params.sizeC, params.sizeA, params.stepC, params.stepA);
        const int64_t countB = rangeB[1] - rangeB[0];
        const int64_t countC = rangeC[1] - rangeC[0];
        if (countB >= RAY_COUNT_LIMIT || countC >= RAY_COUNT_LIMIT || countB * countC >= RAY_COUNT_LIMIT)
            return std::unexpected("光線が多すぎる(2^31 本まで)");

        params.firstRayB = rangeB[0];
        params.firstRayC = rangeC[0];
        params.rayCountB = static_cast<uint32_t>(countB);
        params.rayCountC = static_cast<uint32_t>(countC);

        // --- 1 本の量 ---
        params.energyShort = mr::SunEnergyPerRay(sun.irradianceShortMilliwatts, rayLevel, params.stepA,
                                                 params.lengthQ15);
        params.energyLong = mr::SunEnergyPerRay(sun.irradianceLongMilliwatts, rayLevel, params.stepA, params.lengthQ15);
        if (params.energyShort >= RAY_ENERGY_LIMIT || params.energyLong >= RAY_ENERGY_LIMIT)
            return std::unexpected("光線 1 本の量が 2^47 を超える(強さか光線の細かさが大きすぎる)");

        return params;
    }

    size_t SunColumnWordCount(const MultiresNest& nest) {
        return mr::SUN_LEDGER_WORDS + (size_t{nest.capacity.worldBlocks} * mr::MR_BLOCK_CELLS * mr::SUN_CELL_WORDS);
    }

    std::vector<uint64_t> SweepSunColumn(const MultiresNest& nest, const BakedReactionTable& table,
                                         const mr::SunParams& params, bool reverse) {
        std::vector<uint64_t> words(SunColumnWordCount(nest), 0);
        const CpuTree tree{.nest = &nest};
        const CpuCells cells{.nest = &nest};
        const CpuLights lights{.lights = &table.speciesLight};
        const CpuSink sink{.words = &words};

        const uint64_t rayCount = uint64_t{params.rayCountB} * params.rayCountC;
        for (uint64_t step = 0; step < rayCount; ++step) {
            const uint64_t ray = reverse ? rayCount - 1 - step : step;
            const auto rayB = static_cast<uint32_t>(ray % params.rayCountB);
            const auto rayC = static_cast<uint32_t>(ray / params.rayCountB);
            AddLedger(words, mr::SunWalkRay(tree, cells, lights, sink, params, rayB, rayC));
        }

        return words;
    }

    std::vector<uint64_t> SweepSunColumnCaught(const MultiresNest& nest, const BakedReactionTable& table,
                                               const mr::SunParams& params, const SunCatcher& catcher,
                                               std::vector<SunCaughtRay>& caught) {
        std::vector<uint64_t> words(SunColumnWordCount(nest), 0);
        const CpuTree tree{.nest = &nest};
        const CpuCells cells{.nest = &nest};
        const CpuLights lights{.lights = &table.speciesLight};
        const CpuSink sink{.words = &words};
        const CpuCatcher catching{.catcher = &catcher};

        const uint64_t rayCount = uint64_t{params.rayCountB} * params.rayCountC;
        for (uint64_t ray = 0; ray < rayCount; ++ray) {
            const auto rayB = static_cast<uint32_t>(ray % params.rayCountB);
            const auto rayC = static_cast<uint32_t>(ray / params.rayCountB);
            const mr::SunRay walked = mr::SunWalkRayCaught(tree, cells, lights, sink, catching, params, rayB, rayC);
            AddLedger(words, walked);
            if (walked.caughtShort != 0 || walked.caughtLong != 0)
                caught.push_back(
                    {.rayB = rayB, .rayC = rayC, .caughtShort = walked.caughtShort, .caughtLong = walked.caughtLong});
        }

        return words;
    }

    std::expected<SunColumnResult, std::string> SweepSunColumn(const MultiresNest& nest,
                                                               const BakedReactionTable& table, const SunLight& sun,
                                                               int32_t rayLevel) {
        const auto params = MakeSunParams(nest, sun, rayLevel);
        if (!params)
            return std::unexpected(params.error());

        return SunColumnResult{.params = *params, .words = SweepSunColumn(nest, table, *params)};
    }

}  // namespace bicameral::sim
