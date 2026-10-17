// lab_box_test.cpp — 実験室の箱の CPU 側(sim/lab_box。T-0142)を CPU だけで確かめる:
// コマンドで作るセルが MakeReactionCell と同じ・当てられないコマンドは飛ばす・木を置いて火を付けると燃える(セルロースが減り CO2 が出る)・
// コマンドの無い刻みは元素とエネルギーの合計が変わらない・同じコマンドの列なら同じハッシュの列・記録の読み書き・最初に違ったセルを見つける。
// 表を替えた印(T-0218): セルを変えずに箱をつつく・途中で表を替えると替えた刻みから先だけ変わる・印の入った記録(版 3)と版 2 の読み書き。
// 表の中身(T-0217): 記録の版 4 は使った表の中身を持ち、読み書きで同じ・壊れた中身は断る・版 3 も読める。
// 範囲と量(T-0220): 範囲のコマンドは直方体のセル全部を 1 セルのコマンドと同じに・箱の外や逆の範囲は当てない・
// 分圧と質量から µmol への換算(300 K・1 気圧の 1 セルが空気の材料の量と合う)。
// 計器と比べる段取り(T-0221): 計器の値(置いた量・最高温度)・反応の速さの和が量の差・保存点で切る・A は元の実験と同じ・
// 条件を 1 つ足した B は保存点の刻みから違う・表を替えた記録は CPU の計器で流さない。
// 一部を細かくした箱(T-0314・ADR-0137): 細かくする印で根の一部を 2 段細かくすると、組の数・ブロックの数が見込みどおり・
// 細かいセルは同じ濃度(量の単位 8^-k)・保存量が変わらない・細かいセルに置く/箱のセルに置くと覆う葉全部に・粗い葉の一部には当てない・
// 細かい所の木が燃える・コマンドの無い刻みは保存・枠の無い箱は細かくならない・記録の版 6 は枠の数を持つ。
// 細かくした所の熱(T-0351): 熱い点の広がりの増え方が、陰解法ではレベル 0 の理論の値と小刻み(世界の陽解法)に合い、陰解法なしは遅い。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/multires_conduction.hlsli"
#include "reaction_test_table.h"
#include "sim/lab_box.h"
#include "sim/lab_gauge.h"

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

    // セルの物質の量(µmol)
    uint64_t AmountOf(const reaction::RxCell& cell, uint32_t species) {
        uint64_t total = 0;
        for (uint32_t i = 0; i < cell.speciesCount; ++i)
            total += cell.species[i] == species ? cell.amounts[i] : 0;

        return total;
    }

    // 箱の中の物質の合計(µmol)
    uint64_t TotalOf(const MultiresNest& nest, uint32_t species, lab::LabBoxShape shape = LAB_SINGLE_ROOT_SHAPE) {
        uint64_t total = 0;
        const LabCellRange whole = LabWholeBox(shape);
        for (uint32_t z = 0; z <= whole.high.z; ++z) {
            for (uint32_t y = 0; y <= whole.high.y; ++y) {
                for (uint32_t x = 0; x <= whole.high.x; ++x)
                    total += AmountOf(LabBoxCell(nest, lab::LabCellIndex(x, y, z), shape), species);
            }
        }

        return total;
    }

    // 版 5 の記録のバイト列から箱の形(種の後ろの 16 バイト。T-0173)を抜いて、版 4 までと同じ並びにする(版の語は呼ぶ側が書く)
    std::vector<std::byte> WithoutShape(const std::vector<std::byte>& bytes) {
        constexpr size_t SHAPE_OFFSET = 4 + 4 + 8;
        constexpr size_t SHAPE_BYTES = 16;
        std::vector<std::byte> old(bytes.begin(), bytes.begin() + SHAPE_OFFSET);
        old.insert(old.end(), bytes.begin() + SHAPE_OFFSET + SHAPE_BYTES, bytes.end());

        return old;
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

            // 箱は閉じていて、置く・温度を決めるコマンドは世界の帳簿の「改造」に載る(T-0712): コマンドのある刻みも
            // 「保存量 − 帳簿」が同じ。改造の帳簿はコマンドのある刻みだけ変わる
            EXPECT(ComputeConservedTotals(nest, table, 0) == before);

            hashes.push_back(HashWholeNest(nest));
        }

        return hashes;
    }

    void TestMakeCell(const BakedReactionTable& table, const std::vector<LabMaterial>& materials) {
        EXPECT(materials.size() == 6);  // 魔素(試験)を含む(T-0225)
        EXPECT(FindMaterial(materials, "魔素(試験)") != nullptr);
        for (const LabMaterial& material : materials) {
            for (const uint32_t temperature : {0u, 300000u, 1234567u, 3000000u}) {
                const Command command = MakeLabFillCommand(0, 0, {.x = 1, .y = 2, .z = 3}, material.contents,
                                                           temperature);
                const lab::LabCommand words = ToLabCommand(command);
                EXPECT(lab::LabCommandCell(words, static_cast<uint32_t>(table.species.size()), LAB_SINGLE_ROOT_SHAPE) ==
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

        // 版 1(T-0142。表の版が無い): 見出しの 16 バイト(印・版・種)の後ろの 8 バイトが無い形も読める(表の版は 0)。
        // 版 4 の末尾の表の数(8 バイト。表は無い)は外す
        const std::vector<std::byte> unshaped = WithoutShape(bytes);
        std::vector<std::byte> oldBytes(unshaped.begin(), unshaped.begin() + 16);
        oldBytes.insert(oldBytes.end(), unshaped.begin() + 24, unshaped.end() - 8);
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

    // 範囲のコマンド(T-0220): 範囲の中は 1 セルのコマンドを当てたのと同じ・外は変わらない・当てないものは飛ばす
    void TestRegion(const BakedReactionTable& table, const std::vector<LabMaterial>& materials) {
        const LabMaterial* wood = FindMaterial(materials, "木");
        const LabCellRange range = {.low = {.x = 1, .y = 2, .z = 3}, .high = {.x = 4, .y = 2, .z = 6}};
        MultiresNest regionNest = MakeLabBoxNest(table);
        MultiresNest cellNest = MakeLabBoxNest(table);
        const std::vector<Command> region = {MakeLabFillRegionCommand(0, 0, range, wood->contents, 450000),
                                             MakeLabTemperatureRegionCommand(0, 1, LabWholeBox(), 350000)};
        std::vector<Command> cells;
        for (uint32_t z = 3; z <= 6; ++z) {
            for (uint32_t x = 1; x <= 4; ++x)
                cells.push_back(MakeLabFillCommand(0, 0, {.x = x, .y = 2, .z = z}, wood->contents, 450000));
        }

        for (uint32_t z = 0; z < LAB_BOX_EDGE; ++z) {
            for (uint32_t y = 0; y < LAB_BOX_EDGE; ++y) {
                for (uint32_t x = 0; x < LAB_BOX_EDGE; ++x)
                    cells.push_back(MakeLabTemperatureCommand(0, 1, {.x = x, .y = y, .z = z}, 350000));
            }
        }

        EXPECT(ApplyLabCommands(regionNest, table, region) == 2);
        ApplyLabCommands(cellNest, table, cells);
        EXPECT(HashWholeNest(regionNest) == HashWholeNest(cellNest));
        EXPECT(TotalOf(regionNest, table.SpeciesId("cellulose")) == 16 * wood->contents[0].amount);

        // --- 当てないもの: 箱の外・逆の範囲・熱すぎる ---
        const std::vector<Command> invalid = {
            MakeLabFillRegionCommand(0, 0, {.low = {}, .high = {.x = 8, .y = 0, .z = 0}}, wood->contents, 300000),
            MakeLabFillRegionCommand(0, 1, {.low = {.x = 2}, .high = {.x = 1}}, wood->contents, 300000),
            MakeLabTemperatureRegionCommand(0, 2, LabWholeBox(), 7000000)};
        MultiresNest untouched = MakeLabBoxNest(table);
        const uint64_t before = HashWholeNest(untouched);
        EXPECT(ApplyLabCommands(untouched, table, invalid) == 0);
        EXPECT(HashWholeNest(untouched) == before);
    }

    // 量の換算(T-0220)
    void TestConversions(const BakedReactionTable& table, const std::vector<LabMaterial>& materials) {
        // 1 気圧・300 K の 1 セル = 約 5.08 mol(空気の材料の合計と 0.1% 以内)
        const LabMaterial* air = FindMaterial(materials, "空気");
        const uint64_t airTotal = air->contents[0].amount + air->contents[1].amount;
        const uint64_t atmosphere = LabGasMicromoles(101325, 300000);
        std::printf("lab_box_test: 1 気圧・300 K の 1 セル = %llu µmol(空気の材料 %llu)\n",
                    static_cast<unsigned long long>(atmosphere), static_cast<unsigned long long>(airTotal));
        EXPECT(atmosphere > airTotal - (airTotal / 1000) && atmosphere < airTotal + (airTotal / 1000));
        EXPECT(LabGasMicromoles(202650, 600000) == atmosphere);  // 圧力と温度を一緒に 2 倍
        EXPECT(LabGasMicromoles(101325, 0) == 0 && LabGasMicromoles(LAB_MAX_PRESSURE_PASCAL + 1, 300000) == 0);
        EXPECT(LabGasMicromoles(LAB_MAX_PRESSURE_PASCAL, 1) > 0);

        // セルロース 162.14 g = 1 mol(物質の分子量は表から)
        const uint32_t cellulose = table.SpeciesId("cellulose");
        const uint32_t molarMass = table.molarMasses[cellulose];
        EXPECT(LabMassMicromoles(molarMass, molarMass) == 1000000);
        EXPECT(LabMassMicromoles(1, 0) == 0 && LabMassMicromoles(LAB_MAX_MASS_MILLIGRAMS + 1, molarMass) == 0);
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

        // 版 3・2 の形(表の中身が無い)も読める(版 4 の末尾の表の数 8 バイトを外す)
        const std::vector<std::byte> unshaped = WithoutShape(bytes);
        std::vector<std::byte> oldBytes(unshaped.begin(), unshaped.end() - 8);
        oldBytes[4] = std::byte{3};
        const auto parsedThree = ParseLabRecording(oldBytes);
        EXPECT(parsedThree.has_value() && parsedThree->commands == commands && parsedThree->tables.empty());
        oldBytes[4] = std::byte{2};
        const auto parsedTwo = ParseLabRecording(oldBytes);
        EXPECT(parsedTwo.has_value() && parsedTwo->tableVersion == 0x1111);
        EXPECT(!ParseLabRecording(std::span(bytes).first(bytes.size() - 8)).has_value());  // 版 4 で表の数が無い
    }

    // 表の中身(T-0217): 記録の版 4 は使った表の中身(バイト列のまま)を持つ
    void TestTableContents() {
        const LabRecording recording{.tableVersion = 0x1111,
                                     .tickCount = 2,
                                     .commands = {MakeLabTableCommand(1, 0, 0x2222)},
                                     .hashes = {7, 8},
                                     .tables = {{.version = 0x1111, .bytes = "first"},
                                                {.version = 0x2222, .bytes = std::string("swap\0ped table", 14)}}};
        std::vector<std::byte> bytes = SerializeLabRecording(recording);
        EXPECT(bytes.size() % sizeof(uint64_t) == 0);
        const auto parsed = ParseLabRecording(bytes);
        EXPECT(parsed.has_value() && parsed->tables.size() == 2);
        if (parsed && parsed->tables.size() == 2) {
            EXPECT(parsed->tables[0].version == 0x1111 && parsed->tables[0].bytes == "first");
            EXPECT(parsed->tables[1].version == 0x2222 && parsed->tables[1].bytes == recording.tables[1].bytes);
            EXPECT(parsed->commands == recording.commands && parsed->hashes == recording.hashes);
        }

        // --- 壊れた中身: 末尾が足りない・余る・版の並びが逆 ---
        EXPECT(!ParseLabRecording(std::span(bytes).first(bytes.size() - 8)).has_value());
        std::vector<std::byte> longer = bytes;
        longer.resize(bytes.size() + 8, std::byte{0});
        EXPECT(!ParseLabRecording(longer).has_value());

        LabRecording reversed = recording;
        std::ranges::reverse(reversed.tables);
        EXPECT(!ParseLabRecording(SerializeLabRecording(reversed)).has_value());
    }

    // 計器と比べる段取り(T-0221。CPU だけ。GPU の箱で流すのは window_lab_compare)
    void TestGauge(const BakedReactionTable& table, const std::vector<LabMaterial>& materials) {
        constexpr uint64_t GAUGE_TICKS = RUN_TICKS;  // 60 刻みではまだ CO2 が出ない(待ちの丸め)
        constexpr uint64_t SAVE_POINT_TICK = 2;
        const LabCellPosition cell{.x = 3, .y = 3, .z = 3};
        const LabRecording recording{.tickCount = GAUGE_TICKS, .commands = MakeFireCommands(materials)};
        const auto samples = RunLabGaugeOnCpu(recording, table, cell.Index());
        EXPECT(samples.has_value() && samples->size() == GAUGE_TICKS);
        if (!samples || samples->size() != GAUGE_TICKS)
            return;

        // --- 置いた量・火を付けた温度 ---
        const uint32_t cellulose = table.SpeciesId("cellulose");
        const uint32_t carbonDioxide = table.SpeciesId("carbon_dioxide");
        const uint64_t wood = FindMaterial(materials, "木")->contents[0].amount;
        EXPECT(samples->front().cellAmounts[cellulose] == wood);
        EXPECT(samples->front().boxAmounts[cellulose] == 8 * wood);
        EXPECT(samples->front().maxTemperatureMilliKelvin < 400000);
        EXPECT((*samples)[IGNITE_TICK].maxTemperatureMilliKelvin >= 1000000);

        // --- 反応の速さを足すと量の差になる ---
        int64_t produced = 0;
        for (size_t i = 0; i < samples->size(); ++i)
            produced += LabGaugeRate(*samples, i, carbonDioxide, true);

        EXPECT(LabGaugeRate(*samples, 0, carbonDioxide, true) == 0);
        EXPECT(produced == static_cast<int64_t>(samples->back().boxAmounts[carbonDioxide]) -
                               static_cast<int64_t>(samples->front().boxAmounts[carbonDioxide]));
        EXPECT(produced > 0);

        // --- 保存点で切る・A は元の実験と同じ・B(隣の木を熱くする)は保存点の刻みから違う ---
        const LabRecording cut = CutLabRecording(recording, SAVE_POINT_TICK);
        EXPECT(cut.tickCount == SAVE_POINT_TICK && cut.commands.size() == 8);
        const Command change = MakeLabTemperatureCommand(0, 0, {.x = 4, .y = 3, .z = 3}, IGNITE_MILLIKELVIN);
        const LabComparisonPlan plan = MakeLabComparisonPlan(recording, SAVE_POINT_TICK, 30, change);
        EXPECT(plan.continued.size() == 1 && plan.change.targetTick == SAVE_POINT_TICK && plan.change.sequence == 9);

        const auto a = RunLabGaugeOnCpu(MakeLabBranchRecording(plan, false), table, cell.Index());
        const auto b = RunLabGaugeOnCpu(MakeLabBranchRecording(plan, true), table, cell.Index());
        EXPECT(a.has_value() && b.has_value());
        if (!a || !b)
            return;

        EXPECT(a->size() == SAVE_POINT_TICK + 30 && std::ranges::equal(*a, std::span(*samples).first(a->size())));
        EXPECT(FirstGaugeDifference(*a, *b) == SAVE_POINT_TICK);

        // --- 表を替えた記録は CPU の計器で流さない ---
        LabRecording marked = recording;
        marked.commands.push_back(MakeLabTableCommand(5, 100, 7));
        EXPECT(!RunLabGaugeOnCpu(marked, table, cell.Index()).has_value());
    }

    // 根を並べた箱(T-0173): 根の枠の順・根をまたぐ範囲 = 1 セルずつ・形の外は当てない・根の境を熱が通る・記録の形
    void TestLargeBox(const BakedReactionTable& table, const std::vector<LabMaterial>& materials) {
        constexpr lab::LabBoxShape SHAPE = {.rootsX = 2, .rootsY = 2, .rootsZ = 2};
        const LabMaterial* wood = FindMaterial(materials, "木");

        // --- 根 1 つの箱のセルの番号は今までと同じ(古い記録のコマンドがそのまま読める)---
        EXPECT((LabCellPosition{.x = 7, .y = 5, .z = 3}.Index() == multires::MrCellIndex(7, 5, 3)));
        EXPECT(LabCellPositionOf(lab::LabCellIndex(9, 17, 31)).x == 9 &&
               LabCellPositionOf(lab::LabCellIndex(9, 17, 31)).y == 17 &&
               LabCellPositionOf(lab::LabCellIndex(9, 17, 31)).z == 31);

        // --- 根は x → y → z の順に枠 0 から・どれも頁を持つ・原点 = 根の座標 × 8 ---
        MultiresNest nest = MakeLabBoxNest(table, SHAPE);
        EXPECT(nest.capacity.worldBlocks == 8 && nest.capacity.indexEntries == 16);
        for (uint32_t slot = 0; slot < 8; ++slot) {
            EXPECT(!multires::MrIsUniform(nest.blocks[slot]));
            EXPECT(nest.blocks[slot].originX == int64_t{(slot & 1u) * LAB_BOX_EDGE});
            EXPECT(nest.blocks[slot].originY == int64_t{((slot >> 1) & 1u) * LAB_BOX_EDGE});
            EXPECT(nest.blocks[slot].originZ == int64_t{(slot >> 2) * LAB_BOX_EDGE});
        }

        // --- 根をまたぐ範囲 = 1 セルずつ並べたコマンド ---
        const LabCellRange range = {.low = {.x = 6, .y = 7, .z = 5}, .high = {.x = 9, .y = 8, .z = 10}};
        MultiresNest regionNest = MakeLabBoxNest(table, SHAPE);
        MultiresNest cellNest = MakeLabBoxNest(table, SHAPE);
        const std::vector<Command> region = {MakeLabFillRegionCommand(0, 0, range, wood->contents, 450000)};
        std::vector<Command> cells;
        for (uint32_t z = range.low.z; z <= range.high.z; ++z) {
            for (uint32_t y = range.low.y; y <= range.high.y; ++y) {
                for (uint32_t x = range.low.x; x <= range.high.x; ++x)
                    cells.push_back(MakeLabFillCommand(0, 0, {.x = x, .y = y, .z = z}, wood->contents, 450000));
            }
        }

        EXPECT(ApplyLabCommands(regionNest, table, region, SHAPE) == 1);
        EXPECT(ApplyLabCommands(cellNest, table, cells, SHAPE) == cells.size());
        EXPECT(HashWholeNest(regionNest) == HashWholeNest(cellNest));
        EXPECT(TotalOf(regionNest, table.SpeciesId("cellulose"), SHAPE) == 48 * wood->contents[0].amount);

        // --- 形の外(x = 16)は当てない・箱全体の範囲は反対の隅の根まで届く ---
        MultiresNest edge = MakeLabBoxNest(table, SHAPE);
        const std::vector<Command> outside = {
            MakeLabFillCommand(0, 0, {.x = 16, .y = 0, .z = 0}, wood->contents, 300000),
            MakeLabFillRegionCommand(0, 1, {.low = {}, .high = {.x = 16, .y = 0, .z = 0}}, wood->contents, 300000),
            MakeLabTemperatureRegionCommand(0, 2, LabWholeBox(SHAPE), 350000)};
        EXPECT(ApplyLabCommands(edge, table, outside, SHAPE) == 1);
        const reaction::RxCell corner = LabBoxCell(edge, LabWholeBox(SHAPE).high.Index(), SHAPE);
        EXPECT(reaction::RxComputeThermal(table.View(), corner).temperature >= 350000);
        EXPECT(TotalOf(edge, table.SpeciesId("cellulose"), SHAPE) == 0);

        // --- 根の境を熱が通る: (7, 3, 3) を熱くすると隣の根の (8, 3, 3) のエネルギーが増える。コマンドの無い刻みは合計が同じ ---
        MultiresNest heat = MakeLabBoxNest(table, SHAPE);
        const uint32_t neighbor = LabCellPosition{.x = 8, .y = 3, .z = 3}.Index();
        const int64_t neighborBefore = LabBoxCell(heat, neighbor, SHAPE).energy;
        const std::vector<Command> hot = {MakeLabTemperatureCommand(0, 0, {.x = 7, .y = 3, .z = 3}, 1000000)};
        StepLabBox(heat, table, 0, hot, SHAPE);
        const ConservedTotals before = ComputeConservedTotals(heat, table, 0);
        for (uint64_t tick = 1; tick < 40; ++tick)
            StepLabBox(heat, table, tick, {}, SHAPE);

        EXPECT(ComputeConservedTotals(heat, table, 0) == before);
        EXPECT(LabBoxCell(heat, neighbor, SHAPE).energy > neighborBefore);

        // --- 記録は箱の形を持つ(版 5)。違う形の箱の再生は LabSession が断る(gpu_lab_box_test)---
        const LabRecording recording{.shape = SHAPE, .tickCount = 1, .commands = hot, .hashes = {HashWholeNest(heat)}};
        const auto parsed = ParseLabRecording(SerializeLabRecording(recording));
        EXPECT(parsed.has_value() && SameLabShape(parsed->shape, SHAPE));
        LabRecording broken = recording;
        broken.shape.rootsY = 5;
        EXPECT(!ParseLabRecording(SerializeLabRecording(broken)).has_value());

        // --- 計器は箱全体を数え、見るセルは根をまたいでも引ける ---
        const LabGaugeSample sample = SampleLabGauge(regionNest, table, 0, lab::LabCellIndex(9, 8, 10), SHAPE);
        EXPECT(sample.boxAmounts[table.SpeciesId("cellulose")] == 48 * wood->contents[0].amount);
        EXPECT(sample.cellAmounts[table.SpeciesId("cellulose")] == wood->contents[0].amount);
    }

    // --- 一部を細かくした箱(T-0314)---
    void TestRefinedBox(const BakedReactionTable& table, const std::vector<LabMaterial>& materials) {
        constexpr lab::LabBoxShape SHAPE = {.rootsX = 2, .rootsY = 1, .rootsZ = 1, .refineBlocks = 128};
        const LabMaterial* wood = FindMaterial(materials, "木");
        const LabMaterial* charcoal = FindMaterial(materials, "木炭");
        const uint32_t oxygen = table.SpeciesId("oxygen");
        const uint32_t cellulose = table.SpeciesId("cellulose");
        constexpr int32_t FINEST = 2;

        // --- 根 0 の (2..5)³ を 2 段: レベル 1 は根 0 の 8 つ全部、レベル 2 は (1..2)³ の 8 つ。組は段ごとに 8 = 16 ---
        MultiresNest nest = MakeLabBoxNest(table, SHAPE);
        EXPECT(nest.capacity.worldBlocks == 2 + 128 && nest.capacity.pages == 2 + 128);
        const ConservedTotals initial = ComputeConservedTotals(nest, table, FINEST);
        const LabCellRange inner = {.low = {.x = 2, .y = 2, .z = 2}, .high = {.x = 5, .y = 5, .z = 5}};
        const std::vector<Command> refine = {MakeLabRefineCommand(0, 0, inner, 2)};
        EXPECT(LabRefineRounds(refine, SHAPE).size() == 16);
        StepLabBox(nest, table, 0, refine, SHAPE);
        EXPECT(LabRefinedBlockCount(nest) == 16);
        EXPECT(ComputeConservedTotals(nest, table, FINEST) == initial);

        // --- 細かいセルは同じ濃度(量の数が箱のセルと同じ)。粗い葉の所(レベル 2 の (0, 0, 0))はレベル 2 のセルが無い ---
        const reaction::RxCell coarse = LabBoxCell(nest, lab::LabCellIndex(12, 0, 0), SHAPE);
        const auto fine = LabBoxFineCell(nest, {.x = 9, .y = 9, .z = 9, .level = 2});
        EXPECT(fine.has_value() && AmountOf(*fine, oxygen) == AmountOf(coarse, oxygen));
        EXPECT(!LabBoxFineCell(nest, {.x = 0, .y = 0, .z = 0, .level = 2}).has_value());
        EXPECT(LabBoxFineCell(nest, {.x = 0, .y = 0, .z = 0, .level = 1}).has_value());
        EXPECT(!LabBoxFineCell(nest, {.x = 4, .y = 4, .z = 4, .level = 1}).has_value());  // 覆われている

        // --- 細かいセル 1 つに置く・箱のセル (3, 3, 3) に置くとレベル 2 の 4³ 全部・粗い葉の一部(レベル 2 の (0, 0, 0))には当てない ---
        const std::vector<Command> place = {
            MakeLabFillCommand(1, 0, {.x = 9, .y = 9, .z = 9, .level = 2}, wood->contents, 300000),
            MakeLabFillCommand(1, 1, {.x = 3, .y = 3, .z = 3}, charcoal->contents, 300000),
            MakeLabFillCommand(1, 2, {.x = 0, .y = 0, .z = 0, .level = 2}, charcoal->contents, 300000),
            MakeLabFillCommand(1, 3, {.x = 0, .y = 0, .z = 0, .level = 3}, charcoal->contents, 300000)};
        MultiresNest placed = nest;
        EXPECT(ApplyLabCommands(placed, table, place, SHAPE) == 3);
        const auto woodCell = LabBoxFineCell(placed, {.x = 9, .y = 9, .z = 9, .level = 2});
        EXPECT(woodCell.has_value() && AmountOf(*woodCell, cellulose) == wood->contents[0].amount);
        const uint32_t carbon = table.SpeciesId("carbon");
        uint32_t charcoalCells = 0;
        for (uint32_t z = 12; z < 16; ++z) {
            for (uint32_t y = 12; y < 16; ++y) {
                for (uint32_t x = 12; x < 16; ++x) {
                    const auto cell = LabBoxFineCell(placed, {.x = x, .y = y, .z = z, .level = 2});
                    charcoalCells += cell.has_value() && AmountOf(*cell, carbon) == charcoal->contents[0].amount ? 1
                                                                                                                 : 0;
                }
            }
        }

        EXPECT(charcoalCells == 64);
        const auto untouched = LabBoxFineCell(placed, {.x = 0, .y = 0, .z = 0, .level = 1});
        EXPECT(untouched.has_value() && AmountOf(*untouched, carbon) == 0);

        // --- 細かい所の木に火を付けると燃える。コマンドの無い刻みは保存量が変わらない ---
        MultiresNest burn = nest;
        StepLabBox(burn, table, 1, std::span(place).first(1), SHAPE);
        const std::vector<Command> ignite = {
            MakeLabTemperatureCommand(2, 0, {.x = 9, .y = 9, .z = 9, .level = 2}, IGNITE_MILLIKELVIN)};
        StepLabBox(burn, table, 2, ignite, SHAPE);
        const ConservedTotals lit = ComputeConservedTotals(burn, table, FINEST);
        for (uint64_t tick = 3; tick < 80; ++tick)
            StepLabBox(burn, table, tick, {}, SHAPE);

        EXPECT(ComputeConservedTotals(burn, table, FINEST) == lit);
        EXPECT(ComputeConservedTotals(burn, table, FINEST) == initial);  // 細かくする・置く・火を付けるを通して(T-0712)
        const auto burnt = LabBoxFineCell(burn, {.x = 9, .y = 9, .z = 9, .level = 2});
        EXPECT(burnt.has_value() && AmountOf(*burnt, cellulose) < wood->contents[0].amount);

        // --- 同じ列なら同じハッシュ(決定的)---
        MultiresNest again = MakeLabBoxNest(table, SHAPE);
        StepLabBox(again, table, 0, refine, SHAPE);
        StepLabBox(again, table, 1, std::span(place).first(1), SHAPE);
        StepLabBox(again, table, 2, ignite, SHAPE);
        for (uint64_t tick = 3; tick < 80; ++tick)
            StepLabBox(again, table, tick, {}, SHAPE);

        EXPECT(HashWholeNest(again) == HashWholeNest(burn));
        EXPECT(!FindLabMismatch(again, burn, 79, SHAPE).has_value());

        // --- 当てられない印(段 0・3・箱の外)は組にならない。枠の無い箱は細かくならない(枠が足りない = NO_SPACE)---
        const std::vector<Command> invalid = {
            MakeLabRefineCommand(0, 0, inner, 0), MakeLabRefineCommand(0, 1, inner, 3),
            MakeLabRefineCommand(0, 2, {.low = {}, .high = {.x = 16, .y = 0, .z = 0}}, 1)};
        EXPECT(LabRefineRounds(invalid, SHAPE).empty());
        constexpr lab::LabBoxShape FIXED = {.rootsX = 2, .rootsY = 1, .rootsZ = 1};
        MultiresNest fixed = MakeLabBoxNest(table, FIXED);
        EXPECT(ApplyLabRefines(fixed, refine, FIXED) == 16 && LabRefinedBlockCount(fixed) == 0);

        // --- 違うセルを見つける: 細かいセルを 1 つ変えると、その細かいセルの語 ---
        MultiresNest changed = burn;
        const Command warm = MakeLabTemperatureCommand(79, 0, {.x = 9, .y = 9, .z = 9, .level = 2}, 400000);
        ApplyLabCommands(changed, table, std::span(&warm, 1), SHAPE);
        const auto mismatch = FindLabMismatch(burn, changed, 79, SHAPE);
        EXPECT(mismatch.has_value() && lab::LabCellWordLevel(mismatch->cell) == 2);

        // --- 記録の版 6 は細かくする枠の数を持つ。版 5 で枠の数が 0 でなければ壊れている ---
        const LabRecording recording{
            .shape = SHAPE, .tickCount = 1, .commands = refine, .hashes = {HashWholeNest(nest)}};
        std::vector<std::byte> bytes = SerializeLabRecording(recording);
        const auto parsed = ParseLabRecording(bytes);
        EXPECT(parsed.has_value() && SameLabShape(parsed->shape, SHAPE) && parsed->commands.size() == 1);
        EXPECT(bytes[4] == std::byte{6});
        bytes[4] = std::byte{5};
        EXPECT(!ParseLabRecording(bytes).has_value());
        EXPECT(!SameLabShape(SHAPE, FIXED));
    }

    // --- 細かくした所の熱の速さ(T-0351・ADR-0137 追記 2): 熱い点の超過エネルギーの 2 次のモーメント(広がりの分散)は、拡散なら
    //     1 刻みに 2 × α × Δt(軸ごと)ずつ増える。陽解法でも後退 Euler でも、頭打ちでなければこの増え方は離散の式のまま正確
    //     (Σ x² L g = 2 Σ g)なので、時間の刻み方によらず「熱の伝わる速さ」を測れる。レベル 0(頭打ちでない)の箱の増え方を理論の値とし、
    //     2 段細かくした所の熱い点の増え方を 3 つの刻み方で比べる: 陰解法(LabStepOptions)・小刻み(方式①。世界の陽解法の本当の速さ)・
    //     旧(陰解法なし = Q40 の A。頭打ちで遅い)。頭打ちが起きるよう、空気を 1/11 に薄くする(薄い気体は熱の拡散が速い)。
    //     T-0597 で気体の比熱が定積(N2 29.1 → 20.8)になり、1/16 ではレベル 0 まで頭打ちになったので、拡散の速さが前と同じくらい
    //     (16 × 20.8 ÷ 29.1 ≒ 11)に戻した
    constexpr uint32_t HEAT_THINNING = 11;
    constexpr uint32_t HEAT_HOT_MILLIKELVIN = 600000;

    // 箱を薄い空気で満たすコマンド(範囲は箱全部)
    Command MakeThinAirFill(const BakedReactionTable& table, const MultiresNest& nest, lab::LabBoxShape shape,
                            uint32_t sequence) {
        const reaction::RxCell air = LabBoxCell(nest, lab::LabCellIndex(0, 0, 0), shape);
        const uint32_t oxygen = table.SpeciesId("oxygen");
        const uint32_t nitrogen = table.SpeciesId("nitrogen");
        const std::array<SpeciesAmount, 2> thin = {
            SpeciesAmount{.species = oxygen, .amount = AmountOf(air, oxygen) / HEAT_THINNING},
            SpeciesAmount{.species = nitrogen, .amount = AmountOf(air, nitrogen) / HEAT_THINNING}};

        return MakeLabFillRegionCommand(0, sequence, LabWholeBox(shape), thin, table.templates.temperatureMilliKelvin);
    }

    // レベル level の辺 edge セルの格子で、熱い点 hot からの超過エネルギーの 2 次のモーメント(そのレベルのセルの辺の 2 乗。
    // 背景は熱い点から最も遠い角のセル)。cellAt はそのレベルのセル
    template <typename CellAt>
    double HeatSpread(uint32_t edge, LabCellPosition hot, CellAt cellAt) {
        const int64_t background = cellAt(0, 0, 0).energy;
        double weight = 0;
        double moment = 0;
        for (uint32_t z = 0; z < edge; ++z) {
            for (uint32_t y = 0; y < edge; ++y) {
                for (uint32_t x = 0; x < edge; ++x) {
                    const auto excess = static_cast<double>(cellAt(x, y, z).energy - background);
                    const auto dx = static_cast<double>(x) - hot.x;
                    const auto dy = static_cast<double>(y) - hot.y;
                    const auto dz = static_cast<double>(z) - hot.z;
                    weight += excess;
                    moment += excess * ((dx * dx) + (dy * dy) + (dz * dz));
                }
            }
        }

        return weight > 0 ? moment / weight : 0;
    }

    // 理論の値: 細かくしない 3×3×3 の根の箱(レベル 0 は頭打ちでない)の中央の熱い点の広がりの、1 刻みの増え(レベル 0 のセルの辺の 2 乗)
    double LevelZeroSpreadRate(const BakedReactionTable& table, uint64_t ticks) {
        constexpr lab::LabBoxShape SHAPE = {.rootsX = 3, .rootsY = 3, .rootsZ = 3, .refineBlocks = 0};
        constexpr LabCellPosition HOT = {.x = 12, .y = 12, .z = 12};
        MultiresNest nest = MakeLabBoxNest(table, SHAPE);
        const std::vector<Command> setup = {MakeThinAirFill(table, nest, SHAPE, 0),
                                            MakeLabTemperatureCommand(0, 1, HOT, HEAT_HOT_MILLIKELVIN)};
        EXPECT(ApplyLabCommands(nest, table, setup, SHAPE) == 2);
        const auto spread = [&] {
            return HeatSpread(LAB_BOX_EDGE * 3, HOT, [&](uint32_t x, uint32_t y, uint32_t z) {
                return LabBoxCell(nest, lab::LabCellIndex(x, y, z), SHAPE);
            });
        };
        const double before = spread();
        for (uint64_t tick = 0; tick < ticks; ++tick)
            StepNest(nest, table, LAB_WORLD_SEED, tick, LabStepOptions(SHAPE));

        return (spread() - before) / static_cast<double>(ticks);
    }

    void TestRefinedHeatSpeed(const BakedReactionTable& table, const std::vector<LabMaterial>& materials) {
        constexpr lab::LabBoxShape SHAPE = {
            .rootsX = 1, .rootsY = 1, .rootsZ = 1, .refineBlocks = LAB_DEFAULT_REFINE_BLOCKS};
        constexpr uint32_t FINE_EDGE = LAB_BOX_EDGE * 4;
        constexpr LabCellPosition HOT = {.x = 16, .y = 16, .z = 16, .level = 2};
        constexpr uint64_t TICKS = 4;
        constexpr double FINE_TO_ROOT = 16;  // レベル 2 のセルの辺の 2 乗 → レベル 0

        // --- 箱全部を 2 段細かくして薄い空気で満たし、細かいセル 1 つを熱くする ---
        MultiresNest nest = MakeLabBoxNest(table, SHAPE);
        const std::vector<Command> setup = {MakeLabRefineCommand(0, 0, LabWholeBox(SHAPE), 2),
                                            MakeThinAirFill(table, nest, SHAPE, 1)};
        StepLabBox(nest, table, 0, setup, SHAPE);
        EXPECT(LabRefinedBlockCount(nest) == 72);
        const Command hot = MakeLabTemperatureCommand(1, 0, HOT, HEAT_HOT_MILLIKELVIN);
        EXPECT(ApplyLabCommands(nest, table, std::span(&hot, 1), SHAPE) == 1);

        // --- 頭打ちが起きる場面か(薄い空気の基準のレベル = 頭打ちにならない最後のレベル)。ふつうの材料の基準も出す ---
        const ReactionTableView view = table.View();
        const auto baseOf = [&](const reaction::RxCell& cell) {
            return multires::MrSubcycleBaseLevel(multires::MrCellThermal(view, cell));
        };
        const auto thinCell = LabBoxFineCell(nest, {.level = 2});
        EXPECT(thinCell.has_value());
        const int32_t base = thinCell ? baseOf(*thinCell) : 0;
        // レベル 0 は頭打ちでなく(理論の値に使う)、細かい所は頭打ち(陰解法なしなら遅い)
        EXPECT(base >= 0 && base < static_cast<int32_t>(lab::LAB_MAX_REFINE_LEVELS));
        const MultiresNest fresh = MakeLabBoxNest(table, LAB_SINGLE_ROOT_SHAPE);
        std::printf("lab_box_test: 基準のレベル(頭打ちにならない最後): 薄い空気 %d・空気 %d", base,
                    baseOf(LabBoxCell(fresh, 0)));
        for (const LabMaterial& material : materials) {
            if (material.contents.empty())
                continue;

            const reaction::RxCell cell = MakeReactionCell(table, material.contents,
                                                           table.templates.temperatureMilliKelvin);
            std::printf("・%.*s %d", static_cast<int>(material.name.size()), material.name.data(), baseOf(cell));
        }

        std::printf("\n");

        // --- 3 つの刻み方で流して、広がりの増え方(レベル 0 の単位)を比べる ---
        const auto rateOf = [&](const MultiresStepOptions& options) {
            MultiresNest run = nest;
            const auto spread = [&] {
                return HeatSpread(FINE_EDGE, HOT, [&](uint32_t x, uint32_t y, uint32_t z) {
                    return *LabBoxFineCell(run, {.x = x, .y = y, .z = z, .level = 2});
                });
            };
            const double before = spread();
            for (uint64_t tick = 1; tick <= TICKS; ++tick)
                StepNest(run, table, LAB_WORLD_SEED, tick, options);

            return (spread() - before) / (FINE_TO_ROOT * static_cast<double>(TICKS));
        };
        const double theory = LevelZeroSpreadRate(table, TICKS);
        const double implicit = rateOf(LabStepOptions(SHAPE));
        const auto gap = static_cast<uint32_t>(std::clamp(static_cast<int32_t>(lab::LAB_MAX_REFINE_LEVELS) - base, 0,
                                                          static_cast<int32_t>(MULTIRES_MAX_SUBCYCLE_GAP)));
        const double subcycled = rateOf({.conduction = true, .subcycleBaseLevel = base, .maxSubcycleGap = gap});
        const double capped = rateOf(LAB_STEP_OPTIONS);
        std::printf(
            "lab_box_test: 細かい所の熱の広がり(レベル 0 のセルの辺の 2 乗 / 刻み): 理論(レベル 0)%.4f・"
            "陰解法 %.4f・小刻み %.4f・陰解法なし %.4f\n",
            theory, implicit, subcycled, capped);
        EXPECT(theory > 0);
        EXPECT(std::abs(implicit - theory) < theory * 0.05);
        EXPECT(std::abs(subcycled - theory) < theory * 0.05);
        EXPECT(capped < theory * 0.7);

        // --- 陰解法の刻みでもエネルギーと元素はビット単位で保存 ---
        MultiresNest conserved = nest;
        const ConservedTotals initial = ComputeConservedTotals(conserved, table, 2);
        for (uint64_t tick = 1; tick <= TICKS; ++tick)
            StepLabBox(conserved, table, tick, {}, SHAPE);

        EXPECT(ComputeConservedTotals(conserved, table, 2) == initial);
    }

    // --- 細かくした箱の計器・葉のセル・断面(T-0350)。窓の --auto-lab と同じ、根 1 つの細かくできる箱 ---
    void TestRefinedGauge(const BakedReactionTable& table, const std::vector<LabMaterial>& materials) {
        constexpr lab::LabBoxShape SHAPE = {.rootsX = 1, .rootsY = 1, .rootsZ = 1, .refineBlocks = 128};
        constexpr uint32_t FINE_TO_MICROMOLES = 6;  // レベル 2 の量の単位は 8^-2 µmol
        const LabMaterial* wood = FindMaterial(materials, "木");
        const uint32_t nitrogen = table.SpeciesId("nitrogen");
        const uint32_t cellulose = table.SpeciesId("cellulose");
        const LabCellPosition fine = {.x = 8, .y = 8, .z = 8, .level = 2};
        const uint64_t woodMicromoles = wood->contents[0].amount >> FINE_TO_MICROMOLES;

        // --- 細かくしても、箱全体と箱のセル (4, 4, 4)(細かいセル 64 個)の量は同じ(葉を µmol に直して数える)---
        MultiresNest nest = MakeLabBoxNest(table, SHAPE);
        const LabGaugeSample before = SampleLabGauge(nest, table, 0, lab::LabCellIndex(4, 4, 4), SHAPE);
        const std::vector<Command> refine = {
            MakeLabRefineCommand(0, 0, {.low = {.x = 2, .y = 2, .z = 2}, .high = {.x = 5, .y = 5, .z = 5}}, 2)};
        EXPECT(ApplyLabRefines(nest, refine, SHAPE) == 16 && LabRefinedBlockCount(nest) == 16);
        const LabGaugeSample after = SampleLabGauge(nest, table, 0, lab::LabCellIndex(4, 4, 4), SHAPE);
        EXPECT(after.boxAmounts == before.boxAmounts && after.cellAmounts == before.cellAmounts);
        EXPECT(std::abs(after.cellTemperatureMilliKelvin - before.cellTemperatureMilliKelvin) <= 1);

        // --- 細かいセル 1 つに木: 見る細かいセル・それを含む箱のセル・箱全体のセルロースは置いた量の 64 分の 1 µmol。
        //     粗い葉(レベル 1)の中の細かいセルは、その葉の体積の割合(窒素は箱のセルの 64 分の 1)---
        const std::vector<Command> place = {MakeLabFillCommand(1, 0, fine, wood->contents, 300000)};
        EXPECT(ApplyLabCommands(nest, table, place, SHAPE) == 1);
        const LabGaugeSample watched = SampleLabGauge(nest, table, 1, fine.Index(), SHAPE);
        EXPECT(watched.cell == fine.Index() && watched.cellAmounts[cellulose] == woodMicromoles);
        EXPECT(watched.boxAmounts[cellulose] == woodMicromoles);
        EXPECT(SampleLabGauge(nest, table, 1, lab::LabCellIndex(2, 2, 2), SHAPE).cellAmounts[cellulose] ==
               woodMicromoles);
        const LabGaugeSample corner = SampleLabGauge(nest, table, 1, LabCellPosition{.level = 2}.Index(), SHAPE);
        EXPECT(corner.cellAmounts[nitrogen] == (before.cellAmounts[nitrogen] >> FINE_TO_MICROMOLES));

        // --- 葉のセル: 細かいセルはそのまま、粗い葉の中の細かい位置はその葉、細かく分かれた箱のセルは無い ---
        const auto leaf = LabLeafAt(nest, fine);
        EXPECT(leaf.has_value() && leaf->position.level == 2 &&
               AmountOf(leaf->cell, cellulose) == wood->contents[0].amount);
        const auto coarseLeaf = LabLeafAt(nest, {.x = 1, .y = 1, .z = 1, .level = 2});
        EXPECT(coarseLeaf.has_value() && coarseLeaf->position.level == 1 && coarseLeaf->position.x == 0);
        EXPECT(!LabLeafAt(nest, {.x = 2, .y = 2, .z = 2}).has_value());

        // --- 断面: 木の細かいセルの面はレベル 2、隅はレベル 1。細かくしていない箱は全部レベル 0 で根のセルの温度 ---
        const uint32_t width = LAB_BOX_EDGE * LAB_FINE_SLICE_SCALE;
        const std::vector<LabSliceCell> slice = LabFineSlice(nest, table, SHAPE, 2, 0);
        EXPECT(slice.size() == size_t{width} * width);
        EXPECT(slice[(size_t{fine.y} * width) + fine.x].level == 2 && slice.front().level == 1);
        const MultiresNest plain = MakeLabBoxNest(table, LAB_SINGLE_ROOT_SHAPE);
        const std::vector<LabSliceCell> plainSlice = LabFineSlice(plain, table, LAB_SINGLE_ROOT_SHAPE, 3, 0);
        const int32_t rootTemperature = reaction::RxComputeThermal(table.View(),
                                                                   LabBoxCell(plain, lab::LabCellIndex(0, 0, 3)))
                                            .temperature;
        EXPECT(std::ranges::all_of(plainSlice, [&](const LabSliceCell& cell) {
            return cell.level == 0 && cell.temperatureMilliKelvin == rootTemperature;
        }));

        // --- CPU だけの計器も細かいセルを見る: 火を付けると見る細かいセルのセルロースが減る ---
        const std::vector<Command> commands = {refine[0], place[0],
                                               MakeLabTemperatureCommand(2, 0, fine, IGNITE_MILLIKELVIN)};
        const LabRecording recording{.shape = SHAPE, .tickCount = 40, .commands = commands};
        const auto gauge = RunLabGaugeOnCpu(recording, table, fine.Index());
        EXPECT(gauge.has_value() && gauge->size() == 40);
        EXPECT(gauge.has_value() && gauge->back().cellAmounts[cellulose] < woodMicromoles);
        EXPECT(gauge.has_value() && gauge->front().boxAmounts[nitrogen] == before.boxAmounts[nitrogen]);
    }

    // 版 4 の記録(箱の形が無い)は根 1 つとして読む
    void TestVersionFourRecording() {
        const LabRecording recording{.tableVersion = 0x1111, .tickCount = 1, .hashes = {7}};
        std::vector<std::byte> bytes = SerializeLabRecording(recording);

        // 版 5 → 4: 種の後ろの形(16 バイト)を抜き、版の語を書き換える
        bytes = WithoutShape(bytes);
        bytes[4] = std::byte{4};
        const auto parsed = ParseLabRecording(bytes);
        EXPECT(parsed.has_value() && SameLabShape(parsed->shape, LAB_SINGLE_ROOT_SHAPE) &&
               parsed->tableVersion == 0x1111);
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
    TestTableContents();
    TestRegion(*table, materials);
    TestConversions(*table, materials);
    TestGauge(*table, materials);
    TestLargeBox(*table, materials);
    TestVersionFourRecording();
    TestRefinedBox(*table, materials);
    TestRefinedGauge(*table, materials);
    TestRefinedHeatSpeed(*table, materials);

    if (failureCount > 0) {
        std::printf("lab_box_test: %d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("lab_box_test: OK\n");
    return 0;
}
