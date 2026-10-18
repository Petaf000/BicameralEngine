// gpu_gas.h — 気体の流れ(sim/gas_reference.h の G1・G2)の 1 刻みを GPU の Compute で走らせる(G3 の第 1 段。T-0185・07 §2.3)。
// 1 レベルの箱だけ(多重解像度の木と 05 のセルにつなぐのは T-0208 以降)。小刻みごとに 9 段(shaders/sim/gas_step.hlsl)。
// CPU リファレンス(StepGas)と毎刻みビット一致することを tests/gpu_gas_test.cpp が確かめる。
// CPU が書くのは初めの箱の写し(RecordUpload)とルート定数(小刻みの通し番号)だけ。箱のセルと帳簿は GPU が持つ(D-107)。
//
// 使い方:
//   auto gas = GpuGas::Create(device, box);       // box = MakeGasBox(...) に初めの乱れを入れたもの
//   gas->RecordUpload(list);                     // 最初だけ(box.cells・box.ledger・box.tick から続ける)
//   gas->RecordStep(list, debugRingAddress);     // 1 刻み(何刻みでも続けて記録してよい)
//   gas->RecordReadback(list);  → 投げて待つ → gas->Read(cells, ledger)
// 計測: EnableTiming の後、RecordTimestamp(list, 0) → 刻み → RecordTimestamp(list, 1) → RecordTimingReadback → 待つ → TimingTicks。
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "gpu/resources.h"
#include "sim/gas_reference.h"

namespace bicameral::sim {

    class GpuGas {
    public:
        // 成分の数に上限は無い(T-0598)。reservedSpecies = 箱の成分の数より多く欄を取る数(仮の世界は刻みごとに欄の物質が変わる。T-0592)
        [[nodiscard]] static std::expected<GpuGas, std::string> Create(ID3D12Device5* device, const GasBox& box,
                                                                       uint32_t reservedSpecies = 0);

        // 初めの箱(Create に渡したセルと帳簿)を GPU へ写す
        void RecordUpload(ID3D12GraphicsCommandList10* list);

        // 1 刻み(config.substeps 回の小刻み × 9 段。粘りのある箱は刻みの終わりに粘りの 2 段。T-0775)
        void RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing);

        // 外から刻みを決めて 1 刻み(仮の世界の気体の段。T-0592): 小刻みの通し番号は tick × substeps から(内の刻みの数は進めない)。
        // parameters・ghost・open は t0(設定)・t2(開いた境界の外)・t3(セルの空き。T-0617)に結ぶ別のバッファ
        // (呼ぶ側が段ごとに作る。NON_PIXEL_SHADER_RESOURCE の状態)。t1(層の基準の導く値)は作った時の箱のまま(成分の並びによらない)。
        // viscous = 粘りの 2 段を投げるか(設定は t0 から読むので、呼ぶ側が表から決める。T-0775)
        void RecordStepAt(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t tick,
                          D3D12_GPU_VIRTUAL_ADDRESS parameters, D3D12_GPU_VIRTUAL_ADDRESS ghost,
                          D3D12_GPU_VIRTUAL_ADDRESS open, bool viscous) const;

        // 箱のセル(u0。頭 + 欄 × SpeciesStride の件 × セル)と帳簿(u5。GasGpuLedgerBytes)。仮の世界の段が読み書きする(T-0592)
        [[nodiscard]] ID3D12Resource* CellBuffer() const { return m_uavs[0].Get(); }
        [[nodiscard]] ID3D12Resource* LedgerBuffer() const { return m_uavs[5].Get(); }
        [[nodiscard]] uint32_t Substeps() const { return m_substeps; }
        [[nodiscard]] uint32_t SpeciesStride() const { return m_speciesStride; }

        // 環境の値のホットリロード(T-0444・ADR-0194): box(SetGasEnvironment で環境を替えた後の設定・係数・層の基準)から
        // t0〜t3 を作り直して写す。次に記録する RecordStep から新しい値で刻む(刻みの境目で呼ぶ)。前の t0〜t3 と写しの元は keepAlive へ
        // (呼ぶ側がこのリストが GPU で終わるまで持つ)。箱の作りは同じであること(CheckGasEnvironment)。作れなければ false
        [[nodiscard]] bool RecordEnvironment(ID3D12GraphicsCommandList10* list, const GasBox& box,
                                             std::vector<ComPtr<ID3D12Resource>>& keepAlive);

        // セルと帳簿を読み戻しへ(同じリストで RecordStep を記録した後に)
        void RecordReadback(ID3D12GraphicsCommandList10* list) const;

        // RecordReadback を含むリストが終わってから
        [[nodiscard]] bool Read(std::vector<GasCell>& cells, GasLedger& ledger) const;

