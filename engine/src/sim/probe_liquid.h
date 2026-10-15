// probe_liquid.h — 仮の世界(ProbeSim の 64³ のセル)に液体の流れを載せる CPU リファレンス(T-0609・ADR-0266。07 §3・05 §3)。
//
// 形(ADR-0266。仮・ユーザー未確認):
//   - **セルが唯一の真実**: 液体の成分は、セル(reaction.hlsli の RxCell)の成分のうち表で液体の物質(phase = "liquid"。
//     BakedReactionTable::speciesLiquid)。液体の別の格子は持たない。足した欄は面の運動量 ×3 だけ(区画のセルの + 側の面)。
//   - **3D のパイプ模型 + 列の静水圧**(07 §3・T-0026 の W1): 面ごとに運動量を持ち(スタッガード)、両側の圧力の差と重さで毎刻み変える。
//     圧力は質量の単位(mg)で書く: P = 2 × 支えている上の液体の質量 + 自分の質量 + 溢れの圧力(体積がセルを超えたぶん × 倍率)。
//     支えている上の液体 = (上のセルが支えている分 + 上のセルの質量)× 自分の満ち具合(満ちていないセルは上の水を支えきれず、上の水は落ちてくる)。
//     さらに P に「下のセルが支える割合」(壁・区画の床は 1、液体は満ち具合、空は 0)を掛ける(落ちている水は横へ押し広げない)。
//     縦の面は P_下 − P_上 − (m_下 + m_上)。満ちたセルの上に静かな水があれば、P の差と重さがちょうど打ち消し合う(整数で厳密に 0)。
//   - **運動量の保存**(D-442): 各セルの圧力は自分の両側の面を同じ大きさで逆向きに押すので、内側の力の和は整数で厳密に 0。
//     外から入る運動量(壁と床の押し返し・重さ・摩擦・行き場のない運動量)は帳簿へ。→ Σ 面の運動量 − 帳簿 は毎刻みビット単位で一定。
//     運動量を液体と一緒に運ぶ(移流)のはまだ無い(上流が空になった面の運動量は「行き場なし」として帳簿へ。T-0610)。
//   - **質量・成分・エネルギー**: 面の運動量から 1 刻みに動く質量を決め(上流のセルから。出る合計がセルの液体を超えれば比で縮める)、
//     上流の液体の成分の比で物質量を、液体のぶんのエネルギー(熱 × 動く熱容量 ÷ セルの熱容量 + 動く化学)を一緒に動かす。
//     整数で引いて足すので、物質量とエネルギーは構造的にビット単位で保存。前の刻みの値だけから決める(Jacobi 型。順番に依存しない)。
//   - **壁**: 液体でない凝縮相(固体)を少しでも含むセルは液体を通さない(仮。固体の体積を分け合うのは T-0612)。区画の外の面も壁。
//   - **段の順**: 適用 → 物理の熱 → 表の差し替え → 気体 → **液体** → 伝導と反応(ProbeReference::Advance の (1c))。
// 今の制限(分けたチケット): 位置と運動のエネルギー(落ちる水が温まる)を数えない(T-0610)・相変化は反応表(T-0611)・
//   気体と体積を分け合わない(液体が入っても空気は押し出されない。T-0612)・GPU・保存点・状態のハッシュへの組み込みは名前だけ。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "common/reaction.hlsli"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 液体の格子のセルの 1 辺(mm。仮の世界の 0.5 m)
    constexpr uint64_t PROBE_LIQUID_CELL_EDGE_MILLIMETERS = 500;

    // セルの体積(pL = µmol × mm³/mol。0.125 m³)
    constexpr uint64_t PROBE_LIQUID_CELL_VOLUME_PICOLITERS = PROBE_LIQUID_CELL_EDGE_MILLIMETERS *
                                                             PROBE_LIQUID_CELL_EDGE_MILLIMETERS *
                                                             PROBE_LIQUID_CELL_EDGE_MILLIMETERS * 1000000;

    struct ProbeLiquidOptions {
        // --- 区画(仮の世界の格子の座標。y が上〔world_cells の約束〕)---
        std::array<uint32_t, 3> minCell = {0, 0, 0};
        std::array<uint32_t, 3> sizeCells = {0, 0, 0};  // 0 を含めば液体は流れない

        // --- 模型の値(仮。作り手から見た違いはチケット T-0609 の判断待ち)---
        uint32_t frictionShift = 4;       // 面の運動量を毎刻み 1/2^frictionShift 減らす(底と粘りの摩擦。帳簿へ)
        uint32_t overfillStiffness = 16;  // 体積がセルを超えたぶんの圧力の倍率(閉じた洞窟で圧力を伝える)

        [[nodiscard]] bool Enabled() const { return sizeCells[0] != 0 && sizeCells[1] != 0 && sizeCells[2] != 0; }
    };

    // 外から液体に入った運動量の合計(格子の軸 x・y・z。単位は面の運動量と同じ mg。
    // 本当の運動量 kg·m/s = 値 × 1e-6 × 重力加速度 × 刻みの長さ ÷ 2)。内訳は刻みごとの報告に(床の押し返しと重さは
    // 毎刻み打ち消し合いながら大きくなるので、内訳を積むと 64bit を溢れる)
    struct ProbeLiquidLedger {
        std::array<int64_t, 3> external = {};
    };

    // 1 刻みの報告
    struct ProbeLiquidStepReport {
        // --- この刻みに外から入った運動量の内訳(格子の軸。和が帳簿の増え)---
        std::array<int64_t, 3> wall = {};      // 壁・床・区画の外の面の押し返し
        std::array<int64_t, 3> gravity = {};   // 重さ
        std::array<int64_t, 3> friction = {};  // 摩擦(底と粘り)
        std::array<int64_t, 3> dropped = {};   // 上流が空になった面の運動量(移流が無いぶん。T-0610 で無くす)

        uint32_t wetCells = 0;         // 液体のあるセル
        uint32_t changedCells = 0;     // 値が変わったセル
        uint32_t limitedCells = 0;     // 出る量を中身で縮めたセル
        uint32_t overfilledCells = 0;  // 液体の体積がセルを超えたセル
        uint64_t movedMilligrams = 0;  // 面を通って動いた質量の合計
    };

    class ProbeLiquidField {
    public:
        // 区画を作る。断る: 区画が格子からはみ出す・重力が 0(環境の値 environment.gravity)
        [[nodiscard]] static std::expected<ProbeLiquidField, std::string> Create(const BakedReactionTable& table,
                                                                                 const ProbeLiquidOptions& options);

        // 液体の 1 刻み。cells は格子の全部のセル(その場で書き換える)。値が変わったセルの番号を changed に足す
        ProbeLiquidStepReport Step(const BakedReactionTable& table, std::span<reaction::RxCell> cells,
                                   std::vector<uint32_t>& changed);

        [[nodiscard]] const ProbeLiquidOptions& Options() const { return m_options; }
        [[nodiscard]] const ProbeLiquidLedger& Ledger() const { return m_ledger; }

        // 面の運動量の和(格子の軸)。Σ − 帳簿 が一定(保存の検査)
        [[nodiscard]] int64_t MomentumSum(uint32_t axis) const;

        // 区画の中の番号(x + 幅 × (z + 奥行き × y))→ 格子の全部のセルの中の番号
        [[nodiscard]] uint32_t GridIndex(uint32_t region) const;
        [[nodiscard]] uint32_t RegionCellCount() const { return (uint32_t)m_faces[0].size(); }

        // 面の運動量・帳簿のハッシュ(決定性の検査。セルは呼ぶ側が別に)
        [[nodiscard]] uint64_t Hash() const;

        // セルの液体の質量(ng = µmol × mg/mol)と体積(pL = µmol × mm³/mol)
        [[nodiscard]] static uint64_t LiquidMass(const BakedReactionTable& table, const reaction::RxCell& cell);
        [[nodiscard]] static uint64_t LiquidVolume(const BakedReactionTable& table, const reaction::RxCell& cell);

    private:
        ProbeLiquidOptions m_options;
        uint64_t m_gravityMicrometersPerSecond2 = 0;
        std::array<std::vector<int64_t>, 3> m_faces;  // 区画のセルの + 側の面の運動量(格子の軸 x・y・z)
        ProbeLiquidLedger m_ledger;

        [[nodiscard]] uint32_t RegionIndex(uint32_t x, uint32_t y, uint32_t z) const;
    };

}  // namespace bicameral::sim
