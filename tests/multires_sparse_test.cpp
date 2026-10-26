// multires_sparse_test.cpp — 欄をある所にだけ持つ(疎な頁。T-0921・ADR-0403・20 §10.2・§13.2 の 1)の CPU のテスト。
// 同じ場面を「疎」(MultiresNest::sparseFields = true。欄の頁は 0 でない値のある所にだけ)と「密」(全部の頁)の 2 つの世界で並べて刻み、
// 毎刻み次がビット単位で同じことを確かめる: 状態の要約(HashWholeNest)・世界の要約(HashRealLeaves・HashTruthLeaves)・
// Σ 保存量 − 帳簿(ComputeConservedTotals)・刻みの報告。
//   1. 木の場面(欄の値の半分ほどが 0・全部 0 の欄も): たくさんの要求で細かく/粗くする・静かな頁を畳む / 観察の影の鎖を覗いて引き戻す
//   2. 気体(sim/multires_gas): 空気だけ(静かな大気)/ 燃えている所(底の木箱の O2 を毎刻み CO2 に替えて熱を足す)
//   3. 液体(sim/multires_liquid): 地面と水と空気(左半分の水が右へ流れる)
//   4. 保存点: 疎の世界で途中の保存点に戻して進めると、続けた場合と毎刻み同じ要約
// 記録(M-0 へ。docs/perf.md): 場面ごとの 1 セルあたりのバイト(セルの状態 + 欄。欄は密 / 疎)。
// T-0931: セルの頁を成分の数に合わせた幅で持つ形(ADR-0417)の前後のセルの状態のバイトも出し、場面ごとの要約の束が T-0931 の前と同じことを確かめる。
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

#include "multires_test_scene.h"
#include "reaction_test_table.h"
#include "sim/multires_fields.h"
#include "sim/multires_gas.h"
#include "sim/multires_liquid.h"
#include "sim/multires_nest.h"
#include "sim/multires_state.h"
#include "standard_world_physics.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::sim;
using namespace bicameral::state_field;

namespace {

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr uint64_t SEED = 12345;
    constexpr int32_t AIR_TEMPERATURE = 300000;  // mK

    BakedReactionTable MakeTable() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.environment = StandardWorldPhysics();
        definition.atmosphere = {
            .points = {{.heightMillimeters = 0,
                        .temperatureMilliKelvin = static_cast<uint32_t>(AIR_TEMPERATURE),
                        .fractions = {{.species = "nitrogen", .partsPerMillion = 790000},
                                      {.species = "oxygen", .partsPerMillion = 210000}}}},
            .surfacePressurePascals = 101325,
        };
        definition.species.push_back({.name = "liquid_water",
                                      .composition = {{.element = "H", .count = 2}, {.element = "O", .count = 1}},
                                      .formationEnthalpy = -285830,
                                      .heatCapacity = 75300,
                                      .thermalConductivity = 600,
                                      .phase = SpeciesPhase::Liquid,
                                      .density = 1000});
        definition.species.push_back({.name = "stone",
                                      .composition = {{.element = "C", .count = 1}},
                                      .formationEnthalpy = 0,
                                      .heatCapacity = 8500,
                                      .thermalConductivity = 2000,
                                      .phase = SpeciesPhase::Solid,
                                      .density = 2000});

        auto baked = BakeReactionTable(definition);
        if (!baked) {
            std::printf("表を作れない: %s\n", baked.error().c_str());
            std::exit(1);
        }

