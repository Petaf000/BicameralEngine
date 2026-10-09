// gpu_implicit.h — 細かいレベルの熱の陰解法(方式②。ADR-0019)の GPU 版(T-0117)。CPU リファレンス sim/implicit_conduction の
// StepImplicit(多重格子の V サイクル・誤差の見込みで止める・安全網)と毎刻みビット一致する Compute の段(shaders/sim/implicit_conduct.hlsl)を記録する。
// 試作の約束は CPU と同じ(セルの一覧・熱容量一定)。木につないだ系(T-0119 の multires_implicit_conduction が作る、刻みの初めの温度と
// 粗い側の端数の枠つきのセル)もそのまま解ける(T-0127)。系のセル・面・面の一覧は GPU で作れる(GpuImplicitBuild。T-0129)。伝導の段から呼ぶのはまだ(T-0132)。
// 多重格子の段の形(節・隣・重み・親子)は CPU の BuildImplicitGrid が作ったものを写す(RecordUpload)か、GPU で作った段
// (GpuImplicitLevels。T-0134)を写す(RecordCopyLevels。段の数・節の始まりと数の表も写す)。
// 大きさは上限(GpuImplicitLimits)で決め、段の形・数は GPU のバッファ(計画 u9)から読む(T-0136): 刻みの初めに ImPlanLevels・ImPlanArgs が
// 長い行の節の一覧・ImTail の境・間接の Dispatch の引数を作り、V サイクルは記録の上限の段(GpuImplicitTuning::dispatchLevels)まで
// ExecuteIndirect で積む(その段が無い・下りが止まった段より下なら引数が 0 グループ)。CPU は段の数も ImTail の境も知らない。
//
// 使い方(テスト):
//   auto gpu = GpuImplicit::Create(device, limits);         // または Create(device, grid)(上限 = その系の大きさ)
//   gpu->RecordUpload(list, grid);                          // CPU の系を写す。GPU の系なら RecordReset → RecordCopySystem・RecordCopyLevels
//   gpu->RecordStep(list, ring, options);                   // 1 刻み。何回でも積める
//   gpu->RecordReadback(list);  → 投げて待つ →  gpu->Read(grid, cost);
// V サイクルの回数と安全網の回数は GPU が決める。記録は上限(options.cycles・maxLimitRounds)の回数だけ積み、要らない回は
// 述語(SetPredication。GPU が書いた「止めた」の語)で Dispatch ごと飛ばす。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
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

        // --- GPU が決めた段の形(計測の表に使う。T-0136)---
        uint32_t levelCount = 0;       // 段の数
        uint32_t tailDepth = 0;        // ImTail が受け持つ最初の段(段の数なら受け持たない)
        uint32_t wantedTailDepth = 0;  // 記録の形で切る前の ImTail の境(次の刻みの形を選ぶのに使う。T-0154)
    };

    // 大きさの上限(バッファの大きさ。T-0136)。写す系と段はこれに収まること
    struct GpuImplicitLimits {
        uint32_t cells = 0;
        uint32_t faces = 0;
        uint32_t nodes = 0;    // 全部の段の節
        uint32_t links = 0;    // 全部の段の隣
        uint32_t levels = 64;  // 段の数(CPU の MAX_GRID_LEVELS)
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
        // 段ごとの Dispatch(ExecuteIndirect)を積む段の数(T-0136)。段の数と ImTail の境は GPU が決めるので、記録はこの段まで積み、
        // 要らない段は 0 グループの Dispatch になる(バリアは残る)。下りがこれより深くなる系は、ここから下を ImTail が受け持つ(値は同じ・遅くなりうる)
        uint32_t dispatchLevels = 8;
    };

    // 1 刻みの記録の形(T-0154)。段の数と ImTail の境は GPU が決めるので、CPU が積んだ段ごとの Dispatch のうち要らないものは
    // 0 グループの Dispatch とバリアになり、それが小さい場面の費用の大半になる(述語で飛ばした回もバリアは残る)。
    // 形を変えても解いた値は同じ(積まなかった段は ImTail の 1 グループが同じ順で回す)。変わるのは費用だけなので、
    // 前の刻みの GPU の数(GpuImplicitCost)から選んでよい(ShapeFrom。遅れて読んだ数でもよい)。
    struct GpuImplicitRecordShape {
        // 段ごとの Dispatch を積む段の数(1〜Create の dispatchLevels。0 なら Create の値)。下りがこれより深い系はそこから下を ImTail が回す
        uint32_t dispatchLevels = 0;
        // 最も粗い段の掃き出しを Dispatch で積むか(ImTail に入らない大きい最も粗い段の時だけ要る)。false なら ImTail が受け持つ
        bool coarsestDispatch = true;
    };

    class GpuImplicit {
    public:
        static constexpr uint32_t MAX_TIMESTAMPS = 64;
        static constexpr uint32_t DEFAULT_MAX_LIMIT_ROUNDS = 16;

        [[nodiscard]] static std::expected<GpuImplicit, std::string> Create(ID3D12Device5* device,
                                                                            const GpuImplicitLimits& limits,
                                                                            const GpuImplicitTuning& tuning = {});

        // 上限 = grid の大きさ(段ごとの Dispatch は grid の段の数まで)
        [[nodiscard]] static std::expected<GpuImplicit, std::string> Create(ID3D12Device5* device,
                                                                            const ImplicitGrid& grid,
                                                                            const GpuImplicitTuning& tuning = {});

        [[nodiscard]] static GpuImplicitLimits LimitsOf(const ImplicitGrid& grid);

        // 前の刻みの数から記録の形を選ぶ(T-0154): 段は下りが止まった段(切る前の ImTail の境)まで積み、
        // 最も粗い段の掃き出しは前の刻みが ImTail を使わなかった時だけ積む
        [[nodiscard]] static GpuImplicitRecordShape ShapeFrom(const GpuImplicitCost& previous);

        // CPU の系(セル・面・段の形と段の表)を写す。grid は上限に収まること
        [[nodiscard]] bool RecordUpload(ID3D12GraphicsCommandList* list, const ImplicitGrid& grid);

        // GPU の系を写す前に、バッファを UAV にする(中身は RecordCopySystem・RecordCopyLevels が写す)
        void RecordReset(ID3D12GraphicsCommandList* list);

        // GPU が作った系(GpuImplicitBuild。T-0129)のセル・面・セルの面の一覧と面の数を写す(RecordUpload か RecordReset の後。
        // source・header は COPY_SOURCE の状態で、並びはこの系と同じ: セル・面・面の一覧〔面の数 × 2〕。面の数は header の faceCountOffset の 1 語)。
        // 上限の大きさ(source の終わりまで)を写す
        void RecordCopySystem(ID3D12GraphicsCommandList* list, ID3D12Resource* source, uint64_t cellsOffset,
                              uint64_t facesOffset, uint64_t listsOffset, ID3D12Resource* header,
                              uint64_t faceCountOffset);

        // GPU が作った多重格子の段(GpuImplicitLevels。T-0134)の段の表(header の先頭。implicit_levels.hlsl の見出し)・節・隣・子の一覧を写す
        // (RecordUpload か RecordReset の後。どれも COPY_SOURCE の状態で、並びはこの系と同じ)
        void RecordCopyLevels(ID3D12GraphicsCommandList* list, ID3D12Resource* header, ID3D12Resource* nodes,
                              ID3D12Resource* links, ID3D12Resource* children);

        // 1 刻み。options.method は Multigrid だけ(赤黒・RKL2 は CPU の比べる相手で、GPU には載せない)
        [[nodiscard]] bool RecordStep(ID3D12GraphicsCommandList* list, uint64_t debugRing,
                                      const ImplicitOptions& options,
                                      uint32_t maxLimitRounds = DEFAULT_MAX_LIMIT_ROUNDS,
                                      const GpuImplicitRecordShape& shape = {});

        void RecordTimestamp(ID3D12GraphicsCommandList* list, uint32_t index);
        void RecordReadback(ID3D12GraphicsCommandList* list);

        // 読み戻したセルのエネルギー・端数を grid に、最後の刻みの数を cost に
        [[nodiscard]] bool Read(ImplicitGrid& grid, GpuImplicitCost& cost) const;
        [[nodiscard]] std::vector<uint64_t> ReadTimestamps(uint32_t count) const;

        // V サイクル 1 回に積む Dispatch の数(0 グループのものも含む。最後に RecordStep した形で。計測の表に使う)
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
            BufferPlan,
            BufferArgs,
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
            PassPlanLevels,
            PassPlanArgs,
            PassCount
        };

        // implicit_conduct.hlsl の cbuffer ImConstants と同じ並び
        struct Constants {
            uint32_t maxNodes = 0;
            uint32_t maxCells = 0;
            uint32_t maxFaces = 0;
            uint32_t depth = 0;
            uint32_t dispatchLevels = 0;
            uint32_t color = 0;
            uint32_t tolerance = 0;
            uint32_t slack = 0;
            int32_t correctionScale = 0;
            uint32_t tailMaxNodes = 0;
            uint32_t tailMaxLinks = 0;
            uint32_t coarsestTailMaxNodes = 0;
            uint32_t maxLevels = 0;
            uint32_t coarsestDispatch = 1;
            uint32_t unused1 = 0;
            uint32_t sweeps = 0;
        };

        // 写す 1 つ(RecordCopies。大きさは写す先と source の残りで切る)
        struct CopyRegion {
            Buffer target = BufferCells;
            uint64_t targetOffset = 0;
            ID3D12Resource* source = nullptr;
            uint64_t sourceOffset = 0;
            uint64_t bytes = 0;
        };

        GpuImplicit() = default;

        std::expected<void, std::string> CreatePipelines(ID3D12Device5* device);
        std::expected<void, std::string> CreateBuffers(ID3D12Device5* device);
        [[nodiscard]] uint64_t BufferBytes(Buffer buffer) const;
        void Dispatch(ID3D12GraphicsCommandList* list, Pass pass, uint32_t groups);
        void DispatchIndirect(ID3D12GraphicsCommandList* list, Pass pass, uint32_t slot);
        void RecordPlan(ID3D12GraphicsCommandList* list);
        void RecordCopies(ID3D12GraphicsCommandList* list, std::span<const CopyRegion> regions);
        void RecordSmooth(ID3D12GraphicsCommandList* list, uint32_t depth, uint32_t slot, uint32_t sweeps);
        void RecordVCycle(ID3D12GraphicsCommandList* list, const ImplicitOptions& options);
        void BeginSkippable(ID3D12GraphicsCommandList* list, uint32_t word);
        void EndSkippable(ID3D12GraphicsCommandList* list);

        // --- 上限 ---
        GpuImplicitLimits m_limits;
        GpuImplicitTuning m_tuning;
        Constants m_constants;
        uint32_t m_maxDispatchLevels = 1;  // Create で決めた段ごとの Dispatch の段の上限(引数のバッファの大きさ)
        bool m_predication = true;

        // --- GPU ---
        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12CommandSignature> m_dispatchSignature;  // ExecuteIndirect の Dispatch だけの引数
        std::array<ComPtr<ID3D12PipelineState>, PassCount> m_pipelines;
        std::array<ComPtr<ID3D12Resource>, BufferCount> m_buffers;
        std::array<ComPtr<ID3D12Resource>, BufferCount> m_uploads;
        ComPtr<ID3D12Resource> m_cellsReadback;
        ComPtr<ID3D12Resource> m_stateReadback;
        ComPtr<ID3D12Resource> m_wideReadback;
        ComPtr<ID3D12Resource> m_planReadback;  // 計画の見出し(段の数・ImTail の境)
        ComPtr<ID3D12QueryHeap> m_timestamps;
        ComPtr<ID3D12Resource> m_timestampReadback;
        uint32_t m_timestampCount = 0;
    };

}  // namespace bicameral::sim
