// gpu_implicit.h — 細かいレベルの熱の陰解法(方式②。ADR-0019)の GPU 版(T-0117)。CPU リファレンス sim/implicit_conduction の
// StepImplicit(多重格子の V サイクル・誤差の見込みで止める・安全網)と毎刻みビット一致する Compute の段(shaders/sim/implicit_conduct.hlsl)を記録する。
// 試作の約束は CPU と同じ(セルの一覧・熱容量一定)。木につないだ系(T-0119 の multires_implicit_conduction が作る、刻みの初めの温度と
// 粗い側の端数の枠つきのセル)もそのまま解ける(T-0127)。系のセル・面・面の一覧は GPU で作れる(GpuImplicitBuild。T-0129)。GPU の伝導の段から呼ぶ形は GpuMultiresImplicit(T-0132)。
// 多重格子の段の形(節・隣・重み・親子)は CPU の BuildImplicitGrid が作ったものを写す(RecordUpload)か、GPU で作った段
// (GpuImplicitLevels。T-0134)を写す(RecordCopyLevels。段の数・節の始まりと数の表も写す)。
// 大きさは上限(GpuImplicitLimits)で決め、段の形・数は GPU のバッファ(計画 u9)から読む(T-0136): 刻みの初めに ImPlanLevels・ImPlanArgs が
// 長い行の節の一覧・ImTail の境・間接の Dispatch の引数を作り、V サイクルは記録の上限の段(GpuImplicitTuning::dispatchLevels)まで
// ExecuteIndirect で積む(その段が無い・下りが止まった段より下なら引数が 0 グループ)。CPU は段の数も ImTail の境も知らない。
// 計画の段ごとの表(長い行の節・隣の数の最大・同じ色の隣)は系の形だけで決まるので、系を写した(RecordUpload・RecordCopySystem・
// RecordCopyLevels)後の最初の刻み・解きだけで作る(T-0606)。系のバッファをほかの道で書き換える呼ぶ側は無い前提。
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

        // --- 段ごとの節の数(先頭の KEPT_LEVELS 段。次の刻みの安い回の形を選ぶのに使う。T-0505)---
        static constexpr uint32_t KEPT_LEVELS = 16;
        std::array<uint32_t, KEPT_LEVELS> levelNodes{};
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
        // V サイクルを段ごとの Dispatch で積む回の数(0 なら上限 options.cycles まで。1 以上)。これより後の回は安い回にする(T-0179)。値は同じ
        uint32_t dispatchCycles = 0;
        // 安い回で段ごとの Dispatch を積む段の数(T-0505)。その段から下(最も粗い段まで)を ImTail が 1 グループで回す。
        // 0 なら段 0 から V サイクル全体が 1 グループ(T-0179 の形。Dispatch は 3 つ)。1 グループに大きい段を入れると GPU が止まる
        // (128³ の 1 グループの V サイクルを数十回で 2 秒を超えた。T-0494)ので、ShapeFrom は節が多い段をここに入れる。
        // 下りが止まる段より深くは下りない(値は同じ)
        uint32_t cheapLevels = 0;
        // V サイクルを判定で止めず、ちょうど options.cycles 回まわす(T-0843。CPU の GasProjectionOptions::fixedCycles と同じ)。
        // V の前と各 V の後の止める判定(ImSolveConverged)を積まないので、閾値の欄は読まない。RecordSolve だけが見る
        bool fixedCycles = false;
    };

    class GpuImplicit {
    public:
        static constexpr uint32_t MAX_TIMESTAMPS = 1024;  // 段ごとの時間(T-0587)を何回分も打てるように 64 → 1024
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
        // 最も粗い段の掃き出しは前の刻みが ImTail を使わなかった時だけ積む。安い回は、節が cheapTailMaxNodes を超える先頭の段を
        // 段ごとの Dispatch にする(T-0505。1 グループの仕事の上限)
        [[nodiscard]] static GpuImplicitRecordShape ShapeFrom(const GpuImplicitCost& previous,
                                                              uint32_t cheapTailMaxNodes = CHEAP_TAIL_MAX_NODES);

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

        // --- 一般の右辺で解く形(T-0531。気体の圧力の投影。CPU の模型は sim/gas_projection の Solve)---
        // 段 0 の式 (C_i + Σ g) x_i − Σ g x_j = D_i × rhs_i を V サイクルで解く。作業場(WorkBuffer)の段 0 の値 A・B に初めの近似、
        // 右辺の欄に rhs、刻みの初めの温度の欄にセルごとの閾値(GasPressureThresholds)を書いてから呼ぶ(RecordUploadSolve か、GPU の段が直接)。
        // V の前に 1 回判定し、|残差| が閾値以上のセルが無くなるまで options.cycles を上限に回す(CPU の Solve と同じ順。止めた後の回は述語で飛ばす)。
        // 面の流れ・安全網・足すは積まない。解いた値は作業場の値 A(セルの順)
        [[nodiscard]] bool RecordSolve(ID3D12GraphicsCommandList* list, uint64_t debugRing,
                                       const ImplicitOptions& options, const GpuImplicitRecordShape& shape = {});
        // CPU から右辺・初めの近似・閾値(どれもセルの順)を作業場へ写す(RecordUpload の後。試験と計測用)
        [[nodiscard]] bool RecordUploadSolve(ID3D12GraphicsCommandList* list, std::span<const int64_t> rhs,
                                             std::span<const int64_t> values, std::span<const int64_t> thresholds);
        // 解いた値(セルの数)と数を読み戻す(RecordSolve の後)
        void RecordSolutionReadback(ID3D12GraphicsCommandList* list, uint32_t cellCount);
        [[nodiscard]] bool ReadSolution(std::span<int64_t> values, GpuImplicitCost& cost) const;
        // 作業場(64bit。並びは implicit_conduct.hlsl の先頭: 値 A × 節・値 B × 節・右辺 × 節・刻みの初めの温度 × セル…)
        [[nodiscard]] ID3D12Resource* WorkBuffer() const { return m_buffers[BufferWork].Get(); }
        [[nodiscard]] const GpuImplicitLimits& Limits() const { return m_limits; }

        // バッファを COMMON に戻す(RecordStep の後。同じリストで次の刻みに RecordReset をもう一度呼ぶため。T-0132)
        void RecordRelease(ID3D12GraphicsCommandList* list);

        // 解いたセル(ImGpuCell の並び。RecordStep の後は UAV の状態。伝導の段が変化を読む。T-0132)
        [[nodiscard]] ID3D12Resource* CellsBuffer() const { return m_buffers[BufferCells].Get(); }

        void RecordTimestamp(ID3D12GraphicsCommandList* list, uint32_t index);
        void RecordReadback(ID3D12GraphicsCommandList* list);
        // 数(V の回数・安全網・段の形)だけを読み戻す(セルは写さない。次の刻みの記録の形を選ぶ用。T-0132)
        void RecordCostReadback(ID3D12GraphicsCommandList* list);

        // 読み戻したセルのエネルギー・端数を grid に、最後の刻みの数を cost に
        [[nodiscard]] bool Read(ImplicitGrid& grid, GpuImplicitCost& cost) const;
        [[nodiscard]] bool ReadCost(GpuImplicitCost& cost) const;
        [[nodiscard]] std::vector<uint64_t> ReadTimestamps(uint32_t count) const;

        // ShapeFrom が前の刻みの V の回数に足す余裕の最小(T-0179)
        static constexpr uint32_t CYCLE_MARGIN = 2;
        // 安い回の ImTail(1 グループ)に入れる段の節の上限(T-0505)。V 1 回の 1 グループは節あたり約 40 ns(T-0494 の 64³)なので、
        // 上限 64 回を全部回しても 1 刻み約 10 ms で止まる。docs/perf.md の T-0505 の計測で選んだ
        static constexpr uint32_t CHEAP_TAIL_MAX_NODES = 4096;

        // V サイクル 1 回に積む Dispatch の数(0 グループのものも含む。最後に RecordStep した形で。計測の表に使う)
        [[nodiscard]] uint32_t DispatchesPerCycle(const ImplicitOptions& options) const;

        // 要らない回を述語で飛ばすか(既定 true。false なら段が空で抜けるだけ。計測で比べる用)
        void UsePredication(bool use) { m_predication = use; }
        // ImTail が節の値を groupshared に置くか(既定 true。T-0587。false は前の形〔全体のメモリとバリア〕。値は同じ。計測で比べる用)
        void UseSharedTail(bool use) { m_constants.tailShared = use ? 1 : 0; }
        // 次の解きで段ごとの計画の表を作り直させる(計測用。系の形が刻みごとに変わる時の固定費を測る。T-0843)
        void InvalidatePlan() { m_planReady = false; }

        // --- 段ごとの時間(計測用。T-0587)---
        // BeginStageStamps から EndStageStamps まで、Dispatch のたびに(バリアの後に)タイムスタンプを firstIndex + 1 から打つ
        // (firstIndex に始まりの 1 つ)。段の名前は StageLabels(最後に打った組。種類 | 段 << 8 | 色 << 16)と PassName で
        void BeginStageStamps(ID3D12GraphicsCommandList* list, uint32_t firstIndex);
        void EndStageStamps() { m_stageStamps = false; }
        [[nodiscard]] const std::vector<uint32_t>& StageLabels() const { return m_stageLabels; }
        [[nodiscard]] static const char* PassName(uint32_t pass);

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
            PassSolveConverged,
            PassSolveCheckEnd,
            PassPlanColors,
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
            uint32_t cheapTail =
                0;  // 1 以上なら安い回: ImTail が min(下りが止まる段, cheapTail − 1) から回す(T-0179・T-0505)
            uint32_t sweeps = 0;
            uint32_t tailShared = 1;  // ImTail の節の値を groupshared に置くか(T-0587)
            uint32_t checkColor = 0;  // 1 なら V の後の止める判定(段 0 に同じ色の隣が無ければ色 1 を飛ばす。T-0606)
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
        void RecordDown(ID3D12GraphicsCommandList* list, const ImplicitOptions& options, uint32_t depthEnd);
        void RecordUp(ID3D12GraphicsCommandList* list, const ImplicitOptions& options, uint32_t depthEnd);
        // 安い回: 段 depthEnd より上は段ごとの Dispatch、そこから下は ImTail の 1 グループ(T-0179・T-0505)
        void RecordCheapCycle(ID3D12GraphicsCommandList* list, const ImplicitOptions& options, uint32_t depthEnd);
        void BeginSkippable(ID3D12GraphicsCommandList* list, uint32_t word);
        void EndSkippable(ID3D12GraphicsCommandList* list);

        // --- 上限 ---
        GpuImplicitLimits m_limits;
        GpuImplicitTuning m_tuning;
        Constants m_constants;
        uint32_t m_maxDispatchLevels = 1;  // Create で決めた段ごとの Dispatch の段の上限(引数のバッファの大きさ)
        bool m_predication = true;
        bool m_planReady = false;  // 段ごとの計画の表が今の系の形で作ってある(系を写すと false。T-0606)

        // --- GPU ---
        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12CommandSignature> m_dispatchSignature;  // ExecuteIndirect の Dispatch だけの引数
        std::array<ComPtr<ID3D12PipelineState>, PassCount> m_pipelines;
        std::array<ComPtr<ID3D12Resource>, BufferCount> m_buffers;
        std::array<ComPtr<ID3D12Resource>, BufferCount> m_uploads;
        ComPtr<ID3D12Resource> m_cellsReadback;
        ComPtr<ID3D12Resource> m_stateReadback;
        ComPtr<ID3D12Resource> m_wideReadback;
        ComPtr<ID3D12Resource> m_planReadback;      // 計画の見出し(段の数・ImTail の境)
        ComPtr<ID3D12Resource> m_solveUpload;       // 一般の右辺で解く形の右辺・初めの近似・閾値(セル × 3。T-0531)
        ComPtr<ID3D12Resource> m_solutionReadback;  // 解いた値(セル)
        ComPtr<ID3D12QueryHeap> m_timestamps;
        ComPtr<ID3D12Resource> m_timestampReadback;
        uint32_t m_timestampCount = 0;

        // --- 段ごとの時間(T-0587)---
        void RecordStageStamp(ID3D12GraphicsCommandList* list, Pass pass);
        bool m_stageStamps = false;
        uint32_t m_stageStampNext = 0;
        std::vector<uint32_t> m_stageLabels;
    };

}  // namespace bicameral::sim
