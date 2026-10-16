// gas_reference.h — 気体(空気・煙・湯気)の流れの CPU リファレンス、段 G1・G2(T-0026・T-0184。07 §2・§2.1・§2.2・ADR-0043)。
// 1 レベル(0.5 m 角のセルの直方体)だけ。GPU・多重解像度・反応との結合はまだ無い(G3・G4。T-0185・T-0186)。
//
// 方式: 圧縮性の有限体積法を陽解法で。セルに保存量(成分ごとの物質量・内部エネルギー・運動量 ×3)を持ち、
// 面ごとに 1 つの質量の流れ F を計算して、風上側のセルの中身を同じ割合(F ÷ 風上の質量)だけ移す(成分・エネルギー・運動量が一緒に動く)。
//   F = 中心の質量の流れ(両側の運動量の平均)+ 圧力の拡散(両側の圧力の差 ÷ 4c̃。Rusanov の散逸の半分を密度でなく圧力にかけたもの。
//   半分にしたのは 3 次元の陽解法の安定の条件を c̃ Δt ÷ Δx ≤ 1/3 から約 2/3 に広げ、本物の音速を小刻み 24 で回すため。T-0226)
//   移す塊の熱は風上の熱を熱容量の比で分ける(塊の温度 = 風上の温度。丸めの偏りで偽の温度の縞を作らない)
//   G2: 移す塊の成分の割合は MUSCL(整数の勾配の制限。既定は MC)で面へ延ばし(煙のにじみを減らす)、物質量の端数は決定的な乱数で丸める
//   (R6。切り捨てだと微量の成分が動かない。D-428)。どちらも足し引きの形のままなので保存はビット単位のまま
//   運動量には、面の圧力(両側の平均)の力積を両側へ逆向きに足し引きし、縦の面ごとに重さ −(m − m_基準) g(両側の平均)を足す。
//   力を先に当て、その運動量で流れを出す(前進・後退の順)。
//   圧力は理想気体の本物の値 p から、層ごとの基準の静水圧 p_基準 を引いて α = c̃² ÷ (γ c_iso²) 倍する(実効の音速を c̃ にする。
//   既定の c̃ は本物の音速 340 m/s〔D-440・T-0226〕。α ≈ 0.98)。
//   T-0234(断熱。既定): セルのエネルギーは全エネルギー(熱 + 化学 + 運動)。移す塊は風上のエンタルピー(熱 + 押し出す仕事 pV)と
//   運動エネルギーを運び、上へ動いた塊は重さの仕事 m g Δx を失う(帳簿へ)。温度は全エネルギーから運動エネルギーを引いた熱から。
//   → 圧縮で温まり膨張で冷え(pdV)、衝撃波・混ざって失った運動エネルギーは熱になる(散逸)。全エネルギーは足し引きの形のまま保存。
//   等温(T-0226 までの形)も設定で選べる(α = c̃² ÷ c_iso²・運動エネルギーを数えない)。
//   → 基準の状態(静止大気)では力がちょうど 0 になる(静水圧の釣り合いを離散の式で厳密に。偽の流れが出ない)。
// 保存: 面の流れも力積も「片方から引いた値をそのまま他方へ足す」だけ(04 R2)。箱の外との出入り(開いた境界・壁の力積・重力)は
// GasLedger に数える。閉じた箱で成分の合計はビット単位で一定、エネルギー(断熱では重さの仕事を帳簿へ)と運動量は「初め + 帳簿」とビット単位で一致する。
// 順番: 小刻みの初めの状態だけから全部の面を計算し、整数の足し引きで当てる(Jacobi 型。04 R3)。
//
// データの流れ: GasConfig → MakeGasCoefficients → MakeHydrostaticReference(層ごとの基準のセル)→ MakeGasBox
//   → StepGas(1 刻み = substeps 回の小刻み)→ 合計・温度・速度を読む(SumGas・DeriveGasCell)。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "common/world_physics.hlsli"

namespace bicameral::sim {

    // 成分の欄を中に持つ数(GPU の段〔gas_step.hlsl・GAS_GPU_MAX_SPECIES〕と多重解像度の木〔gas_tree〕が回す欄の数)。
    // 1 レベルの箱の CPU リファレンスは上限なし(GasPerSpecies。5 つ目から外の配列。T-0591・D-401)
    inline constexpr uint32_t GAS_MAX_SPECIES = 4;
    inline constexpr uint32_t GAS_AXES = 3;
    // セルの空きの 1(Q16。GasBox::openQ16。T-0593)
    inline constexpr uint32_t GAS_OPEN_FULL = 1u << 16;

