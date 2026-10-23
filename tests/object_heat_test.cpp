// object_heat_test.cpp — 小さい物が自分の熱を持つ(物の箱の最小版)の CPU の試作(研究・T-0991・docs/design/20 §12.4・ADR-0434)。
// 問題(T-0878): 虫眼鏡の焦点の熱は 0.5 m のセルの平均に均され、焦点で燃え始めた紙が燃え続けない。
// 試作: 紙を「物」にし、物の箱(物の座標のボクセルの並び。細かさは物ごと)のボクセルが、セルと同じ中身(物質の量とエネルギー。
// RxCell。単位はそのレベルの単位 = 8^-L µmol・8^-L mJ)を真実として持つ。反応はセルと同じ関数(StepReactionCellWait)。
// 物とセルの間は「対の面」だけ(ADR-0360 §3・20 §12.2)。どれも刻みの初めの値から計算して(Jacobi)、両側に同じ値を足し引きする:
//   - まっすぐ運ぶ: レンズの光(sim/sun_lens の 2 mm の升の量)が紙の面に当たった分から、紙の照り返しを引いてボクセルへ
//   - まっすぐ運ぶ: 放射(RdPairCoefficient を紙の両面の広さに縮めて。行き先は試作では囲むセル。遠くへの行き先は 23 の家族)
//   - 広がる: 伝導(囲むセルの気体の伝導率 × 面の広さ ÷ 細かい側の半分の長さ)
//   - 広がる: 気体の混ざり(濃さの差 × 混ざる速さ × 面の広さ ÷ 細かい側の半分の長さ。O2 が入り煙が出る。運ぶ気体の熱と化学のエネルギーも一緒)
//   - 物の中: 隣のボクセルどうしの伝導(セルどうしと同じ形)
// 細かさの違う両側の量は細かい側の単位で計算し、粗い側の 1 単位に満たない分は「対の端数」に持つ(保存はビット単位)。
// 確かめる:
//   1. 燃え始めて燃え続ける: 焦点で燃え始め、レンズを外しても反応の熱で燃え続け、焦点のボクセルの燃える固体が燃え切る
//   2. 粗くした時: 同じ紙をセルに混ぜる(物の箱 = セルの細かさ)と、同じ光で燃え始めない
//   3. 保存: ボクセル + セル + 対の端数 − 帳簿(入った日の光)がエネルギーと元素で毎刻み一定(ビット単位)
//   4. 決定的: 2 回で同じ・ボクセルを逆の順に回しても同じ
//   5. 細かさ: 0.5 m と 0.25 m の世界で、燃え始めの刻みと燃え続けた時間が ±10 %(仮)
//   7. 共有の式(shaders/common/object_heat.hlsli。GPU と同じ関数。T-1064)が試作と毎刻みビット一致
//   8. 眠らせ方(T-1065): 冷えた紙は何刻みか回すと 1 刻み回しても何も変わらない
// 試験の中の仮の口(土台の共通の口は足さない。CLAUDE.md §1.9)。シミュの値は整数で、double は表示だけ。
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string_view>
#include <vector>

#include "common/heat_conduction.hlsli"
#include "common/radiation.hlsli"
#include "object_heat_cpu.h"
#include "sim/sun_lens.h"
#include "sun_column_scene.h"

namespace {

    using namespace bicameral;
    using namespace bicameral::sim;
    namespace fx = bicameral::fx;
    namespace mr = bicameral::multires;
    namespace rx = bicameral::reaction;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // --- 試験の値(公開側の試験データ。ゲームの中身ではない。D-006)---
    // レンズは sun_lens_test と同じ虫眼鏡(口の直径 10 cm・f = 0.2 m のガラス)
    constexpr uint32_t LENS_REFRACTION_PERMILLE = 1500;
    constexpr int64_t LENS_RADIUS_UM = 200000;
    constexpr int64_t LENS_APERTURE_UM = 50000;
    constexpr int64_t LENS_THICKNESS_UM = 14000;
    constexpr uint64_t LENS_MICROMOLES_PER_CELL = 26041666667;
    constexpr int64_t GROUND_TOP_UM = 500000;
    constexpr int64_t LENS_X_UM = 1100000;
    constexpr int64_t LENS_Z_UM = 1150000;
    constexpr int64_t LENS_HEIGHT_UM = 200000;  // 薄いレンズの焦点の距離 R ÷ 2(n − 1)

    // 紙: 5 × 5 のボクセル 1 層の物(厚さ 0.1 mm の紙を、地面のすぐ上に枠で支える。地面との接触は試作に入れない)
    constexpr int32_t BODY_LEVEL = 6;  // 物の箱の細かさ: 0.5 m ÷ 2^6 = 7.8 mm
    constexpr int32_t
        SHEET_SIDE = 5;  // 箱 7.8 mm(レベル 6)の時の 1 辺のボクセルの数。ほかの細かさでは同じ大きさ(約 4 cm)になる奇数
    constexpr int64_t CELL_UM = 500000;
    constexpr uint64_t
        FULL_PAPER_UNITS = 1157000000;  // 紙で満たした箱(cellulose 1500 kg/m³。sun_column_scene と同じ数)
    constexpr uint64_t PAPER_THICKNESS_UM = 100;
    constexpr uint64_t AIR_OXYGEN_UNITS = 1092000;  // 空気(sun_column_scene と同じ。どのレベルでも同じ数 = 同じ濃さ)
    constexpr uint64_t AIR_NITROGEN_UNITS = 4108000;
    constexpr uint32_t DEFAULT_DIFFUSIVITY_MM2_PER_S = 20;
    // 境の膜の厚さ(物の形と流れで決まる物理の長さ。箱の細かさによらない。T-1016 の 2 本目)。試作は 4 cm の紙の目安の定数 3.9 mm
    // (箱 7.8 mm の半分と同じ値なので、箱 7.8 mm の結果は 1 本目と同じ)。0 = 箱の半分(T-0991 の形・T-1016 の 1 本目)
    constexpr int64_t FILM_UM = 3906;  // 混ざる速さを書いていない気体(O2 の現実の値の目安)

    constexpr uint64_t WORLD_SEED = 0x0991;
    constexpr uint32_t TICK_LIMIT = 60 * 240;
    constexpr uint32_t LENS_OFF_AFTER_TICKS = 30;  // 燃え始めから 0.5 s でレンズを外す
    constexpr int64_t IGNITION_MILLIWATTS = 500;   // 焦点のボクセルの反応の熱がこれを超えた刻み = 燃え始め
    constexpr int64_t BURNING_MILLIWATTS = 100;    // どれかのボクセルの反応の熱がこれ以上の間 = 燃えている
    constexpr uint32_t QUIET_TICKS = 120;          // 燃えていない刻みがこれだけ続いたら止める

    constexpr SunLight VERTICAL_SUN{.direction = {0, -1, 0},
                                    .irradianceShortMilliwatts = test::SUN_IRRADIANCE_SHORT,
                                    .irradianceLongMilliwatts = test::SUN_IRRADIANCE_LONG};

    // --- 整数の道具 ---

    int64_t FloorDivide(int64_t value, int64_t divisor) {
        const int64_t quotient = value / divisor;

        return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
    }

