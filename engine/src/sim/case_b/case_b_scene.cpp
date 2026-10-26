// case_b_scene.cpp — 案 B の試作の場面(B1。T-0867)。何をするかは case_b_scene.h。
// 場面の絵は描く道具(Canvas。四角を塗る)で作る(サイフォンの頂上の高さのように、形を引数で変えられるように)。
// 細かくする口(T-1106): 0.5 m の絵から 0.25 m の箱を作る時、詰まり 1/10〜9/10 の表面のセルの粒を 4 つの子に同じ詰まりで配ると、
// 上の子が「支えの無い詰まり切らないセル」になって初めから落ち、空気の重い流れを作った(T-1105)。粒と液体は下の子から詰めて配る。
// 寄せ方は多重解像度と同じ sim::PlaceRefineCell(D-664。T-1166 で試作の SettleColumn・SettlePictureCell から置き換えた。答えは同じ)。
#include "sim/case_b/case_b_scene.h"

#include <algorithm>
#include <format>
#include <utility>

#include "common/fixed.hlsli"
#include "core/aliases.h"

namespace bicameral::sim::case_b {

    namespace {

        constexpr int32_t SCENE_MIN_LEVEL = -2;
        constexpr int32_t SCENE_MAX_LEVEL = 2;  // 0.125 m(箱の細かさの上限。砂時計の口を 4〜8 セルに。T-0998)
        constexpr uint64_t ATMOSPHERE = (uint64_t)ATMOSPHERE_PA;

        // 絵(0.5 m のセル。y は下から)
        class Canvas {
        public:
            Canvas(uint32_t width, uint32_t height) : m_rows(height, std::string(width, '.')) {}

            // [x0, x1]・[y0, y1](両端を含む)を塗る
            void Fill(uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1, char symbol) {
                for (uint32_t y = y0; y <= y1; ++y) {
                    for (uint32_t x = x0; x <= x1; ++x)
                        m_rows[m_rows.size() - 1 - y][x] = symbol;
                }
            }

            [[nodiscard]] std::vector<std::string> Rows() const { return m_rows; }

        private:
            std::vector<std::string> m_rows;
        };

        // 凡例(. = 大気・# = 石・~ = 水・: = 詰まった乾いた砂)
        std::vector<LegendEntry> StandardLegend() {
            const std::vector<GasFill> air = {{.material = "air", .molePermille = 1000}};
            return {
                {.symbol = '.', .condensed = {}, .gasPressurePa = ATMOSPHERE, .gas = air},
                {.symbol = '#', .condensed = {{.material = "stone", .volumePct = 100}}, .gasPressurePa = 0, .gas = {}},
                {.symbol = '~', .condensed = {{.material = "water", .volumePct = 100}}, .gasPressurePa = 0, .gas = {}},
                {.symbol = ':',
                 .condensed = {{.material = "sand", .volumePct = 60}},
                 .gasPressurePa = ATMOSPHERE,
                 .gas = air},
            };
        }

        const LegendEntry* FindLegend(const Scene& scene, char symbol) {
            for (const LegendEntry& entry : scene.legend) {
                if (entry.symbol == symbol)
                    return &entry;
            }

            return nullptr;
        }

        // 1 つの箱のセルに入れる量(絵のセルの割合の和。count 個の絵のセルの平均にする)
        struct CellFill {
            std::vector<uint64_t> condensedPct;  // 物質ごとの体積の % の和
            std::vector<uint64_t>
                gasMassPerVolume;  // 物質ごとの気体の質量の和(セルの体積 × 空き % を count × 100 で割る前)
            uint64_t bodyPct = 0;
            uint32_t count = 0;
        };