    // 丸めの乱数の鍵を欄の番号にする印(GasSpecies::roundingKey の既定)
    inline constexpr uint32_t GAS_ROUNDING_KEY_SLOT = UINT32_MAX;

    // 成分ごとの値の並び(T-0591。気体の物質の数に上限を置かない。D-401)。
    // GAS_MAX_SPECIES 個までは中に持ち、それより後ろの欄に書いた時に全部を外の配列へ移す(RxCell のインライン + 溢れと同じ考え。
    // 4 つ以下の箱は確保せず、今までと同じ値・同じ速さ)。並びの外の欄を読むと 0(書いていない成分は 0)。
    // 比べる時も足りない方を 0 で埋める(並びの長さに依らない)
    template <typename T>
    class GasPerSpecies {
    public:
        GasPerSpecies() = default;

        GasPerSpecies(std::initializer_list<T> values) {
            uint32_t slot = 0;
            for (const T& value : values)
                (*this)[slot++] = value;
        }

        // 中に持つ形からの写し(木と GPU の欄。GAS_MAX_SPECIES 個)
        GasPerSpecies(const std::array<T, GAS_MAX_SPECIES>& values)
            : m_inline(values) {}  // NOLINT(google-explicit-constructor)

        [[nodiscard]] uint32_t Count() const { return m_spill.empty() ? GAS_MAX_SPECIES : (uint32_t)m_spill.size(); }

        [[nodiscard]] const T& operator[](size_t slot) const {
            if (slot >= Count())
                return s_zero;

            return Data()[slot];
        }

        T& operator[](size_t slot) {
            if (slot >= Count())
                Grow(slot + 1);

            return Data()[slot];
        }

        [[nodiscard]] bool operator==(const GasPerSpecies& other) const {
            const uint32_t count = std::max(Count(), other.Count());
            for (uint32_t slot = 0; slot < count; ++slot) {
                if (!((*this)[slot] == other[slot]))
                    return false;
            }

            return true;
        }

        operator std::span<const T>() const { return {Data(), Count()}; }  // NOLINT(google-explicit-constructor)

        // 範囲 for(並びの全部の欄)
        T* begin() { return Data(); }                      // NOLINT(readability-identifier-naming)
        T* end() { return Data() + Count(); }              // NOLINT(readability-identifier-naming)
        const T* begin() const { return Data(); }          // NOLINT(readability-identifier-naming)
        const T* end() const { return Data() + Count(); }  // NOLINT(readability-identifier-naming)

    private:
        std::array<T, GAS_MAX_SPECIES> m_inline = {};
        std::vector<T> m_spill;  // 空でなければこちらが全部の欄(中の欄は使わない)
        static inline const T s_zero{};

        [[nodiscard]] T* Data() { return m_spill.empty() ? m_inline.data() : m_spill.data(); }
        [[nodiscard]] const T* Data() const { return m_spill.empty() ? m_inline.data() : m_spill.data(); }

        void Grow(size_t count) {
            if (m_spill.empty())
                m_spill.assign(m_inline.begin(), m_inline.end());

            m_spill.resize(count);
        }
    };

    // 気体の物質(G1 は自前の小さな表。反応の表 RxSpecies とつなぐのは結合の段)
    struct GasSpecies {
        uint32_t molarMass = 0;       // mg/mol(04 §2)
        uint32_t heatCapacity = 0;    // mJ/(mol·K)。断熱では定積のモル比熱 c_v(定圧の分 R は押し出す仕事で入る。T-0234)
        int64_t formationEnergy = 0;  // H0(J/mol = µJ/µmol)。セルのエネルギー = 熱 + Σ 物質量 × H0
        // 移す物質量の乱数の丸めの鍵(R6)。既定は欄の番号。世界のセルに載せる時は物質 ID(欄の並びが変わっても同じ乱数。T-0591)。
        // GPU の段は欄の番号だけ(GpuGas は既定でない鍵を断る)
        uint32_t roundingKey = GAS_ROUNDING_KEY_SLOT;

        [[nodiscard]] bool operator==(const GasSpecies&) const = default;
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

