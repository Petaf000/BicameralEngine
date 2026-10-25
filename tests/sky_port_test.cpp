// sky_port_test.cpp — 空との交換の口(sim/sky_port。T-1145・ADR-0474)の CPU の試験。場面は tests/sun_column_scene.h(太陽の影の柱と同じ)。
//   1. 開けた平らな地面: 中ほどのセルが受ける量(吸った + 返した)= 差(空 − 参照の黒体)× 0.25 m² ÷ 60(±1 %。夜の空・昼の散らばった光)
//   2. 壁の横の面: 開けた縦の面 2 枚で差の 1 倍(1 枚 1/2 倍。±1 %)
//   3. 空の光 = 参照の黒体なら掃きは 0 本(口は何もしない)
//   4. 保存(帯ごと・正と負ごとに 入った = 吸った + 返した + 外へ)・順によらない(逆の順で同じ語)
//   5. 誤りの入力を断る
// 公開側に置く試験データで、ゲームの中身ではない(D-006)。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "common/cascade_light.hlsli"
#include "sim/sky_port.h"
#include "sun_column_scene.h"

namespace {

    using namespace bicameral;
    using namespace bicameral::sim;
    namespace mr = bicameral::multires;
    namespace rx = bicameral::reaction;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("失敗 %d 行: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr uint32_t REFERENCE_MK = 293150;   // 場面のセルの温度(sun_column_scene.h)= 参照の温度
    constexpr uint32_t NIGHT_SKY_MK = 250000;   // 晴れた夜の空の目安(試験の値)
    constexpr uint32_t DIFFUSE_SHORT = 100000;  // 昼の散らばった光(mW/m²。試験の値)

    // セル(0.5 m のセルの座標)が受けた量(吸った + 返した。正 − 負。mJ)
    int64_t Received(const sim::MultiresNest& nest, const SkyPortResult& result, int64_t x, int64_t y, int64_t z,
                     uint32_t band) {
        int64_t received = 0;
        test::ForEachSunLeafCell(nest, [&](uint32_t slot, uint32_t index, const mr::MrBlock& block) {
            const auto corner = test::SunQuarterCorner(block, index);
            if ((corner[0] >> 1) != x || (corner[1] >> 1) != y || (corner[2] >> 1) != z)
                return;

            for (const uint32_t kind : {mr::SUN_KIND_ABSORBED, mr::SUN_KIND_REFLECTED}) {
                const uint32_t word = mr::SunCellWord(slot, index, band, kind);
                received += static_cast<int64_t>(result.words[0][word]) - static_cast<int64_t>(result.words[1][word]);
            }
        });

        return received;
    }

    // 期待: 差(mW/m²)× 0.25 m² ÷ 60 s = 差 ÷ 240(mJ)× 面の割合
    bool Near(int64_t got, int64_t netMilliwatts, int64_t faceNumerator, int64_t faceDenominator) {
        const int64_t expected = (netMilliwatts * faceNumerator) / (240 * faceDenominator);
        const int64_t error = got > expected ? got - expected : expected - got;
        const int64_t magnitude = expected < 0 ? -expected : expected;

        return error * 100 <= magnitude;
    }

    bool Conserves(const SkyPortResult& result) {
        for (const std::vector<uint64_t>& words : result.words) {
            for (uint32_t band = 0; band < rx::LT_BANDS; ++band) {
                const uint64_t entered = words[mr::SunLedgerWord(band, mr::SUN_LEDGER_ENTERED)];
                const uint64_t out = words[mr::SunLedgerWord(band, mr::SUN_LEDGER_ABSORBED)] +
                                     words[mr::SunLedgerWord(band, mr::SUN_LEDGER_REFLECTED)] +
                                     words[mr::SunLedgerWord(band, mr::SUN_LEDGER_ESCAPED)];
                if (entered != out)
                    return false;
            }
        }

        return true;
    }

    SkyPortParams Night() {
        return {.referenceMk = REFERENCE_MK,
                .skyShortMilliwatts = 0,
                .skyLongMilliwatts = SkyBlackLongMilliwatts(NIGHT_SKY_MK)};
    }

    // --- 1. 開けた平らな地面 ---

    void TestFlatGround(const BakedReactionTable& table) {
        // 16 × 4 × 16 m(0.5 m のセルで 32 × 8 × 32)。地面は一番下の 1 段。中ほど(端から 8 セル)は上からの光線だけを受ける
        const sim::MultiresNest nest = test::MakeSunWorld(table, 0, 4, 1, 4, test::FlatGround);
        const rx::RcBands reference = rx::RcBlackIrradianceMilliwatts(REFERENCE_MK);
        SkyPortParams day = Night();
        day.skyShortMilliwatts = DIFFUSE_SHORT;
        for (const SkyPortParams& params : {Night(), day}) {
            const auto result = SweepSkyPort(nest, table, params);
            EXPECT(result.has_value());
            if (!result)
                continue;

            const int64_t netShort = int64_t{params.skyShortMilliwatts} - static_cast<int64_t>(reference.shortBand);
            const int64_t netLong = int64_t{params.skyLongMilliwatts} - static_cast<int64_t>(reference.longBand);
            bool near = true;
            for (int64_t x = 8; x < 24; ++x) {
                for (int64_t z = 8; z < 24; ++z) {
                    near = near && Near(Received(nest, *result, x, 0, z, rx::LT_BAND_LONG), netLong, 1, 1);
                    near = near &&
                           (netShort == 0 || Near(Received(nest, *result, x, 0, z, rx::LT_BAND_SHORT), netShort, 1, 1));
                }
            }

            EXPECT(near);
            EXPECT(Conserves(*result));
            std::printf(
                "開けた地面: 差 短い帯 %lld・長い帯 %lld mW/m²・掃き %u 本、中ほどのセルの受けた量 %lld mJ/刻み"
                "(期待 %lld)\n",
                static_cast<long long>(netShort), static_cast<long long>(netLong), result->sweeps,
                static_cast<long long>(Received(nest, *result, 16, 0, 16, rx::LT_BAND_LONG)),
                static_cast<long long>(netLong / 240));
        }
    }

    // --- 2. 壁の横の面 ---

    void TestWall(const BakedReactionTable& table) {
        // 壁は x = 1〜1.5 m(0.5 m のセルで x = 2)・高さ 0.5〜2.5 m(y = 1〜4)・奥行き全部。真ん中の高さ(y = 2)の壁のセルは
        // −x と +x の 2 枚の縦の面だけが空に開く → 差の 1/2 × 2
        const sim::MultiresNest nest = test::MakeSunWorld(table, 0, 1, 1, 4, test::WallScene);
        const rx::RcBands reference = rx::RcBlackIrradianceMilliwatts(REFERENCE_MK);
        const int64_t netLong = int64_t{Night().skyLongMilliwatts} - static_cast<int64_t>(reference.longBand);
        const auto result = SweepSkyPort(nest, table, Night());
        EXPECT(result.has_value());
        if (!result)
            return;

        bool near = true;
        for (int64_t z = 8; z < 24; ++z)
            near = near && Near(Received(nest, *result, 2, 2, z, rx::LT_BAND_LONG), netLong, 1, 1);

        EXPECT(near);
        EXPECT(Conserves(*result));
        std::printf("壁: 縦の面 2 枚のセルの受けた量 %lld mJ/刻み(期待 %lld = 1/2 × 2)\n",
                    static_cast<long long>(Received(nest, *result, 2, 2, 16, rx::LT_BAND_LONG)),
                    static_cast<long long>(netLong / 240));
    }

    // --- 3. 空 = 参照・4. 順によらない ---

    void TestBalancedAndOrder(const BakedReactionTable& table) {
        const rx::RcBands reference = rx::RcBlackIrradianceMilliwatts(REFERENCE_MK);
        const SkyPortParams balanced{.referenceMk = REFERENCE_MK,
                                     .skyShortMilliwatts = static_cast<uint32_t>(reference.shortBand),
                                     .skyLongMilliwatts = static_cast<uint32_t>(reference.longBand)};
        const auto plan = PlanSkyPort(balanced);
        EXPECT(plan.has_value() && plan->empty());

        const sim::MultiresNest nest = test::MakeSunWorld(table, 0, 2, 1, 2, test::Terrain);
        SkyPortParams mixed = Night();
        mixed.skyShortMilliwatts = DIFFUSE_SHORT;  // 短い帯は正・長い帯は負(正と負の掃きが両方ある)
        const auto forward = SweepSkyPort(nest, table, mixed);
        const auto reversed = SweepSkyPort(nest, table, mixed, true);
        EXPECT(forward.has_value() && reversed.has_value());
        if (!forward || !reversed)
            return;

        EXPECT(forward->sweeps == 8);
        EXPECT(forward->words == reversed->words);
        EXPECT(Conserves(*forward));
        std::printf("空 = 参照で掃き 0 本・地形で正と負の掃き %u 本が逆の順でも同じ語・帯ごとに保存\n",
                    forward->sweeps);
    }

    // --- 5. 誤りの入力 ---

    void TestRejects() {
        SkyPortParams upward = Night();
        upward.directions[0].direction = {1, 1, 1};
        EXPECT(!PlanSkyPort(upward).has_value());

        SkyPortParams shares = Night();
        shares.directions[0].shareQ16 += 1;
        EXPECT(!PlanSkyPort(shares).has_value());

        SkyPortParams grazing = Night();
        grazing.directions = {{.direction = {32768, -1, 0}, .shareQ16 = 1u << 16}};
        EXPECT(!PlanSkyPort(grazing).has_value());
        std::printf("誤り: 上向き・取り分の和・水平に近すぎる向きを断る\n");
    }

}  // namespace

int main() {
    const BakedReactionTable table = test::MakeSunTable();
    TestFlatGround(table);
    TestWall(table);
    TestBalancedAndOrder(table);
    TestRejects();
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("通過\n");
    return 0;
}
