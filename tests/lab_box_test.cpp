// lab_box_test.cpp — 実験室の箱の CPU 側(sim/lab_box。T-0142)を CPU だけで確かめる:
// コマンドで作るセルが MakeReactionCell と同じ・当てられないコマンドは飛ばす・木を置いて火を付けると燃える(セルロースが減り CO2 が出る)・
// コマンドの無い刻みは元素とエネルギーの合計が変わらない・同じコマンドの列なら同じハッシュの列・記録の読み書き・最初に違ったセルを見つける。
// 表を替えた印(T-0218): セルを変えずに箱をつつく・途中で表を替えると替えた刻みから先だけ変わる・印の入った記録(版 3)と版 2 の読み書き。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sim/lab_box.h"
#include "sim/reaction_test_table.h"

namespace {

    using namespace bicameral;
    using namespace bicameral::sim;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr uint32_t IGNITE_MILLIKELVIN = 1500000;
    constexpr uint64_t RUN_TICKS = 300;
    constexpr uint64_t IGNITE_TICK = 3;

    const LabMaterial* FindMaterial(const std::vector<LabMaterial>& materials, std::string_view name) {
        for (const LabMaterial& material : materials) {
            if (material.name == name)
                return &material;
        }

        return nullptr;
    }

    // 箱の中の物質の合計(µmol)
    uint64_t TotalOf(const MultiresNest& nest, uint32_t species) {
        uint64_t total = 0;
        for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; ++index) {
            const reaction::RxCell cell = LabBoxCell(nest, index);
            for (uint32_t i = 0; i < cell.speciesCount; ++i)
                total += cell.species[i] == species ? cell.amounts[i] : 0;
        }

