// warm_start_save_test.cpp — C-0 の部品の温めた始まり(前の刻みの答え)を C-1 の温めた始まりの欄に置いた時の確かめと測り
// (T-0922・ADR-0427。20 §10.4・§13.2 の 3・D-608)。CPU だけ。確かめること:
//   1. 気体の陰の圧力(T-0845 の 5 場面・16³ と 32³ の爆発): 欄に置いても答えは控えだけの時と毎刻みビット一致。途中で保存点
//      (気体の箱の写し + 欄を持つ木の SaveNestState)を取り、戻して法則の控えを捨てて続けると、続けた場合と毎刻み HashGas と
//      HashWholeNest が一致(D-608 の (A))。欄を 0 にして続ける(= (B) 真実だけ)と、どこかの刻みでずれる(ずれた刻みを記録)
//   2. 支えの法則(成分を持つ節 k = 3 の 1 つの系・成分ごとに遅らせる形。片持ち梁・洞窟の天井): 1 と同じ
//   3. 測り(ログの表。docs/perf.md へ): 温めた始まりの欄の 1 セルあたりのバイト(k = 1・3)・保存点を取る / 戻す / 状態のハッシュの時間を、
//      欄を持つ木と持たない木(= SF_WARM_START_IN_STATE を切った時に保存点に入る量)で比べた差
// 欄を持つ木は「部品の節 = 木のセル」を置くだけの木(根のブロックを箱に敷き詰め、全部の頁を起こしておく)。本物の世界の気体・支えの欄の
// 置き場(MR の陰の圧力・支えの GPU)はまだ無いので、法則の真実(気体の箱・支えの場面)は木の外に持つ。
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "common/reaction.hlsli"
#include "common/state_field.hlsli"
#include "core/log.h"
#include "core/singleton.h"
#include "gas_projection_scenes.h"
#include "sim/equilibrium_fields.h"
#include "sim/gas_projection.h"
#include "sim/multires_fields.h"
#include "sim/multires_nest.h"
#include "sim/multires_state.h"
#include "sim/support_law.h"
#include "support_scenes.h"

using namespace bicameral;
using namespace bicameral::gas_scenes;