        void AddPictureCell(const Box& box, const LegendEntry& entry, uint64_t cellVolumeMm3, uint32_t temperature,
                            CellFill& fill) {
            uint64_t taken = entry.bodyPct;
            for (const VolumeFill& volume : entry.condensed) {
                fill.condensedPct[MaterialIndex(box, volume.material)] += volume.volumePct;
                taken += volume.volumePct;
            }

            fill.bodyPct += entry.bodyPct;
            ++fill.count;
            if (taken >= 100 || entry.gasPressurePa == 0)
                return;

            // 空きの体積(セル × 空き %)に、圧力と温度の気体(物質量の割合で分ける)
            const uint64_t openVolume = cellVolumeMm3 * (100 - taken) / 100;
            for (const GasFill& gas : entry.gas) {
                const uint32_t index = MaterialIndex(box, gas.material);
                const int64_t mass = IdealGasMassMg(entry.gasPressurePa, openVolume,
                                                    box.materials[index].molarMassMgPerMol, temperature);
                fill.gasMassPerVolume[index] += (uint64_t)mass * gas.molePermille / 1000;
            }
        }

        void StoreCell(Box& box, uint32_t cell, const CellFill& fill, uint64_t cellVolumeMm3, uint32_t temperature) {
            for (uint32_t index = 0; index < box.materials.size(); ++index) {
                const Material& material = box.materials[index];
                // 体積(mm³)× 密度(kg/m³ = 1e-3 mg/mm³)
                const uint64_t volume = cellVolumeMm3 * fill.condensedPct[index] / (100 * (uint64_t)fill.count);
                uint64_t mass = volume * material.densityKgPerM3 / 1000;
                mass += fill.gasMassPerVolume[index] / fill.count;
                CellMass(box, cell, index) = (int64_t)mass;
            }

            box.bodyOccupancyQ16[cell] = (uint32_t)((fill.bodyPct << 16) / (100 * (uint64_t)fill.count));
            box.energyMillijoules[cell] = ThermalEnergyMillijoules(box, cell, temperature);
        }

        // --- 細かくする口(T-1106)---
        // 質量 → 物質そのものの体積(mm³。密度 kg/m³ = 1e-3 mg/mm³)
        uint64_t SubstanceVolumeMm3(const Material& material, int64_t massMg) {
            if (material.densityKgPerM3 == 0 || massMg <= 0)
                return 0;

            return (uint64_t)massMg * 1000 / material.densityKgPerM3;
        }

        // 細かくする時(T-1106)の絵の 1 セル: 子 childCount 個(n × n)に同じ数で配った時の子 1 つの量(StoreCell と同じ丸め)× 子の数 = 親の量と、
        // 沈み固まった詰まり(Q16。1 = 子の体積の和。隣の傾きを読む。据わる固体・物 = 1・舞う粒 = 0)。寄せるか・寄せ方は
        // sim::PlaceRefineCell(D-664 の置かれ方で配る。T-1166 で wt3 の SettlePictureCell から置き換えた。答えは同じ)
        struct PictureSettle {
            bool blocked = false;     // 物を含む(寄せない)
            int64_t fullnessQ16 = 0;  // SfSettleFullnessQ16
            std::vector<int64_t> massMg;
        };

        PictureSettle ReadPictureCell(const Box& box, std::span<const PlaceMaterial> places, const LegendEntry& entry,
                                      uint32_t childCount, uint64_t childVolumeMm3) {
            PictureSettle cell;
            cell.massMg.assign(box.materials.size(), 0);
            if (entry.suspended)
                return cell;

            std::vector<uint64_t> pct(box.materials.size(), 0);
            for (const VolumeFill& volume : entry.condensed)
                pct[MaterialIndex(box, volume.material)] += volume.volumePct;

            for (uint32_t index = 0; index < box.materials.size(); ++index) {
                const Material& material = box.materials[index];
                const uint64_t volume = childVolumeMm3 * pct[index] / 100;
                cell.massMg[index] = (int64_t)(volume * material.densityKgPerM3 / 1000 * childCount);
            }

            cell.blocked = entry.bodyPct > 0;
            const state_field::SfSettleSum sum = PlaceSettleSum(places, cell.massMg);
            cell.fullnessQ16 = state_field::SfSettleFullnessQ16(
                state_field::SfSettledVolume(sum), childVolumeMm3 * childCount, sum.anchored || cell.blocked);

            return cell;
        }

