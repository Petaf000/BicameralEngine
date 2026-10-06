// gpu_implicit.h — 細かいレベルの熱の陰解法(方式②。ADR-0019)の GPU 版(T-0117)。CPU リファレンス sim/implicit_conduction の
// StepImplicit(多重格子の V サイクル・誤差の見込みで止める・安全網)と毎刻みビット一致する Compute の段(shaders/sim/implicit_conduct.hlsl)を記録する。
// 試作の約束は CPU と同じ(セルの一覧・熱容量一定)。木につないだ系(T-0119 の multires_implicit_conduction が作る、刻みの初めの温度と
// 粗い側の端数の枠つきのセル)もそのまま解ける(T-0127)。系を GPU で作って伝導の段から呼ぶのはまだ(T-0129・T-0132)。
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

    // 段の分け方の調整(計測で選ぶ。既定は T-0120 の値、T-0127 で木につないだ場面で測り直した。docs/perf.md)
    struct GpuImplicitTuning {
        // 節の数がこれ以下で、隣がこれ以下の段から下は、1 グループ(implicit_conduct.hlsl の IM_TAIL_THREADS)で
        // V サイクルを回す(ImTail。T-0120)。ImTail は 1 スレッドで隣を回すので、長い行のある段は入れない。
        // 2048 節にすると試作の鎖(レベル 3〜6)が 1 刻み 1.64 → 1.97 ms と重くなった(1 グループに仕事が寄りすぎる)
        uint32_t tailMaxNodes = 1024;
        uint32_t tailMaxLinks = 32;
        // 上の条件に合わない時も、最も粗い段は節がこれ以下なら(隣の数によらず)ImTail で回す。超えたら掃き出しの Dispatch で回す。
        // 木のたくさんの要求の場面の最も粗い段(3396 節・隣 64)を ImTail に入れると 1 刻み 9.4 ms、入れないと 8.1 ms(T-0127。docs/perf.md)。
        // 試作の熱い点(最も粗い段が小さく隣が多い)は ImTail の方が軽かった(T-0120)
        uint32_t coarsestTailMaxNodes = 1024;
    };

    class GpuImplicit {
    public:
        static constexpr uint32_t MAX_TIMESTAMPS = 64;
        static constexpr uint32_t DEFAULT_MAX_LIMIT_ROUNDS = 16;

        [[nodiscard]] static std::expected<GpuImplicit, std::string> Create(ID3D12Device5* device,
                                                                            const ImplicitGrid& grid,
                                                                            const GpuImplicitTuning& tuning = {});

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

        // ImTail が受け持つ最初の段と段の数(計測の表に使う)
        [[nodiscard]] uint32_t TailDepth() const { return m_tailDepth; }
        [[nodiscard]] uint32_t LevelCount() const { return static_cast<uint32_t>(m_levelOffsets.size() - 1); }

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
            BufferOrder,
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
            PassTail,
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
            uint32_t orderStart = 0;
            uint32_t shortCount = 0;
            uint32_t longCount = 0;
            uint32_t tailStart = 0;
            uint32_t tailDepth = 0;
            uint32_t levelTotal = 0;
            uint32_t sweeps = 0;
        };

        // 節の並び(u9)の 1 区間: 1 スレッドで足す節 shortCount 個 → グループで足す節 longCount 個(T-0120)
        struct OrderRange {
            uint32_t start = 0;
            uint32_t shortCount = 0;
            uint32_t longCount = 0;
        };

        GpuImplicit() = default;

        std::expected<void, std::string> CreatePipelines(ID3D12Device5* device);
        std::expected<void, std::string> CreateBuffers(ID3D12Device5* device);
        void MakeImages(const ImplicitGrid& grid);
        void MakeOrders(const std::vector<std::byte>& nodeImage, const std::vector<std::byte>& listImage);
        void Dispatch(ID3D12GraphicsCommandList* list, Pass pass, uint32_t threads);
        void DispatchOrdered(ID3D12GraphicsCommandList* list, Pass pass, const OrderRange& range);
        void RecordSmooth(ID3D12GraphicsCommandList* list, uint32_t depth, uint32_t sweeps);
        void RecordVCycle(ID3D12GraphicsCommandList* list, uint32_t depth, const ImplicitOptions& options);
        void BeginSkippable(ID3D12GraphicsCommandList* list, uint32_t word);
        void EndSkippable(ID3D12GraphicsCommandList* list);

        // --- 形(Create の grid から)---
        uint32_t m_cellCount = 0;
        uint32_t m_faceCount = 0;
        uint32_t m_nodeTotal = 0;
        std::vector<uint32_t> m_levelOffsets;  // 段ごとの節の始まり(段の数 + 1)
        std::vector<std::array<OrderRange, 2>>
            m_smoothOrders;                        // 段・色ごと: 掃き出しの並び(グループで足すのはその色の長い節だけ)
        OrderRange m_convergedOrder;               // 止める判定(段 0 = セル)の並び
        std::vector<OrderRange> m_restrictOrders;  // 段ごと: 縮約の親の並び(段 0 は使わない)
        uint32_t m_tailDepth = 0;
        GpuImplicitTuning m_tuning;  // ここから最も粗い段までは ImTail の 1 グループで回す(T-0120)
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
