// gpu_implicit.h — 細かいレベルの熱の陰解法(方式②。ADR-0019)の GPU 版(T-0117)。CPU リファレンス sim/implicit_conduction の
// StepImplicit(多重格子の V サイクル・誤差の見込みで止める・安全網)と毎刻みビット一致する Compute の段(shaders/sim/implicit_conduct.hlsl)を記録する。
// 試作の約束は CPU と同じ(セルの一覧・熱容量一定・木の本物のブロックにはつながない。木への組み込みは T-0119)。
// 多重格子の段の形(節・隣・重み・親子)は CPU の BuildImplicitGrid が作ったものを写す(ベイク。刻みの間は変わらない)。
//
// 使い方(テスト):
//   auto gpu = GpuImplicit::Create(device, grid);
//   gpu->RecordUpload(list, grid);                         // 最初だけ(セルのエネルギーと段の形)
//   gpu->RecordStep(list, ring, options);                  // 1 刻み。何回でも積める
//   gpu->RecordReadback(list);  → 投げて待つ →  gpu->Read(grid, cost);
// V サイクルの回数と安全網の回数は GPU が決める。記録は上限(options.cycles・maxLimitRounds)の回数だけ積み、要らない回は
// 述語(SetPredication。GPU が書いた「止めた」の語)で Dispatch ごと飛ばす。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "sim/implicit_conduction.h"

namespace bicameral::sim {

    // GPU の 1 刻みの結果(CPU の ImplicitCost のうち、GPU が数えるもの)
    struct GpuImplicitCost {
        uint32_t cycles = 0;                 // 回した V サイクルの数
        uint32_t limitedCells = 0;           // 安全網で陽解法に戻したセルの数
        uint32_t limitRounds = 0;            // 安全網を繰り返した回数
        int64_t worstExcessMillikelvin = 0;  // 安全網の前に範囲を超えた最大
        bool limitFinished = false;          // 安全網が記録した回数の中で止まった(false なら maxLimitRounds が足りない)
    };

    class GpuImplicit {
    public:
        static constexpr uint32_t MAX_TIMESTAMPS = 64;
        static constexpr uint32_t DEFAULT_MAX_LIMIT_ROUNDS = 16;

        [[nodiscard]] static std::expected<GpuImplicit, std::string> Create(ID3D12Device5* device,
                                                                            const ImplicitGrid& grid);

        // セル(エネルギー・端数)と段の形を写す。grid は Create と同じ形であること
        [[nodiscard]] bool RecordUpload(ID3D12GraphicsCommandList* list, const ImplicitGrid& grid);

        // 1 刻み。options.method は Multigrid だけ(赤黒・RKL2 は CPU の比べる相手で、GPU には載せない)
        [[nodiscard]] bool RecordStep(ID3D12GraphicsCommandList* list, uint64_t debugRing,
                                      const ImplicitOptions& options,
                                      uint32_t maxLimitRounds = DEFAULT_MAX_LIMIT_ROUNDS);

        void RecordTimestamp(ID3D12GraphicsCommandList* list, uint32_t index);
        void RecordReadback(ID3D12GraphicsCommandList* list);

        // 読み戻したセルのエネルギー・端数を grid に、最後の刻みの数を cost に
        [[nodiscard]] bool Read(ImplicitGrid& grid, GpuImplicitCost& cost) const;
        [[nodiscard]] std::vector<uint64_t> ReadTimestamps(uint32_t count) const;

        // 1 刻みに積む Dispatch の数(計測の表に使う)
        [[nodiscard]] uint32_t DispatchesPerCycle(const ImplicitOptions& options) const;

        // 要らない回を述語で飛ばすか(既定 true。false なら段が空で抜けるだけ。計測で比べる用)
        void UsePredication(bool use) { m_predication = use; }

    private:
        enum Buffer : uint8_t {
            BufferCells,
            BufferFaces,
            BufferLists,
            BufferNodes,
            BufferLinks,
            BufferWork,
            BufferState,
            BufferWide,
            BufferPredicate,
            BufferCount
        };

        enum Pass : uint8_t {
            PassBegin,
            PassStart,
            PassSmooth,
            PassRestrict,
            PassProlong,
            PassConverged,
            PassCycleEnd,
            PassFlows,
            PassMark,
            PassLimitEnd,
            PassLimitFaces,
            PassApply,
            PassCount
        };

        // implicit_conduct.hlsl の cbuffer ImConstants と同じ並び
        struct Constants {
            uint32_t nodeTotal = 0;
            uint32_t cellCount = 0;
            uint32_t faceCount = 0;
            uint32_t levelOffset = 0;
            uint32_t levelCount = 0;
            uint32_t color = 0;
            uint32_t tolerance = 0;
            uint32_t slack = 0;
            int32_t correctionScale = 0;
        };

        GpuImplicit() = default;

        std::expected<void, std::string> CreatePipelines(ID3D12Device5* device);
        std::expected<void, std::string> CreateBuffers(ID3D12Device5* device);
        void MakeImages(const ImplicitGrid& grid);
        void Dispatch(ID3D12GraphicsCommandList* list, Pass pass, uint32_t threads);
        void RecordSmooth(ID3D12GraphicsCommandList* list, uint32_t depth, uint32_t sweeps);
        void RecordVCycle(ID3D12GraphicsCommandList* list, uint32_t depth, const ImplicitOptions& options);
        void BeginSkippable(ID3D12GraphicsCommandList* list, uint32_t word);
        void EndSkippable(ID3D12GraphicsCommandList* list);

        // --- 形(Create の grid から)---
        uint32_t m_cellCount = 0;
        uint32_t m_faceCount = 0;
        uint32_t m_nodeTotal = 0;
        std::vector<uint32_t> m_levelOffsets;                      // 段ごとの節の始まり(段の数 + 1)
        std::array<std::vector<std::byte>, BufferCount> m_images;  // 写す中身(セルは RecordUpload で作り直す)
        Constants m_constants;
        bool m_predication = true;

        // --- GPU ---
        ComPtr<ID3D12RootSignature> m_rootSignature;
        std::array<ComPtr<ID3D12PipelineState>, PassCount> m_pipelines;
        std::array<ComPtr<ID3D12Resource>, BufferCount> m_buffers;
        std::array<ComPtr<ID3D12Resource>, BufferCount> m_uploads;
        ComPtr<ID3D12Resource> m_cellsReadback;
        ComPtr<ID3D12Resource> m_stateReadback;
        ComPtr<ID3D12Resource> m_wideReadback;
        ComPtr<ID3D12QueryHeap> m_timestamps;
        ComPtr<ID3D12Resource> m_timestampReadback;
        uint32_t m_timestampCount = 0;
    };

}  // namespace bicameral::sim