        return total;
    }

    // 木を真ん中の 2×2×2 に置き(刻み 0)、刻み IGNITE_TICK に 1 つに火を付ける実験のコマンド
    std::vector<Command> MakeFireCommands(const std::vector<LabMaterial>& materials) {
        const LabMaterial* wood = FindMaterial(materials, "木");
        std::vector<Command> commands;
        uint32_t sequence = 0;
        for (uint32_t i = 0; i < 8; ++i) {
            const LabCellPosition cell{.x = 3 + (i & 1u), .y = 3 + ((i >> 1) & 1u), .z = 3 + (i >> 2)};
            commands.push_back(MakeLabFillCommand(0, sequence++, cell, wood->contents, 300000));
        }

        commands.push_back(
            MakeLabTemperatureCommand(IGNITE_TICK, sequence++, {.x = 3, .y = 3, .z = 3}, IGNITE_MILLIKELVIN));

        return commands;
    }

    // 木の燃焼を 100 倍速くした表(物質の一覧は同じ。gpu_lab_box_test と同じ形)
    std::expected<BakedReactionTable, std::string> MakeFasterTable() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        for (RuleDefinition& rule : definition.rules) {
            if (rule.name == "cellulose_combustion")
                rule.rate.preExponentialMantissa *= 100;
        }

        return BakeReactionTable(definition);
    }

    std::span<const Command> CommandsOf(const std::vector<Command>& commands, uint64_t tick) {
        const auto first = std::ranges::find_if(commands, [&](const Command& c) { return c.targetTick == tick; });
        const auto last = std::ranges::find_if(first, commands.end(),
                                               [&](const Command& c) { return c.targetTick != tick; });

        return {first, last};
    }

    // 実験を CPU だけで流し、刻みごとのハッシュを返す
    std::vector<uint64_t> RunFire(const BakedReactionTable& table, const std::vector<Command>& commands,
                                  MultiresNest& nest) {
        std::vector<uint64_t> hashes;
        for (uint64_t tick = 0; tick < RUN_TICKS; ++tick) {
            const std::span<const Command> now = CommandsOf(commands, tick);
            const ConservedTotals before = ComputeConservedTotals(nest, table, 0);
            StepLabBox(nest, table, tick, now);
            if (now.empty())
                EXPECT(ComputeConservedTotals(nest, table, 0) ==
                       before);  // 箱は閉じている: コマンドの無い刻みは合計が同じ

            hashes.push_back(HashWholeNest(nest));
        }

        return hashes;
    }

    void TestMakeCell(const BakedReactionTable& table, const std::vector<LabMaterial>& materials) {
        EXPECT(materials.size() == 5);
        for (const LabMaterial& material : materials) {
            for (const uint32_t temperature : {0u, 300000u, 1234567u, 3000000u}) {
                const Command command = MakeLabFillCommand(0, 0, {.x = 1, .y = 2, .z = 3}, material.contents,
                                                           temperature);
                const lab::LabCommand words = ToLabCommand(command);
                EXPECT(lab::LabCommandCell(words, static_cast<uint32_t>(table.species.size())) ==
                       multires::MrCellIndex(1, 2, 3));
                const reaction::RxCell made = lab::LabApplyCommand(table.View(), reaction::RxMakeEmptyCell(0), words);
                const reaction::RxCell expected = MakeReactionCell(table, material.contents,
                                                                   static_cast<int32_t>(temperature));
                EXPECT(reaction::RxSameCell(made, expected));
            }
        }
    }

    void TestInvalidCommands(const BakedReactionTable& table, const std::vector<LabMaterial>& materials) {
        MultiresNest nest = MakeLabBoxNest(table);
        EXPECT(!multires::MrIsUniform(nest.blocks[LAB_BOX_SLOT]));

        const LabMaterial* air = FindMaterial(materials, "空気");
        const std::vector<SpeciesAmount> unknown = {{.species = 0, .amount = 1}};
        const std::vector<SpeciesAmount> tooMany = {{1, 1}, {2, 1}, {3, 1}, {4, 1}};
        const std::vector<SpeciesAmount> twice = {{1, 1}, {1, 2}};
        const std::vector<Command> commands = {
            MakeLabFillCommand(0, 0, {.x = 8, .y = 0, .z = 0}, air->contents, 300000),  // 箱の外
            MakeLabFillCommand(0, 1, {.x = 0, .y = 0, .z = 0}, unknown, 300000),        // 物質 0
            MakeLabFillCommand(0, 2, {.x = 0, .y = 0, .z = 0}, tooMany, 300000),        // 4 つ
            MakeLabFillCommand(0, 3, {.x = 0, .y = 0, .z = 0}, twice, 300000),          // 重なり
            MakeLabTemperatureCommand(0, 4, {.x = 0, .y = 0, .z = 0}, 7000000),         // 熱すぎる
            MakeLabTemperatureCommand(0, 5, {.x = 7, .y = 7, .z = 7}, 400000)};         // これだけ当たる
        const uint64_t before = HashWholeNest(nest);
        EXPECT(ApplyLabCommands(nest, table, commands) == 1);
        EXPECT(HashWholeNest(nest) != before);
        EXPECT(reaction::RxSameCell(LabBoxCell(nest, 0), LabBoxCell(MakeLabBoxNest(table), 0)));
        EXPECT(reaction::RxComputeThermal(table.View(), LabBoxCell(nest, multires::MrCellIndex(7, 7, 7))).temperature >=
               400000);
    }

    void TestFire(const BakedReactionTable& table, const std::vector<LabMaterial>& materials) {
        const std::vector<Command> commands = MakeFireCommands(materials);
        MultiresNest first = MakeLabBoxNest(table);
        MultiresNest second = MakeLabBoxNest(table);
        const std::vector<uint64_t> hashes = RunFire(table, commands, first);
        EXPECT(RunFire(table, commands, second) == hashes);  // 決定性

        const uint32_t cellulose = table.SpeciesId("cellulose");
        const uint32_t carbonDioxide = table.SpeciesId("carbon_dioxide");
        const uint64_t placed = 8 * FindMaterial(materials, "木")->contents[0].amount;
        std::printf("lab_box_test: %llu 刻みでセルロース %llu → %llu µmol、CO2 %llu µmol\n",
                    static_cast<unsigned long long>(RUN_TICKS), static_cast<unsigned long long>(placed),
                    static_cast<unsigned long long>(TotalOf(first, cellulose)),
                    static_cast<unsigned long long>(TotalOf(first, carbonDioxide)));
        EXPECT(TotalOf(first, cellulose) < placed);
        EXPECT(TotalOf(first, carbonDioxide) > 0);

        // --- 記録の読み書き ---
        const LabRecording recording{
            .tableVersion = 0x0123456789ABCDEFULL, .tickCount = RUN_TICKS, .commands = commands, .hashes = hashes};
        std::vector<std::byte> bytes = SerializeLabRecording(recording);
        const auto parsed = ParseLabRecording(bytes);
        EXPECT(parsed.has_value());
        if (parsed) {
            EXPECT(parsed->tableVersion == recording.tableVersion);
            EXPECT(parsed->tickCount == RUN_TICKS);
            EXPECT(parsed->commands == commands);
            EXPECT(parsed->hashes == hashes);
        }

        // 版 1(T-0142。表の版が無い): 見出しの 16 バイト(印・版・種)の後ろの 8 バイトが無い形も読める(表の版は 0)
        std::vector<std::byte> oldBytes(bytes.begin(), bytes.begin() + 16);
        oldBytes.insert(oldBytes.end(), bytes.begin() + 24, bytes.end());
        oldBytes[4] = std::byte{1};
        const auto parsedOld = ParseLabRecording(oldBytes);
        EXPECT(parsedOld.has_value() && parsedOld->tableVersion == 0 && parsedOld->hashes == hashes);

        bytes[0] = std::byte{'X'};
        EXPECT(!ParseLabRecording(bytes).has_value());

        // --- 最初に違ったセル ---
        MultiresNest broken = first;
        EXPECT(!FindLabMismatch(first, broken, 7).has_value());
        const uint32_t index = multires::MrCellIndex(5, 1, 2);
        reaction::RxCell cell = LabBoxCell(broken, index);
        cell.energy += 1;
        broken.cells[broken.blocks.size() + (size_t{broken.blocks[LAB_BOX_SLOT].page} * multires::MR_BLOCK_CELLS) +
                     index] = cell;
        const auto mismatch = FindLabMismatch(first, broken, 7);
        EXPECT(mismatch.has_value() && mismatch->cell == index && mismatch->tick == 7);
        if (mismatch)
            std::printf("lab_box_test: 見つけた食い違い: %s\n", mismatch->what.c_str());
    }

    // 表を替えた印(T-0218・ADR-0055)
    void TestTableMark(const BakedReactionTable& table, const std::vector<LabMaterial>& materials) {
        constexpr uint64_t VERSION = 0x0123456789ABCDEFULL;
        constexpr uint64_t SWAP_TICK = 100;
        const auto faster = MakeFasterTable();
        EXPECT(faster.has_value());
        if (!faster)
            return;

        // --- 印は版を持ち、セルを変えずに箱をつつく ---
        const Command mark = MakeLabTableCommand(SWAP_TICK, 9, VERSION);
        EXPECT(LabTableVersionOf(mark) == VERSION);
        EXPECT(!LabTableVersionOf(MakeLabTemperatureCommand(0, 0, {}, 300000)).has_value());

        MultiresNest nest = MakeLabBoxNest(table);
        const std::vector<reaction::RxCell> cellsBefore = nest.cells;
        EXPECT(ApplyLabCommands(nest, table, std::span(&mark, 1)) == 1);
        EXPECT(nest.blocks[LAB_BOX_SLOT].busyTick == multires::MR_BUSY_POKED);
        const auto sameCell = [](const reaction::RxCell& a, const reaction::RxCell& b) {
            return reaction::RxSameCell(a, b);
        };
        EXPECT(std::ranges::equal(nest.cells, cellsBefore, sameCell));

        // --- 刻み SWAP_TICK の始めに速い表へ: その前は替えない時と同じ、後は違う ---
        std::vector<Command> commands = MakeFireCommands(materials);
        commands.push_back(mark);
        MultiresNest plain = MakeLabBoxNest(table);
        MultiresNest swapped = MakeLabBoxNest(table);
        const std::vector<uint64_t> plainHashes = RunFire(table, MakeFireCommands(materials), plain);
        std::vector<uint64_t> swappedHashes;
        for (uint64_t tick = 0; tick < RUN_TICKS; ++tick) {
            const BakedReactionTable& now = tick < SWAP_TICK ? table : *faster;
            StepLabBox(swapped, now, tick, CommandsOf(commands, tick));
            swappedHashes.push_back(HashWholeNest(swapped));
        }

        const uint32_t cellulose = table.SpeciesId("cellulose");
        std::printf("lab_box_test: 刻み %llu で表を替えた: セルロース %llu µmol(替えない時 %llu)\n",
                    static_cast<unsigned long long>(SWAP_TICK),
                    static_cast<unsigned long long>(TotalOf(swapped, cellulose)),
                    static_cast<unsigned long long>(TotalOf(plain, cellulose)));
        EXPECT(std::ranges::equal(std::span(swappedHashes).first(SWAP_TICK), std::span(plainHashes).first(SWAP_TICK)));
        EXPECT(swappedHashes[SWAP_TICK] != plainHashes[SWAP_TICK]);
        EXPECT(TotalOf(swapped, cellulose) < TotalOf(plain, cellulose));

        // --- 印の入った記録(版 3)と、版 2 の見出しの記録 ---
        const LabRecording recording{
            .tableVersion = 0x1111, .tickCount = RUN_TICKS, .commands = commands, .hashes = swappedHashes};
        std::vector<std::byte> bytes = SerializeLabRecording(recording);
        const auto parsed = ParseLabRecording(bytes);
        EXPECT(parsed.has_value() && parsed->commands == commands && parsed->tableVersion == 0x1111);

        bytes[4] = std::byte{2};
        const auto parsedTwo = ParseLabRecording(bytes);
        EXPECT(parsedTwo.has_value() && parsedTwo->tableVersion == 0x1111);
    }

}  // namespace

int main() {
    const auto table = BakeReactionTable(MakeCombustionTestTable());
    if (!table) {
        std::printf("FAILED: 表をベイクできない(%s)\n", table.error().c_str());
        return 1;
    }

    const std::vector<LabMaterial> materials = MakeLabMaterials(*table);
    TestMakeCell(*table, materials);
    TestInvalidCommands(*table, materials);
    TestFire(*table, materials);
    TestTableMark(*table, materials);

    if (failureCount > 0) {
        std::printf("lab_box_test: %d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("lab_box_test: OK\n");
    return 0;
}
