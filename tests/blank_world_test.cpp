// blank_world_test.cpp — ブランクの世界(D-455・ADR-0151 の 3・T-0371)が本当に空で、何も起きないことを CPU で確かめる。
//
// 確かめること:
//   - ブランクの表(script::LoadBlankReactionTable): 物質は使わない 0 番だけ・規則 0・元素 0・読んだパッケージ 0。
//     版は何度作っても同じで、中身(再生ファイルに残す TableBytes)から同じ版の表に作り直せる(RebuildReactionTable)
//   - ブランクの世界(sim::MakeBlankProbeWorld)を CPU リファレンスで進めても、セル・ハッシュ・エネルギーが刻み 0 のまま。
//     空の表でも、試験の表(法則はあるが物が無い)でも同じ
//   - 再生ファイルが世界の種類(ブランク・サンプル)を持ち、読み書きで変わらない
// GPU の刻みが同じになることは gpu_probe_blank_test。
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "core/aliases.h"
#include "reaction_test_table.h"
#include "save/replay_session.h"
#include "script/reaction_table_loader.h"
#include "sim/probe_sim.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    // CPU リファレンスは全部のセル(64³)を毎刻み計算するので debug では遅い。何も起きないことは数刻みで分かる
    constexpr uint64_t STILL_TICKS = 4;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    bool SameCells(std::span<const reaction::RxCell> left, std::span<const reaction::RxCell> right) {
        return left.size() == right.size() && std::memcmp(left.data(), right.data(), left.size_bytes()) == 0;
    }

    // --- ブランクの表 ---

    void TestBlankTable() {
        const auto blank = script::LoadBlankReactionTable();
        EXPECT(blank.has_value());
        if (!blank)
            return;

        EXPECT(blank->table.species.size() == 1);  // 使わない 0 番だけ
        EXPECT(blank->table.rules.empty());
        EXPECT(blank->table.elementNames.empty());
        EXPECT(blank->table.warnings.empty());
        EXPECT(blank->loadOrder.empty() && blank->rejected.empty() && !blank->modifiedWorld);

        // 版は何度作っても同じ・中身から同じ表に作り直せる(再生ファイルの表。T-0193)
        const auto again = script::LoadBlankReactionTable();
        EXPECT(again.has_value() && again->tableVersion == blank->tableVersion &&
               again->tableBytes == blank->tableBytes);

        const auto rebuilt = script::RebuildReactionTable(blank->tableBytes, blank->tableVersion);
        EXPECT(rebuilt.has_value());
        if (rebuilt) {
            EXPECT(rebuilt->tableVersion == blank->tableVersion);
            EXPECT(rebuilt->table.species.size() == 1 && rebuilt->table.rules.empty());
        }
    }

    // --- ブランクの世界は何も起きない ---

    void TestBlankWorldStill(const BakedReactionTable& table, const char* name) {
        const std::vector<reaction::RxCell> blank = MakeBlankProbeWorld();
        EXPECT(blank.size() == PROBE_CELL_COUNT);

        ProbeReference reference(table, blank);
        const uint64_t initialHash = ProbeStateHash(reference.State(0));
        EXPECT(ProbeEnergySum(reference.State(0)) == 0);

        bool still = true;
        for (uint64_t tick = 0; tick < STILL_TICKS; ++tick) {
            reference.Advance(tick, {});
            const auto state = reference.State(tick + 1);
            still = still && ProbeStateHash(state) == initialHash && ProbeEnergySum(state) == 0;
        }

        EXPECT(still);
        EXPECT(SameCells(reference.State(STILL_TICKS), blank));
        std::printf("  %s: %llu 刻み、ハッシュ %016llx のまま: %s\n", name,
                    static_cast<unsigned long long>(STILL_TICKS), static_cast<unsigned long long>(initialHash),
                    still ? "OK" : "FAILED");
    }

    // --- 再生ファイルの世界の種類 ---

    void TestReplayWorld() {
        for (const save::ReplayWorld world : {save::ReplayWorld::Blank, save::ReplayWorld::Sample}) {
            save::ReplayRecorder recorder;
            recorder.SetWorld(world);
            recorder.AddHash(1, 0x1234);
            const save::ReplayFile built = recorder.Build();
            EXPECT(built.world == world);

            const auto bytes = save::SerializeReplay(built);
            EXPECT(bytes.has_value());
            if (!bytes)
                continue;

            const auto parsed = save::ParseReplay(*bytes);
            EXPECT(parsed.has_value() && parsed->world == world);
        }

        // 既定はサンプルの世界(今までの記録と同じ)
        EXPECT(save::ReplayRecorder{}.Build().world == save::ReplayWorld::Sample);
    }

}  // namespace

int main() {
    TestBlankTable();

    if (const auto blank = script::LoadBlankReactionTable())
        TestBlankWorldStill(blank->table, "空の表");

    if (const auto table = BakeReactionTable(MakeCombustionTestTable()))
        TestBlankWorldStill(*table, "試験の表(法則だけで物が無い)");
    else
        EXPECT(false);

    TestReplayWorld();

    std::printf("blank_world_test: %s\n", failureCount == 0 ? "OK" : "FAILED");

    return failureCount == 0 ? 0 : 1;
}
