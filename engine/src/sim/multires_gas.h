// multires_gas.h — 多重解像度の世界の 1 段(根のレベルだけ・子なし)で気体を流す CPU(運ぶ計算の共通の土台の段 3 ②。T-0720・T-0763・T-0765)。
//
// データの流れ: 木のセル(LoadNestCell)→ セルの見立て(common/cell_view.hlsli の MakeCellView。気体の空き gasOpen。T-0713)
//   → セル ⇔ 気体の箱の写し(sim/gas_cells。仮の世界 sim/probe_gas と同じ関数)→ 面の一覧で小刻み(gas_reference の StepGasLinks(box, links)。
//   式は common/gas_flow.hlsli = GPU と同じ関数。T-0719。上限と当てるは当てる段 common/transport_apply.hlsli。T-0765)→ 粘り →
//   静止の丸め(当てる段の後)→ セルへ書き戻す・登録した欄(運動量 ×3・運動のエネルギー・前の質量。T-0711)・世界の帳簿(理由ごと。T-0763)。
// 世界に求めるもの(設計 20 §3): 刻むブロックの一覧(根のレベルの本物のブロック全部)/ セルの読み書き / 面の隣(座標から引く)。
// 縦は世界の上の軸(world_edge.hlsli の WE_UP_AXIS。y = D-518)。気体の箱は z が上なので、世界の軸を WeAxisUpLast で(横 1・横 2・縦)の順に
// 写す。運動量の欄と帳簿の値の並びは気体の箱の軸の順(上が z なら x・y・z)。
//
// 模型の形と約束は仮の世界と同じ(sim/probe_gas.h。ADR-0256・ADR-0268・ADR-0286)。違うのは回し方だけで、仮の世界の区画を世界いっぱいに
// 置いた版と毎刻みビット単位で同じ(試験 multires_gas)。
//   - 区画の代わりに世界の端: 箱の外の面は端の種類(上・下・横。wall = 押し返す / open = 外は層の基準の大気)。箱の中の抜け(ブロックの無い所)は壁。
//   - 面の番号(乱数の丸め)と開いた端の外の層は、世界の箱の座標から(仮の世界と同じ数)。
// 当てる段(T-0765・ADR-0345): 上限は当てる段の倍率(出る側 = 中身 ÷ 出る合計の換算・成分ごとの中身。入る側は縮めない = 気体は器で
//   切らない)、移す塊は包み TxPacket(成分・エネルギー・運ぶ欄 = 運動量)を TxApplyPacket で当てる。式は前と同じ切り捨て 1 回なので
//   仮の世界・GPU の段とビット単位で同じ。
// レベル(T-0765): 根のレベル k のセル(1 辺 0.5 m × 2^-k)。量はレベルの単位なので圧力の式はそのまま、面の式は × 2^k・層は × 2^-k
//   (GasConfig::cellLevel)。小刻みは substeps × 2^σ(σ = max(0, k)。音の速さの数 ν を レベル 0 と同じに保つ)。
// 帳簿(T-0763・ADR-0345): 世界の帳簿の列へ理由ごと。物質 = 世界の端。エネルギーの列 = 世界の端・重さの仕事・運動のエネルギーの欄から
//   熱になった分(物理の熱・静止の丸め)。運動量の列 = 世界の端・壁の力積・重さ・欄が物質に付く・静止の丸め。運動のエネルギーの欄の列 =
//   熱になった分の出。これで ComputeConservedTotals(Σ 保存量 − 帳簿)が気体の刻みで列ごとに変わらない。
// 仮の所(後で替える):
//   - 世界の端の種類は MultiresNest::edges.world から読む(T-0721・T-0772。環境からは sim/multires_world_edges の MakeMultiresWorldEdges)。
//     箱は根のブロックの外接の箱(世界の箱 WeWorld と同じに置く約束。違う箱はまだ見ない)。
//   - 1 段だけ(違うレベルの面・粗い側の受け直しは多重レベルの気体)。活性を見ない(根のブロックを毎刻み全部。T-0723 の後)。
//     陽解法だけ(投影は T-0722)。
// 面の開き具合(T-0976): 世界が面の開き具合の欄(sim/face_opening の FACE_BLOCKED_FIELD_NAME)を登録していれば、面の開きを
//   min(両側の空きの小さい方, 1 − 塞ぐ割合)にする(face_opening.hlsli の FoFaceOpen。GasBox::faceBlockedQ16)。陽解法の面の流れ・力積・粘り・
//   陰の圧力の面の係数が同じ開きを読む。登録していない・塞ぐ割合が全部 0 なら今とビット単位で同じ。
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
        uint32_t
            substeps = 24;  // 陽解法の小刻み(レベル 0 の数。07 §2.3。仮の世界と同じ既定。根のレベル k は × 2^σ。T-0765)
        uint64_t randomSeed = 0;             // 移す物質量の乱数の丸めの種(世界のシード。R6)
        int64_t floorHeightMillimeters = 0;  // 世界の箱の一番下の層の高さ(基準の大気を読む高さ)
        uint32_t restSpeedUnits = 0;         // 静止の丸め(T-0723。仮の世界の ProbeGasOptions と同じ意味。0 = 切る)
    };

    // 登録した欄の番号(MultiresNest::fields の添字。帳簿は世界の帳簿の列。T-0763)
    struct MultiresGasFields {
        uint32_t momentum = 0;    // セル ×3: 気体の運動量(mg·2^-20 m/s。気体の箱の軸 x・y・z〔z が上〕)
        uint32_t kinetic = 0;     // セル: 運動エネルギーのぶん(mJ。セルのエネルギーの外)
        uint32_t massBefore = 0;  // セル: 前の気体の段の終わりの気体の質量(mg。欄が物質に付く。T-0646)
    };

    // 1 刻みの報告は仮の世界と同じ形(比べるため。仮の世界を退かせる時に名前を移す)
    using MultiresGasStepReport = ProbeGasStepReport;

    class MultiresGas {
    public:
        // 気体の欄を登録する(作った直後・根を置く前に)。断る: 基準の大気・音速が無い・小刻みの安定の上限・根のレベルが範囲の外
        // (GAS_MIN_CELL_LEVEL〜GAS_MAX_CELL_LEVEL)・溢れのセル・頁が枠より少ない・欄の名前が重なる
        [[nodiscard]] static std::expected<MultiresGas, std::string> Create(MultiresNest& nest,
                                                                            const BakedReactionTable& table,
                                                                            const MultiresGasOptions& options);

        // 刻み tick の気体の 1 刻み(世界の根のブロックを全部)。セルが変わったブロックはつつき、枠を changedSlots に足す
        MultiresGasStepReport Step(MultiresNest& nest, const BakedReactionTable& table, uint64_t tick,
                                   std::vector<uint32_t>& changedSlots) const;

        [[nodiscard]] const MultiresGasFields& Fields() const { return m_fields; }

        // 作った時の作り手への知らせ(T-0804: 最速を選んだ世界の「まだ効かない」。GAS_INSTANT_NOT_YET_PREFIX。無ければ空。ログにも出す)
        [[nodiscard]] const std::vector<std::string>& Notices() const { return m_notices; }

        // 1 刻みの小刻みの数(options.substeps × 2^σ。σ = max(0, 根のレベル))
        [[nodiscard]] uint32_t Substeps() const { return m_substeps; }

        // 層 layer(世界の箱の一番下の層が 0)の静かな基準の大気のセル(根のレベルの単位。気体を流しても変わらない値。世界を作る側が置く。
        // 根を置く前でも読める)
        [[nodiscard]] reaction::RxCell ReferenceCell(const MultiresNest& nest, const BakedReactionTable& table,
                                                     uint32_t layer) const;

        // 保存の検査の和(仮の世界の Momentum・KineticEnergySum・Ledger と同じ意味。運動量と帳簿は気体の箱の軸 x・y・z)。
        // 帳簿は世界の帳簿の気体の理由の列の「入 − 出」の和(エネルギー = 世界の端 + 重さの仕事。運動量 = 世界の端 + 壁 + 重さ +
        // 欄が物質に付く + 静止の丸め)。保存の検査そのものは ComputeConservedTotals
        [[nodiscard]] int64_t MomentumSum(const MultiresNest& nest, uint32_t axis) const;
        [[nodiscard]] int64_t KineticEnergySum(const MultiresNest& nest) const;
        [[nodiscard]] int64_t LedgerEnergy(const MultiresNest& nest) const;
        [[nodiscard]] std::array<int64_t, GAS_AXES> LedgerMomentum(const MultiresNest& nest) const;

    private:
        MultiresGasOptions m_options;
        MultiresGasFields m_fields;
        uint32_t m_substeps = 0;
        std::vector<std::string> m_notices;
    };

}  // namespace bicameral::sim
