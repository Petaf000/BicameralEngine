// transport_apply_test.cpp — 面の帳簿と当てる段(shaders/common/transport_apply.hlsli。T-0714・ADR-0334)と、
// 流れの種類「熱の伝導(陽)」(transport_conduction.hlsli)の試験。
//
// 確かめること(CPU):
//   1. 上限: 倍率は 1 以下・中身(器の残り)を超えない・両向きで同じ大きさに縮む(順に依存しない・保存)
//   2. 伝導の種類: 当てる段の 1 面の流れ(TxFaceFlow)が、今の式(MrSubstepSameLevelFlow・MrSubstepCrossLevelFlow)とビット一致
//      (いろいろな温度・コンダクタンス・熱容量 × レベル × 小刻み × レベルの差)
//   3. 面の並びの当てる段(TxApplyFaceFlow。陰の伝導の流れを当てる形。T-0715): 同じレベルは逆向きに同じ値、違うレベルは
//      細かい側が送った分 = 粗い側の整数部 + 端数(端数の置き場が無ければ整数の単位の倍数だけ)。保存はビット単位
//   4. 費用: 多重解像度の CPU の木(鎖の場面)を当てる段で 20 刻み進める時間(docs/perf.md。置き換えの前後は ctest の時間で比べる)
//   今の MR の伝導とのビット一致(置き換えた形)は、今の試験(multires_conduction・multires_subcycle・multires_activity など)が確かめる。
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "common/transport_conduction.hlsli"
#include "core/aliases.h"
#include "multires_conduction_scene.h"
#include "reaction_test_table.h"
#include "sim/multires_nest.h"
#include "sim/transport_packet.h"

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

    bool SameScale(TxScale a, TxScale b) {
        return !TxScaleLess(a, b) && !TxScaleLess(b, a);
    }

    // 包み(T-0770): 物質を足す・倍率を掛ける・セルに当てる
    void TestPacket() {
        TxPacket packet = TxMakePacket();
        EXPECT(TxPacketIsEmpty(packet));
        packet = TxPacketWithSpecies(packet, 3, 40);
        packet = TxPacketWithSpecies(packet, 1, 0);  // 0 は足さない
        packet = TxPacketWithSpecies(packet, 1, 10);
        packet.energy = -9;
        packet.carried[0] = 21;
        EXPECT(packet.speciesCount == 2 && !TxPacketIsEmpty(packet));

        const TxPacket half = TxScalePacket(packet, TxSideScale(1, 2));
        EXPECT(half.amounts[0] == 20 && half.amounts[1] == 5 && half.energy == -4 && half.carried[0] == 10);

        // 上流から引き、下流に足す(成分は物質 ID の順・0 の成分は消える)
        reaction::RxCell from = reaction::RxMakeEmptyCell(100);
        from.speciesCount = 2;
        from.species[0] = 1;
        from.amounts[0] = 10;
        from.species[1] = 3;
        from.amounts[1] = 50;
        reaction::RxCell to = reaction::RxMakeEmptyCell(0);
        TxCellChange fromChange;
        TxCellChange toChange;
        AddPacket(fromChange, packet, false);
        AddPacket(toChange, packet, true);
        EXPECT(ApplyCellChange(from, fromChange) && ApplyCellChange(to, toChange));
        EXPECT(from.speciesCount == 1 && from.species[0] == 3 && from.amounts[0] == 10 && from.energy == 109);
        EXPECT(to.speciesCount == 2 && to.species[0] == 1 && to.amounts[0] == 10 && to.species[1] == 3 &&
               to.amounts[1] == 40 && to.energy == -9);
        EXPECT(fromChange.carried[0] == -21 && toChange.carried[0] == 21);
    }
    void TestLimits() {
        EXPECT(TxScaleIsOne(TxSideScale(10, 0)));
        EXPECT(TxScaleIsOne(TxSideScale(10, 10)));
        EXPECT(TxScaleAmount(1000, TxSideScale(5, 10)) == 500);
        EXPECT(TxScaleAmount(1000, TxSideScale(0, 10)) == 0);
        EXPECT(TxScaleFlow(-7, TxScaleOne()) == -7);
        EXPECT(TxScaleIsOne(TxMakeLimit(0, 0, TX_ROOM_UNLIMITED, ~uint64_t{0}).inScale));

        // 倍率は分数のまま = 切り捨ては流れに掛ける 1 回(仮の世界の液体 LqLimitOutflow・気体 GfLimitMovedAmount と同じ式)
        EXPECT(TxScaleAmount(7, TxSideScale(1, 3)) == 2);
        EXPECT(TxScaleAmount(~uint64_t{0}, TxSideScale(~uint64_t{0} - 1, ~uint64_t{0})) == ~uint64_t{0} - 1);

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
            EXPECT(!TxScaleLess(TxScaleOne(), limit.outScale) && TxScaleIsOne(limit.inScale));

            uint64_t scaledTotal = 0;
            for (uint32_t f = 0; f < faces; ++f) {
                const TxScale scale = TxFaceScale(limit, TxNoLimit());
                const int64_t forward = TxScaleFlow(flows[f], scale);
                const int64_t backward = TxScaleFlow(-flows[f], scale);
                EXPECT(forward == -backward);
                EXPECT(forward >= 0 && forward <= flows[f]);
                scaledTotal += static_cast<uint64_t>(forward);

                // 1 回の切り捨て = 流れ × 中身 ÷ 出る合計
                if (outTotal > content)
                    EXPECT(static_cast<uint64_t>(forward) ==
                           FxDivU128By64(FxMulU64Full(static_cast<uint64_t>(flows[f]), content), outTotal).quotient);
            }

            EXPECT(scaledTotal <= content || outTotal <= content);

            // 入る側の倍率と出る側の倍率の小さい方
            const TxLimit target = TxMakeLimit(0, 0, room, outTotal);
            EXPECT(SameScale(TxFaceScale(limit, target),
                             TxScaleLess(target.inScale, limit.outScale) ? target.inScale : limit.outScale));
            EXPECT(SameScale(TxMinLimit(limit, target).outScale,
                             TxScaleLess(target.outScale, limit.outScale) ? target.outScale : limit.outScale));
        }
    }

    // --- 2. 伝導の種類と今の式 ---

    // 1 面の流れだけを読む世界(見立てと粗い隣のレベル)
    struct FaceWorld {
        CvCellView other{};
        int32_t otherLevel = 0;

        [[nodiscard]] CvCellView View(uint32_t /*slot*/, uint32_t /*index*/) const { return other; }
        [[nodiscard]] int32_t Level(uint32_t /*slot*/) const { return otherLevel; }
        // 開いた世界の端の外のセル(T-0721。この試験では端を流さないが、TxFaceFlow が読む約束)
        [[nodiscard]] CvCellView Outside(uint32_t /*row*/, int32_t /*level*/) const { return other; }
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

    // --- 3. 面の並びの当てる段 ---

    // 2 つのセル(枠 0 = 持ち主・枠 1 = 相手)の変化を書き足す世界
    struct PairWorld {
        MrEnergyDelta* deltas = nullptr;  // [2]
        bool otherHasFraction = true;

        [[nodiscard]] bool HasFraction(uint32_t /*slot*/) const { return otherHasFraction; }
        void Add(uint32_t slot, uint32_t /*index*/, const MrEnergyDelta& delta) const {
            deltas[slot] = MrAddEnergyDelta(deltas[slot], delta);
        }
    };

    void TestFaceList() {
        Random random;
        for (uint32_t i = 0; i < 20000; ++i) {
            const auto magnitude = static_cast<int64_t>(random.Below(uint64_t{1} << (random.Below(40) + 1)));
            const int64_t flow = random.Below(2) == 0 ? magnitude : -magnitude;
            const auto gap = static_cast<uint32_t>(random.Below(4));
            std::array<MrEnergyDelta, 2> deltas = {MrMakeEnergyDelta(), MrMakeEnergyDelta()};
            const PairWorld world{.deltas = deltas.data(), .otherHasFraction = random.Below(2) == 0};
            const bool moved = TxApplyFaceFlow(world, 0, 0, 1, 0, gap, flow);
            if (gap == 0) {
                EXPECT(deltas[0].whole == -flow && deltas[0].fraction == 0);
                EXPECT(deltas[1].whole == flow && deltas[1].fraction == 0);
                EXPECT(moved == (flow != 0));
                continue;
            }

            // 細かい側が送った分(細かい単位)= 粗い側が受けた分 × 8^gap(端数の下の桁は 0 = 切り捨てずに全部載る)
            const uint32_t shift = 3 * gap;
            const int64_t sent = -deltas[0].whole;
            const auto coarseInFine = static_cast<int64_t>((static_cast<uint64_t>(deltas[1].whole) << shift) +
                                                           (deltas[1].fraction >> (64 - shift)));
            EXPECT(deltas[0].fraction == 0);
            EXPECT((deltas[1].fraction & ((uint64_t{1} << (64 - shift)) - 1)) == 0);
            EXPECT(sent == coarseInFine);
            EXPECT(world.otherHasFraction ? sent == flow : deltas[1].fraction == 0);
            EXPECT(moved == (sent != 0));
        }

        std::printf("面の並びの当てる段: 20000 面\n");
    }

    // --- 4. 費用 ---
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
    TestPacket();
    TestConductionKind();
    TestFaceList();
    MeasureCost(table);

    if (failureCount != 0) {
        std::printf("transport_apply_test: %d 件失敗\n", failureCount);
        return EXIT_FAILURE;
    }

    std::printf("transport_apply_test: OK\n");

    return EXIT_SUCCESS;
}
