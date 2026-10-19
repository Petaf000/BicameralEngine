// probe_gas.h — 仮の世界(ProbeSim の 64³ のセル)に気体の流れを載せる CPU リファレンス(T-0590・ADR-0256。07 §2・05 §3)。
//
// 形(ADR-0256 の 1〜4。仮・ユーザー未確認):
//   - **セルが唯一の真実**: 気体の成分は、セル(reaction.hlsli の RxCell)の成分のうち表で凝縮相でない物質(condensed = false)。
//     気体の別の格子は持たない。反応は同じセルの O2 を読み、生成物の CO2・H2O はそのまま流れに乗る。
//     足した欄は気体の運動量 ×3(mg·2^-20 m/s)と、その運動エネルギーのぶん(mJ)と、その時の気体の質量(mg)だけ
//     (セルの外に持つ。GPU に載せるのは T-0592)。
//   - **運動量と運動のエネルギーは気体の物質に付く**(T-0646・ADR-0286): 気体の段の外(反応・相変化・置く操作)でセルの気体の質量が
//     変わったら、段の入口で運動量を今の質量に合わせる。減った時は速さを保つ(減った気体は同じ速さで凝縮相へ移り、その運動量は
//     帳簿へ。凝縮相は運動量を持たない)、増えた時は運動量を保つ(増えた気体は静止して現れる)。どちらも運動のエネルギーは前の値を
//     超えず、差は熱になる(気体の箱のエネルギーが前の運動エネルギーのぶんを含むので、帳簿は要らない)。段の終わりに気体が残らない
//     セルの運動量は帳簿へ(質量の無い運動量は持たない)。
//   - **区画**: 気体を流すのは世界の中の直方体の区画だけ(既定は無い = 今までと同じ。ブランクの世界は何も読まずに動く。D-455)。
//     区画の外の面は軸ごとに壁・周期・開いた境界(gas_reference の GasBoundary)。
//   - **段の順**: 刻みの初め(コマンドと物理の熱の入口の後)に気体の 1 刻み → 伝導と反応(06 §2 の段 3 → 段 4)。
//   - **圧力の解き方は差し替えられる**: 陽解法の小刻み(gas_reference の StepGas。既定)か投影(gas_projection の StepGasProjection。
//     T-0503・T-0530。GPU は T-0531)。どちらも同じ箱(GasBox)を進めるので、ここは選ぶだけ。
//
// 1 刻みの流れ: 区画のセル(RxCell)→ 気体の箱(GasBox。区画の気体の成分を物質 ID の順の欄へ。数の上限なし〔GasPerSpecies。T-0591〕。
//   丸めの乱数の鍵は物質 ID〔欄の並びが変わっても同じ乱数〕、
//   エネルギーは気体のぶん = 熱 × 気体の熱容量 ÷ セルの熱容量 + 気体の化学 + 運動エネルギーのぶん)→ StepGas / StepGasProjection
//   → セルへ書き戻す(凝縮相のぶん = 前のエネルギー − 気体のぶん はそのまま、気体の成分とエネルギーを新しい値に)。
//   区画の外との出入り(開いた境界・壁の力積・重さの仕事)は帳簿(物質 ID ごと)に足す。
// セルの空き(T-0593・ADR-0268): 固体と液体が占める体積(物質の密度から。sim/cell_volume)を除いた残りが気体の体積。
//   圧力はその体積で読み、面は両側の空きの小さい方だけ開く(GasBox::openQ16)。空きが 0 のセルは気体を通さない。
//   陽解法も投影(T-0667)も同じ数え方で読む。
//   密度を書いた物質が無い世界・どのセルも空きが全部の刻みは今までとビット単位で同じ。
//   → 区画のセルのエネルギーの合計 + 運動エネルギーのぶん − 帳簿、元素の数 − 帳簿、運動量 − 帳簿 は毎刻みビット単位で一定。
// 比熱: 気体の段は表の比熱(RxSpecies.heatCapacity)を定積の比熱として読む。パッケージは定圧の c_p を書き、ベイクが c_v = c_p − R を作る
//   (ReactionTableDefinition::gasHeatCapacityAtConstantPressure。既定 true。T-0591・ADR-0259・T-0597)。
// 今の制限(分けたチケット): 流すと 1 セルの成分がインラインの数(RX_MAX_CELL_SPECIES)を超える刻みは流さずに数える
//   (溢れのセルは R-MULTI-4)・投影の解き方はセルの空きを読まない(T-0617)・
//   GPU と保存点・写しの早送り・状態のハッシュへの組み込み(T-0592。GPU の段は成分 GAS_MAX_SPECIES 個まで)。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "common/reaction.hlsli"
#include "sim/gas_cells.h"
#include "sim/gas_fastest_check.h"
#include "sim/gas_projection.h"
#include "sim/gas_reference.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 圧力の解き方(07 §2.3・§2.6)
    enum class ProbeGasPressure : uint8_t {
        Explicit,    // 陽解法の小刻み(本物の音速を substeps 回。GPU は gas_step.hlsl)
        Projection,  // 刻みの中で圧力を解く投影(外側の反復つき。GPU は T-0531)
    };

    struct ProbeGasOptions {
        // --- 区画(仮の世界の格子の座標。y が上〔world_cells の約束〕。ADR-0256 の 5)---
        std::array<uint32_t, 3> minCell = {0, 0, 0};
        std::array<uint32_t, 3> sizeCells = {0, 0, 0};  // 0 を含めば気体は無い
        // 区画の外の面(格子の軸 x・y・z の順)
        std::array<GasBoundary, 3> boundary = {GasBoundary::Wall, GasBoundary::Wall, GasBoundary::Wall};
        // 格子の y = 0 の床の高さ(mm。基準の大気をどの高さで読むか)
        int64_t floorHeightMillimeters = 0;

        // --- 解き方 ---
        ProbeGasPressure pressure = ProbeGasPressure::Explicit;
        uint32_t substeps = 24;  // 陽解法の小刻み(07 §2.3)
        GasProjectionOptions
            projection;           // 投影の設定(音速は projection.pressure.soundSpeedMmPerS。既定 0 = 最速。D-467)
        uint64_t randomSeed = 0;  // 移す物質量の乱数の丸めの種(世界のシード。R6)

        // --- 静止の丸め(T-0723。研究。0 = 切る〔既定。今までとビット単位で同じ〕。GPU の区画も同じ形で丸める。T-0775)---
        // 刻みの終わりに、速さがどの軸も restSpeedUnits 単位(2^-20 m/s)未満のセルの運動量を 0 にし、運動エネルギーのぶんを
        // そのセルの熱へ移す(エネルギーの和は変わらない)。消した運動量は帳簿へ(世界の帳簿の列「静止の丸め」WL_REASON_REST_ROUNDING)
        uint32_t restSpeedUnits = 0;

        [[nodiscard]] bool Enabled() const { return sizeCells[0] != 0 && sizeCells[1] != 0 && sizeCells[2] != 0; }
    };

    // 区画の外との出入り(外から入った量を正)。物質 ID ごと
    struct ProbeGasLedger {
        std::vector<int64_t> amounts;                 // µmol。添字 = 物質 ID
        int64_t energy = 0;                           // mJ(開いた境界の出入り + 重さの仕事。07 §2.4)
        std::array<int64_t, GAS_AXES> momentum = {};  // 格子の軸 x・y・z の順

        // 内訳: 静止の丸め(T-0723)で消した運動量(momentum に含む)と熱へ移した運動エネルギー(mJ。セルの中で移すので energy には入れない)
        std::array<int64_t, GAS_AXES> restMomentum = {};
        int64_t restEnergy = 0;
    };

    // 1 刻みの報告
    struct ProbeGasStepReport {
        uint32_t gasSpecies = 0;  // 区画の気体の物質の数
        // 流すと 1 セルの成分がインラインの数(RX_MAX_CELL_SPECIES)を超えるので流さなかった(何も変えない。溢れのセルは R-MULTI-4)
        bool skippedCellOverflow = false;
        uint32_t changedCells = 0;     // 値が変わったセルの数
        uint32_t restedCells = 0;      // 静止の丸めで止めたセルの数(T-0723)
        GasProjectionCost projection;  // 投影の費用(投影のときだけ)

        // --- セルの空き(固体と液体が占めない体積。T-0593・ADR-0268)---
        uint32_t blockedCells = 0;  // 空きが 0(気体が入れない・面は全部閉じる)
        uint32_t partialCells = 0;  // 空きが一部

        // --- 最速 × 小さい空き(T-0788・D-519・ADR-0346。最速の投影のときだけ数える。値は変えない = D-487 ②)---
        uint32_t smallOpenJoints = 0;  // 小さい空きのセルだけでつながった気体の部分の組の数(この刻みのセルの空き)
        std::vector<std::string>
            warnings;  // この刻みに新しく見つかった組の警告の文(頭は GAS_FASTEST_SMALL_OPEN_WARNING_PREFIX)
    };

    class ProbeGasField {
    public:
        // 区画を作る。断る: 区画が格子からはみ出す・基準の大気が無い・音速の基準が 0(表の環境の値)
        [[nodiscard]] static std::expected<ProbeGasField, std::string> Create(const BakedReactionTable& table,
                                                                              const ProbeGasOptions& options);

        // 格子の層 y の基準の大気のセル(静止。区画を静かな大気で満たす時に使う。速度 0)
        [[nodiscard]] reaction::RxCell ReferenceCell(const BakedReactionTable& table, uint32_t gridY) const;

        // 刻み tick の気体の 1 刻み。cells は格子の全部のセル(刻み tick の始めの世代。その場で書き換える)。
        // 値が変わったセルの番号を changed に足す(呼ぶ側が熱のキャッシュを作り直し、活性の種にする)
        ProbeGasStepReport Step(const BakedReactionTable& table, std::span<reaction::RxCell> cells, uint64_t tick,
                                std::vector<uint32_t>& changed);

        [[nodiscard]] const ProbeGasOptions& Options() const { return m_options; }

        // 最速の投影(音速 0)で解くか(最速 × 小さい空きの検査をするのはこのときだけ。T-0788)。
        // 世界が最速を選んだ(環境の gas_pressure_speed = "instant"。T-0804・D-526 ②)か、options が音速 0 の投影
        [[nodiscard]] bool FastestProjection() const;

        // 置き方の検査(T-0788・D-519): 最速の投影なら、cells(格子の全部のセル)の区画のセルの空きを読み、小さい空きのセルだけで
        // つながった気体の部分の組ごとに警告の文を返す(最速でなければ空)。世界を置いた時・ベイクした世界を渡す時に呼ぶ。
        // 刻みの中は Step が空きの変わった刻みに同じ検査をし、新しく見つかった組を ProbeGasStepReport::warnings に入れる
        [[nodiscard]] std::vector<std::string> CheckFastestOpenings(const BakedReactionTable& table,
                                                                    std::span<const reaction::RxCell> cells) const;

        // 区画のセル region(区画の中の並び)で同じ検査(最速でなくても検査する)
        [[nodiscard]] std::vector<std::string> CheckRegionOpenings(const BakedReactionTable& table,
                                                                   std::span<const reaction::RxCell> region) const;
        [[nodiscard]] const ProbeGasLedger& Ledger() const { return m_ledger; }

        // 区画のセル(区画の中の x + 幅 × (z + 奥行き × y)。気体の箱と同じ並び)の運動量と運動エネルギーのぶん
        [[nodiscard]] std::span<const std::array<int64_t, GAS_AXES>> Momentum() const { return m_fields.momentum; }
        [[nodiscard]] int64_t KineticEnergySum() const;
        [[nodiscard]] std::span<const uint64_t> Mass() const {
            return m_fields.mass;
        }  // 前の段の終わりの気体の質量(mg。T-0646)

        // 区画のセルの番号(格子の全部のセルの中の番号)。region は区画の中の番号
        [[nodiscard]] uint32_t GridIndex(uint32_t region) const;
        [[nodiscard]] uint32_t RegionCellCount() const { return (uint32_t)m_fields.momentum.size(); }

        // 基準の大気の物質(ID の順)と、それだけを欄にした静かな箱(GPU の段の設定の型・層の基準・開いた境界の外を作る。T-0592)
        [[nodiscard]] std::span<const uint32_t> AtmosphereSpecies() const { return m_layers.Species(); }
        [[nodiscard]] GasBox MakeAtmosphereBox(const BakedReactionTable& table) const;

        // 気体の欄(運動量・運動エネルギーのぶん・帳簿)のハッシュ(決定性の検査。セルは ProbeStateHash)
        [[nodiscard]] uint64_t Hash() const;

        // 世界の状態のハッシュに足す寄与(区画のセルごとの寄与の和 + 帳簿の寄与。GPU の GasHash と同じ値。
        // common/probe_region_hash.hlsli・T-0620)
        [[nodiscard]] uint64_t HashTerm() const;

    private:
        ProbeGasOptions m_options;
        bool m_worldInstant =
            false;  // 表の環境が最速を選んだ(T-0804。刻みごとに表から読み直す = ホットリロードで替わる)
        GasAtmosphereLayers m_layers;               // 層ごとの基準の大気(区画の層 = 格子の y)
        GasCellFields m_fields;                     // 区画のセルの気体の欄(気体の箱の並び。軸は気体の箱の x・y・z)
        GasProjection m_projection;                 // 前の刻みの圧力(投影の初めの近似)
        std::vector<uint32_t> m_projectionSpecies;  // m_projection を作った時の物質(変わったら捨てる)
        ProbeGasLedger m_ledger;

        // --- 最速 × 小さい空き(T-0788)。前に検査したセルの空きと見つけた組(空きが変わった刻みだけ検査し、新しい組だけ警告する)---
        bool m_openingsChecked = false;
        std::vector<uint32_t> m_checkedOpen;
        std::vector<GasSmallOpenJoint> m_joints;

        [[nodiscard]] GasConfig MakeConfig(const BakedReactionTable& table, std::span<const uint32_t> slots) const;
        [[nodiscard]] std::string JointWarning(const GasSmallOpenJoint& joint) const;
        void SyncWorldPressure(const BakedReactionTable& table);
        void CheckOpeningsOnStep(const GasConfig& config, std::span<const uint32_t> openQ16,
                                 ProbeGasStepReport& report);
    };

}  // namespace bicameral::sim
