// lab_box.cpp — 実験室の箱の CPU 側(lab_box.h)。コマンドを当てる関数は common/lab_box.hlsli(GPU と共通)。
#include "sim/lab_box.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <functional>
#include <type_traits>

#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        // --- 材料の量(1 セル = 0.5 m 角 = 0.125 m³。仮の世界の初めの世界と同じ値。sim/probe_sim.cpp)---
        constexpr uint64_t AIR_OXYGEN_MICROMOLES = 1063800;
        constexpr uint64_t AIR_NITROGEN_MICROMOLES = 4014200;
        constexpr uint64_t AIR_TOTAL_MICROMOLES = AIR_OXYGEN_MICROMOLES + AIR_NITROGEN_MICROMOLES;
        // 木: 体積の 1 割が木(密度 500 kg/m³ のセルロース)、残りは孔の中の空気
        constexpr uint64_t WOOD_CELLULOSE_MICROMOLES = 38600000;
        constexpr uint64_t PORE_OXYGEN_MICROMOLES = 983000;
        constexpr uint64_t PORE_NITROGEN_MICROMOLES = 3697000;
        // 木炭: 体積の 1 割が炭(密度 300 kg/m³ の炭素 = 3.75 kg ≒ 312 mol)、残りは孔の中の空気
        constexpr uint64_t CHARCOAL_CARBON_MICROMOLES = 312000000;

        constexpr uint32_t COMMAND_WORDS_PER_ENTRY = 3;

        // --- 量の換算(T-0220): µmol = p[Pa] × V[m³] × 10^6 ÷ (R[J/(mol·K)] × T[K])
        //     = p × (0.125 × 10^6 × 10^3 × 10^9) ÷ (R × 10^9 × T[mK])。R = 8.314462618(CODATA 2018 の定義値)---
        constexpr uint64_t GAS_NUMERATOR_PER_PASCAL = 125'000'000'000'000'000;  // 0.125 m³ × 10^18
        constexpr uint64_t GAS_CONSTANT_NANO = 8'314'462'618;                   // R × 10^9
        constexpr uint64_t MICROMOLES_PER_MOLE = 1'000'000;

        // --- 記録のファイルの形(リトルエンディアン。版を上げたら読む側も直す)---
        constexpr std::array<char, 4> RECORDING_MAGIC = {'B', 'L', 'A', 'B'};
        // 4: ハッシュの列の後ろに表の中身(T-0217): 数 u64 → 表ごとに 版 u64・バイト数 u64・バイト(8 バイトの境界まで 0 で埋める)
        constexpr uint32_t RECORDING_VERSION = 4;
        // 読める古い版: コマンドに表を替えた印(LAB_COMMAND_TYPE_TABLE)が入りうる(T-0218。形は 2 と同じ)
        constexpr uint32_t RECORDING_VERSION_TABLE_MARKS = 3;
        constexpr uint32_t RECORDING_VERSION_ONE_TABLE = 2;  // 読める古い版(表の版を足した。T-0194)
        constexpr uint32_t RECORDING_VERSION_NO_TABLE = 1;   // 読める古い版(表の版が無い)

        void SetPayload(Command& command, uint32_t word, uint32_t value) {
            command.payload[word] = value;
        }

        Command MakeLabCommand(uint64_t targetTick, uint32_t sequence, uint32_t type, LabCellPosition cell,
                               uint32_t temperatureMilliKelvin) {
            Command command;
            command.targetTick = targetTick;
            command.sequence = sequence;
            command.type = static_cast<uint16_t>(type);
            SetPayload(command, lab::LAB_PAYLOAD_CELL,
                       cell.x < LAB_BOX_EDGE && cell.y < LAB_BOX_EDGE && cell.z < LAB_BOX_EDGE ? cell.Index()
                                                                                               : lab::LAB_NO_CELL);
            SetPayload(command, lab::LAB_PAYLOAD_TEMPERATURE, temperatureMilliKelvin);
            command.size = static_cast<uint16_t>(sizeof(uint32_t) * 2);

            return command;
        }

        // 範囲の語(lab_box.hlsli の LAB_REGION_*)。箱の外の角は箱の外のまま書く(LabRegionCommandValid が当てない)
        uint32_t RegionWord(LabCellRange range) {
            const auto corner = [](LabCellPosition cell) {
                const auto coordinate = [](uint32_t value) {
                    return std::min(value, lab::LAB_REGION_COORD_MASK);
                };

                return coordinate(cell.x) | (coordinate(cell.y) << lab::LAB_REGION_COORD_BITS) |
                       (coordinate(cell.z) << (2 * lab::LAB_REGION_COORD_BITS));
            };

            return corner(range.low) | (corner(range.high) << lab::LAB_REGION_CORNER_BITS);
        }

        template <typename T>
        bool SameBytes(const std::vector<T>& a, const std::vector<T>& b) {
            static_assert(std::is_trivially_copyable_v<T>);
            return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0;
        }

        // --- 記録の読み書きの小さな道具 ---
        template <typename T>
        void Append(std::vector<std::byte>& bytes, const T& value) {
            static_assert(std::is_trivially_copyable_v<T>);
            const auto* begin = reinterpret_cast<const std::byte*>(&value);
            bytes.insert(bytes.end(), begin, begin + sizeof(T));
        }

        template <typename T>
        bool Take(std::span<const std::byte>& bytes, T& value) {
            static_assert(std::is_trivially_copyable_v<T>);
            if (bytes.size() < sizeof(T))
                return false;

            std::memcpy(&value, bytes.data(), sizeof(T));
            bytes = bytes.subspan(sizeof(T));

            return true;
        }

        size_t AlignedSize(size_t size) {
            constexpr size_t ALIGNMENT = sizeof(uint64_t);
            return (size + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT;
        }

        // 記録の末尾の表の中身(版 4。T-0217)。読み終えて余りがあれば壊れている
        std::expected<std::vector<LabTableContent>, std::string> TakeTables(std::span<const std::byte> bytes) {
            uint64_t count = 0;
            if (!Take(bytes, count) || count > bytes.size())
                return std::unexpected("記録の表の中身が壊れている");

            std::vector<LabTableContent> tables(count);
            for (LabTableContent& table : tables) {
                uint64_t size = 0;
                if (!Take(bytes, table.version) || !Take(bytes, size) || size > bytes.size() ||
                    AlignedSize(size) > bytes.size())
                    return std::unexpected("記録の表の中身が壊れている");

                table.bytes.assign(reinterpret_cast<const char*>(bytes.data()), size);
                bytes = bytes.subspan(AlignedSize(size));
            }

            const bool increasing = std::ranges::adjacent_find(tables, std::ranges::greater_equal{},
                                                               &LabTableContent::version) == tables.end();
            if (!bytes.empty() || !increasing)
                return std::unexpected("記録の表の中身が壊れている");

            return tables;
        }

    }  // namespace

    // --- コマンド ---

    Command MakeLabFillCommand(uint64_t targetTick, uint32_t sequence, LabCellPosition cell,
                               std::span<const SpeciesAmount> contents, uint32_t temperatureMilliKelvin) {
        Command command = MakeLabCommand(targetTick, sequence, lab::LAB_COMMAND_TYPE_FILL, cell,
                                         temperatureMilliKelvin);

        // 多すぎる材料は数だけ書く(LabCommandCell が当てないコマンドにする)
        const auto count = static_cast<uint32_t>(contents.size());
        SetPayload(command, lab::LAB_PAYLOAD_COUNT, count);
        const uint32_t written = std::min(count, lab::LAB_MAX_FILL_SPECIES);
        for (uint32_t i = 0; i < written; ++i) {
            const uint32_t word = lab::LAB_PAYLOAD_ENTRIES + (COMMAND_WORDS_PER_ENTRY * i);
            SetPayload(command, word, contents[i].species);
            SetPayload(command, word + 1, static_cast<uint32_t>(contents[i].amount));
            SetPayload(command, word + 2, static_cast<uint32_t>(contents[i].amount >> 32));
        }

        command.size = static_cast<uint16_t>(sizeof(uint32_t) *
                                             (lab::LAB_PAYLOAD_ENTRIES + (COMMAND_WORDS_PER_ENTRY * written)));

        return command;
    }

    Command MakeLabTemperatureCommand(uint64_t targetTick, uint32_t sequence, LabCellPosition cell,
                                      uint32_t temperatureMilliKelvin) {
        return MakeLabCommand(targetTick, sequence, lab::LAB_COMMAND_TYPE_TEMPERATURE, cell, temperatureMilliKelvin);
    }

    Command MakeLabFillRegionCommand(uint64_t targetTick, uint32_t sequence, LabCellRange range,
                                     std::span<const SpeciesAmount> contents, uint32_t temperatureMilliKelvin) {
        Command command = MakeLabFillCommand(targetTick, sequence, range.low, contents, temperatureMilliKelvin);
        command.type = static_cast<uint16_t>(lab::LAB_COMMAND_TYPE_FILL_REGION);
        SetPayload(command, lab::LAB_PAYLOAD_CELL, RegionWord(range));

        return command;
    }

    Command MakeLabTemperatureRegionCommand(uint64_t targetTick, uint32_t sequence, LabCellRange range,
                                            uint32_t temperatureMilliKelvin) {
        Command command = MakeLabTemperatureCommand(targetTick, sequence, range.low, temperatureMilliKelvin);
        command.type = static_cast<uint16_t>(lab::LAB_COMMAND_TYPE_TEMPERATURE_REGION);
        SetPayload(command, lab::LAB_PAYLOAD_CELL, RegionWord(range));

        return command;
    }

    Command MakeLabTableCommand(uint64_t targetTick, uint32_t sequence, uint64_t tableVersion) {
        Command command;
        command.targetTick = targetTick;
        command.sequence = sequence;
        command.type = static_cast<uint16_t>(lab::LAB_COMMAND_TYPE_TABLE);
        SetPayload(command, lab::LAB_PAYLOAD_TABLE_VERSION, static_cast<uint32_t>(tableVersion));
        SetPayload(command, lab::LAB_PAYLOAD_TABLE_VERSION + 1, static_cast<uint32_t>(tableVersion >> 32));
        command.size = static_cast<uint16_t>(sizeof(uint32_t) * 2);

        return command;
    }

    std::optional<uint64_t> LabTableVersionOf(const Command& command) {
        if (command.type != lab::LAB_COMMAND_TYPE_TABLE)
            return std::nullopt;

        const uint64_t low = command.payload[lab::LAB_PAYLOAD_TABLE_VERSION];
        const uint64_t high = command.payload[lab::LAB_PAYLOAD_TABLE_VERSION + 1];

        return low | (high << 32);
    }

    lab::LabCommand ToLabCommand(const Command& command) {
        static_assert(sizeof(lab::LabCommand) == COMMAND_BYTES);
        lab::LabCommand words{};
        std::memcpy(&words, &command, sizeof(words));

        return words;
    }

    // --- 量の換算 ---

    uint64_t LabGasMicromoles(uint64_t pressurePascal, uint32_t temperatureMilliKelvin) {
        const bool outside = temperatureMilliKelvin == 0 ||
                             temperatureMilliKelvin > lab::LAB_MAX_TEMPERATURE_MILLIKELVIN;
        if (outside || pressurePascal > LAB_MAX_PRESSURE_PASCAL)
            return 0;

        // 上限の圧力・1 mK でも商は 2^64 未満(1.25e25 ÷ 8.3e9 ≈ 1.5e15)
        const fx::FxU128 numerator = fx::FxMulU64Full(pressurePascal, GAS_NUMERATOR_PER_PASCAL);
        const uint64_t denominator = GAS_CONSTANT_NANO * temperatureMilliKelvin;

        return fx::FxDivU128By64(numerator, denominator).quotient;
    }

    uint64_t LabMassMicromoles(uint64_t milligrams, uint32_t molarMassMilligramsPerMol) {
        if (molarMassMilligramsPerMol == 0 || milligrams > LAB_MAX_MASS_MILLIGRAMS)
            return 0;

        return fx::FxDivU128By64(fx::FxMulU64Full(milligrams, MICROMOLES_PER_MOLE), molarMassMilligramsPerMol).quotient;
    }

    // --- 材料 ---

    std::vector<LabMaterial> MakeLabMaterials(const BakedReactionTable& table) {
        const uint32_t oxygen = table.SpeciesId("oxygen");
        const uint32_t nitrogen = table.SpeciesId("nitrogen");
        const uint32_t cellulose = table.SpeciesId("cellulose");
        const uint32_t carbon = table.SpeciesId("carbon");
        const uint32_t carbonDioxide = table.SpeciesId("carbon_dioxide");

        std::vector<LabMaterial> materials;
        const auto add = [&](std::string_view name, std::vector<SpeciesAmount> contents) {
            const bool known = std::ranges::all_of(contents,
                                                   [](const SpeciesAmount& entry) { return entry.species != 0; });
            if (known)
                materials.push_back({.name = name, .contents = std::move(contents)});
        };

        add("空気", {{oxygen, AIR_OXYGEN_MICROMOLES}, {nitrogen, AIR_NITROGEN_MICROMOLES}});
        add("木", {{cellulose, WOOD_CELLULOSE_MICROMOLES},
                   {oxygen, PORE_OXYGEN_MICROMOLES},
                   {nitrogen, PORE_NITROGEN_MICROMOLES}});
        add("木炭", {{carbon, CHARCOAL_CARBON_MICROMOLES},
                     {oxygen, PORE_OXYGEN_MICROMOLES},
                     {nitrogen, PORE_NITROGEN_MICROMOLES}});
        add("二酸化炭素", {{carbonDioxide, AIR_TOTAL_MICROMOLES}});
        add("窒素", {{nitrogen, AIR_TOTAL_MICROMOLES}});

        return materials;
    }

    // --- 箱 ---

    MultiresCapacity LabBoxCapacity(const BakedReactionTable& table) {
        return {.worldBlocks = 1,
                .observerBlocks = 0,
                .fractions = 1,
                .pages = 1,
                .indexEntries = 2,
                .ledgerColumns = 1 + static_cast<uint32_t>(table.species.size()),
                .rootLevel = 0};
    }

    MultiresNest MakeLabBoxNest(const BakedReactionTable& table) {
        const std::array<SpeciesAmount, 2> air = {
            SpeciesAmount{.species = table.SpeciesId("oxygen"), .amount = AIR_OXYGEN_MICROMOLES},
            SpeciesAmount{.species = table.SpeciesId("nitrogen"), .amount = AIR_NITROGEN_MICROMOLES}};
        const RxCell fill = MakeReactionCell(table, air, LAB_INITIAL_TEMPERATURE_MILLIKELVIN);

        // --- 頁を持たせるため、1 つだけ違うセルで根を置き(PlaceRootBlock は全部同じなら一様にする)、すぐ戻す ---
        MultiresNest nest = MakeMultiresNest(LabBoxCapacity(table));
        std::vector<RxCell> cells(MR_BLOCK_CELLS, fill);
        cells[0] = RxMakeEmptyCell(0);
        const uint32_t slot = PlaceRootBlock(nest, 0, 0, 0, cells);
        FX_ASSERT(slot == LAB_BOX_SLOT && !MrIsUniform(nest.blocks[slot]));
        nest_detail::CellAt(nest, slot, 0) = fill;

        return nest;
    }

    RxCell LabBoxCell(const MultiresNest& nest, uint32_t index) {
        return LoadNestCell(nest, LAB_BOX_SLOT, index);
    }

    uint32_t ApplyLabCommands(MultiresNest& nest, const BakedReactionTable& table, std::span<const Command> commands) {
        if (MrIsUniform(nest.blocks[LAB_BOX_SLOT]))
            return 0;  // shaders/sim/lab_box.hlsl と同じ(箱はいつも頁を持つ)

        const ReactionTableView view = table.View();
        const auto speciesCount = static_cast<uint32_t>(table.species.size());
        uint32_t applied = 0;
        for (const Command& command : commands) {
            const lab::LabCommand words = ToLabCommand(command);

            // --- 表を替えた印: セルは変えず、箱をつつくだけ(lab_box.hlsl と同じ。表は呼ぶ側が先に替えている。T-0218)---
            if (lab::LabCommandMarksTable(words)) {
                applied += 1;
                continue;
            }

            // --- 範囲(T-0220): x → y → z の順に 1 セルずつ(lab_box.hlsl と同じ順)---
            if (lab::LabRegionCommandValid(words, speciesCount)) {
                const lab::LabRegion region = lab::LabCommandRegion(words);
                for (uint32_t z = region.lowZ; z <= region.highZ; ++z) {
                    for (uint32_t y = region.lowY; y <= region.highY; ++y) {
                        for (uint32_t x = region.lowX; x <= region.highX; ++x) {
                            RxCell& target = nest_detail::CellAt(nest, LAB_BOX_SLOT, MrCellIndex(x, y, z));
                            target = lab::LabApplyCommand(view, target, words);
                        }
                    }
                }

                applied += 1;
                continue;
            }

            const uint32_t cell = lab::LabCommandCell(words, speciesCount);
            if (cell == lab::LAB_NO_CELL)
                continue;

            RxCell& target = nest_detail::CellAt(nest, LAB_BOX_SLOT, cell);
            target = lab::LabApplyCommand(view, target, words);
            applied += 1;
        }

        // --- 変えたブロックは「つつかれた」(lab_box.hlsl と同じ。種の印は状態の要約に入らない)---
        if (applied > 0)
            nest_detail::PokeBlock(nest, LAB_BOX_SLOT);

        return applied;
    }

    void StepLabBox(MultiresNest& nest, const BakedReactionTable& table, uint64_t tick,
                    std::span<const Command> commands) {
        ApplyLabCommands(nest, table, commands);
        StepNest(nest, table, LAB_WORLD_SEED, tick, LAB_STEP_OPTIONS);
    }

    // --- 比べる ---

    std::optional<LabMismatch> FindLabMismatch(const MultiresNest& cpu, const MultiresNest& gpu, uint64_t tick) {
        if (HashWholeNest(cpu) == HashWholeNest(gpu))
            return std::nullopt;

        LabMismatch mismatch{.tick = tick};
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const RxCell a = LabBoxCell(cpu, index);
            const RxCell b = LabBoxCell(gpu, index);
            if (RxSameCell(a, b))
                continue;

            mismatch.cell = index;
            mismatch.cpu = a;
            mismatch.gpu = b;
            mismatch.what = std::format("刻み {} のセル ({}, {}, {}) が違う", tick, MrCellX(index), MrCellY(index),
                                        MrCellZ(index));
            return mismatch;
        }

        // --- セルは同じ: 違う欄の名前 ---
        std::string_view field = "その他";
        if (!SameBytes(cpu.blocks, gpu.blocks))
            field = "ブロックの見出し";
        else if (!SameBytes(cpu.fractions, gpu.fractions))
            field = "端数";
        else if (!SameBytes(cpu.ledger, gpu.ledger))
            field = "世界の帳簿";
        else if (cpu.counters != gpu.counters)
            field = "数える欄";
        else if (!SameBytes(cpu.freeBlocks, gpu.freeBlocks) || !SameBytes(cpu.freeFractions, gpu.freeFractions))
            field = "空きのスタック";

        mismatch.what = std::format("刻み {} の{}が違う(セルは全部同じ)", tick, field);
        return mismatch;
    }

    // --- 記録 ---

    std::vector<std::byte> SerializeLabRecording(const LabRecording& recording) {
        std::vector<std::byte> bytes;
        Append(bytes, RECORDING_MAGIC);
        Append(bytes, RECORDING_VERSION);
        Append(bytes, LAB_WORLD_SEED);
        Append(bytes, recording.tableVersion);
        Append(bytes, recording.tickCount);
        Append(bytes, static_cast<uint64_t>(recording.commands.size()));
        Append(bytes, static_cast<uint64_t>(recording.hashes.size()));
        for (const Command& command : recording.commands)
            Append(bytes, command);

        for (const uint64_t hash : recording.hashes)
            Append(bytes, hash);

        // --- 表の中身(T-0217)---
        Append(bytes, static_cast<uint64_t>(recording.tables.size()));
        for (const LabTableContent& table : recording.tables) {
            Append(bytes, table.version);
            Append(bytes, static_cast<uint64_t>(table.bytes.size()));
            const auto* begin = reinterpret_cast<const std::byte*>(table.bytes.data());
            bytes.insert(bytes.end(), begin, begin + table.bytes.size());
            bytes.resize(AlignedSize(bytes.size()), std::byte{0});
        }

        return bytes;
    }

    std::expected<LabRecording, std::string> ParseLabRecording(std::span<const std::byte> bytes) {
        std::array<char, 4> magic{};
        uint32_t version = 0;
        uint64_t seed = 0;
        uint64_t commandCount = 0;
        uint64_t hashCount = 0;
        LabRecording recording;
        if (!Take(bytes, magic) || magic != RECORDING_MAGIC)
            return std::unexpected("実験の記録ではない");

        const auto knownVersion = [](uint32_t value) {
            return value == RECORDING_VERSION || value == RECORDING_VERSION_TABLE_MARKS ||
                   value == RECORDING_VERSION_ONE_TABLE || value == RECORDING_VERSION_NO_TABLE;
        };
        if (!Take(bytes, version) || !knownVersion(version))
            return std::unexpected(std::format("記録の版 {} は読めない", version));

        if (!Take(bytes, seed) || seed != LAB_WORLD_SEED)
            return std::unexpected("記録の見出しが壊れている");

        if (version != RECORDING_VERSION_NO_TABLE && !Take(bytes, recording.tableVersion))
            return std::unexpected("記録の見出しが壊れている");

        if (!Take(bytes, recording.tickCount) || !Take(bytes, commandCount) || !Take(bytes, hashCount))
            return std::unexpected("記録の見出しが壊れている");

        const uint64_t listBytes = (commandCount * COMMAND_BYTES) + (hashCount * sizeof(uint64_t));
        const bool sized = version == RECORDING_VERSION ? bytes.size() >= listBytes : bytes.size() == listBytes;
        if (commandCount > bytes.size() || hashCount > bytes.size() || !sized || hashCount != recording.tickCount)
            return std::unexpected("記録の大きさが合わない");

        recording.commands.resize(commandCount);
        for (Command& command : recording.commands)
            (void)Take(bytes, command);

        recording.hashes.resize(hashCount);
        for (uint64_t& hash : recording.hashes)
            (void)Take(bytes, hash);

        if (version == RECORDING_VERSION) {
            auto tables = TakeTables(bytes);
            if (!tables)
                return std::unexpected(tables.error());

            recording.tables = std::move(*tables);
        }

        const bool sorted = std::ranges::is_sorted(recording.commands, CommandPrecedes);
        if (!sorted || (!recording.commands.empty() && recording.commands.back().targetTick >= recording.tickCount))
            return std::unexpected("記録のコマンドの並びが壊れている");

        return recording;
    }

}  // namespace bicameral::sim
