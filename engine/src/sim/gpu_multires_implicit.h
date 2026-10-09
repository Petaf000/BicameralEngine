// gpu_multires_implicit.h — GPU の伝導の段から細かいレベルの熱の陰解法(方式②。ADR-0019・D-434 案 a)を呼ぶ(T-0132)。
// CPU リファレンスは multires_implicit_conduction.cpp の AddImplicitConduction(StepConduction の最後の小刻みの流れの後)。
// データの流れ(1 刻み。GpuMultires::RecordConduction が流れの段の後・ConductApply の前に Record を呼ぶ):
//   GpuMultires の木(流れの後。凍った印つき)→ GpuImplicitBuild(未知数・境のセル・面・セルの面の一覧)→ GpuImplicitLevels(多重格子の段)
//   → GpuImplicit(写して 1 刻み解く)→ GpuImplicitBuild::RecordApply(解いた変化を伝導の変化の表へ。活性なら伝導の一覧へ)→ ConductApply が足す
// CPU は系の大きさも段の数も知らない(大きさは上限から。T-0136)。記録の形(GpuImplicitRecordShape)は前の刻みの数から選ぶ(T-0154。
// 数は遅れて読んだものでよい: 形は費用だけを変え、解いた値は同じ)。多重格子の段を Dispatch で積む回の数も前の刻みの段の数から選ぶ
// (GpuImplicitLevels::RoundsFrom。T-0179。残りの回と小さい段の回は LvTail。LvTail は WARP で落ちる〔T-0147〕ので、ソフトウェアの
// アダプタでは今まで通り上限まで Dispatch で積む)。V サイクルも前の刻みの回数 + 余裕の回までを段ごとの Dispatch で積み、その後の回は
// ImTail が段 0 から 1 グループで回す安い回にする(GpuImplicitRecordShape::dispatchCycles。T-0179)。
// 使い方: multires->EnableImplicitConduction(device, limits) の後、MultiresStepOptions::implicitConduction の刻みを RecordStep・RecordStepActive で。
// 上限(GpuMultiresImplicitLimits = MultiresImplicitLimits。T-0178): 流れの段の前に RecordAdmit が、系に入れるブロックを枠の順に予算
// (MakeMultiresImplicitBudget)の中まで選ぶ。入らないブロックはその刻み陽解法のまま(Q22 の案 A。仮)。多重格子の 2 段目からが上限に
// 入らなければ縮約を止める。CPU と比べる時は options.implicitLimits に同じ上限を入れる(CPU の MarkImplicitBlocks・BuildImplicitGrid)。
#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "sim/gpu_implicit.h"
#include "sim/gpu_implicit_build.h"
#include "sim/gpu_implicit_levels.h"

namespace bicameral::sim {

    class GpuMultiresImplicit {
    public:
        [[nodiscard]] static std::expected<GpuMultiresImplicit, std::string> Create(
            ID3D12Device5* device, const GpuMultires& multires, const MultiresCapacity& capacity,
            const GpuMultiresImplicitLimits& limits);

        // 系に入れるブロックを選んで陰解法の印を付ける(T-0178。GpuMultires の伝導の段の流れの前。頁と端数の枠を配った後)
        void RecordAdmit(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, GpuMultires& multires,
                         const MultiresStepOptions& options);

        // 1 刻みの陰解法(GpuMultires の伝導の段から。RecordAdmit の後。刻みの印と stepFlags は multires に置いたまま)
        void Record(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, GpuMultires& multires,
                    const MultiresStepOptions& options);

        // 計測用: true なら Record が段の境に multires のタイムスタンプを打つ(PHASE_STAMP_FIRST から順に: 系を作る前・段の前・解く前・
        // 足す前・足した後。読むのは multires.ReadTimestamps)
        void StampPhases(bool stamp) { m_stampPhases = stamp; }
        static constexpr uint32_t PHASE_STAMP_FIRST = 2;
        static constexpr uint32_t PHASE_STAMP_COUNT = 5;

        // 試験用: 前の刻みによらず一番安い積み方で積む(T-0179): 多重格子の段の回は全部 LvTail(LvTail を積まない WARP では上限まで
        // Dispatch のまま)・V サイクルは 1 回目の後を全部 ImTail の安い回。前の刻みより系が深い・回が多い刻みの道を確かめる用
        void ForceCheapest(bool force) { m_forceCheapest = force; }
        [[nodiscard]] bool LevelRoundsSelectable() const { return m_levels.TailAllowed(); }

        // 最後に読み戻せた刻みの数(V の回数・安全網・段の形。リストの実行が終わった後に読むとその刻みの値)
        [[nodiscard]] bool ReadCost(GpuImplicitCost& cost) const { return m_implicit.ReadCost(cost); }

    private:
        GpuMultiresImplicit(GpuImplicitBuild build, GpuImplicitLevels levels, GpuImplicit implicit)
            : m_build(std::move(build)), m_levels(std::move(levels)), m_implicit(std::move(implicit)) {}

        GpuImplicitBuild m_build;
        GpuImplicitLevels m_levels;
        GpuImplicit m_implicit;
        bool m_recorded = false;  // 一度でも記録したか(前の刻みの数があるか)
        bool m_stampPhases = false;
        bool m_forceCheapest = false;
    };

}  // namespace bicameral::sim