        // 絵のセル (x, y) の表面の傾き(Q16。右へ絵の 1 セルで上がる高さ。y − 1〜y + 1 の詰まりの和・minmod)。絵の外は下 = 詰まり切る・上 = 空
        int64_t PictureSlopeQ16(std::span<const PictureSettle> cells, uint32_t width, uint32_t x, uint32_t y) {
            const auto rows = (uint32_t)(cells.size() / width);
            const auto surface = [&](uint32_t column) {
                const int64_t below = y == 0 ? state_field::SF_SETTLE_FULL_Q16
                                             : cells[((size_t)(y - 1) * width) + column].fullnessQ16;
                const int64_t above = y + 1 >= rows ? 0 : cells[((size_t)(y + 1) * width) + column].fullnessQ16;

                return state_field::SfColumnSurfaceQ16(below, cells[((size_t)y * width) + column].fullnessQ16, above);
            };

            if (x == 0 || x + 1 >= width)
                return 0;

            return state_field::SfSurfaceSlopeQ16(surface(x - 1), surface(x), surface(x + 1), true);
        }

        // 子のセルに凝縮相を書き、残りの空きに凡例の圧力の気体(AddPictureCell と同じ式)・熱
        void WriteSettledChild(Box& box, uint32_t cell, const LegendEntry& entry, std::span<const int64_t> massMg,
                               uint64_t cellVolumeMm3, uint32_t temperature) {
            uint64_t taken = 0;
            for (uint32_t index = 0; index < box.materials.size(); ++index) {
                CellMass(box, cell, index) = massMg[index];
                taken += SubstanceVolumeMm3(box.materials[index], massMg[index]);
            }

            if (entry.gasPressurePa > 0 && taken < cellVolumeMm3) {
                const uint64_t openVolume = cellVolumeMm3 - taken;
                for (const GasFill& gas : entry.gas) {
                    const uint32_t index = MaterialIndex(box, gas.material);
                    const int64_t mass = IdealGasMassMg(entry.gasPressurePa, openVolume,
                                                        box.materials[index].molarMassMgPerMol, temperature);
                    CellMass(box, cell, index) += (int64_t)((uint64_t)mass * gas.molePermille / 1000);
                }
            }

            box.bodyOccupancyQ16[cell] = 0;
            box.energyMillijoules[cell] = ThermalEnergyMillijoules(box, cell, temperature);
        }

        // 細かくする時(T-1106): 寄せる絵のセルの子を全部書き、書いた子に印を付ける(残りは呼ぶ側が同じ数で配る。T-1106 の前と同じビット)。
        // 重さの向きは −y(BuildScene の箱の既定)
        void StoreSettledCells(const Scene& scene, Box& box, uint32_t fine, uint64_t cellVolumeMm3,
                               std::vector<uint8_t>& written) {
            const uint32_t width = SceneWidth(scene);
            const uint32_t height = SceneHeight(scene);
            const std::vector<PlaceMaterial> places = ScenePlaceMaterials(box.materials);
            std::vector<PictureSettle> cells;
            cells.reserve((size_t)width * height);
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    const LegendEntry& entry = *FindLegend(scene, scene.rows[height - 1 - y][x]);
                    cells.push_back(ReadPictureCell(box, places, entry, fine * fine, cellVolumeMm3));
                }
            }

            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    const PictureSettle& cell = cells[((size_t)y * width) + x];
                    // 箱は 2 次元(z の子は 1 つ)。子の番号は PlacedChildren::ChildIndex(列, 0, 段)
                    const PlaceCell place = {.massMg = cell.massMg,
                                             .slopeXQ16 = PictureSlopeQ16(cells, width, x, y),
                                             .slopeZQ16 = 0,
                                             .blocked = cell.blocked};
                    const PlacedChildren children = PlaceRefineCell(places, place, fine, 1, cellVolumeMm3);
                    if (!children.settled)
                        continue;