    // value × multiplier ÷ divisor(符号つき。128bit の積で桁あふれしない。0 の向きに切る)
    int64_t MulDivToward0(int64_t value, uint64_t multiplier, uint64_t divisor) {
        const auto magnitude = static_cast<uint64_t>(value < 0 ? -value : value);
        const auto result = static_cast<int64_t>(
            fx::FxDivU128By64(fx::FxMulU64Full(magnitude, multiplier), divisor).quotient);

        return value < 0 ? -result : result;
    }

    uint64_t AmountOf(const rx::RxCell& cell, uint32_t species) {
        const uint32_t slot = rx::RxFindSlot(cell, species);

        return slot == rx::RX_NO_SLOT ? 0 : cell.amounts[slot];
    }

    // 物質の量を足し引きする(負になる・成分の枠があふれるなら false)
    bool AddAmount(rx::RxCell& cell, uint32_t species, int64_t delta) {
        if (delta == 0)
            return true;

        const uint32_t slot = rx::RxFindSlot(cell, species);
        if (slot == rx::RX_NO_SLOT) {
            if (delta < 0)
                return false;

            const auto added = rx::RxTryAddSpecies(cell, species, static_cast<uint64_t>(delta));
            cell = added.cell;

            return added.overflowed == 0;
        }

        const int64_t now = static_cast<int64_t>(cell.amounts[slot]) + delta;
        if (now < 0)
            return false;

        cell.amounts[slot] = static_cast<uint64_t>(now);
        cell = rx::RxCompactCell(cell);

        return true;
    }

    // --- 表と世界 ---

    BakedReactionTable MakeTable() {
        // 伝導率は現実の値(試験の倍率は T-1066 で表から外した)
        return test::MakeSunTable([](ReactionTableDefinition& definition) {
            SpeciesDefinition glass{.name = "object_heat_lens_glass",
                                    .composition = {{.element = "C", .count = 1}},
                                    .formationEnthalpy = 0,
                                    .heatCapacity = 8500,
                                    .thermalConductivity = 1000,
                                    .phase = SpeciesPhase::Solid,
                                    .density = 2500};
            glass.lightAbsorptionShort = 5;
            glass.lightAbsorptionLong = 4000000000u;
            glass.lightReflectanceShortPermille = 0;
            glass.lightReflectanceLongPermille = 0;
            glass.lightRefractionPermille = LENS_REFRACTION_PERMILLE;
            definition.species.push_back(glass);
        });
    }

    // 対の面の膜の気体の値(研究の 1 本目と 2 本目)。1 本目 Constant: 物質の値(室温の値)のまま。
    // 2 本目 KineticTheory: 膜の温度(両側の平均)で、剛体球の気体の運動論の形に変える(混ざる速さ ∝ T^1.5・伝導率 ∝ T^0.5。
    // 物質の値は室温 293.15 K の値として読む)。熱い物のまわりで O2 が速く入り、熱も少し速く逃げる
    enum class FilmLaw : uint8_t {
        Constant,
        KineticTheory,
    };

    struct FilmScale {
        uint64_t diffusionQ16 = 65536;
        uint64_t conductionQ16 = 65536;
    };

    FilmScale FilmScaleOf(FilmLaw law, int32_t voxelMilliKelvin, int32_t airMilliKelvin) {
        if (law == FilmLaw::Constant)
            return {};

        // 膜の温度の気体の値(エンジンの式 HcGasScaleAt。伝導率 ∝ T^0.5・混ざる速さ ∝ T^1.5。D-639・T-1066)
        const uint64_t film = (static_cast<uint64_t>(voxelMilliKelvin) + static_cast<uint64_t>(airMilliKelvin)) / 2;
        const rx::HcGasScale scale = rx::HcGasScaleAt(static_cast<uint32_t>(film));

        return {.diffusionQ16 = scale.diffusionQ16, .conductionQ16 = scale.conductionQ16};
    }

    // 紙の 1 辺のボクセルの数(紙の大きさを細かさによらず約 4 cm に。奇数にして焦点をボクセルの真ん中に)
    int32_t SheetSide(int32_t bodyLevel) {
        const int32_t side = bodyLevel >= 6 ? SHEET_SIDE << (bodyLevel - 6) : SHEET_SIDE >> (6 - bodyLevel);

        return std::max(side, 1) | 1;
    }

    // 物の中の隣のボクセルどうしの伝導率の決め方(T-1016 の調べ)。MoleWeighted: 今のセルと同じ(HcCellConductance。物質量で重み付けた平均)。
    // VolumeWeighted: 体積で重み付けた平行の道(固体の体積の割合 × 固体の伝導率 + 残り × 気体の伝導率)。Off: 物の中の伝導なし
    enum class InnerLaw : uint8_t {
        MoleWeighted,
        VolumeWeighted,
        Off,
    };

    // 試作の場面: 紙の物と、それを囲む空気のセル 1 つ(セルのレベルの単位)と、対の端数・帳簿
    struct Scene {
        int32_t bodyLevel = BODY_LEVEL;
        int32_t cellLevel = 0;
        FilmLaw film = FilmLaw::KineticTheory;
        InnerLaw inner = InnerLaw::VolumeWeighted;
        int64_t filmUm = FILM_UM;
        int32_t side = SHEET_SIDE;
        // 対の面の小刻み(1 刻みに何回。箱 7.8 mm で 1、細かい箱は 4^(L − 6)。広がる家族の陽の小刻み 4^σ と同じ形。T-1016)
        uint32_t substeps = 1;
        std::vector<rx::RxCell> voxels;  // [x + side × z]
        std::vector<int64_t> light;      // ボクセルが 1 刻みに吸うレンズの光(ボクセルの単位)
        rx::RxCell air{};
        int64_t energyCarry = 0;                  // 対の端数(ボクセルの単位。まだセルへ渡していない分)
        std::map<uint32_t, int64_t> amountCarry;  // 物質 ID → 対の端数(ボクセルの単位)
        int64_t sunIn = 0;                        // 帳簿: 入った日の光(ボクセルの単位)
        bool overflowed = false;                  // 量が負になる・成分の枠があふれた(試作の誤り)
        // ボクセルが最後に変わった刻み(反応の待ちの丸めの changedTick。受け渡しで変わった刻み t は t − 1、反応で変わった刻み t は t。T-1098)
        std::vector<uint64_t> changedTicks;
        uint64_t tick = 0;  // 今の刻み(StepExchange の前に書く)
    };

    int64_t FineScale(const Scene& scene) {
        return int64_t{1} << (3 * (scene.bodyLevel - scene.cellLevel));
    }

