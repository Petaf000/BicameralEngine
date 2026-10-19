// gpu_gas_projection.h — 気体の圧力の投影(本物の音速の陰解法。V サイクル固定・外側の反復 0。T-0842・T-0845)の 1 刻みを GPU で走らせる。
// CPU リファレンスは sim/gas_projection の StepGasProjection(GasProjectionOptions の fixedCycles・outerIterations = 1・substeps = 1)。
// 1 刻み = 導く値 → 式を作る(面の K_f・F*、セルの C_i・右辺、多重格子の段の重み)→ V サイクル(GpuImplicit::RecordSolve)→ 当てる(x の力積・面の流れ)
// → 運ぶ(陽解法の気体の段 5〜9 をそのまま)。段の中身は shaders/sim/gas_projection.hlsl と gas_step.hlsl、呼ぶ順はこのファイルの .cpp。
// 箱のセルと帳簿は GpuGas が持つ(D-107)。多重格子の段の形(節・隣・親子)は CPU の BuildImplicitGrid の系を写し(RecordUploadShape)、
// 値(容量・対角・重み・右辺)は毎刻み GPU が作る。形は系の形(セルの空き・つながり)と段の数で決まり、刻みの値で段の数が変わることがあるので、
// 形が変わった刻みは呼ぶ側が写し直す(試験は CPU の模型の系と比べて決める。形が変わる刻みを GPU で見つける形は残り。T-0845 のチケット)。
// 既定の道(陽解法の気体 GpuGas::RecordStep)は変えない。これは計測と比べる口(切ってあるのが既定)。
//
// 使い方(試験):
//   auto projection = GpuGasProjection::Create(device, box, grid, options);   // grid = CPU の模型が刻み 0 に作った系(段の形)
//   projection->RecordUpload(list, grid);                                    // 最初のリスト: 箱と段の形を写し、前の解を 0 に
//   projection->RecordStep(list, ring);                                      // 1 刻み(何刻みでも続けて記録してよい)
//   projection->RecordReadback(list); projection->RecordEnd(list);  → 投げて待つ → projection->Read(cells, ledger)
//   次のリスト: projection->RecordBegin(list, nullptr か写し直す段の形) → RecordStep … → RecordEnd
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "gpu/resources.h"
#include "sim/gas_reference.h"
#include "sim/gpu_gas.h"
#include "sim/gpu_implicit.h"
#include "sim/implicit_conduction.h"

namespace bicameral::sim {

    struct GpuGasProjectionOptions {
        uint32_t soundSpeedMmPerS = 340000;  // 式の音速 c̃(C_i = V ÷ c̃²。0 = 最速)
        ImplicitOptions solver = {.method = ImplicitMethod::Multigrid, .cycles = 2};  // V サイクルの回数(ちょうど)
        GpuImplicitRecordShape shape = {.fixedCycles = true};  // V の記録の形(fixedCycles は必ず立てる)
    };

    // 1 刻みの部分(タイムスタンプの区切り。計測の表の順)
    enum class GpuGasProjectionPart : uint8_t {
        Build,   // 導く値・面の K_f と F*・セルの C_i と右辺・段 0 の重み
        Levels,  // 粗い段の容量・面の係数・重み
        Solve,   // V サイクル
        Apply,   // x を p̃ へ・面の力・力を当てる・面の流れ
        Advect,  // 出ていく割合 〜 移す
        Count,
    };

    class GpuGasProjection {
    public:
        static constexpr uint32_t STAMPS_PER_STEP = (uint32_t)GpuGasProjectionPart::Count + 1;
        static constexpr uint32_t MAX_TIMED_STEPS = 64;

        [[nodiscard]] static std::expected<GpuGasProjection, std::string> Create(
            ID3D12Device5* device, const GasBox& box, const ImplicitGrid& grid, const GpuGasProjectionOptions& options);

        // リストの初め: 箱(Create に渡したセルと帳簿)と段の形を写し、前の解(初めの近似)を 0 にする(最初のリストだけ)
        [[nodiscard]] bool RecordUpload(ID3D12GraphicsCommandList10* list, const ImplicitGrid& grid);

        // 2 つ目からのリストの初め: shape があれば段の形だけを写し直す(系の形が変わった刻みの前。前の解はそのまま。
        // Create の上限に収まること)。無ければ陰解法のバッファを UAV に戻すだけ
        [[nodiscard]] bool RecordBegin(ID3D12GraphicsCommandList10* list, const ImplicitGrid* shape);

