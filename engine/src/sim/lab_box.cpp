// lab_box.cpp — 実験室の箱の CPU 側(lab_box.h)。コマンドを当てる関数は common/lab_box.hlsli(GPU と共通)。
#include "sim/lab_box.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <functional>
#include <type_traits>

#include "common/implicit_conduction.hlsli"
#include "common/units.hlsli"
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
        // 仮の魔素(T-0225。試験用の仮の値): 空気の 1 割の物質量。筆で「足す」と、そのセルの木や炭に触媒として効く
        constexpr uint64_t MANA_TEST_MICROMOLES = AIR_TOTAL_MICROMOLES / 10;

        constexpr uint32_t COMMAND_WORDS_PER_ENTRY = 3;

        // --- 量の換算(T-0220): µmol = p[Pa] × V[m³] × 10^6 ÷ (R[J/(mol·K)] × T[K])
        //     = p × (0.125 × 10^6 × 10^3 × 10^9) ÷ (R × 10^9 × T[mK])。R = 8.314462618(CODATA 2018 の定義値)---
        constexpr uint64_t GAS_NUMERATOR_PER_PASCAL = 125'000'000'000'000'000;               // 0.125 m³ × 10^18
        constexpr uint64_t GAS_CONSTANT_NANO = fx::GAS_CONSTANT_NANOJOULES_PER_MOLE_KELVIN;  // R × 10^9(units.hlsli)
        constexpr uint64_t MICROMOLES_PER_MOLE = 1'000'000;

        // --- 記録のファイルの形(リトルエンディアン。版を上げたら読む側も直す)---
        constexpr std::array<char, 4> RECORDING_MAGIC = {'B', 'L', 'A', 'B'};
        // 6: 箱の形の 4 語目が細かくする枠の数(T-0314。lab::LabBoxShape::refineBlocks)。コマンドに細かくする印
        //    (LAB_COMMAND_TYPE_REFINE)と細かいセルの語が入りうる。ほかは 5 と同じ
        constexpr uint32_t RECORDING_VERSION = 6;
        // 読める古い版: 種の後ろに箱の形(T-0173): 根の数 x・y・z(u32 × 3)+ 0(u32)。ほかは 4 と同じ
        constexpr uint32_t RECORDING_VERSION_SHAPE = 5;
        // 読める古い版(根 1 つの箱)。4: ハッシュの列の後ろに表の中身(T-0217): 数 u64 → 表ごとに 版 u64・バイト数 u64・
        // バイト(8 バイトの境界まで 0 で埋める)
        constexpr uint32_t RECORDING_VERSION_TABLE_BYTES = 4;
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
            // 箱の外(どの形の箱にも入らない座標・段)は LAB_NO_CELL。形より外の根は当てる時に断る(LabCommandCell)
            const bool levelValid = cell.level <= lab::LAB_MAX_REFINE_LEVELS;
            const uint32_t edge = levelValid ? lab::LAB_MAX_BOX_EDGE << cell.level : 0;
            const bool inBounds = levelValid && cell.x < edge && cell.y < edge && cell.z < edge;
            SetPayload(command, lab::LAB_PAYLOAD_CELL, inBounds ? cell.Index() : lab::LAB_NO_CELL);
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

        // common/magic_patch.hlsli の List の約束(箱の世界のパッチの列。T-0024)
        struct NestPatchList {
            std::span<uint32_t> words;

            [[nodiscard]] uint32_t PatchWord(uint32_t word) const { return words[word]; }
            void SetPatchWord(uint32_t word, uint32_t value) const { words[word] = value; }
        };

        // common/lab_box.hlsli の Blocks の約束(箱の根の枠。T-0173)
        struct NestBlocks {
            std::span<const MrBlock> blocks;

            [[nodiscard]] MrBlock Block(uint32_t slot) const { return blocks[slot]; }
        };

        // common/lab_box.hlsli の Cells の約束(lab_box.hlsl の LabCells と同じ: 一様なブロックには当てない)
        struct NestCells {
            MultiresNest* nest;
            ReactionTableView table;

            [[nodiscard]] bool Apply(uint32_t slot, uint32_t local, const lab::LabCommand& command) const {
                if (MrIsUniform(nest->blocks[slot]))
                    return false;

                RxCell& target = nest_detail::CellAt(*nest, slot, local);
                target = lab::LabApplyCommand(table, target, command);

                return true;
            }
        };

        // つついた根(lab::LabRootBit)の木の全部の枠をつつく(lab_box.hlsl の PokeRoots と同じ。T-0314)
        void PokeRoots(MultiresNest& nest, lab::LabBoxShape shape, uint64_t roots) {
            for (uint32_t root = 0; root < lab::LabRootCount(shape); ++root) {
                if (((roots >> root) & 1) == 0)
                    continue;

                for (uint32_t ordinal = 0; ordinal < lab::LAB_SUBTREE_SLOTS; ++ordinal) {
                    const uint32_t slot = lab::LabSubtreeSlot(NestBlocks{.blocks = nest.blocks}, root, ordinal);
                    if (slot != MR_NO_BLOCK)
                        nest_detail::PokeBlock(nest, slot);
                }
            }
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

    Command MakeLabPatchCommand(uint64_t targetTick, uint32_t sequence, uint64_t ruleKey, uint32_t effect,
                                uint32_t durationTicks, LabCellRange range) {
        Command command;
        command.targetTick = targetTick;
        command.sequence = sequence;
        command.type = static_cast<uint16_t>(lab::LAB_COMMAND_TYPE_PATCH);
        SetPayload(command, MP_COMMAND_RULE_KEY, static_cast<uint32_t>(ruleKey));
        SetPayload(command, MP_COMMAND_RULE_KEY + 1, static_cast<uint32_t>(ruleKey >> 32));
        SetPayload(command, MP_COMMAND_EFFECT, effect);
        SetPayload(command, MP_COMMAND_DURATION, durationTicks);
        const std::array<uint32_t, 3> low = {range.low.x, range.low.y, range.low.z};
        const std::array<uint32_t, 3> high = {range.high.x, range.high.y, range.high.z};
        for (uint32_t axis = 0; axis < 3; ++axis) {
            SetPayload(command, MP_COMMAND_LOW + axis, low[axis]);
            SetPayload(command, MP_COMMAND_HIGH + axis, high[axis]);
        }

        command.size = static_cast<uint16_t>(sizeof(uint32_t) * MP_COMMAND_WORDS);

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
        const uint32_t manaTest = table.SpeciesId("mana_test");

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
        add("魔素(試験)", {{manaTest, MANA_TEST_MICROMOLES}});

        return materials;
    }

    // --- 箱 ---

    MultiresCapacity LabBoxCapacity(const BakedReactionTable& table, lab::LabBoxShape shape) {
        FX_ASSERT(lab::LabShapeValid(shape));
        const uint32_t roots = lab::LabRootCount(shape);
        const uint32_t blocks = roots + shape.refineBlocks;
        // 細かくできる箱(T-0314)は、子の端数と、レベルの違う面で粗い側が受ける端数(17 §4)の枠を枠の数だけ足す
        const uint32_t fractions = shape.refineBlocks == 0 ? 1 : 1 + blocks;

        return {.worldBlocks = blocks,
                .observerBlocks = 0,
                .fractions = fractions,
                .pages = blocks,
                .indexEntries = std::bit_ceil(2 * blocks),
                .ledgerColumns = 1 + static_cast<uint32_t>(table.species.size()),
                .rootLevel = 0};
    }

    MultiresImplicitLimits LabImplicitLimits(lab::LabBoxShape shape) {
        constexpr uint32_t LINKS_PER_UNKNOWN = 24;  // 段 0 の隣(未知数 × 12 まで)と粗い段の隣の分
        const uint32_t unknowns = shape.refineBlocks * MR_BLOCK_CELLS;
        const uint32_t cells = shape.refineBlocks * ImBlockCellBound(MR_BLOCK_CELLS);

        return {.unknowns = unknowns, .cells = cells, .nodes = 2 * cells, .links = LINKS_PER_UNKNOWN * unknowns};
    }

    MultiresStepOptions LabStepOptions(lab::LabBoxShape shape) {
        if (shape.refineBlocks == 0)
            return LAB_STEP_OPTIONS;

        // 基準(頭打ちにならない最後のレベル)は箱のセルのレベル 0 に置く: 箱には何でも置けるので物質で決めず、細かくした所は全部陰解法。
        // 頭打ちにならない所を陰解法で解いても本当の速さ(後退 Euler を 1 mK まで解く。D-436)なので、どの物質でも細かい所は本当の速さになる
        MultiresStepOptions options = LAB_STEP_OPTIONS;
        options.subcycleBaseLevel = 0;
        options.implicitConduction = true;
        options.implicitMaxGap = lab::LAB_MAX_REFINE_LEVELS;
        options.implicitLimits = LabImplicitLimits(shape);

        return options;
    }

    MultiresNest MakeLabBoxNest(const BakedReactionTable& table, lab::LabBoxShape shape) {
        const std::array<SpeciesAmount, 2> air = {
            SpeciesAmount{.species = table.SpeciesId("oxygen"), .amount = AIR_OXYGEN_MICROMOLES},
            SpeciesAmount{.species = table.SpeciesId("nitrogen"), .amount = AIR_NITROGEN_MICROMOLES}};
        // 表に空気の物質が無い(ブランクの世界など。T-0371)なら空のセルで満たす
        const bool hasAir = air[0].species != 0 && air[1].species != 0;
        const RxCell fill = hasAir ? MakeReactionCell(table, air, LAB_INITIAL_TEMPERATURE_MILLIKELVIN)
                                   : RxMakeEmptyCell(0);

        // --- 頁を持たせるため、1 つだけ違うセルで根を置き(PlaceRootBlock は全部同じなら一様にする)、すぐ戻す。
        //     根は x → y → z の順に置く(空きのスタックから枠 0, 1, 2… が出る = lab::LabRootSlot)---
        MultiresNest nest = MakeMultiresNest(LabBoxCapacity(table, shape));
        std::vector<RxCell> cells(MR_BLOCK_CELLS, fill);
        cells[0] = RxMakeEmptyCell(hasAir ? 0 : 1);
        for (uint32_t rootZ = 0; rootZ < shape.rootsZ; ++rootZ) {
            for (uint32_t rootY = 0; rootY < shape.rootsY; ++rootY) {
                for (uint32_t rootX = 0; rootX < shape.rootsX; ++rootX) {
                    const uint32_t slot = PlaceRootBlock(nest, int64_t{rootX} * LAB_BOX_EDGE,
                                                         int64_t{rootY} * LAB_BOX_EDGE, int64_t{rootZ} * LAB_BOX_EDGE,
                                                         cells);
                    FX_ASSERT(slot == lab::LabRootSlot(shape, rootX, rootY, rootZ) && !MrIsUniform(nest.blocks[slot]));
                    nest_detail::CellAt(nest, slot, 0) = fill;
                }
            }
        }

        return nest;
    }

    RxCell LabBoxCell(const MultiresNest& nest, uint32_t index, lab::LabBoxShape shape) {
        return LoadNestCell(nest, lab::LabCellSlot(shape, index), lab::LabCellLocal(index));
    }

    uint32_t ApplyLabCommands(MultiresNest& nest, const BakedReactionTable& table, std::span<const Command> commands,
                              lab::LabBoxShape shape) {
        const ReactionTableView view = table.View();
        const auto speciesCount = static_cast<uint32_t>(table.species.size());
        const auto ruleCount = static_cast<uint32_t>(table.rules.size());

        // --- 当てる空間の葉のセル全部に当てる(一様なブロックには当てない。lab_box.hlsl と同じ。T-0314)---
        uint64_t touched = 0;  // つつく根(lab::LabRootBit)
        const auto applyToTarget = [&](const lab::LabTarget& target, const lab::LabCommand& words) {
            touched |= lab::LabApplyToTarget(NestBlocks{.blocks = nest.blocks}, NestCells{.nest = &nest, .table = view},
                                             shape, target, words);
        };

        uint32_t applied = 0;
        for (const Command& command : commands) {
            const lab::LabCommand words = ToLabCommand(command);

            // --- 表を替えた印: セルは変えず、箱の根を全部つつくだけ(lab_box.hlsl と同じ。表は呼ぶ側が先に替えている。T-0218)---
            if (lab::LabCommandMarksTable(words)) {
                touched |= lab::LabAllRoots(shape);
                applied += 1;
                continue;
            }

            // --- 魔法のパッチ(T-0024): 列に足す。足せたら範囲が重なる根をつつく(lab_box.hlsl と同じ)---
            if (lab::LabCommandIsPatch(words)) {
                const NestPatchList list{.words = nest.magicPatches};
                const uint64_t roots = lab::LabApplyPatchCommand(list, view, NestBlocks{.blocks = nest.blocks}, shape,
                                                                 words, ruleCount);
                touched |= roots;
                applied += roots != 0 ? 1 : 0;
                continue;
            }

            // --- 範囲(T-0220): 範囲の葉のセル全部(細かくした所は細かいセル全部。lab_box.hlsl と同じ順)---
            if (lab::LabRegionCommandValid(words, speciesCount, shape)) {
                applyToTarget(lab::LabRegionTarget(words), words);
                applied += 1;
                continue;
            }

            const uint32_t cell = lab::LabCommandCell(words, speciesCount, shape);
            if (cell == lab::LAB_NO_CELL)
                continue;

            applyToTarget(lab::LabCellTarget(cell), words);
            applied += 1;
        }

        // --- 変えた根の木は「つつかれた」(lab_box.hlsl と同じ。種の印は状態の要約に入らない)---
        PokeRoots(nest, shape, touched);

        return applied;
    }

    // --- 物質の付け替え(T-0242)---

    SpeciesRemapReport RemapLabBox(MultiresNest& nest, const SpeciesRemap& remap, const BakedReactionTable& next,
                                   uint64_t tick) {
        SpeciesRemapReport report;
        if (remap.identity)
            return report;

        for (RxCell& cell : nest.cells)
            report.Add(RemapReactionCell(remap, next, cell));

        // 魔法の転換の替える先も新しい ID に(消えた物質への転換は効かなくなる。GPU はスレッド 0 が同じ関数。T-0377)
        report.conversionsDisabled = RemapPatchList(remap, nest.magicPatches, tick);

        return report;
    }

    Command RemapLabCommand(const Command& command, const SpeciesRemap& remap) {
        if (remap.identity)
            return command;

        // --- 魔法のパッチ: 転換の替える先を新しい ID に(消えた物質なら 0 = 当てる時に断られる。T-0377)---
        if (command.type == lab::LAB_COMMAND_TYPE_PATCH) {
            Command remapped = command;
            remapped.payload[MP_COMMAND_EFFECT] = RemapPatchEffect(remap, command.payload[MP_COMMAND_EFFECT]);

            return remapped;
        }

        const bool fills = command.type == lab::LAB_COMMAND_TYPE_FILL ||
                           command.type == lab::LAB_COMMAND_TYPE_FILL_REGION;
        if (!fills)
            return command;

        // --- 材料を読み直して、残る物質だけ新しい ID で書き直す(残る物質どうしの順は変わらない)---
        const uint32_t count = std::min(command.payload[lab::LAB_PAYLOAD_COUNT], lab::LAB_MAX_FILL_SPECIES);
        std::vector<SpeciesAmount> contents;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t word = lab::LAB_PAYLOAD_ENTRIES + (COMMAND_WORDS_PER_ENTRY * i);
            const uint32_t oldId = command.payload[word];
            const uint32_t newId = oldId < remap.newIds.size() ? remap.newIds[oldId] : 0;
            if (newId == 0)
                continue;

            const uint64_t amount = command.payload[word + 1] | (uint64_t{command.payload[word + 2]} << 32);
            contents.push_back({.species = newId, .amount = amount});
        }

        Command remapped = command;
        remapped.payload.fill(0);
        remapped.payload[lab::LAB_PAYLOAD_CELL] = command.payload[lab::LAB_PAYLOAD_CELL];
        remapped.payload[lab::LAB_PAYLOAD_TEMPERATURE] = command.payload[lab::LAB_PAYLOAD_TEMPERATURE];
        remapped.payload[lab::LAB_PAYLOAD_COUNT] = static_cast<uint32_t>(contents.size());
        for (uint32_t i = 0; i < contents.size(); ++i) {
            const uint32_t word = lab::LAB_PAYLOAD_ENTRIES + (COMMAND_WORDS_PER_ENTRY * i);
            remapped.payload[word] = contents[i].species;
            remapped.payload[word + 1] = static_cast<uint32_t>(contents[i].amount);
            remapped.payload[word + 2] = static_cast<uint32_t>(contents[i].amount >> 32);
        }

        remapped.size = static_cast<uint16_t>(sizeof(uint32_t) *
                                              (lab::LAB_PAYLOAD_ENTRIES + (COMMAND_WORDS_PER_ENTRY * contents.size())));

        return remapped;
    }

    bool ApplyLabPatchBoundaries(MultiresNest& nest, uint64_t tick, lab::LabBoxShape shape) {
        const NestPatchList list{.words = nest.magicPatches};
        const uint64_t roots = lab::LabPatchBoundaries(list, NestBlocks{.blocks = nest.blocks}, shape, tick);
        PokeRoots(nest, shape, roots);

        return roots != 0;
    }

    // --- 一部を細かくする(T-0314・ADR-0137)---

    Command MakeLabRefineCommand(uint64_t targetTick, uint32_t sequence, LabCellRange range, uint32_t levels) {
        Command command;
        command.targetTick = targetTick;
        command.sequence = sequence;
        command.type = static_cast<uint16_t>(lab::LAB_COMMAND_TYPE_REFINE);
        SetPayload(command, lab::LAB_PAYLOAD_CELL, RegionWord(range));
        SetPayload(command, lab::LAB_PAYLOAD_REFINE_LEVELS, levels);
        command.size = static_cast<uint16_t>(sizeof(uint32_t) * 2);

        return command;
    }

    std::vector<std::vector<MrRequest>> LabRefineRounds(std::span<const Command> commands, lab::LabBoxShape shape) {
        std::vector<lab::LabCommand> refines;
        uint32_t deepest = 0;
        for (const Command& command : commands) {
            const lab::LabCommand words = ToLabCommand(command);
            if (!lab::LabRefineCommandValid(words, shape))
                continue;

            refines.push_back(words);
            deepest = std::max(deepest, lab::LabPayload(words, lab::LAB_PAYLOAD_REFINE_LEVELS));
        }

        // --- 段ごと・親の中の位置(座標の偶奇)ごとに 1 組。同じ組の要求は親がみな違う(取り合わない)---
        std::vector<std::vector<MrRequest>> rounds;
        for (uint32_t level = 1; level <= deepest; ++level) {
            for (uint32_t parity = 0; parity < MR_CHILDREN_PER_CELL; ++parity) {
                std::vector<std::array<uint32_t, 3>> blocks;  // ブロックの座標(z, y, x。並べる順)
                for (const lab::LabCommand& words : refines) {
                    if (lab::LabPayload(words, lab::LAB_PAYLOAD_REFINE_LEVELS) < level)
                        continue;

                    const lab::LabRegion region = lab::LabCommandRegion(words);
                    const auto first = [&](uint32_t low) {
                        return (low << level) / MR_BLOCK_EDGE;
                    };
                    const auto last = [&](uint32_t high) {
                        return (((high + 1) << level) - 1) / MR_BLOCK_EDGE;
                    };
                    for (uint32_t z = first(region.lowZ); z <= last(region.highZ); ++z) {
                        for (uint32_t y = first(region.lowY); y <= last(region.highY); ++y) {
                            for (uint32_t x = first(region.lowX); x <= last(region.highX); ++x) {
                                if (((x & 1) | ((y & 1) << 1) | ((z & 1) << 2)) == parity)
                                    blocks.push_back({z, y, x});
                            }
                        }
                    }
                }

                std::ranges::sort(blocks);
                blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
                if (blocks.empty())
                    continue;

                // 1 組の要求は MR_MAX_REQUESTS まで(箱の最大 32³ を 2 段細かくしても 16³ ÷ 8 = 512)
                FX_ASSERT(blocks.size() <= MR_MAX_REQUESTS);
                std::vector<MrRequest>& requestGroup = rounds.emplace_back();
                for (const auto& [z, y, x] : blocks) {
                    requestGroup.push_back(MrMakeRequest(MR_REQUEST_REFINE, static_cast<int32_t>(level),
                                                         int64_t{x} * MR_BLOCK_EDGE, int64_t{y} * MR_BLOCK_EDGE,
                                                         int64_t{z} * MR_BLOCK_EDGE));
                }
            }
        }

        return rounds;
    }

    uint32_t ApplyLabRefines(MultiresNest& nest, std::span<const Command> commands, lab::LabBoxShape shape) {
        const std::vector<std::vector<MrRequest>> rounds = LabRefineRounds(commands, shape);
        for (const std::vector<MrRequest>& requestGroup : rounds) {
            SubmitRequests(nest, requestGroup);
            ProcessRequests(nest);
        }

        return static_cast<uint32_t>(rounds.size());
    }

    std::optional<RxCell> LabBoxFineCell(const MultiresNest& nest, LabCellPosition cell) {
        const uint32_t mask = MR_BLOCK_EDGE - 1;
        const uint32_t slot = LookupBlock(nest, static_cast<int32_t>(cell.level), cell.x & ~mask, cell.y & ~mask,
                                          cell.z & ~mask);
        if (slot == MR_NO_BLOCK)
            return std::nullopt;

        const uint32_t local = MrCellIndex(cell.x & mask, cell.y & mask, cell.z & mask);
        if (MrIsCoveredCell(nest.blocks[slot], local))
            return std::nullopt;

        return LoadNestCell(nest, slot, local);
    }

    uint32_t LabRefinedBlockCount(const MultiresNest& nest) {
        uint32_t count = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind == MR_BLOCK_REAL && block.level > 0)
                count += 1;
        }

        return count;
    }

    void StepLabBox(MultiresNest& nest, const BakedReactionTable& table, uint64_t tick,
                    std::span<const Command> commands, lab::LabBoxShape shape) {
        ApplyLabRefines(nest, commands, shape);
        ApplyLabPatchBoundaries(nest, tick, shape);
        ApplyLabCommands(nest, table, commands, shape);
        StepNest(nest, table, LAB_WORLD_SEED, tick, LabStepOptions(shape));
    }

    // --- 比べる ---

    std::optional<LabMismatch> FindLabMismatch(const MultiresNest& cpu, const MultiresNest& gpu, uint64_t tick,
                                               lab::LabBoxShape shape) {
        if (HashWholeNest(cpu) == HashWholeNest(gpu))
            return std::nullopt;

        // --- 根の枠の順 → 根の中の番号の順 ---
        LabMismatch mismatch{.tick = tick};
        for (uint32_t ordinal = 0; ordinal < LabBoxCellCount(shape); ++ordinal) {
            const uint32_t slot = ordinal / MR_BLOCK_CELLS;
            const uint32_t local = ordinal % MR_BLOCK_CELLS;
            const RxCell a = LoadNestCell(cpu, slot, local);
            const RxCell b = LoadNestCell(gpu, slot, local);
            if (RxSameCell(a, b))
                continue;

            const uint32_t rootX = slot % shape.rootsX;
            const uint32_t rootY = (slot / shape.rootsX) % shape.rootsY;
            const uint32_t rootZ = slot / (shape.rootsX * shape.rootsY);
            const uint32_t index = lab::LabCellIndex(MrCellX(local) + (rootX * LAB_BOX_EDGE),
                                                     MrCellY(local) + (rootY * LAB_BOX_EDGE),
                                                     MrCellZ(local) + (rootZ * LAB_BOX_EDGE));
            mismatch.cell = index;
            mismatch.cpu = a;
            mismatch.gpu = b;
            mismatch.what = std::format("刻み {} のセル ({}, {}, {}) が違う", tick, lab::LabCellX(index),
                                        lab::LabCellY(index), lab::LabCellZ(index));
            return mismatch;
        }

        // --- 細かくした子(T-0314): 枠の順 → 枠の中の番号の順(どちらも本物のブロックの所だけ)---
        for (uint32_t slot = lab::LabRootCount(shape); slot < cpu.capacity.worldBlocks; ++slot) {
            const MrBlock& block = cpu.blocks[slot];
            if (block.kind != MR_BLOCK_REAL || gpu.blocks[slot].kind != MR_BLOCK_REAL)
                continue;

            for (uint32_t local = 0; local < MR_BLOCK_CELLS; ++local) {
                const RxCell a = LoadNestCell(cpu, slot, local);
                const RxCell b = LoadNestCell(gpu, slot, local);
                if (RxSameCell(a, b))
                    continue;

                const auto x = static_cast<uint32_t>(block.originX) + MrCellX(local);
                const auto y = static_cast<uint32_t>(block.originY) + MrCellY(local);
                const auto z = static_cast<uint32_t>(block.originZ) + MrCellZ(local);
                mismatch.cell = lab::LabFineCellWord(static_cast<uint32_t>(block.level), x, y, z);
                mismatch.cpu = a;
                mismatch.gpu = b;
                mismatch.what = std::format("刻み {} のレベル {} のセル ({}, {}, {}) が違う", tick, block.level, x, y,
                                            z);
                return mismatch;
            }
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
        else if (cpu.magicPatches != gpu.magicPatches)
            field = "魔法のパッチの列";

        mismatch.what = std::format("刻み {} の{}が違う(セルは全部同じ)", tick, field);
        return mismatch;
    }

    // --- 記録 ---

    std::vector<std::byte> SerializeLabRecording(const LabRecording& recording) {
        std::vector<std::byte> bytes;
        Append(bytes, RECORDING_MAGIC);
        Append(bytes, RECORDING_VERSION);
        Append(bytes, LAB_WORLD_SEED);
        Append(bytes, recording.shape.rootsX);
        Append(bytes, recording.shape.rootsY);
        Append(bytes, recording.shape.rootsZ);
        Append(bytes, recording.shape.refineBlocks);
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
            return value == RECORDING_VERSION || value == RECORDING_VERSION_SHAPE ||
                   value == RECORDING_VERSION_TABLE_BYTES || value == RECORDING_VERSION_TABLE_MARKS ||
                   value == RECORDING_VERSION_ONE_TABLE || value == RECORDING_VERSION_NO_TABLE;
        };
        if (!Take(bytes, version) || !knownVersion(version))
            return std::unexpected(std::format("記録の版 {} は読めない", version));

        if (!Take(bytes, seed) || seed != LAB_WORLD_SEED)
            return std::unexpected("記録の見出しが壊れている");

        // --- 箱の形(版 5。T-0173)・細かくする枠の数(版 6。版 5 は 0。T-0314)---
        if (version == RECORDING_VERSION || version == RECORDING_VERSION_SHAPE) {
            if (!Take(bytes, recording.shape.rootsX) || !Take(bytes, recording.shape.rootsY) ||
                !Take(bytes, recording.shape.rootsZ) || !Take(bytes, recording.shape.refineBlocks) ||
                (version == RECORDING_VERSION_SHAPE && recording.shape.refineBlocks != 0) ||
                !lab::LabShapeValid(recording.shape))
                return std::unexpected("記録の箱の形が壊れている");
        }

        if (version != RECORDING_VERSION_NO_TABLE && !Take(bytes, recording.tableVersion))
            return std::unexpected("記録の見出しが壊れている");

        if (!Take(bytes, recording.tickCount) || !Take(bytes, commandCount) || !Take(bytes, hashCount))
            return std::unexpected("記録の見出しが壊れている");

        const uint64_t listBytes = (commandCount * COMMAND_BYTES) + (hashCount * sizeof(uint64_t));
        const bool withTables = version == RECORDING_VERSION || version == RECORDING_VERSION_SHAPE ||
                                version == RECORDING_VERSION_TABLE_BYTES;
        const bool sized = withTables ? bytes.size() >= listBytes : bytes.size() == listBytes;
        if (commandCount > bytes.size() || hashCount > bytes.size() || !sized || hashCount != recording.tickCount)
            return std::unexpected("記録の大きさが合わない");

        recording.commands.resize(commandCount);
        for (Command& command : recording.commands)
            (void)Take(bytes, command);

        recording.hashes.resize(hashCount);
        for (uint64_t& hash : recording.hashes)
            (void)Take(bytes, hash);

        if (withTables) {
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