namespace chr = std::chrono;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    // --- 欄を持つ木: 箱 sizeX × sizeY × sizeZ のセルを覆う根のブロック(レベル 0)を敷き詰め、全部を頁にしておく ---
    struct WarmHost {
        sim::MultiresNest nest;
        uint32_t field = sim::MULTIRES_NO_FIELD;
        uint64_t cells = 0;
    };

    WarmHost MakeWarmHost(uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ, uint32_t components, bool withField) {
        constexpr uint32_t EDGE = multires::MR_BLOCK_EDGE;
        const uint32_t blocksX = (sizeX + EDGE - 1) / EDGE;
        const uint32_t blocksY = (sizeY + EDGE - 1) / EDGE;
        const uint32_t blocksZ = (sizeZ + EDGE - 1) / EDGE;
        const uint32_t blocks = blocksX * blocksY * blocksZ;
        uint32_t indexEntries = 1;
        while (indexEntries < 2 * blocks)
            indexEntries *= 2;

        WarmHost host{.nest = sim::MakeMultiresNest({.worldBlocks = blocks,
                                                     .observerBlocks = 0,
                                                     .fractions = 0,
                                                     .pages = blocks,
                                                     .indexEntries = indexEntries,
                                                     .ledgerColumns = 1}),
                      .cells = uint64_t{sizeX} * sizeY * sizeZ};
        if (withField) {
            host.field = sim::RegisterWarmStartField(host.nest, "balance.warm_start", state_field::SF_PHASE_NONE,
                                                     components);
        }

        // セルのエネルギーをセルごとに変え、根を一様にしない(頁を持つ = 起きている)
        std::vector<reaction::RxCell> cells(multires::MR_BLOCK_CELLS);
        for (uint32_t index = 0; index < cells.size(); ++index)
            cells[index] = reaction::RxMakeEmptyCell(index);

        for (uint32_t z = 0; z < blocksZ; ++z) {
            for (uint32_t y = 0; y < blocksY; ++y) {
                for (uint32_t x = 0; x < blocksX; ++x)
                    (void)sim::PlaceRootBlock(host.nest, int64_t{x} * EDGE, int64_t{y} * EDGE, int64_t{z} * EDGE,
                                              cells);
            }
        }

        return host;
    }

    sim::EquilibriumWarmStart WarmOf(WarmHost& host) {
        return {.nest = &host.nest, .field = host.field, .component = 0};
    }

    // 続けた場合と保存点から続けた場合の刻みごとの要約
    struct Hashes {
        std::vector<uint64_t> law;    // 法則の要約(HashGas・SupportLaw::Hash)
        std::vector<uint64_t> whole;  // 欄を持つ木の状態の要約(HashWholeNest。温めた始まりが入る)
    };

    // 保存点から続けた結果の比べ(firstDiff = 法則の要約が最初に違った刻み。無ければ -1)
    int64_t FirstDifference(const std::vector<uint64_t>& reference, const std::vector<uint64_t>& resumed,
                            size_t offset) {
        for (size_t i = 0; i < resumed.size(); ++i) {
            if (reference[offset + i] != resumed[i])
                return (int64_t)(offset + i);
        }

        return -1;
    }

    // --- 1. 気体の陰の圧力 ---
    sim::GasProjectionOptions GasOptions(const sim::EquilibriumWarmStart& warm) {
        sim::GasProjectionOptions options;
        options.pressure.soundSpeedMmPerS = REAL_SOUND;
        options.fixedCycles = true;
        options.solver.cycles = 2;
        options.outerIterations = 1;
        options.equilibrium = true;
        options.warmStart = warm;

        return options;
    }

    void RunGas(sim::GasBox& box, WarmHost& host, const sim::GasProjectionOptions& options, uint32_t from, uint32_t to,
                Hashes& hashes) {
        sim::GasProjection state;  // 法則の控え(保存点に入らない。続きは欄から)
        for (uint32_t tick = from; tick < to; ++tick) {
            (void)sim::StepGasProjection(box, state, options);
            hashes.law.push_back(sim::HashGas(box));
            hashes.whole.push_back(sim::HashWholeNest(host.nest));
        }
    }

    void TestGasSave(Scene scene, uint32_t edge, uint32_t ticks, uint32_t saveTick) {
        const std::string name = std::format("気体 {}³ {}", edge, SceneName(scene));
        WarmHost host = MakeWarmHost(edge, edge, edge, 1, true);
        const sim::GasProjectionOptions options = GasOptions(WarmOf(host));

        // --- 控えだけの道(前の形)と、欄に置いた道を並べる。保存点は saveTick の刻みの前 ---
        sim::GasBox plainBox = MakeScene(scene, edge);
        Hashes plain;
        RunGas(plainBox, host, GasOptions({}), 0, ticks, plain);

        sim::GasBox box = MakeScene(scene, edge);
        Hashes continued;
        RunGas(box, host, options, 0, saveTick, continued);
        const sim::GasBox savedBox = box;
        const sim::MultiresSaveState saved = sim::SaveNestState(host.nest);
        RunGas(box, host, options, saveTick, ticks, continued);
        Expect(continued.law == plain.law, std::format("{}: 欄に置くと答えが変わった", name));

        // --- (A) 保存点から続ける(法則の控えは捨てる)---
        box = savedBox;
        Expect(sim::RestoreNestState(host.nest, saved), std::format("{}: 保存点に戻せない", name));
        Hashes resumed;
        RunGas(box, host, options, saveTick, ticks, resumed);
        const int64_t lawDiff = FirstDifference(continued.law, resumed.law, saveTick);
        const int64_t wholeDiff = FirstDifference(continued.whole, resumed.whole, saveTick);
        Expect(lawDiff < 0 && wholeDiff < 0,
               std::format("{}: 保存点から続けた刻み {} でずれた", name, std::max(lawDiff, wholeDiff)));

        // --- (B) 真実だけ(温めた始まりを 0 にして続ける)---
        box = savedBox;
        (void)sim::RestoreNestState(host.nest, saved);
        sim::ClearFieldStore(host.nest.fields[host.field].store);
        Hashes truthOnly;
        RunGas(box, host, options, saveTick, ticks, truthOnly);
        const int64_t truthDiff = FirstDifference(continued.law, truthOnly.law, saveTick);
        Expect(truthDiff >= 0, std::format("{}: 温めた始まりを 0 にしても答えが変わらない(試験が弱い)", name));
        const bool stillApart = truthOnly.law.back() != continued.law.back();

        Log(Channel::Sim, Level::Info,
            "| T-0922 気体 | {} | {} 刻み・保存点 {} | (A) 欄も保存: 毎刻み一致 {} | (B) 真実だけ: 刻み {} "
            "からずれ・最後も{} |",
            name, ticks, saveTick, lawDiff < 0 && wholeDiff < 0 ? "○" : "×", truthDiff,
            stillApart ? "ずれたまま" : "戻った");
    }

    // --- 2. 支えの法則 ---
    void RunSupport(const sim::SupportScene& scene, const sim::SupportOptions& options, WarmHost& host, uint32_t from,
                    uint32_t to, Hashes& hashes) {
        sim::SupportLaw law(scene, options);  // 法則の控えは捨てる(続きは欄から)
        for (uint32_t tick = from; tick < to; ++tick) {
            law.Step();
            hashes.law.push_back(law.Hash());
            hashes.whole.push_back(sim::HashWholeNest(host.nest));
        }
    }

    void TestSupportSave(std::string_view shape, const sim::SupportScene& scene, sim::SupportSolve solve,
                         uint32_t ticks, uint32_t saveTick) {
        const std::string name = std::format("支え {} {}", shape,
                                             solve == sim::SupportSolve::Coupled ? "1 つの系" : "遅らせる");
        WarmHost host = MakeWarmHost(scene.width, scene.height, 1, sim::SUPPORT_COMPONENTS, true);
        sim::SupportOptions options{.solve = solve};
        options.blocks.cycles = 2;

        // --- 控えだけの道(1 つの法則で続ける)---
        Hashes plain;
        {
            sim::SupportLaw law(scene, options);
            for (uint32_t tick = 0; tick < ticks; ++tick) {
                law.Step();
                plain.law.push_back(law.Hash());
            }
        }

        options.warmStart = WarmOf(host);
        Hashes continued;
        RunSupport(scene, options, host, 0, saveTick, continued);
        const sim::MultiresSaveState saved = sim::SaveNestState(host.nest);
        {
            // 保存点の後も同じ法則の物で続ける(控えと欄の両方が答えを持つ)
            Hashes rest;
            RunSupport(scene, options, host, saveTick, ticks, rest);
            continued.law.insert(continued.law.end(), rest.law.begin(), rest.law.end());
            continued.whole.insert(continued.whole.end(), rest.whole.begin(), rest.whole.end());
        }
        Expect(continued.law == plain.law, std::format("{}: 欄に置くと答えが変わった", name));

        // --- (A) 保存点から ---
        Expect(sim::RestoreNestState(host.nest, saved), std::format("{}: 保存点に戻せない", name));
        Hashes resumed;
        RunSupport(scene, options, host, saveTick, ticks, resumed);
        const int64_t lawDiff = FirstDifference(continued.law, resumed.law, saveTick);
        const int64_t wholeDiff = FirstDifference(continued.whole, resumed.whole, saveTick);
        Expect(lawDiff < 0 && wholeDiff < 0,
               std::format("{}: 保存点から続けた刻み {} でずれた", name, std::max(lawDiff, wholeDiff)));

        // --- (B) 真実だけ ---
        (void)sim::RestoreNestState(host.nest, saved);
        sim::ClearFieldStore(host.nest.fields[host.field].store);
        Hashes truthOnly;
        RunSupport(scene, options, host, saveTick, ticks, truthOnly);
        const int64_t truthDiff = FirstDifference(continued.law, truthOnly.law, saveTick);
        Expect(truthDiff >= 0, std::format("{}: 温めた始まりを 0 にしても答えが変わらない(試験が弱い)", name));

        Log(Channel::Sim, Level::Info,
            "| T-0922 支え | {} | {} 刻み・保存点 {} | (A) 欄も保存: 毎刻み一致 {} | (B) 真実だけ: 刻み {} "
            "からずれ・最後も{} |",
            name, ticks, saveTick, lawDiff < 0 && wholeDiff < 0 ? "○" : "×", truthDiff,
            truthOnly.law.back() != continued.law.back() ? "ずれたまま" : "戻った");
    }

    // --- 3. 測り: 欄を持つ木と持たない木で、保存点・戻す・要約の時間と量の差 ---
    template <typename Work>
    uint64_t MedianNs(uint32_t repeats, Work&& work) {
        std::vector<uint64_t> samples;
        for (uint32_t r = 0; r < repeats; ++r) {
            const auto begin = chr::steady_clock::now();
            work();
            const auto end = chr::steady_clock::now();
            samples.push_back((uint64_t)chr::duration_cast<chr::nanoseconds>(end - begin).count());
        }

        std::ranges::sort(samples);

        return samples[samples.size() / 2];
    }

    struct SaveCost {
        uint64_t saveNs = 0;
        uint64_t restoreNs = 0;
        uint64_t hashNs = 0;
    };

    SaveCost MeasureSave(WarmHost& host, uint32_t repeats) {
        SaveCost cost;
        sim::MultiresSaveState saved = sim::SaveNestState(host.nest);
        cost.saveNs = MedianNs(repeats, [&] { saved = sim::SaveNestState(host.nest); });
        cost.restoreNs = MedianNs(repeats, [&] { (void)sim::RestoreNestState(host.nest, saved); });
        uint64_t sink = 0;
        cost.hashNs = MedianNs(repeats, [&] { sink ^= sim::HashWholeNest(host.nest); });
        Expect(sink != 1, "測り: 要約を使う");

        return cost;
    }

    double PerCell(uint64_t value, uint64_t cells) {
        return (double)value / (double)cells;
    }

    void MeasureSaveCost(uint32_t edge, uint32_t components, uint32_t repeats) {
        WarmHost with = MakeWarmHost(edge, edge, edge, components, true);
        WarmHost without = MakeWarmHost(edge, edge, edge, components, false);

        // 起きている所の全部のセルに 0 でない値(疎でも頁が全部ある = いちばん重い場合)
        std::vector<sim::EquilibriumNode> nodes;
        nodes.reserve(with.cells);
        for (uint32_t z = 0; z < edge; ++z) {
            for (uint32_t y = 0; y < edge; ++y) {
                for (uint32_t x = 0; x < edge; ++x)
                    nodes.push_back({.level = 0, .x = x, .y = y, .z = z, .capacity = 1});
            }
        }

        std::vector<int64_t> values(nodes.size() * components);
        for (size_t i = 0; i < values.size(); ++i)
            values[i] = (int64_t)(i * 2654435761u % 1000003u) - 500000;

        Expect(sim::StoreWarmStart(WarmOf(with), nodes, values, components) == 0, "測り: 全部の節を書ける");
        Expect(sim::LoadWarmStart(WarmOf(with), nodes, components) == values, "測り: 書いた値を読める");

        const SaveCost withField = MeasureSave(with, repeats);
        const SaveCost withoutField = MeasureSave(without, repeats);
        const uint64_t bytes = sim::WarmStartFieldBytes(with.nest, with.field);
        const auto diff = [](uint64_t a, uint64_t b) {
            return a > b ? a - b : 0;
        };
        Log(Channel::Sim, Level::Info,
            "| T-0922 セーブ | {}³ = {} セル・k = {} | 欄 {} B = {:.2f} B/セル | 保存点 {:.2f} → {:.2f} ns/セル(差 "
            "{:.2f})| "
            "戻す {:.2f} → {:.2f}(差 {:.2f})| 要約 {:.2f} → {:.2f}(差 {:.2f})|",
            edge, with.cells, components, bytes, PerCell(bytes, with.cells), PerCell(withoutField.saveNs, with.cells),
            PerCell(withField.saveNs, with.cells), PerCell(diff(withField.saveNs, withoutField.saveNs), with.cells),
            PerCell(withoutField.restoreNs, with.cells), PerCell(withField.restoreNs, with.cells),
            PerCell(diff(withField.restoreNs, withoutField.restoreNs), with.cells),
            PerCell(withoutField.hashNs, with.cells), PerCell(withField.hashNs, with.cells),
            PerCell(diff(withField.hashNs, withoutField.hashNs), with.cells));
    }

    int Run() {
        for (const Scene scene : SCENES)
            TestGasSave(scene, 16, 40, 15);

        TestGasSave(Scene::Blast, 32, 20, 8);

        TestSupportSave("片持ち梁 16 × 4 m", test::Beam(32, 8, 500, false), sim::SupportSolve::Coupled, 60, 20);
        TestSupportSave("洞窟 0.5 m", test::Cave(500), sim::SupportSolve::Coupled, 40, 12);
        TestSupportSave("片持ち梁 16 × 4 m", test::Beam(32, 8, 500, false), sim::SupportSolve::Lagged, 60, 20);

        MeasureSaveCost(64, 1, 9);
        MeasureSaveCost(64, 3, 9);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "warm_start_save_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "warm_start_save_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