    // レンズの光を、紙のボクセルごとの 1 刻みの吸う量(ボクセルの単位)にする。升の真ん中が入るボクセルへ
    std::vector<int64_t> LensLightPerVoxel(const BakedReactionTable& table, int32_t cellLevel, int32_t bodyLevel) {
        const uint32_t roots = cellLevel == 0 ? 1u : 2u;
        const MultiresNest nest = test::MakeSunWorld(
            table, cellLevel, roots, roots, roots,
            [](int64_t, int64_t y, int64_t) { return y < 2 ? test::SunMaterial::Stone : test::SunMaterial::Air; });
        const SunLensShape shape{.centreUm = {LENS_X_UM, GROUND_TOP_UM + LENS_HEIGHT_UM, LENS_Z_UM},
                                 .apertureUm = LENS_APERTURE_UM,
                                 .frontRadiusUm = LENS_RADIUS_UM,
                                 .backRadiusUm = LENS_RADIUS_UM,
                                 .thicknessUm = LENS_THICKNESS_UM,
                                 .axis = 1,
                                 .facing = 1,
                                 .species = table.SpeciesId("object_heat_lens_glass"),
                                 .micromolesPerCell = LENS_MICROMOLES_PER_CELL,
                                 .lensLevel = 9};
        const auto focused = FocusSunLight(nest, table, VERTICAL_SUN, cellLevel, shape);
        const int32_t side = SheetSide(bodyLevel);
        std::vector<int64_t> light(static_cast<size_t>(side * side), 0);
        if (!focused) {
            std::printf("FAILED: 集められない: %s\n", focused.error().c_str());
            ++failureCount;

            return light;
        }

        // 紙が当たった光を吸う割合(短い帯。レンズは長い帯を通さない)
        const uint32_t paper = table.SpeciesId("cellulose");
        const uint64_t absorbQ16 = 65536 - table.speciesLight[paper].reflectanceShortQ16;
        const uint32_t toFine = static_cast<uint32_t>(3 * bodyLevel);
        const uint32_t fromRays = mr::LN_ENERGY_SHIFT + static_cast<uint32_t>(3 * cellLevel);
        for (const auto& [spot, energy] : focused->hits) {
            if (spot[0] % 3 != 1)
                continue;

            // 升の真ん中から紙のボクセルの番号(紙の真ん中 = レンズの軸)
            const int64_t centreX = (spot[1] * mr::LN_SPOT_UM) + (mr::LN_SPOT_UM / 2);
            const int64_t centreZ = (spot[2] * mr::LN_SPOT_UM) + (mr::LN_SPOT_UM / 2);
            const int64_t x = FloorDivide((2 * (centreX - LENS_X_UM) * (int64_t{1} << bodyLevel)) + (side * CELL_UM),
                                          2 * CELL_UM);
            const int64_t z = FloorDivide((2 * (centreZ - LENS_Z_UM) * (int64_t{1} << bodyLevel)) + (side * CELL_UM),
                                          2 * CELL_UM);
            if (x < 0 || x >= side || z < 0 || z >= side)
                continue;

            const uint64_t fine = (energy << toFine) >> fromRays;
            light[static_cast<size_t>(x + (side * z))] += static_cast<int64_t>((fine * absorbQ16) >> 16);
        }

        return light;
    }

    Scene MakeScene(const BakedReactionTable& table, int32_t cellLevel, int32_t bodyLevel,
                    FilmLaw film = FilmLaw::KineticTheory) {
        Scene scene;
        scene.film = film;
        scene.bodyLevel = bodyLevel;
        scene.cellLevel = cellLevel;

        // 厚さ 0.1 mm の紙をボクセルの厚さで薄めた量(そのレベルの単位 = 濃さ × 0.125 m³)
        const uint64_t paperUnits = (FULL_PAPER_UNITS * PAPER_THICKNESS_UM << bodyLevel) /
                                    static_cast<uint64_t>(CELL_UM);
        const std::vector<SpeciesAmount> voxel = {
            {.species = table.SpeciesId("cellulose"), .amount = paperUnits},
            {.species = table.SpeciesId("oxygen"), .amount = AIR_OXYGEN_UNITS},
            {.species = table.SpeciesId("nitrogen"), .amount = AIR_NITROGEN_UNITS}};
        scene.side = SheetSide(bodyLevel);
        scene.substeps = bodyLevel > BODY_LEVEL ? 1u << (2 * (bodyLevel - BODY_LEVEL)) : 1u;
        scene.changedTicks.assign(static_cast<size_t>(scene.side * scene.side), 0);
        scene.voxels.assign(static_cast<size_t>(scene.side * scene.side),
                            MakeReactionCell(table, voxel, test::SUN_ROOM_TEMPERATURE_MK));
        scene.air = test::MakeSunCell(table, test::SunMaterial::Air);
        scene.light = LensLightPerVoxel(table, cellLevel, bodyLevel);

        return scene;
    }

    // --- 対の面(ボクセル ⇄ 囲むセル)---

    // ボクセルからセルへ(正)の 1 刻みの量(ボクセルの単位)
    struct PairFlow {
        int64_t energy = 0;
        std::map<uint32_t, int64_t> amounts;
    };

    // 紙の両面の「面の広さ ÷ 膜の厚さ」(µm)= 2 s² ÷ 膜。膜が 0 なら箱の半分(= 4 s。箱の細かさで変わる)
    int64_t PairLengthUm(const Scene& scene) {
        if (scene.filmUm == 0)
            return (4 * CELL_UM) >> scene.bodyLevel;

        return (2 * CELL_UM * CELL_UM >> (2 * scene.bodyLevel)) / scene.filmUm;
    }

    // 伝導: 囲むセルの気体の伝導率 × 4 s × ΔT × Δt(ボクセルの単位)
    int64_t PairConduction(const ReactionTableView& view, const Scene& scene, const rx::RxThermal& voxel,
                           const rx::RxThermal& air) {
        // 室温の値(膜の温度の倍率は下で掛ける)
        const uint64_t conductivity = uint64_t{rx::HcCellConductance(view, scene.air, rx::HC_ROOM_TEMPERATURE_MK)}
                                      << rx::HC_CONDUCTANCE_SHIFT;
        const int64_t difference = int64_t{voxel.temperature} - int64_t{air.temperature};
        const FilmScale scale = FilmScaleOf(scene.film, voxel.temperature, air.temperature);
        const uint64_t multiplier = (conductivity * static_cast<uint64_t>(PairLengthUm(scene)) * scale.conductionQ16) >>
                                    16;

        // mW/(m·K) × µm × mK = 10^-12 W・1/60 s → mJ は ÷ (10^6 × 60000)。ボクセルの単位は × 8^L
        return MulDivToward0(difference * (int64_t{1} << (3 * scene.bodyLevel)), multiplier,
                             60000000000ull * scene.substeps);
    }

    // 放射: 紙の吸い込み(長い帯)× 囲む所(吸い込み 1)。RdPairCoefficient は 0.25 m² の面の値なので、紙の両面 2 s² に縮め、
    // ボクセルの単位(× 8^L)にする: × 2 × 2^L
    int64_t PairRadiation(const BakedReactionTable& table, const Scene& scene, const rx::RxCell& cell,
                          const rx::RxThermal& voxel, const rx::RxThermal& air) {
        fx::FxU128 sum{};
        for (uint32_t i = 0; i < cell.speciesCount; ++i)
            sum = rx::RdAddAbsorption(sum, table.speciesLight[cell.species[i]].absorptionLong, cell.amounts[i]);

        // 面の広さ s² の吸い込み: τ = Σ a·n ÷ (0.25 m² × 2^L)(n はボクセルの単位)
        const uint32_t absorptance = rx::RdAbsorptanceQ16(rx::RdOpticalDepthQ32(sum) >> scene.bodyLevel);
        const uint64_t weight = rx::RdPairWeightQ32(absorptance, rx::RD_ONE_Q16, rx::RD_ONE_Q16);
        const uint64_t coefficient = rx::RdPairCoefficient(static_cast<uint32_t>(voxel.temperature),
                                                           static_cast<uint32_t>(air.temperature), weight)
                                     << (1 + scene.bodyLevel);
        const uint64_t limit = rx::RdPairLimit(voxel.heatCapacity, voxel.heatCapacity, 32768);

        return rx::RdPairFlow(static_cast<uint32_t>(voxel.temperature), static_cast<uint32_t>(air.temperature),
                              std::min(coefficient, limit) / scene.substeps);
    }

