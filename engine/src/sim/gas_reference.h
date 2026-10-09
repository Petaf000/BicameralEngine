// gas_reference.h — 気体(空気・煙・湯気)の流れの CPU リファレンス、段 G1・G2(T-0026・T-0184。07 §2・§2.1・§2.2・ADR-0043)。
// 1 レベル(0.5 m 角のセルの直方体)だけ。GPU・多重解像度・反応との結合はまだ無い(G3・G4。T-0185・T-0186)。
//
// 方式: 圧縮性の有限体積法を陽解法で。セルに保存量(成分ごとの物質量・内部エネルギー・運動量 ×3)を持ち、
// 面ごとに 1 つの質量の流れ F を計算して、風上側のセルの中身を同じ割合(F ÷ 風上の質量)だけ移す(成分・エネルギー・運動量が一緒に動く)。
//   F = 中心の質量の流れ(両側の運動量の平均)+ 圧力の拡散(両側の圧力の差 ÷ 2c̃。Rusanov の散逸を密度でなく圧力にかけたもの)
//   移す塊の熱は風上の熱を熱容量の比で分ける(塊の温度 = 風上の温度。丸めの偏りで偽の温度の縞を作らない)
//   G2: 移す塊の成分の割合は MUSCL(整数の勾配の制限。既定は MC)で面へ延ばし(煙のにじみを減らす)、物質量の端数は決定的な乱数で丸める
//   (R6。切り捨てだと微量の成分が動かない。D-428)。どちらも足し引きの形のままなので保存はビット単位のまま
//   運動量には、面の圧力(両側の平均)の力積を両側へ逆向きに足し引きし、縦の面ごとに重さ −(m − m_基準) g(両側の平均)を足す。
//   力を先に当て、その運動量で流れを出す(前進・後退の順)。
//   圧力は理想気体の本物の値 p から、層ごとの基準の静水圧 p_基準 を引いて α 倍する(音速を c̃ に落とす。PGS。Q11 は仮で C)。
//   → 基準の状態(静止大気)では力がちょうど 0 になる(静水圧の釣り合いを離散の式で厳密に。偽の流れが出ない)。
// 保存: 面の流れも力積も「片方から引いた値をそのまま他方へ足す」だけ(04 R2)。箱の外との出入り(開いた境界・壁の力積・重力)は
// GasLedger に数える。閉じた箱で成分とエネルギーの合計はビット単位で一定、運動量は「初め + 帳簿」とビット単位で一致する。
// 順番: 小刻みの初めの状態だけから全部の面を計算し、整数の足し引きで当てる(Jacobi 型。04 R3)。
//
// データの流れ: GasConfig → MakeGasCoefficients → MakeHydrostaticReference(層ごとの基準のセル)→ MakeGasBox
//   → StepGas(1 刻み = substeps 回の小刻み)→ 合計・温度・速度を読む(SumGas・DeriveGasCell)。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace bicameral::sim {

    inline constexpr uint32_t GAS_MAX_SPECIES = 4;
    inline constexpr uint32_t GAS_AXES = 3;

    // 気体の物質(G1 は自前の小さな表。反応の表 RxSpecies とつなぐのは結合の段)
    struct GasSpecies {
        uint32_t molarMass = 0;       // mg/mol(04 §2)
        uint32_t heatCapacity = 0;    // mJ/(mol·K)
        int64_t formationEnergy = 0;  // H0(J/mol = µJ/µmol)。セルのエネルギー = 熱 + Σ 物質量 × H0
    };

    // 軸ごとの境界(両端とも同じ)
    enum class GasBoundary : uint8_t {
        Wall,      // 閉じた壁: 物は通さず、圧力の力積だけ受ける(帳簿へ)
        Periodic,  // 反対側とつながる
        Open,      // 外は層ごとの基準の状態 + 風(無限の溜め。出入りは帳簿へ)
    };

    // 移す塊の成分の割合を面へ延ばす方法(G2。T-0184・07 §2.2)
    enum class GasReconstruction : uint8_t {
        Upwind,              // 1 次の風上(G1。風上のセルの割合のまま)
        Minmod,              // MUSCL・minmod の制限(最も控えめ)
        MonotonizedCentral,  // MUSCL・MC の制限(既定。にじみの増え方が minmod の約半分。07 §2.2)
    };

    struct GasConfig {
        // --- 箱 ---
        uint32_t sizeX = 8;
        uint32_t sizeY = 8;
        uint32_t sizeZ = 8;  // z が上
        std::array<GasBoundary, GAS_AXES> boundary = {GasBoundary::Wall, GasBoundary::Wall, GasBoundary::Wall};

        // --- 物質 ---
        uint32_t speciesCount = 0;
        std::array<GasSpecies, GAS_MAX_SPECIES> species = {};

        // --- 刻み ---
        uint32_t substeps = 4;              // 1 刻み(1/60 s)を何回に分けるか
        uint32_t soundSpeedMmPerS = 30000;  // 落とした音速 c̃(mm/s)。Q11 は仮で C(約 30 m/s。設定で変えられる)
        bool gravity = true;

        // --- 風(開いた境界の外の速度。2^-20 m/s)---
        std::array<int32_t, GAS_AXES> windVelocity = {0, 0, 0};

        // --- 成分の輸送(G2。T-0184・07 §2.2)---
        GasReconstruction reconstruction = GasReconstruction::MonotonizedCentral;
        bool stochasticRounding = true;  // 移す物質量の端数を決定的な乱数で丸める(R6)。false は切り捨て(G1)
        uint64_t randomSeed = 0;         // 丸めの乱数の種(世界のシード。R6)
    };

    // 小刻みの係数(設定から 1 回だけ作る。割り算はここだけで、小刻みの中は掛け算とシフト。04 §6・ADR-0010)
    struct GasCoefficients {
        std::array<uint64_t, GAS_MAX_SPECIES> massPerAmount = {};  // mg/µmol の Q32
        uint64_t centralFlow = 0;                                  // (P_左 + P_右)× これ >> 52 = 質量の流れ(mg/小刻み)
        uint64_t pressureFlow = 0;                                 // (p̃_左 − p̃_右)× これ >> 40 = 質量の流れ(mg/小刻み)
        uint64_t pressureImpulse = 0;  // (p̃_左 + p̃_右)× これ >> 16 = 面の力積(運動量の単位/小刻み)
        uint64_t gravityImpulse = 0;   // (m − m_基準)× これ >> 16 = 重さの力積(運動量の単位/小刻み)
        uint64_t soundScale = 0;  // α = c̃² ÷ c_iso² の Q32(c_iso² = p ÷ ρ は地面の層の基準から。MakeGasBox が入れる)
        uint32_t soundTemperature =
            0;  // 地面の層の基準の温度(mK)。これより熱いセルは α を T_基準 ÷ T 倍して実効の音速を c̃ に抑える
    };

    // セル(保存量だけ)。単位はレベル 0(04 §2)
    struct GasCell {
        std::array<uint64_t, GAS_MAX_SPECIES> amounts = {};  // µmol
        int64_t energy = 0;                                  // mJ(熱 + 化学。運動エネルギーは入れない。07 §2.1)
        std::array<int64_t, GAS_AXES> momentum = {};         // mg·2^-20 m/s
    };

    // セルから導く値(小刻みの初めに作る)
    struct GasDerived {
        uint64_t mass = 0;           // mg
        uint64_t inverseMass = 0;    // floor((2^64 − 1) ÷ mass)。風上の割合 = |F| × これ >> 32(Q32)
        uint64_t inverseAmount = 0;  // floor((2^64 − 1) ÷ 全物質量)。成分の割合 = 物質量 × これ >> 32(Q32。MUSCL)
        uint32_t temperature = 0;    // mK
        int64_t pressure = 0;        // µPa(本物の値)
        int64_t scaledPressure = 0;  // p̃ = α(p − p_基準)(µPa)
    };

    // 箱の外との出入りの帳簿(外から入った量を正)。閉じた箱の検査はこれを差し引く(07 §4)
    struct GasLedger {
        std::array<int64_t, GAS_MAX_SPECIES> amounts = {};
        int64_t energy = 0;
        std::array<int64_t, GAS_AXES> momentum = {};
    };

    struct GasTotals {
        std::array<uint64_t, GAS_MAX_SPECIES> amounts = {};
        int64_t energy = 0;
        std::array<int64_t, GAS_AXES> momentum = {};
    };

    struct GasBox {
        // --- 設定と基準 ---
        GasConfig config;
        GasCoefficients coefficients;
        std::vector<GasCell> reference;  // 層(z)ごとの基準の静止したセル
        std::vector<GasDerived> referenceDerived;
        std::vector<GasCell> ghost;  // 層ごとの開いた境界の外(基準 + 風の運動量)

        // --- 状態 ---
        std::vector<GasCell> cells;  // x が最も速く回る(x + sizeX × (y + sizeY × z))
        GasLedger ledger;
        uint64_t tick = 0;

        [[nodiscard]] uint32_t Index(uint32_t x, uint32_t y, uint32_t z) const {
            return x + config.sizeX * (y + config.sizeY * z);
        }
    };

    [[nodiscard]] GasCoefficients MakeGasCoefficients(const GasConfig& config);

    // 温度と物質量からセルを作る(速度 0)。エネルギー = Σ 物質量 × (H0 + 比熱 × T)
    [[nodiscard]] GasCell MakeGasCell(const GasConfig& config, std::span<const uint64_t> amounts,
                                      uint32_t temperatureMilliKelvin);

    // 一定の温度で、下の層の重さの分だけ圧力を下げていく基準の層(z = 0 が地面の圧力)。成分の割合は Q32
    [[nodiscard]] std::vector<GasCell> MakeHydrostaticReference(const GasConfig& config,
                                                                std::span<const uint32_t> fractionsQ32,
                                                                uint32_t temperatureMilliKelvin,
                                                                uint64_t surfacePressureMicroPascal);

    // 箱を作る: 全部のセルを層の基準 + 風の速度で埋める
    [[nodiscard]] GasBox MakeGasBox(const GasConfig& config, std::vector<GasCell> reference);

    // セルから導く値(層 z の基準を使う)
    [[nodiscard]] GasDerived DeriveGasCell(const GasBox& box, const GasCell& cell, uint32_t z);

    // 速度(2^-20 m/s)= 運動量 ÷ 質量
    [[nodiscard]] int64_t GasVelocity(const GasBox& box, const GasCell& cell, uint32_t axis);

    // 1 刻み(substeps 回の小刻み)
    void StepGas(GasBox& box);

    [[nodiscard]] GasTotals SumGas(const GasBox& box);

    // 状態のハッシュ(決定性の検査)
    [[nodiscard]] uint64_t HashGas(const GasBox& box);

}  // namespace bicameral::sim
