// multires_liquid.h — 多重解像度の世界の 1 段(根のレベルだけ・子なし)で液体を流す CPU(運ぶ計算の共通の土台の段 2 ②。T-0717)。
//
// データの流れ: 木のセル(LoadNestCell)→ セルの見立て(common/cell_view.hlsli の MakeCellView。T-0713)→ 液体の見立て(LqMakeCellView)
//   → 液体の芯(common/liquid_flow.hlsli。仮の世界 sim/probe_liquid と GPU の probe_liquid_tick と同じ関数。T-0716)
//   → 当てる段(common/transport_apply.hlsli の上限・包み・当てる。T-0770)でセルへ・登録した欄(面の運動量・面の運動のエネルギー・
//   位置のエネルギー・前の質量。T-0711)・世界の帳簿の列(量 × 理由。T-0769)に書く。
// 世界に求めるもの(設計 20 §3 の 4 つのうち 3 つ): 刻むブロックの一覧(根のレベルの本物のブロック全部)/ セルの読み書き /
// 面の隣(世界の Neighbor = MrFindFaceNeighbor + 開いた端の WeEdgeNeighbor。T-0771)。端数は使わない(1 段なので違うレベルの面が無い)。
// 面の回し方は当てる段の TxFlowCell と同じ「セルごとに 6 面・自分の側だけ当てる」(T-0771)。面の欄はセルの + 側 3 面(SF_PLACE_FACE)で、
// 面の力は + 側の面の欄の持ち主(− 側のセル)が 1 回決め、両側はそれを読む(TxFaceFieldIsOwn)。
// 上は世界の上の軸 WE_UP_AXIS(y。D-518。縦の面・柱・位置のエネルギーの高さ。T-0774)。世界の端は MultiresNest::edges から(T-0774):
//   wall = 閉じた面(仮の世界の区画の外と同じ)・open = 外のセル(外の柱の行。液体は無いので出ていくだけ。下が開いていれば支えない)。
//
// 模型の形と約束は仮の世界と同じ(sim/probe_liquid.h。ADR-0266・ADR-0276・ADR-0268・ADR-0286)。違うのは回し方だけ:
//   - 区画の代わりに世界の端(wall)が閉じた面。仮の世界の区画を世界いっぱいに置いた版と毎刻みビット単位で同じ(試験 multires_liquid。
//     仮の世界の格子〔y が上〕から WE_UP_AXIS へ写して比べる)。
//   - 位置のエネルギーの高さは、刻むブロックの一番下の段から(仮の世界の「区画の床から」)。
// 仮の所(後で替える。docs/tickets/T-0717):
//   - 上限は当てる段の「出る側と入る側の倍率の min」。液体の入る側は器で切らない(溢れの圧力が入る側の安定の条件。T-0770 の仮)ので、
//     値は仮の世界の出る側だけの上限(LqLimitOutflow)とビット単位で同じ。欄が物質に付く(FollowMassChange)は T-0718 で当てる段へ。
//   - 活性を見ない(根のブロックを毎刻み全部)。根のレベル以外のブロック・子・溢れのセル・頁の不足は断る。レベル ≠ 0 の係数は未確認(段 2 の後)。
//   - 1 刻みの段の列(気体 → 液体 → 伝導)には入れていない(呼ぶ側が Step を呼ぶ。段の列の登録は T-0714)。
//   - 開いた世界の端の面は運動量を持ち越さない(− 側の端の面には欄の置き場が無いので ± の両方を同じに): 刻みの初めに内側の面の速さを写し
//     (流れ出しの境界 = 速さの勾配 0)、力を足して流し、刻みの終わりの値は帳簿の「世界の端」へ(ADR-0349 Proposed)。
//     外の柱が無い(真空)の開いた端は閉じた面(WeEdgeNeighbor・伝導と同じ)。
//   - 静止の丸め(restSpeedMicrometersPerSecond。既定 0 = 切る)は仮の世界の液体の面と同じ形(common/rest_rounding.hlsli の RrRestMomentum。T-0777)。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "common/cell_view.hlsli"
#include "common/liquid_flow.hlsli"
#include "sim/multires_nest.h"
#include "sim/probe_liquid.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 模型の値(仮の世界の ProbeLiquidOptions の区画を除いた分。値の意味と既定は同じ)
    struct MultiresLiquidOptions {
        uint32_t frictionShift = 7;
        uint32_t overfillStiffness = 16;
        bool advection = true;
        uint32_t maxSpeedMillimetersPerSecond = 15000;
        uint32_t
            restSpeedMicrometersPerSecond = 0;  // 静止の丸め(0 = 切る。仮の世界の ProbeLiquidOptions と同じ。T-0777)
    };

    // 登録した欄の番号(MultiresNest::fields の添字)
    struct MultiresLiquidFields {
        uint32_t momentum = 0;    // 面 ×3: 面の運動量(mg。仮の世界の m_faces)
        uint32_t kinetic = 0;     // 面 ×3: 面の運動のエネルギー(mJ)
        uint32_t potential = 0;   // セル: 位置のエネルギー(mJ)
        uint32_t massBefore = 0;  // セル: 前の刻みの終わりの液体の質量(mg。欄が物質に付く。T-0656)
        uint32_t carry = 0;  // 面 ×3: 1 刻みに動く質量の 1 mg 未満の端数(流れの分母の単位。仮の世界の m_carry。T-0778)
    };

    // 1 刻みの報告は仮の世界と同じ形 + 開いた世界の端(比べるため。仮の世界を退かせる時に名前を移す)
    struct MultiresLiquidStepReport : ProbeLiquidStepReport {
        std::array<int64_t, 3> edge =
            {};  // 開いた世界の端の面の運動量(持ち越さず帳簿の「世界の端」へ。外から入った向きが正。T-0774)
        uint64_t edgeMilligrams = 0;  // 開いた世界の端から外へ出た液体(mg)
    };

    class MultiresLiquid {
    public:
        // 液体の欄を登録する(作った直後・根を置く前に)。断る: 重力が 0・模型の値の範囲・溢れのセル・頁が枠より少ない・欄の名前が重なる
        [[nodiscard]] static std::expected<MultiresLiquid, std::string> Create(MultiresNest& nest,
                                                                               const BakedReactionTable& table,
                                                                               const MultiresLiquidOptions& options);

        // 液体の 1 刻み(世界の根のブロックを全部)。セルが変わったブロックはつつき(次の刻みに反応を評価)、枠を changedSlots に足す
        MultiresLiquidStepReport Step(MultiresNest& nest, const BakedReactionTable& table, uint64_t tick,
                                      std::vector<uint32_t>& changedSlots) const;

        [[nodiscard]] const MultiresLiquidFields& Fields() const { return m_fields; }
        [[nodiscard]] const liquid::LqParams& Params() const { return m_params; }

        // 保存の検査の和(仮の世界の MomentumSum・PotentialEnergySum・KineticEnergySum・Ledger と同じ意味。Ledger は世界の帳簿の列から。
        // 保存の検査そのものは ComputeConservedTotals〔液体の欄の列も含む。T-0769〕)
        [[nodiscard]] int64_t MomentumSum(const MultiresNest& nest, uint32_t axis) const;
        [[nodiscard]] int64_t PotentialEnergySum(const MultiresNest& nest) const;
        [[nodiscard]] int64_t KineticEnergySum(const MultiresNest& nest) const;
        [[nodiscard]] ProbeLiquidLedger Ledger(const MultiresNest& nest) const;

    private:
        MultiresLiquidOptions m_options;
        MultiresLiquidFields m_fields;
        liquid::LqParams m_params = {};
    };

    // レベル level のセルの 1 辺(mm。レベル 0 = 0.5 m・細かくすると半分)
    [[nodiscard]] uint64_t LiquidCellEdgeMillimeters(int32_t level);

    // セルの見立てから液体の見立て(液体の質量・体積・固体の体積・熱・熱容量。体積はレベルの単位なのでセルの体積もレベルの単位)
    [[nodiscard]] liquid::LqCellView MakeLiquidCellView(const multires::CvCellView& view);

}  // namespace bicameral::sim