        return std::move(*baked);
    }

    // --- 比べる・測る ---

    // 2 つの世界(疎・密)が同じか: 状態の要約・世界の要約・Σ 保存量 − 帳簿
    bool SameState(const MultiresNest& sparse, const MultiresNest& dense, const BakedReactionTable& table,
                   int32_t finestLevel) {
        return HashWholeNest(sparse) == HashWholeNest(dense) && HashRealLeaves(sparse) == HashRealLeaves(dense) &&
               HashTruthLeaves(sparse) == HashTruthLeaves(dense) &&
               ComputeConservedTotals(sparse, table, finestLevel) == ComputeConservedTotals(dense, table, finestLevel);
    }

    // --- 前とビット一致(T-0931): 場面ごとに毎刻みの状態の要約・世界の要約を 1 つに束ね、セルの持ち方を変える前の値と比べる ---
    // 値は T-0931 の前(main d35b5c4。セル = RxCell 112 B の密な並び)で測った。0 は「まだ無い」(出すだけ)。
    // T-1209 で取り直した: 7d87b630(T-1077)が帳簿の理由を 1 つ足し(WL_REASON_OBJECT_EXCHANGE)、帳簿の区画が 22 → 24 欄に伸びた
    // (足した 2 欄はこの場面では全部 0)。帳簿を前の 22 欄だけにして取ると 6 場面とも前の値
    // (0xcf8ee04b11cfb313・0x33d63cfb8ecc3101・0x18b67f0d77dea1d0・0xbdae7d98a6a1fb69・0xe9aae7d44564f3d4・0x606acadec1f56d5f)と一致した
    // = 状態の値は同じで、区画の長さだけが変わった。帳簿の理由・列の数を変えたら取り直す
    enum Scene : uint32_t { SceneStress, SceneShadow, SceneReal, SceneAir, SceneBurning, SceneLiquid, SceneCount };
    constexpr std::array<uint64_t, SceneCount> GOLDEN_DIGESTS = {0xea0a6ae8c141e9c9, 0xad8c1f1a19990bba,
                                                                 0xd5417fcbda4e221d, 0x04ad05e1a4443107,
                                                                 0x9e558f3ef5837703, 0x5706c9b62720c953};

    uint64_t AddDigest(uint64_t digest, const MultiresNest& nest) {
        digest = fx::FxHashCombine(digest, HashWholeNest(nest));

        return fx::FxHashCombine(digest, HashRealLeaves(nest));
    }

    void CheckDigest(const char* scene, Scene index, uint64_t digest) {
        std::printf("%s: 要約の束 0x%016llx(前 0x%016llx)\n", scene, static_cast<unsigned long long>(digest),
                    static_cast<unsigned long long>(GOLDEN_DIGESTS[index]));
        EXPECT(GOLDEN_DIGESTS[index] == 0 || digest == GOLDEN_DIGESTS[index]);
    }

    // 1 セルあたりのバイト(使っている所だけ。確保した容量ではない)
    struct CellBytes {
        uint64_t cells = 0;       // 本物の刻むセル(覆われていない)の数
        uint64_t stateBytes = 0;  // 見出し + セル(一様なら値 1 つ・頁なら 512)+ 端数の枠
        uint64_t fixedBytes = 0;  // 同じ、ただし頁のセルを前の持ち方(RxCell 112 B 固定)で数えたもの(T-0931 の前)
        uint64_t pageWidths = 0;  // 頁の幅の和(平均の幅を出す)
        uint64_t pages = 0;       // 頁を持つ枠の数
        NestFieldMemory fields;
    };

    // セルの頁 1 つのバイト(T-0931: エネルギー 8 B + 成分の数 1 B + 幅 × 12 B を 512 セル。持たない頁は 0)
    uint64_t CellPageBytes(const MultiresCellPage& page) {
        return page.energy.empty() ? 0
                                   : uint64_t{MR_BLOCK_CELLS} * (sizeof(int64_t) + sizeof(uint8_t) + (12 * page.width));
    }

    CellBytes MeasureBytes(const MultiresNest& nest) {
        CellBytes bytes;
        for (const MrBlock& block : nest.blocks) {
            if (block.kind == MR_BLOCK_UNUSED)
                continue;

            const uint64_t common = sizeof(MrBlock) + sizeof(reaction::RxCell) +
                                    (block.fraction == MR_NO_FRACTION ? 0
                                                                      : uint64_t{MR_BLOCK_CELLS} * sizeof(MrFraction));
            bytes.stateBytes += common;
            bytes.fixedBytes += common;
            if (!MrIsUniform(block)) {
                const MultiresCellPage& page = nest.cells.pages[block.page];
                bytes.stateBytes += CellPageBytes(page);
                bytes.fixedBytes += uint64_t{MR_BLOCK_CELLS} * sizeof(reaction::RxCell);
                bytes.pageWidths += page.width;
                bytes.pages += 1;
            }

            if (block.kind != MR_BLOCK_REAL)
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                bytes.cells += MrIsSteppedCell(block, index) ? 1 : 0;
        }

        bytes.fields = MeasureNestFields(nest);

        return bytes;
    }

    uint64_t FieldBytes(const NestFieldMemory& memory) {
        return memory.uniformBytes + memory.pageBytes + memory.tableBytes;
    }

    // 場面の 1 セルあたりのバイトを出す(小数 2 桁を整数で: 100 倍して割る)
    void PrintBytes(const char* scene, const MultiresNest& sparse, const MultiresNest& dense) {
        const CellBytes s = MeasureBytes(sparse);
        const CellBytes d = MeasureBytes(dense);
        const auto perCell = [&](uint64_t value) {
            return s.cells == 0 ? 0 : value * 100 / s.cells;
        };
        const uint64_t state = perCell(s.stateBytes);
        const uint64_t fixed = perCell(s.fixedBytes);
        const uint64_t width = s.pages == 0 ? 0 : s.pageWidths * 100 / s.pages;
        std::printf(
            "1 セルあたり %s: セルの状態 前(RxCell 112 B 固定)%llu.%02llu B → 後(成分の数に合わせた幅)%llu.%02llu B"
            "・頁 %llu・頁の幅の平均 %llu.%02llu\n",
            scene, static_cast<unsigned long long>(fixed / 100), static_cast<unsigned long long>(fixed % 100),
            static_cast<unsigned long long>(state / 100), static_cast<unsigned long long>(state % 100),
            static_cast<unsigned long long>(s.pages), static_cast<unsigned long long>(width / 100),
            static_cast<unsigned long long>(width % 100));
        EXPECT(s.stateBytes <= s.fixedBytes);
        const uint64_t denseFields = perCell(FieldBytes(d.fields));
        const uint64_t sparseFields = perCell(FieldBytes(s.fields));
        std::printf(
            "1 セルあたり %s: セル %llu・欄の数 %zu・セルの状態 %llu.%02llu B・欄 密 %llu.%02llu B / 疎 %llu.%02llu B"
            "(疎の頁 %llu B・頁の表 %llu B・一様 %llu B)・合計 密 %llu.%02llu / 疎 %llu.%02llu B\n",
            scene, static_cast<unsigned long long>(s.cells), sparse.fields.size(),
            static_cast<unsigned long long>(state / 100), static_cast<unsigned long long>(state % 100),
            static_cast<unsigned long long>(denseFields / 100), static_cast<unsigned long long>(denseFields % 100),
            static_cast<unsigned long long>(sparseFields / 100), static_cast<unsigned long long>(sparseFields % 100),
            static_cast<unsigned long long>(s.fields.pageBytes), static_cast<unsigned long long>(s.fields.tableBytes),
            static_cast<unsigned long long>(s.fields.uniformBytes),
            static_cast<unsigned long long>((state + denseFields) / 100),
            static_cast<unsigned long long>((state + denseFields) % 100),
            static_cast<unsigned long long>((state + sparseFields) / 100),
            static_cast<unsigned long long>((state + sparseFields) % 100));
        EXPECT(s.stateBytes == d.stateBytes && s.cells == d.cells);
        EXPECT(s.fields.pageBytes <= d.fields.pageBytes);
    }

    // --- 1. 木の場面 ---

    struct TreeFields {
        uint32_t amount = MULTIRES_NO_FIELD;  // セル・量(保存量)
        uint32_t face = MULTIRES_NO_FIELD;    // 面 ×3・量(保存量)
        uint32_t tag = MULTIRES_NO_FIELD;     // セル・SAME・ハッシュに入らない
        uint32_t zero = MULTIRES_NO_FIELD;    // セル・ハッシュに入る・いつも 0(疎なら頁を持たない)
        uint32_t block = MULTIRES_NO_FIELD;
        uint32_t world = MULTIRES_NO_FIELD;
    };

    TreeFields RegisterTreeFields(MultiresNest& nest) {
        constexpr uint32_t CONSERVED = SF_FLAG_CONSERVED | SF_FLAG_HASHED;
        TreeFields fields;
        fields.amount = RegisterNestField(
            nest, "amount", SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_VOLUME, CONSERVED, 1));
        fields.face = RegisterNestField(
            nest, "face", SfMakeFieldDesc(SF_PLACE_FACE, SF_TYPE_I32, SF_PHASE_LIQUID, SF_SCALE_VOLUME, CONSERVED, 3));
        fields.tag = RegisterNestField(nest, "tag",
                                       SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_U32, SF_PHASE_GAS, SF_SCALE_SAME, 0, 1));
        fields.zero = RegisterNestField(
            nest, "zero", SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_SAME, SF_FLAG_HASHED, 2));
        fields.block = RegisterNestField(
            nest, "block",
            SfMakeFieldDesc(SF_PLACE_BLOCK, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_SAME, SF_FLAG_HASHED, 1));
        fields.world = RegisterNestField(
            nest, "world",
            SfMakeFieldDesc(SF_PLACE_WORLD, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_SAME, SF_FLAG_HASHED, 2));

        return fields;
    }

    // 値の 1/3 ほどが 0 のいろいろな値(-1000〜1000)
    int64_t SparseValue(uint64_t a, uint64_t b, uint32_t purpose) {
        const uint64_t hash = fx::FxHash64(SEED, a, b, purpose);
        if (hash % 3 == 0)
            return 0;

        return static_cast<int64_t>((hash >> 8) % 2001) - 1000;
    }

    // 頁を持つ枠 slot の欄を埋める(根 slot が偶数なら tag も。奇数の根の tag は 0 のまま = 疎なら頁を持たない)
    void FillTreeFields(MultiresNest& nest, const TreeFields& fields, uint32_t slot) {
        if (MrIsUniform(nest.blocks[slot]))
            return;

        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            StoreNestField(nest, fields.amount, slot, index, 0, SparseValue(slot, index, 1));
            for (uint32_t c = 0; c < 3; ++c)
                StoreNestField(nest, fields.face, slot, index, c, SparseValue(slot, index, 2 + c));

            if (slot % 2 == 0)
                StoreNestField(nest, fields.tag, slot, index, 0, SparseValue(slot, index, 5) + 1000);
        }

        StoreNestBlockField(nest, fields.block, slot, 0, SparseValue(slot, 0, 6));
    }

    using TreeStep = std::function<void(MultiresNest&, uint64_t)>;

    // 木の場面を疎と密で並べて刻み、毎刻み比べる(根 roots 個。8 なら 2×2×2・1 なら原点だけ)
    void RunTreeScene(const BakedReactionTable& table, const char* scene, Scene sceneIndex,
                      const std::function<MultiresNest()>& make, uint32_t roots, const TreeStep& step, uint64_t ticks,
                      int32_t finestLevel) {
        std::array<MultiresNest, 2> worlds;
        for (uint32_t i = 0; i < 2; ++i) {
            MultiresNest nest = make();
            nest.sparseFields = i == 0;
            const TreeFields fields = RegisterTreeFields(nest);
            const std::vector<reaction::RxCell> cells = test::MakeMultiresRootCells(table);
            for (uint32_t root = 0; root < roots; ++root) {
                const uint32_t slot = PlaceRootBlock(nest, int64_t{root & 1u} * 8, int64_t{(root >> 1) & 1u} * 8,
                                                     int64_t{(root >> 2) & 1u} * 8, cells);
                FillTreeFields(nest, fields, slot);
            }

            StoreNestWorldField(nest, fields.world, 1, -77);
            worlds[i] = std::move(nest);
        }

        int mismatches = SameState(worlds[0], worlds[1], table, finestLevel) ? 0 : 1;
        uint64_t firstMismatch = mismatches == 0 ? UINT64_MAX : 0;
        uint64_t digest = AddDigest(0, worlds[0]);
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            step(worlds[0], tick);
            step(worlds[1], tick);
            digest = AddDigest(digest, worlds[0]);
            if (!SameState(worlds[0], worlds[1], table, finestLevel)) {
                ++mismatches;
                firstMismatch = std::min(firstMismatch, tick);
            }
        }

        std::printf("%s(%llu 刻み): 疎と密の要約が毎刻み同じ %s(違った最初の刻み %lld)・畳んだ頁 %u\n", scene,
                    static_cast<unsigned long long>(ticks), mismatches == 0 ? "はい" : "いいえ",
                    firstMismatch == UINT64_MAX ? -1LL : static_cast<long long>(firstMismatch),
                    worlds[0].counters[MR_COUNTER_FOLDED]);
        EXPECT(mismatches == 0);
        CheckDigest(scene, sceneIndex, digest);
        PrintBytes(scene, worlds[0], worlds[1]);
    }

    void TestTreeScenes(const BakedReactionTable& table) {
        // --- たくさんの要求(細かく・粗く・無効)+ 静かな頁を畳む ---
        RunTreeScene(
            table, "木: たくさんの要求", SceneStress,
            [&] {
                return MakeMultiresNest(
                    test::MakeMultiresCapacity(table, test::STRESS_WORLD_BLOCKS, 0, test::STRESS_FRACTIONS));
            },
            8,
            [&](MultiresNest& nest, uint64_t tick) {
                SubmitRequests(nest, test::MakeStressRequests(nest, tick));
                FoldQuietPages(nest, tick);
                ProcessRequests(nest);
                StepActive(nest, table, test::STRESS_SEED, tick);
            },
            test::STRESS_TICKS, test::STRESS_MAX_LEVEL);

        // --- 観察の影の鎖(細かくする・毎刻み引き戻す・捨てる)+ 本物の鎖 ---
        for (const test::MultiresScenario scenario : {test::MultiresScenario::Shadow, test::MultiresScenario::Real}) {
            RunTreeScene(
                table, scenario == test::MultiresScenario::Shadow ? "木: 影の鎖" : "木: 本物の鎖",
                scenario == test::MultiresScenario::Shadow ? SceneShadow : SceneReal,
                [&] {
                    return MakeMultiresNest(test::MakeMultiresCapacity(
                        table, test::MULTIRES_WORLD_BLOCKS, test::MULTIRES_LEVELS, test::MULTIRES_FRACTION_CAPACITY));
                },
                1, [&](MultiresNest& nest, uint64_t tick) { test::StepMultiresScene(nest, table, scenario, tick); },
                test::MULTIRES_END_TICK, static_cast<int32_t>(test::MULTIRES_LEVELS));
        }
    }

    // --- 2. 気体 ---

    constexpr std::array<uint32_t, 3> GAS_SIZE = {16, 24, 16};  // 世界の軸(y が上 = WE_UP_AXIS)
    constexpr uint32_t WOOD_MIN = 6;                            // x・z
    constexpr uint32_t WOOD_BOTTOM = 1;
    constexpr uint32_t WOOD_SIZE = 3;

    struct GasWorld {
        MultiresNest nest;
        MultiresGas gas;
    };

    bool InWood(uint32_t x, uint32_t y, uint32_t z) {
        return x >= WOOD_MIN && x < WOOD_MIN + WOOD_SIZE && z >= WOOD_MIN && z < WOOD_MIN + WOOD_SIZE &&
               y >= WOOD_BOTTOM && y < WOOD_BOTTOM + WOOD_SIZE;
    }

    std::unique_ptr<GasWorld> MakeGasWorld(const BakedReactionTable& table, bool sparse, bool burning) {
        const uint32_t blocks = (GAS_SIZE[0] / 8) * (GAS_SIZE[1] / 8) * (GAS_SIZE[2] / 8);
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(table, blocks, 0, 1));
        nest.sparseFields = sparse;
        nest.edges.world = {.rootLevel = 0,
                            .lowX = 0,
                            .lowY = 0,
                            .lowZ = 0,
                            .highX = GAS_SIZE[0],
                            .highY = GAS_SIZE[1],
                            .highZ = GAS_SIZE[2],
                            .top = WE_WALL,
                            .bottom = WE_WALL,
                            .sides = WE_WALL};
        auto gas = MultiresGas::Create(nest, table, {.randomSeed = SEED, .floorHeightMillimeters = 0});
        if (!gas) {
            std::printf("気体を作れない: %s\n", gas.error().c_str());
            std::exit(1);
        }

        auto world = std::make_unique<GasWorld>(GasWorld{.nest = std::move(nest), .gas = *gas});
        const uint32_t woodSpecies = table.SpeciesId("cellulose");
        for (uint32_t bz = 0; bz < GAS_SIZE[2]; bz += 8) {
            for (uint32_t by = 0; by < GAS_SIZE[1]; by += 8) {
                for (uint32_t bx = 0; bx < GAS_SIZE[0]; bx += 8) {
                    std::vector<reaction::RxCell> cells(MR_BLOCK_CELLS);
                    for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                        const uint32_t x = bx + MrCellX(index);
                        const uint32_t y = by + MrCellY(index);
                        const uint32_t z = bz + MrCellZ(index);
                        cells[index] = world->gas.ReferenceCell(world->nest, table, y);
                        if (!burning || !InWood(x, y, z))
                            continue;

                        // 燃える木箱: 空気の 1 割と同じ物質量のセルロース・熱い(圧力は後の段で釣り合う)
                        std::vector<SpeciesAmount> amounts = {{.species = woodSpecies, .amount = 50000000}};
                        for (uint32_t i = 0; i < cells[index].speciesCount; ++i)
                            amounts.push_back(
                                {.species = cells[index].species[i], .amount = cells[index].amounts[i] * 3 / 10});

                        std::ranges::sort(amounts, {}, &SpeciesAmount::species);
                        cells[index] = MakeReactionCell(table, amounts, 1000000);
                    }

                    PlaceRootBlock(world->nest, bx, by, bz, cells);
                }
            }
        }

        return world;
    }

    // 燃やす(反応の代わり。multires_gas_test の BurnWood と同じ形): O2 の 1/8 を CO2 に替えて熱を足す
    void Burn(const BakedReactionTable& table, GasWorld& world) {
        const uint32_t oxygen = table.SpeciesId("oxygen");
        const uint32_t carbonDioxide = table.SpeciesId("carbon_dioxide");
        for (uint32_t y = WOOD_BOTTOM; y < WOOD_BOTTOM + WOOD_SIZE; ++y) {
            for (uint32_t z = WOOD_MIN; z < WOOD_MIN + WOOD_SIZE; ++z) {
                for (uint32_t x = WOOD_MIN; x < WOOD_MIN + WOOD_SIZE; ++x) {
                    const uint32_t slot = LookupBlock(world.nest, 0, x & ~7u, y & ~7u, z & ~7u);
                    const uint32_t index = MrCellIndex(x & 7u, y & 7u, z & 7u);
                    const reaction::RxCell cell = LoadNestCell(world.nest, slot, index);
                    std::vector<SpeciesAmount> amounts;
                    uint64_t burned = 0;
                    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                        uint64_t amount = cell.amounts[i];
                        if (cell.species[i] == oxygen) {
                            burned = amount / 8;
                            amount -= burned;
                        }

                        amounts.push_back({.species = cell.species[i], .amount = amount});
                    }

                    const auto found = std::ranges::find(amounts, carbonDioxide, &SpeciesAmount::species);
                    if (found != amounts.end())
                        found->amount += burned;
                    else
                        amounts.push_back({.species = carbonDioxide, .amount = burned});

                    std::erase_if(amounts, [](const SpeciesAmount& amount) { return amount.amount == 0; });
                    std::ranges::sort(amounts, {}, &SpeciesAmount::species);
                    reaction::RxCell after = reaction::RxMakeEmptyCell(cell.energy + 40000);
                    for (uint32_t i = 0; i < amounts.size(); ++i) {
                        after.species[i] = amounts[i].species;
                        after.amounts[i] = amounts[i].amount;
                    }

                    after.speciesCount = static_cast<uint32_t>(amounts.size());
                    EXPECT(ExpandNestPage(world.nest, slot));
                    StoreNestCell(world.nest, slot, index, after);
                    PokeNestBlock(world.nest, slot);
                }
            }
        }
    }

    void StepGas(const BakedReactionTable& table, GasWorld& world, bool burning, uint64_t tick,
                 MultiresGasStepReport& report) {
        if (burning)
            Burn(table, world);

        std::vector<uint32_t> changedSlots;
        report = world.gas.Step(world.nest, table, tick, changedSlots);
        FoldQuietPages(world.nest, tick);  // 静かな頁を畳む(疎は全部 0 の欄の頁を返す)
    }

    bool SameGasReport(const MultiresGasStepReport& a, const MultiresGasStepReport& b) {
        return a.gasSpecies == b.gasSpecies && a.changedCells == b.changedCells && a.restedCells == b.restedCells &&
               a.blockedCells == b.blockedCells && a.partialCells == b.partialCells &&
               a.energyLimitedFaces == b.energyLimitedFaces;
    }

    void TestGasScene(const BakedReactionTable& table, bool burning, uint64_t ticks) {
        const char* scene = burning ? "気体: 燃えている所" : "気体: 空気だけ";
        const std::unique_ptr<GasWorld> sparse = MakeGasWorld(table, true, burning);
        const std::unique_ptr<GasWorld> dense = MakeGasWorld(table, false, burning);
        int mismatches = SameState(sparse->nest, dense->nest, table, 0) ? 0 : 1;
        uint64_t digest = AddDigest(0, sparse->nest);
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            MultiresGasStepReport sparseReport;
            MultiresGasStepReport denseReport;
            StepGas(table, *sparse, burning, tick, sparseReport);
            StepGas(table, *dense, burning, tick, denseReport);
            digest = AddDigest(digest, sparse->nest);
            mismatches += SameGasReport(sparseReport, denseReport) && SameState(sparse->nest, dense->nest, table, 0)
                              ? 0
                              : 1;
        }

        std::printf("%s(%llu 刻み): 疎と密の要約・報告が毎刻み同じ %s・運動エネルギー %lld mJ\n", scene,
                    static_cast<unsigned long long>(ticks), mismatches == 0 ? "はい" : "いいえ",
                    static_cast<long long>(sparse->gas.KineticEnergySum(sparse->nest)));
        EXPECT(mismatches == 0);
        if (burning)
            EXPECT(sparse->gas.KineticEnergySum(sparse->nest) > 0);

        CheckDigest(scene, burning ? SceneBurning : SceneAir, digest);

        PrintBytes(scene, sparse->nest, dense->nest);
    }

    // --- 3. 液体: 地面と水と空気 ---

    constexpr std::array<uint32_t, 3> LIQUID_SIZE = {16, 16, 16};
    constexpr uint64_t FULL_WATER = 6900000000;  // µmol(multires_liquid_test と同じ)

    enum class Kind : uint8_t { Air, Water, Stone };

    Kind GroundKind(uint32_t x, uint32_t y) {
        if (y <= 2)
            return Kind::Stone;

        return x < 8 && y <= 6 ? Kind::Water : Kind::Air;
    }

    reaction::RxCell MakeLiquidCell(const BakedReactionTable& table, Kind kind) {
        std::vector<SpeciesAmount> amounts = {{.species = table.SpeciesId("nitrogen"), .amount = 4000000},
                                              {.species = table.SpeciesId("oxygen"), .amount = 1000000}};
        if (kind == Kind::Water)
            amounts.push_back({.species = table.SpeciesId("liquid_water"), .amount = FULL_WATER});

        if (kind == Kind::Stone)
            amounts = {{.species = table.SpeciesId("stone"), .amount = 21000000000}};

        std::ranges::sort(amounts, {}, &SpeciesAmount::species);

        return MakeReactionCell(table, amounts, AIR_TEMPERATURE);
    }

    struct LiquidWorld {
        MultiresNest nest;
        MultiresLiquid liquid;
    };

    std::unique_ptr<LiquidWorld> MakeLiquidWorld(const BakedReactionTable& table, bool sparse) {
        const uint32_t blocks = (LIQUID_SIZE[0] / 8) * (LIQUID_SIZE[1] / 8) * (LIQUID_SIZE[2] / 8);
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(table, blocks, 0, 1));
        nest.sparseFields = sparse;
        auto liquid = MultiresLiquid::Create(nest, table, {});
        if (!liquid) {
            std::printf("液体を作れない: %s\n", liquid.error().c_str());
            std::exit(1);
        }

        auto world = std::make_unique<LiquidWorld>(LiquidWorld{.nest = std::move(nest), .liquid = *liquid});
        for (uint32_t bz = 0; bz < LIQUID_SIZE[2]; bz += 8) {
            for (uint32_t by = 0; by < LIQUID_SIZE[1]; by += 8) {
                for (uint32_t bx = 0; bx < LIQUID_SIZE[0]; bx += 8) {
                    std::vector<reaction::RxCell> cells(MR_BLOCK_CELLS);
                    for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                        cells[index] = MakeLiquidCell(table, GroundKind(bx + MrCellX(index), by + MrCellY(index)));

                    PlaceRootBlock(world->nest, bx, by, bz, cells);
                }
            }
        }

        return world;
    }

    bool SameLiquidReport(const MultiresLiquidStepReport& a, const MultiresLiquidStepReport& b) {
        return a.changedCells == b.changedCells && a.movedMilligrams == b.movedMilligrams && a.edge == b.edge &&
               a.edgeMilligrams == b.edgeMilligrams && a.wall == b.wall && a.gravity == b.gravity &&
               a.friction == b.friction;
    }

    void TestLiquidScene(const BakedReactionTable& table, uint64_t ticks) {
        const char* scene = "液体: 地面と水と空気";
        const std::unique_ptr<LiquidWorld> sparse = MakeLiquidWorld(table, true);
        const std::unique_ptr<LiquidWorld> dense = MakeLiquidWorld(table, false);
        int mismatches = SameState(sparse->nest, dense->nest, table, 0) ? 0 : 1;
        uint64_t digest = AddDigest(0, sparse->nest);
        uint64_t moved = 0;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            std::vector<uint32_t> changedSlots;
            const MultiresLiquidStepReport a = sparse->liquid.Step(sparse->nest, table, tick, changedSlots);
            const MultiresLiquidStepReport b = dense->liquid.Step(dense->nest, table, tick, changedSlots);
            FoldQuietPages(sparse->nest, tick);
            FoldQuietPages(dense->nest, tick);
            digest = AddDigest(digest, sparse->nest);
            moved += a.movedMilligrams;
            mismatches += SameLiquidReport(a, b) && SameState(sparse->nest, dense->nest, table, 0) ? 0 : 1;
        }

        std::printf("%s(%llu 刻み): 疎と密の要約・報告が毎刻み同じ %s・動いた液体 のべ %llu mg\n", scene,
                    static_cast<unsigned long long>(ticks), mismatches == 0 ? "はい" : "いいえ",
                    static_cast<unsigned long long>(moved));
        EXPECT(mismatches == 0);
        EXPECT(moved > 0);
        CheckDigest(scene, SceneLiquid, digest);
        PrintBytes(scene, sparse->nest, dense->nest);
    }

    // --- 4. 保存点(疎の世界。燃えている所の途中から)---

    void TestSavePoint(const BakedReactionTable& table) {
        const std::unique_ptr<GasWorld> world = MakeGasWorld(table, true, true);
        MultiresGasStepReport report;
        for (uint64_t tick = 0; tick < 6; ++tick)
            StepGas(table, *world, true, tick, report);

        const MultiresSaveState save = SaveNestState(world->nest);
        std::vector<uint64_t> hashes;
        for (uint64_t tick = 6; tick < 12; ++tick) {
            StepGas(table, *world, true, tick, report);
            hashes.push_back(HashWholeNest(world->nest));
        }

        EXPECT(RestoreNestState(world->nest, save));
        int mismatches = 0;
        for (uint64_t tick = 6; tick < 12; ++tick) {
            StepGas(table, *world, true, tick, report);
            mismatches += HashWholeNest(world->nest) == hashes[tick - 6] ? 0 : 1;
        }

        std::printf("保存点(疎・燃えている所): 戻して 6 刻み 毎刻み同じ要約 %s\n", mismatches == 0 ? "はい" : "いいえ");
        EXPECT(mismatches == 0);

        // 疎の保存点は密の世界には戻さない(置き場の形が違う。形が違えば何もせず false)
        const std::unique_ptr<GasWorld> dense = MakeGasWorld(table, false, true);
        const uint64_t before = HashWholeNest(dense->nest);
        EXPECT(!RestoreNestState(dense->nest, save) && HashWholeNest(dense->nest) == before);
    }

    // --- 5. 置き場の端: 0 を書いても頁を作らない・全部 0 に戻った頁は眠る判定で返す・返した頁を使い回す ---

    void TestStoreEdges(const BakedReactionTable& table) {
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(table, 4, 0, 1));
        const TreeFields fields = RegisterTreeFields(nest);
        const uint32_t slot = PlaceRootBlock(nest, 0, 0, 0, test::MakeMultiresRootCells(table));
        const MultiresFieldStore& store = nest.fields[fields.amount].store;
        EXPECT(store.sparse && FieldPageValues(store, nest.blocks[slot].page) == nullptr);

        StoreNestField(nest, fields.amount, slot, 3, 0, 0);
        EXPECT(FieldPageValues(store, nest.blocks[slot].page) == nullptr);

        StoreNestField(nest, fields.amount, slot, 3, 0, 5);
        EXPECT(FieldPageValues(store, nest.blocks[slot].page) != nullptr &&
               LoadNestField(nest, fields.amount, slot, 3, 0) == 5 &&
               LoadNestField(nest, fields.amount, slot, 4, 0) == 0);
        const size_t pagesBefore = store.pages.size();

        StoreNestField(nest, fields.amount, slot, 3, 0, 0);
        nest_fields::ReleaseZeroPages(nest, slot);
        EXPECT(FieldPageValues(store, nest.blocks[slot].page) == nullptr && store.freePages.size() == 1);

        StoreNestField(nest, fields.amount, slot, 7, 0, -9);
        EXPECT(store.pages.size() == pagesBefore && store.freePages.empty() &&
               LoadNestField(nest, fields.amount, slot, 7, 0) == -9 &&
               LoadNestField(nest, fields.amount, slot, 3, 0) == 0);
    }

}  // namespace

int main() {
    const BakedReactionTable table = MakeTable();
    TestStoreEdges(table);
    TestTreeScenes(table);
    TestGasScene(table, false, 12);
    TestGasScene(table, true, 30);
    TestLiquidScene(table, 240);
    TestSavePoint(table);

    if (failureCount != 0) {
        std::printf("multires_sparse_test: FAILED (%d 件)\n", failureCount);
        return 1;
    }

    std::printf("multires_sparse_test: OK\n");

    return 0;
}