    // エネルギーの扱い(T-0234・07 §2.4)
    enum class GasThermodynamics : uint8_t {
        Isothermal,  // 等温(T-0226 まで): エネルギーは熱 + 化学、塊は風上の熱だけを運ぶ。圧縮・衝撃波で温まらない
        Adiabatic,  // 断熱(既定): エネルギーは熱 + 化学 + 運動、塊はエンタルピーと運動エネルギーを運び、重さの仕事を帳簿へ
    };

    struct GasConfig {
        // --- 箱 ---
        uint32_t sizeX = 8;
        uint32_t sizeY = 8;
        uint32_t sizeZ = 8;  // z が上
        std::array<GasBoundary, GAS_AXES> boundary = {GasBoundary::Wall, GasBoundary::Wall, GasBoundary::Wall};

        // --- 物質 ---
        uint32_t speciesCount = 0;
        GasPerSpecies<GasSpecies> species;  // 数の上限なし(GPU と木は GAS_MAX_SPECIES まで。T-0591)

        // --- 刻み ---
        uint32_t substeps = 24;  // 1 刻み(1/60 s)を何回に分けるか。c̃ Δt ÷ Δx = 0.47(安定の限界は約 2/3。T-0226)
        // 音速 c̃(mm/s。世界の環境の値。ADR-0176)。器の既定は 0 = 無い(ブランクの世界)。地球の 340 m/s(D-440)は標準パッケージ
        // earth_environment の値を sim::ApplyWorldPhysics で入れる(world_physics.h。T-0373)。上限は GasSoundSpeedLimitMmPerS(substeps)
        uint32_t soundSpeedMmPerS = 0;
        bool gravity = true;
        // 重力の強さ(µm/s²。世界のデータ。ADR-0171・world_physics.hlsli)。重さの力積・層の重さの圧力・重さの仕事はここから作る
        // 器の既定は 0 = ブランクの世界(D-480)。地球の値は標準パッケージ earth_environment の値を sim::ApplyWorldPhysics で入れる
        uint32_t gravityMicrometersPerSecond2 = 0;
        GasThermodynamics thermodynamics = GasThermodynamics::Adiabatic;

        // これより熱いセルは実効の音速をそこで頭打ちにする(mK。安定の条件を守る)。0 = 安定の上限から求める(GasSoundCapTemperature。
        // D-461〔Q32 案 D〕・T-0269。既定の 340 m/s・小刻み 24 で約 372 K。T-0631)。UINT32_MAX = 頭打ちなし(試験だけ)。
        // 地面の層の基準の温度を入れると T-0234 までの形(Q32 の案 A。熱いセルの音速を c̃ に抑える)
        uint32_t soundCapTemperature = 0;

        // --- 風(開いた境界の外の速度。2^-20 m/s)---
        std::array<int32_t, GAS_AXES> windVelocity = {0, 0, 0};

        // --- 成分の輸送(G2。T-0184・07 §2.2)---
        GasReconstruction reconstruction = GasReconstruction::MonotonizedCentral;
        bool stochasticRounding = true;  // 移す物質量の端数を決定的な乱数で丸める(R6)。false は切り捨て(G1)
        uint64_t randomSeed = 0;         // 丸めの乱数の種(世界のシード。R6)

        [[nodiscard]] bool operator==(const GasConfig&) const = default;
    };

    // 音速の基準の上限(mm/s。器の決まり。ADR-0176): 安定の条件 ν = c̃ Δt_小刻み ÷ Δx ≤ 0.6(Δx = 0.5 m・Δt_小刻み = 1/60 s ÷ 小刻み)。
    // 圧力の拡散(陽解法の 3 次元の拡散)の境は約 2/3 で、余裕を見て 0.6(T-0226)。既定の小刻み 24 で 432 m/s
    [[nodiscard]] constexpr uint64_t GasSoundSpeedLimitMmPerS(uint32_t substeps) {
        return (uint64_t)substeps * 18000;
    }

