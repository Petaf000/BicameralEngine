// transport_apply_test.cpp — 面の帳簿と当てる段(shaders/common/transport_apply.hlsli。T-0714・ADR-0334)と、
// 流れの種類「熱の伝導(陽)」(transport_conduction.hlsli)の試験。
//
// 確かめること(CPU):
//   1. 上限: 倍率は 1 以下・中身(器の残り)を超えない・両向きで同じ大きさに縮む(順に依存しない・保存)
//   2. 伝導の種類: 当てる段の 1 面の流れ(TxFaceFlow)が、今の式(MrSubstepSameLevelFlow・MrSubstepCrossLevelFlow)とビット一致
//      (いろいろな温度・コンダクタンス・熱容量 × レベル × 小刻み × レベルの差)
//   3. 費用: 多重解像度の CPU の木(鎖の場面)を当てる段で 20 刻み進める時間(docs/perf.md。置き換えの前後は ctest の時間で比べる)
//   今の MR の伝導とのビット一致(置き換えた形)は、今の試験(multires_conduction・multires_subcycle・multires_activity など)が確かめる。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "common/transport_conduction.hlsli"
#include "core/aliases.h"
#include "multires_conduction_scene.h"
#include "reaction_test_table.h"
#include "sim/multires_nest.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        if (failureCount < 20)
            std::printf("FAILED line %d: %s\n", line, text);

        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // 決まった乱数(xorshift64)
    struct Random {
        uint64_t state = 0x9E3779B97F4A7C15ull;

        uint64_t Next() {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;

            return state;
        }

        uint64_t Below(uint64_t bound) { return Next() % bound; }
    };

    // --- 1. 上限 ---
    void TestLimits() {
        EXPECT(TxSideScale(10, 0) == TX_SCALE_ONE);
        EXPECT(TxSideScale(10, 10) == TX_SCALE_ONE);
        EXPECT(TxSideScale(5, 10) == TX_SCALE_ONE / 2);
        EXPECT(TxSideScale(0, 10) == 0);
        EXPECT(TxScaleFlow(-7, TX_SCALE_ONE) == -7);

        Random random;
        for (uint32_t i = 0; i < 20000; ++i) {
            // セル 1 つの出ていく流れ 1〜6 本と中身。縮めた和は中身を超えない
            const uint32_t faces = 1 + static_cast<uint32_t>(random.Below(6));
            int64_t flows[6] = {};
            uint64_t outTotal = 0;
            for (uint32_t f = 0; f < faces; ++f) {
                flows[f] = static_cast<int64_t>(random.Below(uint64_t{1} << (1 + random.Below(40))));
                outTotal += static_cast<uint64_t>(flows[f]);
            }

            const uint64_t content = random.Below(outTotal + 2);
            const uint64_t room = random.Below(uint64_t{1} << 40);
            const TxLimit limit = TxMakeLimit(content, outTotal, room, 0);
            EXPECT(limit.outScale <= TX_SCALE_ONE && limit.inScale == TX_SCALE_ONE);

            uint64_t scaledTotal = 0;
            for (uint32_t f = 0; f < faces; ++f) {
                const uint64_t scale = TxFaceScale(limit, TxNoLimit());
                const int64_t forward = TxScaleFlow(flows[f], scale);
                const int64_t backward = TxScaleFlow(-flows[f], scale);
                EXPECT(forward == -backward);
                EXPECT(forward >= 0 && forward <= flows[f]);
                scaledTotal += static_cast<uint64_t>(forward);
            }

            EXPECT(scaledTotal <= content || outTotal <= content);

            // 入る側の倍率と出る側の倍率の小さい方
            const TxLimit target = TxMakeLimit(0, 0, room, outTotal);
            EXPECT(TxFaceScale(limit, target) == std::min(limit.outScale, target.inScale));
            EXPECT(TxMinLimit(limit, target).outScale == std::min(limit.outScale, target.outScale));
        }
    }

    // --- 2. 伝導の種類と今の式 ---

    // 1 面の流れだけを読む世界(見立てと粗い隣のレベル)
    struct FaceWorld {
        CvCellView other{};
        int32_t otherLevel = 0;

        [[nodiscard]] CvCellView View(uint32_t /*slot*/, uint32_t /*index*/) const { return other; }
        [[nodiscard]] int32_t Level(uint32_t /*slot*/) const { return otherLevel; }
    };

    MrThermal RandomThermal(Random& random) {
        MrThermal thermal{};
        thermal.temperature = static_cast<uint32_t>(random.Below(4000000));
        thermal.conductance = static_cast<uint32_t>(random.Below(uint64_t{1} << (random.Below(24) + 1)));
        thermal.capacityLimit = (random.Below(uint64_t{1} << 36) >> HC_CAPACITY_SHIFT) * HC_LIMIT_PER_CAPACITY;

        return thermal;
    }

    CvCellView ViewOf(MrThermal thermal) {
        CvCellView view{};
        view.thermal = thermal;

        return view;
    }

    void TestConductionKind() {
        Random random;
        uint32_t compared = 0;
        for (uint32_t i = 0; i < 50000; ++i) {
            const MrThermal self = RandomThermal(random);
            const MrThermal other = RandomThermal(random);
            const int32_t base = static_cast<int32_t>(random.Below(6)) - 1;
            const auto maxGap = static_cast<uint32_t>(random.Below(4));
            const int32_t level = static_cast<int32_t>(random.Below(9)) - 2;
            const TxConduction kind = TxMakeConduction(base, maxGap);
            const uint32_t shift = MrSubcycleShift(level, base, maxGap);

            // --- 同じレベル ---
            const FaceWorld same{.other = ViewOf(other), .otherLevel = level};
            const int64_t sameFlow = TxFaceFlow(kind, same, ViewOf(self), level,
                                                MrMakeFaceNeighbor(MR_NEIGHBOR_SAME, 0, 0, 0));
            EXPECT(sameFlow == MrSubstepSameLevelFlow(self, other, level, shift));

            // --- 粗い隣(レベルの差 1〜3)---
            const auto gap = static_cast<uint32_t>(1 + random.Below(3));
            const FaceWorld coarse{.other = ViewOf(other), .otherLevel = level - static_cast<int32_t>(gap)};
            const uint32_t coarseShift = MrSubcycleShift(coarse.otherLevel, base, maxGap);
            const int64_t crossFlow = TxFaceFlow(kind, coarse, ViewOf(self), level,
                                                 MrMakeFaceNeighbor(MR_NEIGHBOR_COARSER, 0, 0, gap));
            EXPECT(crossFlow == MrSubstepCrossLevelFlow(self, other, level, gap, shift, coarseShift));
            compared += 2;
        }

        std::printf("伝導の種類: %u 面を今の式と比べた\n", compared);
    }

    // --- 3. 費用 ---
    void MeasureCost(const BakedReactionTable& table) {
        constexpr uint64_t TICKS = 20;
        MultiresNest nest = test::MakeChainNest(table, 16);
        MultiresStepOptions options;
        options.conduction = true;

        const auto begin = chr::steady_clock::now();
        for (uint64_t tick = 0; tick < TICKS; ++tick)
            StepActive(nest, table, test::CONDUCTION_SEED, tick, options);

        const auto elapsed = chr::duration_cast<chr::microseconds>(chr::steady_clock::now() - begin).count();
        std::printf("費用: 鎖の場面(伝導・活性)%llu 刻み %lld µs(1 刻み %lld µs)\n",
                    static_cast<unsigned long long>(TICKS), static_cast<long long>(elapsed),
                    static_cast<long long>(elapsed / static_cast<long long>(TICKS)));
    }

}  // namespace

int main() {
    const BakedReactionTable table = BakeReactionTable(MakeCombustionTestTable()).value();

    TestLimits();
    TestConductionKind();
    MeasureCost(table);

    if (failureCount != 0) {
        std::printf("transport_apply_test: %d 件失敗\n", failureCount);
        return EXIT_FAILURE;
    }

    std::printf("transport_apply_test: OK\n");

    return EXIT_SUCCESS;
}
