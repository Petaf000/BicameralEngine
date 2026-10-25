// sky_port_test.cpp — 空との交換の口(sim/sky_port。T-1145・ADR-0474)の CPU の試験。場面は tests/sun_column_scene.h(太陽の影の柱と同じ)。
//   1. 開けた平らな地面: 中ほどのセルが受ける量(吸った + 返した)= 差(空 − 参照の黒体)× 0.25 m² ÷ 60(±1 %。夜の空・昼の散らばった光)
//   2. 壁の横の面: 開けた縦の面 2 枚で差の 1 倍(1 枚 1/2 倍。±1 %)
//   3. 空の光 = 参照の黒体なら掃きは 0 本(口は何もしない)
//   4. 保存(帯ごと・正と負ごとに 入った = 吸った + 返した + 外へ)・順によらない(逆の順で同じ語)
//   5. 誤りの入力を断る
//   6. 帯の正と負を 1 本の光線にまとめた掃き(SweepSkyPort・T-1147)が、帯を分けた掃き(PlanSkyPort を 1 本ずつ)の和とビット単位で同じ
//   7. 向きを刻みで回す形(T-1147。仮): n 刻みの和が全部の向きの n 刻み分と丸めの差だけで同じ・1 刻みの揺れの大きさ(測り)
// 公開側に置く試験データで、ゲームの中身ではない(D-006)。
#include <algorithm>
#include <array>
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

        EXPECT(forward->sweeps == 4);  // 向き 4 つ(帯の正と負は 1 本の光線にまとめる)
        EXPECT(forward->words == reversed->words);
        EXPECT(Conserves(*forward));
        std::printf("空 = 参照で掃き 0 本・地形で正と負の掃き %u 本が逆の順でも同じ語・帯ごとに保存\n",
                    forward->sweeps);
    }

    // --- 6. まとめた掃き = 帯を分けた掃きの和(T-1147)---

    // 定義: PlanSkyPort の掃き(帯の正と負を別に)を 1 本ずつ太陽の影の柱で走らせて、正と負に足す
    std::array<std::vector<uint64_t>, 2> SeparateSweeps(const sim::MultiresNest& nest, const BakedReactionTable& table,
                                                        const SkyPortParams& params, uint32_t& sweeps) {
        std::array<std::vector<uint64_t>, 2> sums;
        for (std::vector<uint64_t>& sum : sums)
            sum.assign(SunColumnWordCount(nest), 0);

        sweeps = 0;
        const auto plan = PlanSkyPort(params);
        if (!plan)
            return sums;

        for (const SkyPortSweep& sweep : *plan) {
            const auto sunParams = MakeSunParams(nest, sweep.light, params.rayLevel);
            if (!sunParams)
                continue;

            const std::vector<uint64_t> words = SweepSunColumn(nest, table, *sunParams);
            std::vector<uint64_t>& sum = sums[sweep.negative ? 1 : 0];
            for (size_t word = 0; word < words.size(); ++word)
                sum[word] += words[word];

            ++sweeps;
        }

        return sums;
    }

    void TestMergedMatchesSeparate(const BakedReactionTable& table) {
        const sim::MultiresNest terrain = test::MakeSunWorld(table, 0, 2, 1, 2, test::Terrain);
        const sim::MultiresNest wall = test::MakeSunWorld(table, 0, 1, 1, 4, test::WallScene);
        SkyPortParams day = Night();
        day.skyShortMilliwatts = DIFFUSE_SHORT;  // 短い帯は正・長い帯は負
        SkyPortParams warm = Night();
        warm.skyLongMilliwatts = SkyBlackLongMilliwatts(310000);  // 長い帯も正(空が参照より暖かい)
        bool same = true;
        for (const sim::MultiresNest* nest : {&terrain, &wall}) {
            for (const SkyPortParams& params : {Night(), day, warm}) {
                uint32_t separate = 0;
                const auto expected = SeparateSweeps(*nest, table, params, separate);
                const auto merged = SweepSkyPort(*nest, table, params);
                same = same && merged.has_value() && merged->words == expected && separate >= merged->sweeps;
            }
        }

        EXPECT(same);
        std::printf(
            "まとめた掃き(向き 1 つに 1 本)= 帯を分けた掃き(正と負で最大 8 本)の和: 地形・壁 × "
            "夜・昼・暖かい空でビット一致\n");
    }

    // --- 7. 向きを刻みで回す(T-1147。仮)---

    // セルごとの受けた量(長い帯。正 − 負)を並べる
    std::vector<int64_t> ReceivedCells(const sim::MultiresNest& nest, const SkyPortResult& result) {
        std::vector<int64_t> cells;
        test::ForEachSunLeafCell(nest, [&](uint32_t slot, uint32_t index, const mr::MrBlock& /*block*/) {
            int64_t received = 0;
            for (const uint32_t kind : {mr::SUN_KIND_ABSORBED, mr::SUN_KIND_REFLECTED}) {
                const uint32_t word = mr::SunCellWord(slot, index, rx::LT_BAND_LONG, kind);
                received += static_cast<int64_t>(result.words[0][word]) - static_cast<int64_t>(result.words[1][word]);
            }

            cells.push_back(received);
        });

        return cells;
    }

    int64_t Magnitude(int64_t value) {
        return value < 0 ? -value : value;
    }

    void TestRotation(const BakedReactionTable& table) {
        const sim::MultiresNest terrain = test::MakeSunWorld(table, 0, 2, 1, 2, test::Terrain);
        const SkyPortParams params = Night();
        const auto all = SweepSkyPort(terrain, table, params);
        EXPECT(all.has_value());
        if (!all)
            return;

        // --- 4 刻み(向き 4 つを 1 つずつ・4 倍の取り分)---
        const std::vector<int64_t> once = ReceivedCells(terrain, *all);
        std::vector<int64_t> rotatedSum(once.size(), 0);
        std::vector<int64_t> worstTick(once.size(), 0);
        const auto count = static_cast<uint32_t>(params.directions.size());
        for (uint32_t tick = 0; tick < count; ++tick) {
            SkyPortParams rotated = params;
            rotated.rotateDirections = true;
            rotated.tick = tick;
            const auto result = SweepSkyPort(terrain, table, rotated);
            EXPECT(result.has_value() && result->sweeps == 1);
            if (!result)
                return;

            const std::vector<int64_t> cells = ReceivedCells(terrain, *result);
            for (size_t cell = 0; cell < cells.size(); ++cell) {
                rotatedSum[cell] += cells[cell];
                worstTick[cell] = std::max(worstTick[cell], Magnitude(cells[cell] - once[cell]));
            }
        }

        // --- 比べる(開けた地面の量の 1/10 以上を受けるセル)---
        const int64_t open = Magnitude(
            once.empty() ? 0 : *std::max_element(once.begin(), once.end(), [](int64_t a, int64_t b) {
                return Magnitude(a) < Magnitude(b);
            }));
        int64_t sumErrorPpm = 0;
        int64_t tickSwingPercent = 0;
        uint32_t compared = 0;
        for (size_t cell = 0; cell < once.size(); ++cell) {
            const int64_t reference = Magnitude(once[cell]);
            if (reference * 10 < open)
                continue;

            ++compared;
            const int64_t sumError = Magnitude(rotatedSum[cell] - (once[cell] * count));
            sumErrorPpm = std::max(sumErrorPpm, sumError * 1000000 / (reference * count));
            tickSwingPercent = std::max(tickSwingPercent, worstTick[cell] * 100 / reference);
        }

        EXPECT(compared > 0);
        EXPECT(sumErrorPpm <=
               5000);  // 4 刻みの和は丸めの差だけ(1 本の量・区間ごとの切り捨て。測りで 0.12 %。線は 0.5 %)
        std::printf(
            "向きを刻みで回す(地形 16 × 4 × 16 m・夜の空): 比べたセル %u・4 刻みの和のずれ 最大 %lld ppm・"
            "1 刻みの揺れ 最大 %lld %%(全部の向きの 1 刻み分に対して)\n",
            compared, static_cast<long long>(sumErrorPpm), static_cast<long long>(tickSwingPercent));
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
    TestMergedMatchesSeparate(table);
    TestRotation(table);
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("通過\n");
    return 0;
}