    // 気体の混ざり: 物質ごとに 濃さの差 × 混ざる速さ × 4 s × Δt。運ぶ気体は出ていく側の温度の熱と化学のエネルギーを持っていく
    void PairGas(const BakedReactionTable& table, const Scene& scene, const rx::RxCell& cell,
                 const rx::RxThermal& voxel, const rx::RxThermal& air, PairFlow& flow) {
        std::set<uint32_t> gases;
        for (uint32_t i = 0; i < cell.speciesCount; ++i)
            gases.insert(cell.species[i]);

        for (uint32_t i = 0; i < scene.air.speciesCount; ++i)
            gases.insert(scene.air.species[i]);

        const FilmScale film = FilmScaleOf(scene.film, voxel.temperature, air.temperature);
        for (const uint32_t species : gases) {
            if (table.speciesPhase[species] != SpeciesPhase::Gas)
                continue;

            // 濃さ × 0.125 m³ は、どのレベルでも単位の数そのもの(ボクセルの単位とセルの単位で比べられる)
            const int64_t difference = static_cast<int64_t>(AmountOf(cell, species)) -
                                       static_cast<int64_t>(AmountOf(scene.air, species));
            const uint64_t diffusivity = table.speciesTransport[species].diffusivityMm2PerS.value_or(
                DEFAULT_DIFFUSIVITY_MM2_PER_S);
            // mm²/s × µm × (単位 ÷ 0.125 m³)× 1/60 s = 単位 × 8 ÷ (60 × 10^12)。ボクセルの単位は × 8^L
            const uint64_t multiplier = (diffusivity * static_cast<uint64_t>(PairLengthUm(scene)) *
                                         film.diffusionQ16) >>
                                        16;
            int64_t moved = MulDivToward0(difference * (int64_t{8} << (3 * scene.bodyLevel)), multiplier,
                                          60000000000000ull * scene.substeps);
            if (moved > 0)
                moved = std::min(moved, static_cast<int64_t>(AmountOf(cell, species)));

            if (moved == 0)
                continue;

            // 運ぶエネルギー = 量 × (h0 + 比熱 × 出ていく側の温度)。µJ/µmol と nJ/(µmol·K) × mK → mJ は ÷ 10^9
            const rx::RxSpecies values = table.species[species];
            const int64_t temperature = moved > 0 ? voxel.temperature : air.temperature;
            const int64_t perUnit = (values.h0 * 1000000) + (int64_t{values.heatCapacity} * temperature);
            flow.amounts[species] += moved;
            // 量 × 1 単位のエネルギーは 64bit を超えうる(熱分解の直後の水蒸気。T-1016 で見つけた桁あふれ)ので 128bit の積で
            const int64_t carried = MulDivToward0(moved, static_cast<uint64_t>(perUnit < 0 ? -perUnit : perUnit),
                                                  1000000000);
            flow.energy += perUnit < 0 ? -carried : carried;
        }
    }

    // 物質量で重み付けた平均(T-1066 より前のセルの式。調べの比べ用)
    uint64_t MoleConductivity(const BakedReactionTable& table, const rx::RxCell& cell) {
        uint64_t amount = 0;
        uint64_t weighted = 0;
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            amount += cell.amounts[i];
            weighted += cell.amounts[i] * table.species[cell.species[i]].conductivity;
        }