    // 熱いセルの音速の頭打ちの温度(mK。D-461〔Q32 案 D〕・T-0269・T-0631・07 §2.4): 圧力の拡散の数が安定の上限に届く温度。
    // 圧力の拡散は Δp̃ ÷ 4c̃(c̃ は世界の基準で決まった値)なので、実効の音速 c_eff = c̃ √(T ÷ T_基準)(c_iso² ∝ T)のセルの
    // 拡散の数は c_eff² ÷ c̃ × Δt ÷ Δx = ν_c̃ × T ÷ T_基準(音の速さの数 c_eff Δt ÷ Δx より大きい。熱いセルではこちらが縛る)。
    // これが上限 ÷ c̃ 倍の ν(1 レベルの箱は GasSoundSpeedLimitMmPerS = ν 0.6。多重解像度の木は細かいレベルの分だけ小さい)に届く温度
    // T_基準 × 上限 ÷ c̃(切り捨て)。既定の 340 m/s・小刻み 24・293.15 K で約 372 K(実効の音速 約 383 m/s)。
    // T-0269 の T_基準 ×(上限 ÷ c̃)²(約 473 K)は音の速さの数だけを見ていて、373〜473 K のセルで拡散の数が 2/3 を超え、
    // 1 セルおきの縞(奇偶のモード。陽解法の拡散の最も不安定な波)が育った(T-0631)。
    // c̃ が上限なら T_基準。uint32 を超えるなら UINT32_MAX(頭打ちなし)
    [[nodiscard]] uint32_t GasSoundCapTemperature(uint32_t soundSpeedMmPerS, uint64_t soundSpeedLimitMmPerS,
                                                  uint32_t referenceTemperature);

    // 小刻みの係数(設定から 1 回だけ作る。割り算はここだけで、小刻みの中は掛け算とシフト。04 §6・ADR-0010)
    struct GasCoefficients {
        GasPerSpecies<uint64_t> massPerAmount;  // mg/µmol の Q32
        uint64_t centralFlow = 0;               // (P_左 + P_右)× これ >> 52 = 質量の流れ(mg/小刻み)
        uint64_t pressureFlow = 0;              // (p̃_左 − p̃_右)× これ >> 40 = 質量の流れ(mg/小刻み)
        uint64_t pressureImpulse = 0;           // (p̃_左 + p̃_右)× これ >> 16 = 面の力積(運動量の単位/小刻み)
        uint64_t gravityImpulse = 0;            // (m − m_基準)× これ >> 16 = 重さの力積(運動量の単位/小刻み)
        uint64_t hydrostatic = 0;               // 質量(mg)× これ ÷ 10^4 = 1 セルの高さの層の重さの圧力(µPa。レベル 0)
        uint64_t gravityWork = 0;               // 質量(mg)× これ ÷ 10^6 = 1 セル上がる重さの仕事(µJ。レベル 0)
        uint64_t soundScale =
            0;  // α = c̃² ÷ (γ c_iso²) の Q32(等温は γ = 1。c_iso² = p ÷ ρ・γ は地面の層の基準から。MakeGasBox が入れる)
        uint32_t soundTemperature =
            0;  // 頭打ちの温度 T_頭(mK。MakeGasBox が入れる)。これより熱いセルは α を T_頭 ÷ T 倍して実効の音速を抑える
    };

    // セル(保存量だけ)。単位はレベル 0(04 §2)
    struct GasCell {
        GasPerSpecies<uint64_t> amounts;  // µmol
        int64_t energy = 0;               // mJ(断熱: 熱 + 化学 + 運動〔T-0234〕。等温: 熱 + 化学。07 §2.1・§2.4)
        std::array<int64_t, GAS_AXES> momentum = {};  // mg·2^-20 m/s
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
        GasPerSpecies<int64_t> amounts;
        int64_t energy = 0;  // 開いた境界の出入り + 重さの仕事(断熱。上へ動いた塊が失う m g Δx。T-0234)
        std::array<int64_t, GAS_AXES> momentum = {};
    };

    struct GasTotals {
        GasPerSpecies<uint64_t> amounts;
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

        // セルの空き(気体が使える体積の割合。Q16。GAS_OPEN_FULL = セルの全部。T-0593・ADR-0268)。空 = 全部のセルが全部(今まで通り。
        // ビット単位で同じ)。圧力は空きの体積で読み(p = nRT ÷ 空きの体積)、面は両側の空きの小さい方だけ開く(残りは壁として押し返し、
        // 帳簿へ)。基準との質量の差は「基準 × 空き」から数える(固体の多いセルの静かな大気は動かない)。
        // 陽解法(StepGas)だけが読む(投影は T-0617)。世界のセルから毎刻み作る(sim/cell_volume。sim/probe_gas)
        std::vector<uint32_t> openQ16;

        [[nodiscard]] uint32_t Index(uint32_t x, uint32_t y, uint32_t z) const {
            return x + config.sizeX * (y + config.sizeY * z);
        }
    };

