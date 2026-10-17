// multires_gas.h — 多重解像度の世界の 1 段(根のレベル 0 だけ・子なし)で気体を流す CPU(運ぶ計算の共通の土台の段 3 ②。T-0720)。
//
// データの流れ: 木のセル(LoadNestCell)→ セルの見立て(common/cell_view.hlsli の MakeCellView。気体の空き gasOpen。T-0713)
//   → セル ⇔ 気体の箱の写し(sim/gas_cells。仮の世界 sim/probe_gas と同じ関数)→ 面の一覧で小刻み(gas_reference の StepGasLinks(box, links)。
//   式は common/gas_flow.hlsli = GPU と同じ関数。T-0719)→ セルへ書き戻す・登録した欄(運動量 ×3・運動のエネルギー・前の質量。T-0711)・帳簿。
// 世界に求めるもの(設計 20 §3): 刻むブロックの一覧(根のレベルの本物のブロック全部)/ セルの読み書き / 面の隣(座標から引く)。
// 縦は世界の上の軸(world_edge.hlsli の WE_UP_AXIS。仮は z)。気体の箱は z が上なので、世界の軸を WeAxisUpLast で(横 1・横 2・縦)の順に
// 写す。運動量の欄と帳簿の値の並びは気体の箱の軸の順(上が z なら x・y・z)。
//
// 模型の形と約束は仮の世界と同じ(sim/probe_gas.h。ADR-0256・ADR-0268・ADR-0286)。違うのは回し方だけで、仮の世界の区画を世界いっぱいに
// 置いた版と毎刻みビット単位で同じ(試験 multires_gas)。
//   - 区画の代わりに世界の端: 箱の外の面は端の種類(上・下・横。wall = 押し返す / open = 外は層の基準の大気)。箱の中の抜け(ブロックの無い所)は壁。
//   - 面の番号(乱数の丸め)と開いた端の外の層は、世界の箱の座標から(仮の世界と同じ数)。
// 仮の所(後で替える。docs/tickets/T-0720):
//   - 上限・当てる: 気体の箱の小刻みの中(出ていく側だけの上限 LimitOutflow・LimitMovedAmounts)。当てる段(T-0714 の TxFlowCell。
//     入る側の上限も)へ載せ替えるのと、係数をレベルの関数にするのは T-0765(投影は T-0722)。
//   - 帳簿: 物質は世界の帳簿の「世界の端」の列。エネルギーと運動量は世界の欄 gas.ledger_*(理由ごとに分けて世界の帳簿へ移すのは T-0763)。
//   - 世界の端の種類は MultiresNest::edges.world から読む(T-0721・T-0772。環境からは sim/multires_world_edges の MakeMultiresWorldEdges)。
//     箱は根のブロックの外接の箱(世界の箱 WeWorld と同じに置く約束。違う箱はまだ見ない)。
//   - レベル 0 だけ(係数は 0.5 m のセル)。活性を見ない(根のブロックを毎刻み全部。T-0723 の後)。陽解法だけ(投影は T-0722)。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "sim/gas_cells.h"
#include "sim/multires_nest.h"
#include "sim/probe_gas.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    struct MultiresGasOptions {
        uint32_t substeps = 24;              // 陽解法の小刻み(07 §2.3。仮の世界と同じ既定)
        uint64_t randomSeed = 0;             // 移す物質量の乱数の丸めの種(世界のシード。R6)
        int64_t floorHeightMillimeters = 0;  // 世界の箱の一番下の層の高さ(基準の大気を読む高さ)
        uint32_t restSpeedUnits = 0;         // 静止の丸め(T-0723。仮の世界の ProbeGasOptions と同じ意味。0 = 切る)
    };

    // 登録した欄の番号(MultiresNest::fields の添字)
    struct MultiresGasFields {
        uint32_t momentum = 0;        // セル ×3: 気体の運動量(mg·2^-20 m/s。x・y・z)
        uint32_t kinetic = 0;         // セル: 運動エネルギーのぶん(mJ。セルのエネルギーの外)
        uint32_t massBefore = 0;      // セル: 前の気体の段の終わりの気体の質量(mg。欄が物質に付く。T-0646)
        uint32_t ledgerMomentum = 0;  // 世界 ×3: 外から入った運動量(壁の力積・開いた端・無くなった気体)
        uint32_t ledgerEnergy = 0;    // 世界: 外から入ったエネルギー(開いた端・重さの仕事。mJ)
    };

    // 1 刻みの報告は仮の世界と同じ形(比べるため。仮の世界を退かせる時に名前を移す)
    using MultiresGasStepReport = ProbeGasStepReport;

    class MultiresGas {
    public:
        // 気体の欄を登録する(作った直後・根を置く前に)。断る: 基準の大気・音速が無い・小刻みの安定の上限・根のレベルが 0 でない・
        // 溢れのセル・頁が枠より少ない・欄の名前が重なる
        [[nodiscard]] static std::expected<MultiresGas, std::string> Create(MultiresNest& nest,
                                                                            const BakedReactionTable& table,
                                                                            const MultiresGasOptions& options);

        // 刻み tick の気体の 1 刻み(世界の根のブロックを全部)。セルが変わったブロックはつつき、枠を changedSlots に足す
        MultiresGasStepReport Step(MultiresNest& nest, const BakedReactionTable& table, uint64_t tick,
                                   std::vector<uint32_t>& changedSlots) const;

        [[nodiscard]] const MultiresGasFields& Fields() const { return m_fields; }

        // 保存の検査の和(仮の世界の Momentum・KineticEnergySum・Ledger と同じ意味。運動量と帳簿は世界の軸 x・y・z)
        [[nodiscard]] int64_t MomentumSum(const MultiresNest& nest, uint32_t axis) const;
        [[nodiscard]] int64_t KineticEnergySum(const MultiresNest& nest) const;
        [[nodiscard]] int64_t LedgerEnergy(const MultiresNest& nest) const;
        [[nodiscard]] std::array<int64_t, GAS_AXES> LedgerMomentum(const MultiresNest& nest) const;

    private:
        MultiresGasOptions m_options;
        MultiresGasFields m_fields;
    };

}  // namespace bicameral::sim