        return amount == 0 ? 0 : weighted / amount;
    }

    // 箱の伝導率(mW/(m·K))。VolumeWeighted はエンジンのセルの式(HcCellConductance。体積の平行の道・気体は ∝ √T。T-1066)
    uint64_t InnerConductivity(const BakedReactionTable& table, const Scene& scene, const rx::RxCell& cell,
                               int32_t temperatureMk) {
        if (scene.inner == InnerLaw::VolumeWeighted) {
            const auto temperature = static_cast<uint32_t>(std::max(temperatureMk, 0));

            return uint64_t{rx::HcCellConductance(table.View(), cell, temperature)} << rx::HC_CONDUCTANCE_SHIFT;
        }

        return MoleConductivity(table, cell);
    }

    // 物の中の伝導(隣のボクセルどうし。面 s²・距離 s。伝導率は両側の小さい方)。a → b が正
    int64_t InnerConduction(const BakedReactionTable& table, const Scene& scene, const rx::RxCell& a,
                            const rx::RxCell& b, const rx::RxThermal& thermalA, const rx::RxThermal& thermalB) {
        if (scene.inner == InnerLaw::Off)
            return 0;

        const uint64_t conductivity = std::min(InnerConductivity(table, scene, a, thermalA.temperature),
                                               InnerConductivity(table, scene, b, thermalB.temperature));
        const int64_t difference = int64_t{thermalA.temperature} - int64_t{thermalB.temperature};
        const uint64_t multiplier = conductivity * static_cast<uint64_t>(CELL_UM >> scene.bodyLevel);

        return MulDivToward0(difference * (int64_t{1} << (3 * scene.bodyLevel)), multiplier,
                             60000000000ull * scene.substeps);
    }

    // 1 刻みの受け渡し(Jacobi: 刻みの初めの値だけから計算し、全部を後で当てる)。reverse ならボクセルを逆の順に
    void StepExchangeOnce(const BakedReactionTable& table, Scene& scene, bool lensOn, bool reverse, uint32_t sub) {
        const ReactionTableView view = table.View();
        const size_t count = scene.voxels.size();
        std::vector<rx::RxThermal> thermals(count);
        for (size_t i = 0; i < count; ++i)
            thermals[i] = rx::RxComputeThermal(view, scene.voxels[i]);

        const rx::RxThermal air = rx::RxComputeThermal(view, scene.air);

        // --- 流れを計算する ---
        std::vector<PairFlow> flows(count);
        std::vector<int64_t> inner(count, 0);
        for (size_t step = 0; step < count; ++step) {
            const size_t i = reverse ? count - 1 - step : step;
            const rx::RxCell& cell = scene.voxels[i];
            flows[i].energy = PairConduction(view, scene, thermals[i], air) +
                              PairRadiation(table, scene, cell, thermals[i], air);
            PairGas(table, scene, cell, thermals[i], air, flows[i]);

            // 物の中の面は +x と +z の隣だけを持ち主として 1 回
            const auto side = static_cast<size_t>(scene.side);
            const auto x = static_cast<int32_t>(i % side);
            const auto z = static_cast<int32_t>(i / side);
            if (x + 1 < scene.side) {
                const int64_t heat = InnerConduction(table, scene, cell, scene.voxels[i + 1], thermals[i],
                                                     thermals[i + 1]);
                inner[i] -= heat;
                inner[i + 1] += heat;
            }

            if (z + 1 < scene.side) {
                const size_t next = i + side;
                const int64_t heat = InnerConduction(table, scene, cell, scene.voxels[next], thermals[i],
                                                     thermals[next]);
                inner[i] -= heat;
                inner[next] += heat;
            }
        }

        // --- 当てる: ボクセルへ・セルの分は細かい側の単位で合わせて端数と一緒に渡す ---
        int64_t toCellEnergy = 0;
        std::map<uint32_t, int64_t> toCellAmounts;
        for (size_t i = 0; i < count; ++i) {
            rx::RxCell& cell = scene.voxels[i];
            const rx::RxCell before = cell;
            // 光は小刻みに等しく分ける(端数は後ろの小刻みへ。足すと 1 刻みの量に戻る)
            const int64_t steps = scene.substeps;
            const int64_t sunlight = lensOn ? ((scene.light[i] * (sub + 1)) / steps) - ((scene.light[i] * sub) / steps)
                                            : 0;
            cell.energy += sunlight + inner[i] - flows[i].energy;
            scene.sunIn += sunlight;
            toCellEnergy += flows[i].energy;
            for (const auto& [species, moved] : flows[i].amounts) {
                scene.overflowed |= !AddAmount(cell, species, -moved);
                toCellAmounts[species] += moved;
            }

            if (!rx::RxSameCell(before, cell))
                scene.changedTicks[i] = scene.tick - 1;
        }

        const int64_t scale = FineScale(scene);
        const int64_t energyTotal = toCellEnergy + scene.energyCarry;
        const int64_t energyCoarse = FloorDivide(energyTotal, scale);
        scene.energyCarry = energyTotal - (energyCoarse * scale);
        scene.air.energy += energyCoarse;
        for (const auto& [species, moved] : toCellAmounts) {
            const int64_t total = moved + scene.amountCarry[species];
            const int64_t coarse = FloorDivide(total, scale);
            scene.amountCarry[species] = total - (coarse * scale);
            scene.overflowed |= !AddAmount(scene.air, species, coarse);
        }
    }

    // 1 刻みの受け渡し = 対の面の小刻みを scene.substeps 回(刻みの初めの値は小刻みごと)
    void StepExchange(const BakedReactionTable& table, Scene& scene, bool lensOn, bool reverse) {
        for (uint32_t sub = 0; sub < scene.substeps; ++sub)
            StepExchangeOnce(table, scene, lensOn, reverse, sub);
    }

    // 反応(セルと同じ関数)。ボクセルごとの反応の熱(化学のエネルギーの減り。ボクセルの単位の µJ/刻み)を返す
    std::vector<int64_t> StepReactions(const BakedReactionTable& table, Scene& scene, uint64_t tick) {
        const ReactionTableView view = table.View();
        std::vector<int64_t> released(scene.voxels.size(), 0);
        for (size_t i = 0; i < scene.voxels.size(); ++i) {
            rx::RxCell& cell = scene.voxels[i];
            const int64_t before = rx::RxChemicalEnergy(view, cell);
            const rx::RxCell stepped = StepReactionCellWait(table, cell, WORLD_SEED, tick, scene.changedTicks[i], i)
                                           .cell;
            if (!rx::RxSameCell(stepped, cell))
                scene.changedTicks[i] = tick;

            cell = stepped;
            released[i] = before - rx::RxChemicalEnergy(view, cell);
        }

        return released;
    }

    // 反応の熱(ボクセルの単位の µJ/刻み)≥ しきい値(mW)
    bool AtLeastMilliwatts(int64_t released, int64_t milliwatts, int32_t level) {
        // µJ/刻み × 60 ÷ 1000 = mW(ボクセルの単位は ÷ 8^L)
        return released * 60 >= milliwatts * 1000 * (int64_t{1} << (3 * level));
    }

    // --- 保存の検査: エネルギーと元素(ボクセルの単位)---

    struct Totals {
        int64_t energy = 0;
        std::vector<int64_t> elements;

        [[nodiscard]] bool operator==(const Totals&) const = default;
    };

    Totals Measure(const BakedReactionTable& table, const Scene& scene) {
        const int64_t scale = FineScale(scene);
        const size_t elementCount = table.elementNames.size();
        Totals totals;
        totals.energy = (scene.air.energy * scale) + scene.energyCarry - scene.sunIn;
        totals.elements.assign(elementCount, 0);
        for (const rx::RxCell& cell : scene.voxels) {
            totals.energy += cell.energy;
            const std::vector<uint64_t> counts = CountElements(table, cell);
            for (size_t e = 0; e < elementCount; ++e)
                totals.elements[e] += static_cast<int64_t>(counts[e]);
        }

        const std::vector<uint64_t> airCounts = CountElements(table, scene.air);
        for (size_t e = 0; e < elementCount; ++e)
            totals.elements[e] += static_cast<int64_t>(airCounts[e]) * scale;

        for (const auto& [species, carry] : scene.amountCarry) {
            for (size_t e = 0; e < elementCount; ++e)
                totals.elements[e] += carry * table.speciesElements[(species * elementCount) + e];
        }

        return totals;
    }

    // 燃える固体(炭素の数: cellulose × 6 + carbon)
    uint64_t FuelCarbon(const BakedReactionTable& table, const rx::RxCell& cell) {
        return (6 * AmountOf(cell, table.SpeciesId("cellulose"))) + AmountOf(cell, table.SpeciesId("carbon"));
    }

    uint64_t Digest(const Scene& scene) {
        uint64_t digest = HashReactionCell(scene.air);
        for (const rx::RxCell& cell : scene.voxels)
            digest = fx::FxHashCombine(digest, HashReactionCell(cell));

        digest = fx::FxHashCombine(digest, static_cast<uint64_t>(scene.energyCarry));
        for (const auto& [species, carry] : scene.amountCarry)
            digest = fx::FxHashCombine(fx::FxHashCombine(digest, species), static_cast<uint64_t>(carry));

        return digest;
    }

    // --- 1. 燃え始めて燃え続ける ---

    struct BurnRun {
        uint32_t ignitionTick = 0;  // 0 = 燃え始めない
        uint32_t lensOffTick = 0;
        uint32_t lastBurningTick = 0;
        uint32_t focalLeftPermille = 1000;  // 焦点のボクセルの燃える固体の残り
        uint32_t burntVoxels = 0;           // 燃える固体が半分以下になったボクセル
        int32_t peakMilliKelvin = 0;
        int32_t airMilliKelvin = 0;
        double lensWatts = 0.0;           // 表示だけ
        uint32_t focalLightPermille = 0;  // 紙が吸う光のうち焦点のボクセルの分
        int32_t voxelCount = 0;
        bool conserved = true;
        bool heatStayedPositive =
            true;  // ボクセルの熱が負にならなかった(桁あふれの見張り。保存の検査は両側に同じ値なので見つけない)
        uint64_t digest = 0;
    };

    BurnRun RunBurn(const BakedReactionTable& table, int32_t cellLevel, int32_t bodyLevel,
                    FilmLaw film = FilmLaw::KineticTheory, bool reverse = false, bool trace = false,
                    InnerLaw inner = InnerLaw::VolumeWeighted, int64_t filmUm = FILM_UM) {
        Scene scene = MakeScene(table, cellLevel, bodyLevel, film);
        scene.inner = inner;
        scene.filmUm = filmUm;
        const ReactionTableView view = table.View();
        const auto focal = static_cast<size_t>(std::max_element(scene.light.begin(), scene.light.end()) -
                                               scene.light.begin());
        const uint64_t focalFuel = FuelCarbon(table, scene.voxels[focal]);
        const Totals initial = Measure(table, scene);
        BurnRun run;
        run.voxelCount = scene.side * scene.side;
        int64_t lightSum = 0;
        for (const int64_t light : scene.light)
            lightSum += light;

        run.focalLightPermille = static_cast<uint32_t>((scene.light[focal] * 1000) / std::max<int64_t>(lightSum, 1));
        for (const int64_t light : scene.light)
            run.lensWatts += static_cast<double>(light) * 60.0 / 1000.0 /
                             static_cast<double>(int64_t{1} << (3 * bodyLevel));

        for (uint32_t tick = 1; tick <= TICK_LIMIT; ++tick) {
            const bool lensOn = run.ignitionTick == 0 || tick < run.ignitionTick + LENS_OFF_AFTER_TICKS;
            if (!lensOn && run.lensOffTick == 0)
                run.lensOffTick = tick;

            scene.tick = tick;
            StepExchange(table, scene, lensOn, reverse);
            const std::vector<int64_t> released = StepReactions(table, scene, tick);
            if (run.ignitionTick == 0 && AtLeastMilliwatts(released[focal], IGNITION_MILLIWATTS, bodyLevel))
                run.ignitionTick = tick;

            for (size_t i = 0; i < released.size(); ++i) {
                if (AtLeastMilliwatts(released[i], BURNING_MILLIWATTS, bodyLevel))
                    run.lastBurningTick = tick;

                const rx::RxThermal thermal = rx::RxComputeThermal(view, scene.voxels[i]);
                run.peakMilliKelvin = std::max(run.peakMilliKelvin, thermal.temperature);
                run.heatStayedPositive = run.heatStayedPositive && thermal.heat > 0;
            }

            if (trace && tick % 5 == 0 && tick <= 200) {
                const rx::RxCell& cell = scene.voxels[focal];
                std::printf(
                    "  刻み %u: 焦点 %.0f K・隣 %.0f K・O2 %llu・H2O %llu・紙 %llu・炭 %llu・CO2 %llu・反応の熱 %.3f "
                    "W\n",
                    tick, rx::RxComputeThermal(view, cell).temperature / 1000.0,
                    rx::RxComputeThermal(view, scene.voxels[focal + 1]).temperature / 1000.0,
                    static_cast<unsigned long long>(AmountOf(cell, table.SpeciesId("oxygen"))),
                    static_cast<unsigned long long>(AmountOf(cell, table.SpeciesId("water_vapor"))),
                    static_cast<unsigned long long>(AmountOf(cell, table.SpeciesId("cellulose"))),
                    static_cast<unsigned long long>(AmountOf(cell, table.SpeciesId("carbon"))),
                    static_cast<unsigned long long>(AmountOf(cell, table.SpeciesId("carbon_dioxide"))),
                    static_cast<double>(released[focal]) * 60.0 / 1.0e6 /
                        static_cast<double>(int64_t{1} << (3 * bodyLevel)));
            }

            run.conserved = run.conserved && !scene.overflowed && Measure(table, scene) == initial;
            if (run.lensOffTick != 0 && tick > run.lastBurningTick + QUIET_TICKS)
                break;
        }

        run.focalLeftPermille = static_cast<uint32_t>((FuelCarbon(table, scene.voxels[focal]) * 1000) /
                                                      std::max<uint64_t>(focalFuel, 1));
        for (const rx::RxCell& cell : scene.voxels)
            run.burntVoxels += FuelCarbon(table, cell) * 2 <= focalFuel ? 1u : 0u;

        run.airMilliKelvin = rx::RxComputeThermal(view, scene.air).temperature;
        run.digest = Digest(scene);

        return run;
    }

    void PrintBurn(const char* label, const BurnRun& run) {
        std::printf(
            "%s: 紙が吸うレンズの光 %.2f W・燃え始め %u 刻み(%.2f s)・レンズを外した %u 刻み・最後に燃えていた %u 刻み"
            "(外してから %.2f s 燃え続けた)・焦点のボクセルの燃える固体の残り %.1f %%・燃えたボクセル %u / %d・"
            "一番熱い %.0f K・囲むセル %.4f K・保存 %s・熱 %s\n",
            label, run.lensWatts, run.ignitionTick, run.ignitionTick / 60.0, run.lensOffTick, run.lastBurningTick,
            run.lastBurningTick > run.lensOffTick ? (run.lastBurningTick - run.lensOffTick) / 60.0 : 0.0,
            run.focalLeftPermille / 10.0, run.burntVoxels, run.voxelCount, run.peakMilliKelvin / 1000.0,
            run.airMilliKelvin / 1000.0, run.conserved ? "0" : "崩れた", run.heatStayedPositive ? "正" : "負になった");
    }

    uint32_t BurnTicks(const BurnRun& run) {
        return run.lastBurningTick > run.lensOffTick ? run.lastBurningTick - run.lensOffTick : 0;
    }

    void TestBurnsOn(const BakedReactionTable& table) {
        // 1 本目(膜の値は室温のまま)は記録だけ: 燃え始めるが、炭が O2 の入りより放射で冷えて 1 s ほどで消える
        PrintBurn("1 本目(0.5 m の世界・紙の箱 7.8 mm・膜の値は室温のまま)",
                  RunBurn(table, 0, BODY_LEVEL, FilmLaw::Constant));
        const BurnRun coarse = RunBurn(table, 0, BODY_LEVEL);
        const BurnRun fine = RunBurn(table, 1, BODY_LEVEL);
        PrintBurn("燃え続ける(0.5 m の世界・紙の箱 7.8 mm)", coarse);
        PrintBurn("燃え続ける(0.25 m の世界・紙の箱 7.8 mm)", fine);

        // 1. 燃え始め・レンズを外しても燃え続け、焦点のボクセルが燃え切る
        EXPECT(coarse.ignitionTick != 0 && coarse.lensOffTick != 0);
        EXPECT(BurnTicks(coarse) >= 60);
        EXPECT(coarse.focalLeftPermille <= 100);

        // 3. 保存
        EXPECT(coarse.conserved && fine.conserved);
        EXPECT(coarse.heatStayedPositive && fine.heatStayedPositive);

        // 5. 細かさ: 燃え始めの刻みと燃え続けた時間が ±10 %(仮)
        EXPECT(fine.ignitionTick != 0);
        const auto difference = [](uint32_t a, uint32_t b) {
            return static_cast<double>(a > b ? a - b : b - a) / static_cast<double>(std::max<uint32_t>(a, 1));
        };
        std::printf("細かさ: 燃え始めの差 %.2f %%・燃え続けた時間の差 %.2f %%\n",
                    100.0 * difference(coarse.ignitionTick, fine.ignitionTick),
                    100.0 * difference(BurnTicks(coarse), BurnTicks(fine)));
        EXPECT(difference(coarse.ignitionTick, fine.ignitionTick) <= 0.10);
        EXPECT(difference(BurnTicks(coarse), BurnTicks(fine)) <= 0.10);

        // 4. 決定的: 2 回で同じ・逆の順でも同じ
        const BurnRun again = RunBurn(table, 0, BODY_LEVEL);
        const BurnRun reversed = RunBurn(table, 0, BODY_LEVEL, FilmLaw::KineticTheory, true);
        std::printf("決定的: 要約 %016llx・2 回目 %016llx・逆の順 %016llx\n",
                    static_cast<unsigned long long>(coarse.digest), static_cast<unsigned long long>(again.digest),
                    static_cast<unsigned long long>(reversed.digest));
        EXPECT(coarse.digest == again.digest && coarse.digest == reversed.digest);

        // 6. 物の箱の細かさ(T-1016): 箱 3.9 mm でも燃え始めて燃え続け、焦点のボクセルが燃え切る。燃え続けた時間は箱 7.8 mm と ±20 %(仮)。
        //    燃え始めの時刻は箱で変わる(細かい箱ほど焦点の強い所を分けて持つ。記録だけ)
        const BurnRun finerBox = RunBurn(table, 0, BODY_LEVEL + 1);
        PrintBurn("箱の細かさ(0.5 m の世界・紙の箱 3.9 mm)", finerBox);
        std::printf("箱の細かさ: 燃え続けた時間の差 %.2f %%\n",
                    100.0 * difference(BurnTicks(coarse), BurnTicks(finerBox)));
        EXPECT(finerBox.ignitionTick != 0 && finerBox.focalLeftPermille <= 100 && finerBox.conserved &&
               finerBox.heatStayedPositive);
        EXPECT(difference(BurnTicks(coarse), BurnTicks(finerBox)) <= 0.20);

        // 箱が焦点より粗いと焦点の熱が箱に均される(合否にしない)
        PrintBurn("記録(0.5 m の世界・紙の箱 15.6 mm)", RunBurn(table, 0, BODY_LEVEL - 1));
    }

    // --- 2. 粗くした時: 同じ紙をセルに混ぜる ---

    void TestMixedIntoCell(const BakedReactionTable& table) {
        const ReactionTableView view = table.View();
        const Scene sheet = MakeScene(table, 0, BODY_LEVEL);

        // 紙の 25 ボクセルの量と光を、レベル 0 の単位にして 1 つのセル(空気 + 紙)へ。光の端数は持ち越す
        const int64_t scale = FineScale(sheet);
        const uint32_t cellulose = table.SpeciesId("cellulose");
        uint64_t paper = 0;
        int64_t light = 0;
        for (size_t i = 0; i < sheet.voxels.size(); ++i) {
            paper += AmountOf(sheet.voxels[i], cellulose);
            light += sheet.light[i];
        }

        const std::vector<SpeciesAmount> mixed = {
            {.species = cellulose, .amount = paper / static_cast<uint64_t>(scale)},
            {.species = table.SpeciesId("oxygen"), .amount = AIR_OXYGEN_UNITS},
            {.species = table.SpeciesId("nitrogen"), .amount = AIR_NITROGEN_UNITS}};
        rx::RxCell cell = MakeReactionCell(table, mixed, test::SUN_ROOM_TEMPERATURE_MK);
        const int32_t start = rx::RxComputeThermal(view, cell).temperature;
        int64_t carry = 0;
        int64_t releasedTotal = 0;
        for (uint32_t tick = 1; tick <= 60 * 60; ++tick) {
            const int64_t total = light + carry;
            cell.energy += total / scale;
            carry = total % scale;
            const int64_t before = rx::RxChemicalEnergy(view, cell);
            cell = StepReactionCellWait(table, cell, WORLD_SEED, tick, tick - 1, 0).cell;
            releasedTotal += before - rx::RxChemicalEnergy(view, cell);
        }

        const int32_t end = rx::RxComputeThermal(view, cell).temperature;
        std::printf(
            "粗くした時(同じ紙を 0.5 m のセルに混ぜ、レンズを 60 s 当て続ける): 温度 %.4f K → %.4f K・反応の熱 %.3f J"
            "(燃え始めない)\n",
            start / 1000.0, end / 1000.0, static_cast<double>(releasedTotal) / 1.0e6);
        EXPECT(end - start < 10000 && releasedTotal < 1000000);
    }

    // --- T-1016 の調べ(--probe。合否にしない): 物の箱の細かさ × 物の中の伝導率の決め方 ---

    void Probe(const BakedReactionTable& table) {
        struct Model {
            const char* name;
            InnerLaw inner;
            int64_t filmUm;
        };
        constexpr std::array<Model, 4> MODELS = {
            Model{"1 本目の形: 膜 = 箱の半分・物の中は物質量の平均", InnerLaw::MoleWeighted, 0},
            Model{"膜 = 箱の半分・物の中は体積の平行", InnerLaw::VolumeWeighted, 0},
            Model{"膜 = 3.9 mm の定数・物の中は物質量の平均", InnerLaw::MoleWeighted, FILM_UM},
            Model{"2 本目: 膜 = 3.9 mm の定数・物の中は体積の平行", InnerLaw::VolumeWeighted, FILM_UM}};
        for (const Model& model : MODELS) {
            for (int32_t level = 5; level <= 8; ++level) {
                const BurnRun run = RunBurn(table, 0, level, FilmLaw::KineticTheory, false, false, model.inner,
                                            model.filmUm);
                char label[200];
                std::snprintf(label, sizeof(label), "調べ(%s・箱 %.1f mm・焦点の光 %.1f %%)", model.name,
                              500.0 / static_cast<double>(1 << level), run.focalLightPermille / 10.0);
                PrintBurn(label, run);
            }
        }
    }

    // --- 7. 共有の式(shaders/common/object_heat.hlsli・tests/object_heat_cpu.h。GPU と同じ関数。T-1064)が試作とビット一致 ---

    // 試作の場面を共有の形の世界にする(物 1 つ・紙は x と z に隣・外に面するのは両面)
    test::OhWorld SharedWorldOf(const Scene& scene, const test::OhTables& tables) {
        test::OhWorld world;
        const auto side = static_cast<uint32_t>(scene.side);
        world.objects.push_back({.voxelBegin = 0,
                                 .voxelCount = side * side,
                                 .sizeX = side,
                                 .sizeY = 1,
                                 .cell = 0,
                                 .bodyLevel = scene.bodyLevel,
                                 .cellLevel = scene.cellLevel,
                                 .substeps = scene.substeps,
                                 .filmUm = static_cast<uint32_t>(scene.filmUm)});
        for (uint32_t z = 0; z < side; ++z) {
            for (uint32_t x = 0; x < side; ++x) {
                const uint32_t neighbours = (x > 0 ? 1u : 0u) | (x + 1 < side ? 2u : 0u) | (z > 0 ? 16u : 0u) |
                                            (z + 1 < side ? 32u : 0u);
                world.infos.push_back({.object = 0, .shape = neighbours | (2u << rx::OH_EXPOSED_SHIFT)});
            }
        }

        world.voxels = scene.voxels;
        world.light = scene.light;
        world.cells = {scene.air};
        world.ranges = {{.cell = 0, .objectBegin = 0, .objectCount = 1}};
        rx::OhFlow carry = rx::OhZeroFlow();
        carry.energy = scene.energyCarry;
        for (const auto& [species, amount] : scene.amountCarry)
            carry.amounts[tables.species[species].gasIndex] = amount;

        world.carries = {carry};
        world.sunIn = scene.sunIn;
        test::FinishObjectHeatWorld(world);
        for (size_t i = 0; i < scene.changedTicks.size(); ++i)
            world.waits[i].changedTick = scene.changedTicks[i];

        return world;
    }

    bool SameAsShared(const Scene& scene, const test::OhWorld& world, const test::OhTables& tables) {
        if (HashReactionCell(scene.air) != HashReactionCell(world.cells[0]) ||
            scene.energyCarry != world.carries[0].energy || scene.sunIn != world.sunIn)
            return false;

        for (size_t i = 0; i < scene.voxels.size(); ++i) {
            if (HashReactionCell(scene.voxels[i]) != HashReactionCell(world.voxels[i]))
                return false;
        }

        for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g) {
            const uint32_t species = tables.gases.species[g];
            const auto found = scene.amountCarry.find(species);
            const int64_t carry = found == scene.amountCarry.end() ? 0 : found->second;
            if (species != 0 && carry != world.carries[0].amounts[g])
                return false;
        }

        return true;
    }

    // 試作の燃える場面(レンズは燃え始めから 0.5 s で外す)を、試作と共有の形で並べて回し、毎刻み比べる。食い違った刻み(0 = 最後まで同じ)
    uint32_t RunSharedAlongside(const BakedReactionTable& table, const test::OhTables& tables, int32_t cellLevel,
                                int32_t bodyLevel, uint32_t* ticks) {
        Scene scene = MakeScene(table, cellLevel, bodyLevel);
        test::OhWorld world = SharedWorldOf(scene, tables);
        const test::OhCpuTable shared{.view = table.View(), .objectSpecies = tables.species};
        const auto focal = static_cast<size_t>(std::max_element(scene.light.begin(), scene.light.end()) -
                                               scene.light.begin());
        uint32_t ignitionTick = 0;
        uint32_t lastBurningTick = 0;
        for (uint32_t tick = 1; tick <= TICK_LIMIT; ++tick) {
            const bool lensOn = ignitionTick == 0 || tick < ignitionTick + LENS_OFF_AFTER_TICKS;
            scene.tick = tick;
            StepExchange(table, scene, lensOn, false);
            const std::vector<int64_t> released = StepReactions(table, scene, tick);
            test::StepObjectHeat(shared, tables.gases, world, lensOn, WORLD_SEED, tick);
            *ticks = tick;
            if (!SameAsShared(scene, world, tables) || world.failed)
                return tick;

            if (ignitionTick == 0 && AtLeastMilliwatts(released[focal], IGNITION_MILLIWATTS, bodyLevel))
                ignitionTick = tick;

            for (const int64_t heat : released) {
                if (AtLeastMilliwatts(heat, BURNING_MILLIWATTS, bodyLevel))
                    lastBurningTick = tick;
            }

            if (ignitionTick != 0 && tick >= ignitionTick + LENS_OFF_AFTER_TICKS &&
                tick > lastBurningTick + QUIET_TICKS)
                break;
        }

        return 0;
    }

    void TestSharedForm(const BakedReactionTable& table, const test::OhTables& tables) {
        struct Case {
            const char* name;
            int32_t cellLevel;
            int32_t bodyLevel;
        };
        constexpr std::array<Case, 3> CASES = {Case{"0.5 m の世界・箱 7.8 mm", 0, BODY_LEVEL},
                                               Case{"0.25 m の世界・箱 7.8 mm", 1, BODY_LEVEL},
                                               Case{"0.5 m の世界・箱 3.9 mm(小刻み 4)", 0, BODY_LEVEL + 1}};
        for (const Case& item : CASES) {
            uint32_t ticks = 0;
            const uint32_t mismatch = RunSharedAlongside(table, tables, item.cellLevel, item.bodyLevel, &ticks);
            std::printf("共有の式(object_heat.hlsli)と試作(%s): %u 刻み %s\n", item.name, ticks,
                        mismatch == 0 ? "毎刻みビット一致" : "食い違った");
            EXPECT(mismatch == 0);
        }
    }

    // --- 8. 眠らせ方(T-1065): 冷えた紙は、何刻みか回すと 1 刻み回しても何も変わらない(眠れる)か ---

    void TestColdSleeps(const BakedReactionTable& table, const test::OhTables& tables) {
        constexpr uint32_t COLD_TICKS = 1200;
        Scene scene = MakeScene(table, 0, BODY_LEVEL);
        test::OhWorld world = SharedWorldOf(scene, tables);
        const test::OhCpuTable shared{.view = table.View(), .objectSpecies = tables.species};
        uint32_t lastChanged = 0;
        uint64_t previous = 0;
        uint64_t awakeAtHalf = 0;
        for (uint32_t tick = 1; tick <= COLD_TICKS; ++tick) {
            test::StepObjectHeat(shared, tables.gases, world, false, WORLD_SEED, tick);
            if (tick == COLD_TICKS / 2)
                awakeAtHalf = world.awakeExchangeBlocks + world.awakeReactBlocks;

            uint64_t digest = HashReactionCell(world.cells[0]);
            for (const rx::RxCell& voxel : world.voxels)
                digest = fx::FxHashCombine(digest, HashReactionCell(voxel));

            digest = fx::FxHashCombine(digest, static_cast<uint64_t>(world.carries[0].energy));
            for (const int64_t amount : world.carries[0].amounts)
                digest = fx::FxHashCombine(digest, static_cast<uint64_t>(amount));

            if (digest != previous)
                lastChanged = tick;

            previous = digest;
        }

        std::printf(
            "眠らせ方: 冷えた紙(室温の空気の中・光なし)は %u 刻み目から %u 刻みまで 1 刻み回しても何も変わらない(%s)\n",
            lastChanged + 1, COLD_TICKS, lastChanged < COLD_TICKS ? "眠れる" : "眠れない");
        EXPECT(lastChanged < COLD_TICKS / 2);

        // ブロックごとに眠らせる(T-1098): 後半の 600 刻みで回したブロックは 0(費用 0)
        const uint64_t awakeLater = world.awakeExchangeBlocks + world.awakeReactBlocks - awakeAtHalf;
        std::printf("眠らせ方(ブロック): 後半 %u 刻みで回したブロック %llu(ブロック %zu)\n", COLD_TICKS / 2,
                    static_cast<unsigned long long>(awakeLater), world.blocks.size());
        EXPECT(awakeLater == 0);
    }

}  // namespace

int main(int argc, char** argv) {
    const BakedReactionTable table = MakeTable();
    if (argc > 1 && std::string_view(argv[1]) == "--probe") {
        Probe(table);

        return 0;
    }

    TestBurnsOn(table);
    TestMixedIntoCell(table);
    const auto tables = test::MakeObjectHeatTables(table, DEFAULT_DIFFUSIVITY_MM2_PER_S);
    EXPECT(tables.has_value());
    if (tables) {
        TestSharedForm(table, *tables);
        TestColdSleeps(table, *tables);
    }

    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("object_heat: 全部通った\n");

    return 0;
}