        // リストの終わり(陰解法のバッファを COMMON へ。読み戻しの後に)
        void RecordEnd(ID3D12GraphicsCommandList10* list) { m_implicit.RecordRelease(list); }

        // 1 刻み。timed なら部分の区切りにタイムスタンプを打つ(EnableTiming の後。MAX_TIMED_STEPS 刻みまで)
        [[nodiscard]] bool RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                      bool timed = false);

        void RecordReadback(ID3D12GraphicsCommandList10* list) const { m_gas.RecordReadback(list); }
        [[nodiscard]] bool Read(std::vector<GasCell>& cells, GasLedger& ledger) const {
            return m_gas.Read(cells, ledger);
        }

        [[nodiscard]] uint64_t Tick() const { return m_tick; }

        // V の記録の形(GpuImplicit::ShapeFrom で前の刻みの数から選ぶ。値は同じで費用だけ変わる。fixedCycles は立てたまま)
        void SetSolveShape(GpuImplicitRecordShape shape) {
            shape.fixedCycles = true;
            m_options.shape = shape;
        }

        // V の数と段の形の読み戻し(RecordStep の後。RecordEnd の前)
        void RecordCostReadback(ID3D12GraphicsCommandList10* list) { m_implicit.RecordCostReadback(list); }
        [[nodiscard]] bool ReadCost(GpuImplicitCost& cost) const { return m_implicit.ReadCost(cost); }

        // --- 計測 ---
        [[nodiscard]] bool EnableTiming(ID3D12Device* device);
        void ResetTiming() { m_timedSteps = 0; }
        void RecordTimingReadback(ID3D12GraphicsCommandList10* list) const;
        // 刻みごとの部分の時間(タイムスタンプの刻み。[刻み][部分])
        [[nodiscard]] std::vector<std::array<uint64_t, (size_t)GpuGasProjectionPart::Count>> ReadTiming() const;

    private:
        // shaders/sim/gas_projection.hlsl の入口(.cso の名前と同じ順)
        enum class Pass : uint8_t { Face, Cell, Level, Pressure, Flow, Count };

        // b0 のルート定数(gas_step.hlsl の RootConstants + gas_projection.hlsl の GAS_STEP_EXTRA_CONSTANTS と同じ順)
        struct Constants {
            uint32_t roundingTickLow = 0;
            uint32_t roundingTickHigh = 0;
            uint32_t cellCount = 0;
            uint32_t speciesStride = 0;
            uint32_t maxNodes = 0;
            uint32_t maxLinks = 0;
            uint32_t levelStart = 0;
            uint32_t levelCount = 0;
            uint32_t perSoundLow = 0;
            uint32_t perSoundHigh = 0;
            uint32_t statePerSoundLow = 0;
            uint32_t statePerSoundHigh = 0;
            uint32_t linkLow = 0;
            uint32_t linkHigh = 0;
        };

        GpuGasProjection(GpuGas gas, GpuImplicit implicit) : m_gas(std::move(gas)), m_implicit(std::move(implicit)) {}

        [[nodiscard]] std::expected<void, std::string> CreatePipelines(ID3D12Device5* device);
        void SetLevels(const ImplicitGrid& grid);
        [[nodiscard]] bool UploadShape(ID3D12GraphicsCommandList10* list, const ImplicitGrid& grid);
        void RecordPass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, Pass pass,
                        const Constants& constants, uint32_t threads) const;
        void RecordStamp(ID3D12GraphicsCommandList10* list, bool timed, uint32_t part) const;

        GpuGas m_gas;
        GpuImplicit m_implicit;
        GpuGasProjectionOptions m_options;
        Constants m_constants;  // 刻みで変わらない欄(通し番号・段は RecordStep が埋める)
        uint64_t m_tick = 0;
        std::vector<uint32_t> m_levelOffsets;  // 段ごとの節の始まり(段の数 + 1。今の段の形)

        ComPtr<ID3D12RootSignature> m_rootSignature;
        std::array<ComPtr<ID3D12PipelineState>, (size_t)Pass::Count> m_pipelines;
        gpu::ResidentBuffer m_parameters;  // t0: 投影の係数の設定(GasPressureCoefficients)
        ComPtr<ID3D12Resource> m_scratch;  // u10: 容量・対角・面の係数(128bit)
        ComPtr<ID3D12QueryHeap> m_timestamps;
        ComPtr<ID3D12Resource> m_timestampReadback;
        uint32_t m_timedSteps = 0;
    };

}  // namespace bicameral::sim