        [[nodiscard]] uint64_t Tick() const { return m_tick; }

        // --- 調べる用(HW と WARP の食い違いの切り分け。T-0185)。面(u3)とセルごとの合計(u4)を 64bit の語の並びで読み戻す ---
        void RecordDebugReadback(ID3D12GraphicsCommandList10* list) const;
        void SetDebugPassLimit(uint32_t passCount) { m_debugPassLimit = passCount; }  // 小刻みの段をここまでで止める
        [[nodiscard]] bool ReadDebug(std::vector<uint64_t>& faceWords, std::vector<uint64_t>& sumWords) const;

        // --- 計測(タイムスタンプ 2 つの差。単位はキューの GetTimestampFrequency)---
        [[nodiscard]] bool EnableTiming(ID3D12Device* device);
        void RecordTimestamp(ID3D12GraphicsCommandList10* list, uint32_t slot) const;  // slot 0 = 始め、1 = 終わり
        void RecordTimingReadback(ID3D12GraphicsCommandList10* list) const;            // 2 つを打った同じリストで
        [[nodiscard]] std::optional<uint64_t> TimingTicks() const;

    private:
        // shaders/sim/gas_step.hlsl の入口(呼ぶ順。.cso の名前と同じ順)
        enum class Pass : uint8_t {
            Derive,
            FaceForce,
            ApplyForce,
            FaceFlow,
            Outflow,
            FaceMoved,
            MovedSum,
            FaceFinal,
            Transfer,
            Viscosity,  // 刻みの終わりに 1 回(粘りのある箱だけ。T-0775)
            ViscosityApply,
            Count,
        };

        static constexpr size_t SUBSTEP_PASS_COUNT = (size_t)Pass::Viscosity;  // 小刻みごとの段の数

        // b0 のルート定数(gas_step.hlsl の RootConstants と同じ順)
        struct Constants {
            uint32_t roundingTickLow = 0;
            uint32_t roundingTickHigh = 0;
            uint32_t cellCount = 0;
            uint32_t speciesStride = 0;
        };

        GpuGas() = default;

        [[nodiscard]] std::expected<void, std::string> CreatePipelines(ID3D12Device5* device);
        [[nodiscard]] std::expected<void, std::string> CreateBuffers(ID3D12Device5* device, const GasBox& box);
        [[nodiscard]] static std::array<gpu::ResidentBuffer, 4> CreateLayerBuffers(ID3D12Device* device,
                                                                                   const GasBox& box,
                                                                                   uint32_t reservedSpecies);
        void RecordPass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, Pass pass,
                        const Constants& constants, const std::array<D3D12_GPU_VIRTUAL_ADDRESS, 4>& srvs) const;
        void RecordSubsteps(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t tick,
                            const std::array<D3D12_GPU_VIRTUAL_ADDRESS, 4>& srvs, bool viscous) const;
        [[nodiscard]] std::array<D3D12_GPU_VIRTUAL_ADDRESS, 4> OwnSrvs() const;

        uint32_t m_cellCount = 0;
        uint32_t m_speciesCount = 0;   // 箱の成分の数(読み戻す欄)
        uint32_t m_speciesStride = 1;  // 1 件の成分の欄の数(gas_step.hlsl の g_speciesStride)
        uint32_t m_substeps = 0;
        uint64_t m_tick = 0;
        uint32_t m_debugPassLimit = 9;
        bool m_viscous = false;  // 粘りのある箱(GasHasViscosity。T-0775)

        ComPtr<ID3D12RootSignature> m_rootSignature;
        std::array<ComPtr<ID3D12PipelineState>, (size_t)Pass::Count> m_pipelines;
        std::array<ComPtr<ID3D12Resource>, 6> m_uavs;  // u0〜u5(gas_step.hlsl の結び付け)
        // t0 パラメータ・t1 層の基準の導く値・t2 層の開いた境界の外・t3 セルの空き(T-0617)。既定のヒープの写し(T-0410。写すのは RecordUpload)
        std::array<gpu::ResidentBuffer, 4> m_srvs;
        ComPtr<ID3D12Resource> m_cellUpload;    // 初めのセル
        ComPtr<ID3D12Resource> m_ledgerUpload;  // 初めの帳簿
        ComPtr<ID3D12Resource> m_cellReadback;
        ComPtr<ID3D12Resource> m_ledgerReadback;
        ComPtr<ID3D12Resource> m_faceReadback;  // 調べる用
        ComPtr<ID3D12Resource> m_sumReadback;
        ComPtr<ID3D12QueryHeap> m_timestamps;
        ComPtr<ID3D12Resource> m_timestampReadback;
    };

}  // namespace bicameral::sim
