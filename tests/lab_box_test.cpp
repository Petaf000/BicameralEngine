// lab_box_test.cpp — 実験室の箱の CPU 側(sim/lab_box。T-0142)を CPU だけで確かめる:
// コマンドで作るセルが MakeReactionCell と同じ・当てられないコマンドは飛ばす・木を置いて火を付けると燃える(セルロースが減り CO2 が出る)・
// コマンドの無い刻みは元素とエネルギーの合計が変わらない・同じコマンドの列なら同じハッシュの列・記録の読み書き・最初に違ったセルを見つける。
// 表を替えた印(T-0218): セルを変えずに箱をつつく・途中で表を替えると替えた刻みから先だけ変わる・印の入った記録(版 3)と版 2 の読み書き。
// 表の中身(T-0217): 記録の版 4 は使った表の中身を持ち、読み書きで同じ・壊れた中身は断る・版 3 も読める。
// 範囲と量(T-0220): 範囲のコマンドは直方体のセル全部を 1 セルのコマンドと同じに・箱の外や逆の範囲は当てない・
// 分圧と質量から µmol への換算(300 K・1 気圧の 1 セルが空気の材料の量と合う)。
// 計器と比べる段取り(T-0221): 計器の値(置いた量・最高温度)・反応の速さの和が量の差・保存点で切る・A は元の実験と同じ・
// 条件を 1 つ足した B は保存点の刻みから違う・表を替えた記録は CPU の計器で流さない。
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
#include "sim/lab_gauge.h"
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
            if (now.empty())
                EXPECT(ComputeConservedTotals(nest, table, 0) ==
                       before);  // 箱は閉じている: コマンドの無い刻みは合計が同じ

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

    if (failureCount > 0) {
        std::printf("lab_box_test: %d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("lab_box_test: OK\n");
    return 0;
}