    [[nodiscard]] GasCoefficients MakeGasCoefficients(const GasConfig& config);

    // 温度と物質量からセルを作る(速度 0)。エネルギー = Σ 物質量 × (H0 + 比熱 × T)
    [[nodiscard]] GasCell MakeGasCell(const GasConfig& config, std::span<const uint64_t> amounts,
                                      uint32_t temperatureMilliKelvin);

    // 一定の温度で、下の層の重さの分だけ圧力を下げていく基準の層(z = 0 が地面の圧力)。成分の割合は Q32。
    // 全部の層が同じ温度と成分の、下の層ごとの版(ビット単位で同じ)
    [[nodiscard]] std::vector<GasCell> MakeHydrostaticReference(const GasConfig& config,
                                                                std::span<const uint32_t> fractionsQ32,
                                                                uint32_t temperatureMilliKelvin,
                                                                uint64_t surfacePressureMicroPascal);

    // 1 つの層の基準の大気(高さの分布。T-0499・world_atmosphere.h の SampleAtmosphere をその層の高さで読んだもの)
    struct GasReferenceLayer {
        uint32_t temperatureMilliKelvin = 0;
        GasPerSpecies<uint32_t> fractionsQ32;  // 気体の物質の順
    };

    // 層ごとの温度と成分で、下の層の重さの分だけ圧力を下げていく基準の層(z = 0 が地面の圧力。圧力は作り手が書かず、
    // 重さと温度から静水圧の釣り合いで出す。D-494)。呼ぶ側の約束: layers は config.sizeZ 層
    [[nodiscard]] std::vector<GasCell> MakeHydrostaticReference(const GasConfig& config,
                                                                std::span<const GasReferenceLayer> layers,
                                                                uint64_t surfacePressureMicroPascal);

    // 箱を作る: 全部のセルを層の基準 + 風の速度で埋める
    [[nodiscard]] GasBox MakeGasBox(const GasConfig& config, std::vector<GasCell> reference);

    // --- 環境の値のホットリロード(T-0444・ADR-0194。world_physics.h)---

    // 今の箱の設定 current を next に替えられるか。違ってよいのは環境の値(重力・音速の基準)だけ(箱の形・物質・小刻み・境界・風・
    // 丸めは世界の作り)。音速の基準は 0 にできない(気体の箱に要る)・小刻みの安定の上限まで、重力は器の上限まで。外なら理由
    [[nodiscard]] std::expected<void, std::string> CheckGasEnvironment(const GasConfig& current, const GasConfig& next);

    // 箱の環境を替える: 設定・係数・層の基準(reference。新しい重力・基準の大気で MakeHydrostaticReference が作ったもの)と、
    // そこから導く値・開いた境界の外・α・頭打ちの温度を作り直す。状態(セル・帳簿・刻み)はそのまま(保存量に触れない)なので、
    // 新しい重力・基準と釣り合っていない分は次の刻みから流れになる。刻みの境目(StepGas の間)で呼ぶ。
    // 呼ぶ側の約束: CheckGasEnvironment(box.config, config) が通る・reference は sizeZ 層。GPU は GpuGas::RecordEnvironment
    void SetGasEnvironment(GasBox& box, const GasConfig& config, std::vector<GasCell> reference);

    // セルから導く値(層 z の基準を使う)
    [[nodiscard]] GasDerived DeriveGasCell(const GasBox& box, const GasCell& cell, uint32_t z);

    // 運動エネルギー(µJ)= Σ (運動量² >> 41)÷ 質量(断熱のときだけ。等温は 0。T-0234)
    [[nodiscard]] int64_t GasKineticEnergy(const GasBox& box, const GasCell& cell);

    // 運動量を替え、運動エネルギーの差をエネルギーへ足す(熱を変えずに動かす。断熱のとき)
    void SetGasMomentum(const GasBox& box, GasCell& cell, const std::array<int64_t, GAS_AXES>& momentum);

    // 速度(2^-20 m/s)= 運動量 ÷ 質量
    [[nodiscard]] int64_t GasVelocity(const GasBox& box, const GasCell& cell, uint32_t axis);

    // 1 刻み(substeps 回の小刻み)
    void StepGas(GasBox& box);

    [[nodiscard]] GasTotals SumGas(const GasBox& box);

    // 状態のハッシュ(決定性の検査)
    [[nodiscard]] uint64_t HashGas(const GasBox& box);

}  // namespace bicameral::sim