                    const LegendEntry& entry = *FindLegend(scene, scene.rows[height - 1 - y][x]);
                    for (uint32_t j = 0; j < fine; ++j) {
                        for (uint32_t row = 0; row < fine; ++row) {
                            const uint32_t child = CellIndex(box, (x * fine) + j, (y * fine) + row, 0);
                            WriteSettledChild(box, child, entry, children.massMg[children.ChildIndex(j, 0, row)],
                                              cellVolumeMm3, scene.temperatureMilliKelvin);
                            written[child] = 1;
                        }
                    }
                }
            }
        }

        std::expected<void, std::string> CheckScene(const Scene& scene, int32_t level) {
            if (level < SCENE_MIN_LEVEL || level > SCENE_MAX_LEVEL)
                return std::unexpected(
                    std::format("場面のレベル {} が {}〜{} の外", level, SCENE_MIN_LEVEL, SCENE_MAX_LEVEL));

            const uint32_t width = SceneWidth(scene);
            for (const std::string& row : scene.rows) {
                if (row.size() != width)
                    return std::unexpected(std::format("{}: 絵の行の長さがそろっていない", scene.name));

                for (const char symbol : row) {
                    if (FindLegend(scene, symbol) == nullptr)
                        return std::unexpected(std::format("{}: 凡例に無い文字 '{}'", scene.name, symbol));
                }
            }

            for (const LegendEntry& entry : scene.legend) {
                const auto known = [&](const std::string& name) {
                    return rng::any_of(scene.materials, [&](const MaterialDefinition& m) { return m.name == name; });
                };
                for (const VolumeFill& volume : entry.condensed) {
                    if (!known(volume.material))
                        return std::unexpected(
                            std::format("{}: 凡例 '{}' の物質 {} が無い", scene.name, entry.symbol, volume.material));
                }

                for (const GasFill& gas : entry.gas) {
                    if (!known(gas.material))
                        return std::unexpected(
                            std::format("{}: 凡例 '{}' の物質 {} が無い", scene.name, entry.symbol, gas.material));
                }
            }

            const uint32_t factor = level < 0 ? 1u << (-level) : 1u;
            if (width % factor != 0 || SceneHeight(scene) % factor != 0)
                return std::unexpected(std::format("{}: 絵の大きさが {} で割り切れない", scene.name, factor));

            return {};
        }

        MaterialDefinition MakeMaterial(std::string name, WrittenPhase phase, uint32_t molarMass,
                                        uint32_t heatCapacity) {
            MaterialDefinition definition;
            definition.name = std::move(name);
            definition.phase = phase;
            definition.molarMassMgPerMol = molarMass;
            definition.heatCapacity = heatCapacity;

            return definition;
        }

    }  // namespace

    uint32_t SceneWidth(const Scene& scene) {
        return scene.rows.empty() ? 0 : (uint32_t)scene.rows.front().size();
    }

    uint32_t SceneHeight(const Scene& scene) {
        return (uint32_t)scene.rows.size();
    }

    std::vector<PlaceMaterial> ScenePlaceMaterials(std::span<const Material> materials) {
        std::vector<PlaceMaterial> places;
        places.reserve(materials.size());
        for (const Material& material : materials) {
            const uint32_t kind = material.phase == Phase::Gas      ? state_field::SF_SETTLE_GAS
                                  : material.phase == Phase::Liquid ? state_field::SF_SETTLE_LIQUID
                                  : material.phase == Phase::Grain  ? state_field::SF_SETTLE_GRAIN
                                                                    : state_field::SF_SETTLE_ANCHORED;
            places.push_back({.kind = kind,
                              .densityKgPerM3 = material.densityKgPerM3,
                              .packedFractionPct = material.packedFractionPct});
        }

        return places;
    }

    std::expected<Box, std::string> BuildScene(const Scene& scene, int32_t level, MomentumLayout layout,
                                               Diagnostics* diagnostics) {
        if (auto checked = CheckScene(scene, level); !checked)
            return std::unexpected(checked.error());

        const uint32_t coarse = level < 0 ? 1u << (-level) : 1u;  // 箱の 1 セルが覆う絵のセル(1 辺)
        const uint32_t fine = level > 0 ? 1u << level : 1u;       // 絵の 1 セルを分ける数(1 辺)
        BoxConfig config;
        config.sizeX = SceneWidth(scene) * fine / coarse;
        config.sizeY = SceneHeight(scene) * fine / coarse;
        config.sizeZ = 1;
        config.level = level;
        config.layout = layout;
        auto created = CreateBox(config, scene.materials, scene.environment, diagnostics);
        if (!created)
            return std::unexpected(std::format("{}: {}", scene.name, created.error()));

        Box box = std::move(*created);
        const uint64_t cellVolume = CellVolumeMm3(box);
        const uint32_t height = SceneHeight(scene);
        std::vector<uint8_t> written(CellCount(box), 0);
        if (fine > 1 && scene.refineFill == RefineFill::Settle)
            StoreSettledCells(scene, box, fine, cellVolume, written);

        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            if (written[cell] != 0)
                continue;

            const std::array<uint32_t, 3> at = CellCoordinates(box, cell);
            CellFill fill;
            fill.condensedPct.assign(box.materials.size(), 0);
            fill.gasMassPerVolume.assign(box.materials.size(), 0);
            for (uint32_t dy = 0; dy < coarse; ++dy) {
                for (uint32_t dx = 0; dx < coarse; ++dx) {
                    const uint32_t x = (at[0] * coarse / fine) + dx;
                    const uint32_t y = (at[1] * coarse / fine) + dy;
                    const char symbol = scene.rows[height - 1 - y][x];
                    AddPictureCell(box, *FindLegend(scene, symbol), cellVolume, scene.temperatureMilliKelvin, fill);
                }
            }

            StoreCell(box, cell, fill, cellVolume, scene.temperatureMilliKelvin);
        }

        ResetLedger(box);

        return box;
    }

    std::optional<BoxRegion> FindRegion(const Scene& scene, std::string_view name, int32_t level) {
        for (const SceneRegion& region : scene.regions) {
            if (region.name != name)
                continue;

            const uint32_t coarse = level < 0 ? 1u << (-level) : 1u;
            const uint32_t fine = level > 0 ? 1u << level : 1u;
            const auto scale = [&](uint32_t value) {
                return value * fine / coarse;
            };

            return BoxRegion{.xBegin = scale(region.xBegin),
                             .xEnd = scale(region.xEnd),
                             .yBegin = scale(region.yBegin),
                             .yEnd = scale(region.yEnd)};
        }

        return std::nullopt;
    }

    // --- 物質 ---
    std::vector<MaterialDefinition> StandardMaterials() {
        MaterialDefinition air = MakeMaterial("air", WrittenPhase::Gas, 28964, 29100);
        air.viscosityMicroPascalSec = 18;

        MaterialDefinition water = MakeMaterial("water", WrittenPhase::Liquid, 18015, 75300);
        water.densityKgPerM3 = 1000;
        water.viscosityMicroPascalSec = 890;

        // 粒の density は粒そのものの密度(仮。詰まり 60 % で積もった砂は 1590 kg/m³。T-0866 の問い)
        MaterialDefinition sand = MakeMaterial("sand", WrittenPhase::Solid, 60084, 44400);
        sand.densityKgPerM3 = 2650;
        sand.grainSizeUm = 300;
        sand.reposeAngleDegrees = 34;
        sand.cohesionPa = 0;
        sand.packedFractionPct = 60;

        MaterialDefinition stone = MakeMaterial("stone", WrittenPhase::Solid, 60084, 44400);
        stone.densityKgPerM3 = 2600;

        return {air, water, sand, stone};
    }

    namespace {

        Scene BaseScene(std::string name, std::string note) {
            Scene scene;
            scene.name = std::move(name);
            scene.note = std::move(note);
            scene.materials = StandardMaterials();
            scene.legend = StandardLegend();

            return scene;
        }

    }  // namespace

    // --- 場面 ---
    Scene MakeSandPileScene() {
        Scene scene = BaseScene(
            "sand_pile",
            "砂山 4 列 × 12 段が崩れる。R3: 崩れる角度 34° ±1 セル・60 秒でほぼ止まる・0.5 m と 0.25 m で同じ角度");
        Canvas canvas(32, 16);
        canvas.Fill(0, 31, 0, 0, '#');
        canvas.Fill(14, 17, 1, 12, ':');
        scene.rows = canvas.Rows();
        scene.regions = {{.name = "pile", .xBegin = 0, .xEnd = 32, .yBegin = 1, .yEnd = 16}};

        return scene;
    }

    Scene MakeSandLeveeScene() {
        Scene scene = BaseScene(
            "sand_levee",
            "砂の堤(台形)の左に水。R2: 染みる(Darcy)・R4: 半分濡れて急な面・満ちてだれる。浸食・決壊の溝は 2 周目(R5)");
        Canvas canvas(32, 12);
        canvas.Fill(0, 31, 0, 0, '#');
        canvas.Fill(8, 23, 1, 1, ':');
        canvas.Fill(10, 21, 2, 2, ':');
        canvas.Fill(12, 19, 3, 3, ':');
        canvas.Fill(14, 17, 4, 4, ':');
        canvas.Fill(0, 7, 1, 1, '~');
        canvas.Fill(0, 9, 2, 2, '~');
        canvas.Fill(0, 11, 3, 3, '~');
        scene.rows = canvas.Rows();
        scene.regions = {{.name = "upstream", .xBegin = 0, .xEnd = 8, .yBegin = 1, .yEnd = 12},
                         {.name = "levee", .xBegin = 8, .xEnd = 24, .yBegin = 1, .yEnd = 12},
                         {.name = "downstream", .xBegin = 24, .xEnd = 32, .yBegin = 1, .yEnd = 12}};

        return scene;
    }

    Scene MakeDamBreakScene() {
        Scene scene = BaseScene("dam_break",
                                "水の塊 6 × 4(3 m × 2 m)が t = 0 に崩れる(T-0610 と同じ形)。R1: 向こうの壁に届く 2.13 "
                                "秒 ±5 %・壁際の最高 1.82 セル ±0.1");
        Canvas canvas(24, 8);
        canvas.Fill(0, 23, 0, 0, '#');
        canvas.Fill(0, 5, 1, 4, '~');
        scene.rows = canvas.Rows();
        scene.regions = {{.name = "wall_column", .xBegin = 23, .xEnd = 24, .yBegin = 1, .yEnd = 8},
                         {.name = "reservoir", .xBegin = 0, .xEnd = 6, .yBegin = 1, .yEnd = 8}};

        return scene;
    }

    Scene MakeSiphonScene(uint32_t crestCells) {
        // 頂上の管の中の行 = crestCells(水面 y 8 より (crestCells − 8) × 0.5 m 上)。既定 16 は 4 m 上(流れる)。28 は 10 m 上(切れるはず)
        const uint32_t crest = std::max<uint32_t>(crestCells, 10);
        Scene scene = BaseScene(
            "siphon",
            std::format("逆 U の石の管(水で満たす)。頂上は水面の {} m 上。R1・R6a: 流れ続け、約 10 m で切れる",
                        (crest - 8) / 2));
        Canvas canvas(28, crest + 4);
        canvas.Fill(0, 27, 0, 0, '#');
        canvas.Fill(1, 8, 1, 8, '~');          // 上の池
        canvas.Fill(9, 9, 1, 9, '#');          // 池の右の壁
        canvas.Fill(5, 5, 3, crest + 1, '#');  // 管の壁(左の脚の外)
        canvas.Fill(7, 7, 3, crest - 1, '#');  // 管の壁(左の脚の内)
        canvas.Fill(5, 21, crest + 1, crest + 1, '#');
        canvas.Fill(7, 19, crest - 1, crest - 1, '#');
        canvas.Fill(19, 19, 3, crest - 1, '#');
        canvas.Fill(21, 21, 3, crest + 1, '#');
        canvas.Fill(6, 6, 2, crest, '~');  // 管の中の水
        canvas.Fill(6, 20, crest, crest, '~');
        canvas.Fill(20, 20, 2, crest, '~');
        scene.rows = canvas.Rows();
        scene.regions = {{.name = "source", .xBegin = 1, .xEnd = 9, .yBegin = 1, .yEnd = crest + 4},
                         {.name = "outlet", .xBegin = 10, .xEnd = 28, .yBegin = 1, .yEnd = 3},
                         {.name = "crest", .xBegin = 6, .xEnd = 21, .yBegin = crest, .yEnd = crest + 1}};

        return scene;
    }

    Scene MakeTrappedAirScene() {
        Scene scene = BaseScene("trapped_air",
                                "水に差し込んだ逆さのコップ(潜水鐘。口は水面の 2 m 下・中の空気 3 m)。R1: "
                                "中の水位が釣り合いの計算と ±2 %(約 0.4 m 上がる。外の水位の下がりを入れて計算し直す)");
        Canvas canvas(24, 16);
        canvas.Fill(0, 23, 0, 0, '#');
        canvas.Fill(0, 23, 1, 8, '~');
        canvas.Fill(8, 15, 11, 11, '#');
        canvas.Fill(8, 8, 5, 10, '#');
        canvas.Fill(15, 15, 5, 10, '#');
        canvas.Fill(9, 14, 5, 10, '.');
        scene.rows = canvas.Rows();
        scene.regions = {{.name = "bell", .xBegin = 9, .xEnd = 15, .yBegin = 5, .yEnd = 11},
                         {.name = "outside", .xBegin = 0, .xEnd = 8, .yBegin = 1, .yEnd = 16}};

        return scene;
    }

    Scene MakePressureRelaxScene() {
        Scene scene = BaseScene("pressure_relax",
                                "閉じた空気の箱の左半分が +2 kPa(衝撃の区より弱い)。R1 の B-c: "
                                "弱い差が揺れずに戻る時間が 0.5 m と 2 m で ±10 %");
        scene.legend.push_back({.symbol = '+',
                                .condensed = {},
                                .gasPressurePa = ATMOSPHERE + 2000,
                                .gas = {{.material = "air", .molePermille = 1000}}});
        Canvas canvas(16, 8);
        canvas.Fill(0, 7, 0, 7, '+');
        scene.rows = canvas.Rows();
        scene.regions = {{.name = "high", .xBegin = 0, .xEnd = 8, .yBegin = 0, .yEnd = 8},
                         {.name = "low", .xBegin = 8, .xEnd = 16, .yBegin = 0, .yEnd = 8}};

        return scene;
    }

    Scene MakeStillPondScene() {
        Scene scene = BaseScene(
            "still_pond",
            "水深 2 m の静かな池(幅 12 m)。R1: 水面のセルの圧力の揺れが大気の 0.1 % 未満・ほぼ止まる・水位が動かない");
        Canvas canvas(24, 8);
        canvas.Fill(0, 23, 0, 0, '#');
        canvas.Fill(0, 23, 1, 4, '~');
        scene.rows = canvas.Rows();
        scene.regions = {{.name = "pond", .xBegin = 0, .xEnd = 24, .yBegin = 1, .yEnd = 8}};

        return scene;
    }

    Scene MakeConnectedVesselsScene() {
        // 左の池 10 列 × 8 段・右の池 10 列 × 2 段・間の石の壁(x 10〜13・y 3〜10)の下の 2 段が管。上の空気はつながっている。
        // 水の量 = 80 + 8 + 20 = 108 セル → 釣り合いは両側とも 5 段(2.5 m)
        Scene scene = BaseScene("connected_vessels",
                                "連通管(左 4 m・右 1 m)。R1: 両側の水位が 2.5 m にそろう(±2 %)・揺り戻しが収まる");
        Canvas canvas(24, 12);
        canvas.Fill(0, 23, 0, 0, '#');
        canvas.Fill(0, 9, 1, 8, '~');
        canvas.Fill(10, 23, 1, 2, '~');
        canvas.Fill(10, 13, 3, 10, '#');
        scene.rows = canvas.Rows();
        scene.regions = {{.name = "left", .xBegin = 0, .xEnd = 10, .yBegin = 1, .yEnd = 12},
                         {.name = "right", .xBegin = 14, .xEnd = 24, .yBegin = 1, .yEnd = 12}};

        return scene;
    }

    std::vector<Scene> AllScenes() {
        return {MakeSandPileScene(),   MakeSandLeveeScene(),     MakeDamBreakScene(),  MakeSiphonScene(),
                MakeTrappedAirScene(), MakePressureRelaxScene(), MakeStillPondScene(), MakeConnectedVesselsScene()};
    }

}  // namespace bicameral::sim::case_b
