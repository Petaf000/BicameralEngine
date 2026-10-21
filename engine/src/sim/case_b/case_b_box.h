// case_b_box.h — 案 B(多流体の運動量。docs/design/21・ADR-0376)を CPU で確かめる試作の箱(B0。T-0866。21 §5.2)。GPU を知らない。
// 案 B の判定(21 §5.1)のための**試作**で、判定で案 A に戻る時はこのフォルダ(engine/src/sim/case_b)ごと捨てる(21 §4)。
//
// 箱が持つもの(真実): セル × 物質の質量(mg)・セルのエネルギー(mJ)・面の運動量(相ごと。案 ① = 気体・液体・粒 / 案 ④ = 流体・粒)・
// 面の持ち越し(端数)・物の占める体積と速さ(決まった相)・導かれる圧力(陰の解き方の初めの近似に使うので状態に入れる)・帳簿・刻み。
// 箱がしないこと: 物理の式(押し返し方・引きずり・降伏・運ぶ)。式は R1 以降の作業役が「段」(CaseBStages)として差し込む。
// 1 刻み = 見立て(BuildCaseBView: 相ごとの質量・体積の割合・柱の重さ)→ 段を 21 §2.14 の順に呼ぶ → 刻みを進める。
// 保存は「受け渡し」の関数だけで中身を変えることで守る(04 R2): 箱の中の移しは Move*、外との出入りと外からの力積は Exchange*・ApplyImpulse が帳簿に書く。
// CheckCaseBConservation が「今の合計 = 初めの合計 + 帳簿」をビット単位で比べる。
// 陰の圧力は口(CaseBPressureSolver)から呼ぶ。土台の釣り合う家族の共通部品(C-0)ができたらそれを差し込む。それまでは
// 熱の陰解法の多重格子(implicit_conduction。T-0503 の模型と同じ使い方)を包んだ MakeCaseBImplicitGridSolver。
// 単位: 質量 mg・エネルギー mJ・運動量 mg·2^-20 m/s(04 §2 の運動量)・速さ 2^-20 m/s・圧力 µPa・加速度 µm/s²・長さ µm。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bicameral::sim::case_b {

    // --- 相 ---
    // 作り手が書く相(パッケージの phase)
    enum class WrittenPhase : uint8_t { Gas, Liquid, Solid };

    // 箱の中の相。粒の大きさを書いた固体が粒(D-553)、書かない固体は据わる(速さ 0 の決まった相)
    enum class Phase : uint8_t { Gas, Liquid, Grain, Fixed };

    inline constexpr uint32_t MOVING_PHASE_COUNT = 3;  // 気体・液体・粒(Phase の 0〜2)

    // 面の運動量の置き場(21 §2.2)
    enum class MomentumLayout : uint8_t {
        PerPhase,       // 案 ①: 相ごとの面 ×3(気体・液体・粒)
        GrainSeparate,  // 案 ④: 粒だけ別 + 気体と液体は混ざった運動量(流体・粒)
    };

    // --- 物質の欄(21 §1.1。書かなければ std::nullopt)---
    struct MaterialDefinition {
        std::string name;
        WrittenPhase phase = WrittenPhase::Gas;

        // --- 今ある欄 ---
        uint32_t molarMassMgPerMol = 0;                   // 1 mg/mol(04 §2 の原子量・モル質量)
        uint32_t heatCapacity = 0;                        // mJ/(mol·K)
        std::optional<uint32_t> densityKgPerM3;           // 液体・固体の体積を決める
        std::optional<uint32_t> viscosityMicroPascalSec;  // viscosity_upa_s
        std::optional<uint32_t> permeabilityUm2;          // permeability_um2
        std::optional<uint32_t> suctionPa;                // suction_pa
        std::optional<uint32_t> reposeAngleDegrees;       // repose_angle_deg

        // --- ★ 案 B で足す粒の 3 つ ---
        std::optional<uint32_t> grainSizeUm;        // grain_size_um。書いたら粒
        std::optional<uint32_t> cohesionPa;         // cohesion_pa
        std::optional<uint32_t> packedFractionPct;  // packed_fraction_pct(30〜74)

        // --- 古い書き方 ---
        bool solidMotionLoose = false;  // solid_motion = "loose"(D-553 で粒の大きさに畳んだ。警告して読む)
    };

    // 既定を当てた物質(箱が読む値。0 は「無い」)
    struct Material {
        std::string name;
        Phase phase = Phase::Gas;
        uint32_t molarMassMgPerMol = 0;
        uint32_t heatCapacity = 0;
        uint32_t densityKgPerM3 = 0;
        uint32_t viscosityMicroPascalSec = 0;
        uint32_t permeabilityUm2 = 0;
        uint32_t suctionPa = 0;

        // --- 粒だけ(ほかの相は 0)---
        uint32_t grainSizeUm = 0;
        uint32_t reposeAngleDegrees = 0;
        uint32_t cohesionPa = 0;
        uint32_t packedFractionPct = 0;
    };

    // 環境の欄(21 §1.1。★ S7・S8)
    struct EnvironmentDefinition {
        std::optional<uint32_t> dropSizeUm;            // drop_size_um
        std::optional<int64_t> shockZonePa;            // shock_zone_pa
        std::optional<int64_t> pressureRelaxMsPerM;    // pressure_relax_ms_per_m
        std::optional<uint32_t> shockZoneSpeedMmPerS;  // 既定は未確認(R1 と床の試験で置く)
        std::optional<uint32_t> shockZoneMarginMm;     // 同上
        std::optional<uint32_t> grainPassRatioPct;     // 1 周目は読むだけ
    };

    struct Environment {
        uint32_t dropSizeUm = 0;
        int64_t shockZonePa = 0;
        int64_t pressureRelaxMsPerM = 0;
        uint32_t shockZoneSpeedMmPerS = 0;  // 0 = 未確認(書かれていない)
        uint32_t shockZoneMarginMm = 0;
        uint32_t grainPassRatioPct = 0;
    };

    // 書き間違いの知らせ(21 §1.3)。誤りがあれば箱を作らない
    struct Diagnostics {
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
    };

    // --- 既定の値(21 §1.1・§6 の仮)---
    inline constexpr uint32_t DEFAULT_REPOSE_ANGLE_DEGREES = 35;
    inline constexpr uint32_t DEFAULT_PACKED_FRACTION_PCT = 60;
    inline constexpr uint32_t PACKED_FRACTION_MIN_PCT = 30;
    inline constexpr uint32_t PACKED_FRACTION_MAX_PCT = 74;
    inline constexpr uint32_t DEFAULT_GAS_VISCOSITY = 18;      // µPa·s(空気)
    inline constexpr uint32_t DEFAULT_LIQUID_VISCOSITY = 890;  // µPa·s(水)
    inline constexpr uint32_t DEFAULT_DROP_SIZE_UM = 2000;
    inline constexpr int64_t DEFAULT_SHOCK_ZONE_PA = 10000;        // B-d(仮)
    inline constexpr int64_t DEFAULT_PRESSURE_RELAX_MS_PER_M = 3;  // B-d(仮)
    inline constexpr int64_t ATMOSPHERE_PA = 101325;

    // --- 箱の形と刻み ---
    inline constexpr uint32_t LEVEL0_CELL_EDGE_UM = 500000;  // レベル 0 のセル = 0.5 m

    struct BoxConfig {
        uint32_t sizeX = 1;
        uint32_t sizeY = 1;
        uint32_t sizeZ = 1;
        int32_t level = 0;  // セルの辺 = 0.5 m × 2^-level(−2 = 2 m・1 = 0.25 m)
        MomentumLayout layout = MomentumLayout::PerPhase;

        // 座標の加速(重さ + 座標の加速の見かけの力。21 §2.15 の準備)。柱は絶対値の最も大きい軸に沿って作る
        std::array<int64_t, 3> accelerationUmPerS2 = {0, -9806650, 0};
        uint32_t ticksPerSecond = 60;
        uint64_t seed = 0;  // 決まった乱数(04 R6)の世界のシード
    };

    // --- 帳簿(04 R2: 外との出入りと外からの力積だけを書く)---
    // Pressure = 圧力の力積(閉じた箱では壁の反力の和に当たる。R1 の段。T-0868)
    enum class ImpulseSource : uint8_t { Gravity, Wall, FixedPhase, Body, Outside, Pressure, Count };
    enum class EnergySource : uint8_t { Outside, Work, Count };

    inline constexpr uint32_t IMPULSE_SOURCE_COUNT = (uint32_t)ImpulseSource::Count;
    inline constexpr uint32_t ENERGY_SOURCE_COUNT = (uint32_t)EnergySource::Count;

    struct Ledger {
        std::vector<int64_t> massInMg;  // 物質ごとに外から入った量(出たら負)
        std::array<int64_t, ENERGY_SOURCE_COUNT> energyInMillijoules{};
        std::array<std::array<int64_t, 3>, IMPULSE_SOURCE_COUNT> impulses{};  // 外から面の運動量に足した和(軸ごと)
    };

    // 保存量の合計(CheckCaseBConservation が初めと比べる)
    struct ConservedTotals {
        std::vector<int64_t> massMg;  // 物質ごと
        int64_t energyMillijoules = 0;
        std::array<int64_t, 3> momentum{};  // 全部の面・全部の置き場の和(軸ごと)
    };

    struct Box {
        BoxConfig config;
        std::vector<Material> materials;
        Environment environment;

        // --- セル(番号 = x + sizeX × (y + sizeY × z))---
        std::vector<int64_t> massMg;                       // セル × 物質(cell × 物質の数 + 物質)
        std::vector<int64_t> energyMillijoules;            // セル
        std::vector<uint32_t> bodyOccupancyQ16;            // 物の占める体積の割合(T-0820 の口。2^16 = 全部)
        std::vector<std::array<int64_t, 3>> bodyVelocity;  // 物の速さ(2^-20 m/s。決まった相)
        std::vector<int64_t> pressureMicroPascal;          // 導かれる圧力(段が書く。次の刻みの初めの近似)

        // --- 面(軸ごと。セルの + 側の面 × 置き場。番号 = cell × 置き場の数 + 置き場。箱の外側の面は壁で 0 のまま)---
        std::array<std::vector<int64_t>, 3> faceMomentum;
        std::array<std::vector<int64_t>, 3> faceCarry;  // 運ぶ家族の面の持ち越し(端数。21 §2.10。単位は運ぶ段が決める)

        // --- 帳簿と刻み ---
        Ledger ledger;
        ConservedTotals initial;
        uint64_t tick = 0;
    };

    // 物質と環境を読む(既定を当て、誤りと警告を diagnostics に足す)。誤りがあれば std::unexpected(誤りを改行でつないだ文)
    [[nodiscard]] std::expected<Box, std::string> CreateBox(const BoxConfig& config,
                                                            std::span<const MaterialDefinition> materials,
                                                            const EnvironmentDefinition& environment,
                                                            Diagnostics* diagnostics = nullptr);

    // 読むだけ(箱を作らずに知らせを集める。セルの辺は粒の大きさの検査に使う)
    [[nodiscard]] std::vector<Material> ResolveMaterials(std::span<const MaterialDefinition> materials,
                                                         uint32_t cellEdgeUm, Diagnostics& diagnostics);
    [[nodiscard]] Environment ResolveEnvironment(const EnvironmentDefinition& environment, Diagnostics& diagnostics);

    // --- 形 ---
    [[nodiscard]] uint32_t CellCount(const Box& box);
    [[nodiscard]] uint32_t CellIndex(const Box& box, uint32_t x, uint32_t y, uint32_t z);
    [[nodiscard]] std::array<uint32_t, 3> CellCoordinates(const Box& box, uint32_t cell);
    [[nodiscard]] uint32_t SlotCount(MomentumLayout layout);
    [[nodiscard]] uint32_t MomentumSlot(MomentumLayout layout, Phase phase);  // Phase は動く相(0〜2)
    [[nodiscard]] uint32_t FaceIndex(const Box& box, uint32_t cell, uint32_t slot);
    // 軸 axis で cell の + 側の隣(無ければ std::nullopt = 壁)
    [[nodiscard]] std::optional<uint32_t> Neighbor(const Box& box, uint32_t cell, uint32_t axis);
    [[nodiscard]] uint32_t CellEdgeUm(int32_t level);
    [[nodiscard]] uint64_t CellAreaMm2(const Box& box);
    [[nodiscard]] uint64_t CellVolumeMm3(const Box& box);
    [[nodiscard]] uint32_t MaterialIndex(const Box& box, std::string_view name);  // 無ければ物質の数
    [[nodiscard]] int64_t& CellMass(Box& box, uint32_t cell, uint32_t material);
    [[nodiscard]] int64_t CellMass(const Box& box, uint32_t cell, uint32_t material);

    // --- 受け渡し(保存を構造で守る。04 R2)---
    void MoveMass(Box& box, uint32_t from, uint32_t to, uint32_t material, int64_t massMg);
    void ExchangeMass(Box& box, uint32_t cell, uint32_t material, int64_t massMg);  // 外から(負なら外へ)
    void MoveEnergy(Box& box, uint32_t from, uint32_t to, int64_t millijoules);
    void ExchangeEnergy(Box& box, uint32_t cell, int64_t millijoules, EnergySource source);

    // 面の運動量の受け渡し(同じ軸の面どうし・置き場どうし)と、外からの力積
    struct FaceRef {
        uint32_t axis = 0;
        uint32_t cell = 0;
        uint32_t slot = 0;
    };

    void MoveMomentum(Box& box, FaceRef from, FaceRef to, int64_t momentum);
    void ApplyImpulse(Box& box, FaceRef face, int64_t momentum, ImpulseSource source);

    // --- 見立て(刻みの初めに 1 回。21 §2.3 の柱・§2.1 の相)---
    struct CellView {
        std::array<int64_t, MOVING_PHASE_COUNT> phaseMassMg{};  // 気体・液体・粒
        int64_t fixedMassMg = 0;
        int64_t totalMassMg = 0;

        // 体積の割合(Q16。2^16 = セル)
        uint32_t liquidQ16 = 0;
        uint32_t grainSolidQ16 = 0;   // 粒そのものの体積
        uint32_t grainPackedQ16 = 0;  // 粒が詰まった時に占める体積(粒そのもの ÷ 詰まり)。2^16 以上で詰まった
        uint32_t fixedQ16 = 0;
        uint32_t bodyQ16 = 0;
        uint32_t openQ16 = 0;  // 残り(液体・粒そのもの・据わる固体・物を除いた空き。孔も含む)
        bool overfilled = false;

        // 柱(重さの軸に沿って。µPa)
        int64_t columnAboveMicroPascal = 0;  // 上(重さの逆向き)にある全部のセルの重さ ÷ 面積
        int64_t weightMicroPascal = 0;       // 自分の重さ ÷ 面積
    };

    struct View {
        std::vector<CellView> cells;
        uint32_t gravityAxis = 1;
        bool gravityDown = true;     // 加速が負の向き(− 側が「下」)
        int64_t gravityUmPerS2 = 0;  // 柱の軸の加速の大きさ
    };

    [[nodiscard]] View BuildView(const Box& box);

    // 面の速さ(2^-20 m/s)= 面の運動量 ÷ 両側のセルのその置き場の質量の平均。質量が 0 なら 0
    [[nodiscard]] int64_t FaceVelocity(const Box& box, const View& view, FaceRef face);
    // 流れの向きの口(#91・21 §1.3): 相ごとのセルの速さ(両側の面の平均)
    [[nodiscard]] std::array<int64_t, 3> CellPhaseVelocity(const Box& box, const View& view, uint32_t cell,
                                                           Phase phase);

    // --- 陰の圧力の口(C-0 を差し込む所)---
    // 式: C_i x_i + Σ_f K_f (x_i − x_j) = b_i(C・K は mg/µPa の Q48、x は µPa、b は mg)
    struct PressureLink {
        uint32_t left = 0;
        uint32_t right = 0;
        uint64_t coefficient = 0;  // K_f(Q48)
    };

    struct PressureSystem {
        uint32_t sizeX = 0;  // 節 = 箱のセル(座標は多重格子の段を作るのに使う)
        uint32_t sizeY = 0;
        uint32_t sizeZ = 0;
        std::vector<uint64_t> capacities;  // C_i(Q48。0 より大きいこと)
        std::vector<PressureLink> links;
        std::vector<int64_t> sources;  // b_i(mg)
    };

    struct PressureCost {
        uint32_t cycles = 0;
        int64_t worstErrorMg = 0;  // 解いた後の max |式の誤差|(mg)
    };

    // values は初めの近似(箱の pressureMicroPascal を渡す)で、解いた値で上書きする
    using PressureSolver = std::function<PressureCost(const PressureSystem&, std::vector<int64_t>& values)>;

    // C-0 が無い間の解き方: 熱の陰解法の多重格子の V サイクルを、誤差が toleranceMg 以下になるまで(上限 maxCycles)
    [[nodiscard]] PressureSolver MakeImplicitGridSolver(uint32_t maxCycles, int64_t toleranceMg);
    [[nodiscard]] int64_t PressureSystemWorstError(const PressureSystem& system, std::span<const int64_t> values);

    // --- 段(式を差し込む所。21 §2.14 の運ぶ家族の中の順)---
    struct StepContext {
        const View& view;
        const PressureSolver& pressureSolver;
    };

    using Stage = std::function<void(Box&, const StepContext&)>;

    struct Stages {
        Stage drag;        // 引きずりの消去(R2)
        Stage pressure;    // 陰の圧力(R1。pressureSolver を呼ぶ)
        Stage shockZone;   // 衝撃の区の陽の小刻み(床の試験)
        Stage momentum;    // 面の運動量(重さ・圧力の坂・粘り)
        Stage grainYield;  // 粒の降伏(R3・R4)
        Stage transport;   // 運ぶ(質量・エネルギー・運動量を風上から)
        Stage apply;       // 当てる段(静止の丸めなど)
    };

    // 1 刻み: 見立て → 段を順に(空の段は飛ばす)→ 刻みを進める
    void StepBox(Box& box, const Stages& stages, const PressureSolver& pressureSolver);

    // --- 保存と決定性 ---
    [[nodiscard]] ConservedTotals MeasureTotals(const Box& box);

    struct ConservationError {
        std::vector<int64_t> massMg;  // 物質ごと(今 − 初め − 帳簿)
        int64_t energyMillijoules = 0;
        std::array<int64_t, 3> momentum{};
        uint32_t wallFaces = 0;  // 箱の外側の面(壁)に運動量が残っている数
        bool negativeMass = false;

        [[nodiscard]] bool IsZero() const;
    };

    [[nodiscard]] ConservationError CheckConservation(const Box& box);
    // 今の合計を「初め」にする(場面を作り終えた時に呼ぶ。帳簿も 0 に戻す)
    void ResetLedger(Box& box);
    // 箱の状態全体の 64bit ハッシュ(セル・面・物・圧力・帳簿・刻みを決まった順に。04 §5)
    [[nodiscard]] uint64_t HashBox(const Box& box);

    // 粗くする(21 §2.12: 粗い箱の真実は細かい箱の真実の和)。動き出す前の箱(面の運動量が全部 0)だけ。
    // 大きさは factor(2 の冪)で割り切れること。厚さ 1 セルの軸は板として押し出す(中身 × factor。場面の箱と同じ)。
    // 0.5 m と 2 m の比べ(B1)で、場面の絵からでなく動かした後の形から粗い箱を作る時に使う
    [[nodiscard]] std::expected<Box, std::string> CoarsenBox(const Box& fine, uint32_t factor);

    // 初めの中身を作る道具: 温度(mK)から熱のエネルギー(mJ)= Σ 質量 ÷ モル質量 × 比熱 × 温度
    [[nodiscard]] int64_t ThermalEnergyMillijoules(const Box& box, uint32_t cell, uint32_t temperatureMilliKelvin);
    // 理想気体の質量(mg)= 圧力 × 体積 × モル質量 ÷(R × 温度)
    [[nodiscard]] int64_t IdealGasMassMg(uint64_t pressurePa, uint64_t volumeMm3, uint32_t molarMassMgPerMol,
                                         uint32_t temperatureMilliKelvin);

}  // namespace bicameral::sim::case_b
