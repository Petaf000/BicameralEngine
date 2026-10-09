// probe_sim.h — 仮の刻み(shaders/common/probe_sim.hlsli)を「単位」の列として GPU で走らせる道具と、その CPU リファレンス
// (T-0004・T-0012・T-0086・T-0005)。
//
// 刻みのループの形(06 §4・ADR-0011)と、Work Graphs の伝播(T-0005)を確かめる。中身(64³ のセルの成分 + エネルギー、温度の差の伝導と
// 反応。T-0089。初めは空気の中の木箱)は段ごとに本物に置き換える:
//   - 伝導の単位は Work Graph(shaders/sim/probe_conduct.hlsl)。入力は GPU が作る活性の一覧(DispatchGraph の GPU の入力)なので、
//     どこが活性か・何ブロック計算するかを CPU は知らない(D-107)。熱が広がっている所だけが計算される。
//   - 1 刻み = 決まった数の単位(適用 → 伝導 → 重さ × k → ハッシュ)。フレームの切れ目はどの単位の間にも来てよい(刻みはフレームをまたぐ)。
//   - CPU は毎フレーム、そのフレームに投げる単位(何番目の刻みの何番目から何個)を、フレームの枠ごとのリストに記録して投げる。
//     同じリストは前の実行が終わるまで投げ直せない(debug layer [553])ので、使い回す記録済みのリストではなく毎フレーム記録する。
//     CPU が書くのはコマンドの並び・ルート定数・Dispatch だけ(世界の状態には触れない。D-107)。
//   - 単位ごとのタイムスタンプを取り、CPU は単位の GPU 時間から次のフレームに投げる数を決める(frame/sim_scheduler)。
//   - 刻みの最後の単位が状態のハッシュを GPU で取り、刻みの中のイベントを並べてリングへ写す(06 §2 段 9)。フレームの終わりに CPU へ読み戻す(待たない)。
//   - コマンドはフレームのリストの先頭で GPU のコマンドキューへ足す。コマンドは自分の刻みの適用の単位まで GPU で待つ(06 §3)。
//     CPU の約束: 足すコマンドは (targetTick, sequence) の昇順で、targetTick はまだ記録していない最初の適用の刻み(NextApplyTick)以上、
//     数は FreeCommandSlots() 以下(RecordFrame が確かめる)。
//   - 抽出(描画が読む)は、投げた単位の後ろで、刻みの境界の状態を写す(1 フレームに 1 回まで)。
//
// 使い方:
//   auto table = BakeReactionTable(MakeCombustionTestTable());
//   auto sim = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, *table, {.busyIterations = n, .busyPieces = k});
//   ID3D12CommandList* list = sim->RecordFrame(slot, {.firstTick = t, .firstUnit = u, .unitCount = c, .commands = 新しいコマンド, ...});
//   computeQueue.Submit(list) → フェンスが進んだら sim->ReadFrame(slot)
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/probe_sim.hlsli"
#include "common/probe_world.hlsli"
#include "gpu/com_ptr.h"
#include "gpu/debug_ring.h"
#include "gpu/graph_trace.h"
#include "gpu/readback_ring.h"
#include "gpu/work_graph.h"
#include "gpu/work_graph_stats.h"
#include "sim/command.h"
#include "sim/gpu_physics.h"
#include "sim/physics_scene.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // --- コマンド(06 §3 の 64 バイト。sim/command.h)---

    using ProbeCommand = Command;
    static_assert(sizeof(ProbeCommand) == PROBE_COMMAND_BYTES);

    // セル (x, y, z) を約 2700 K 温める熱を足す(PROBE_POKE_HEATING_MILLIKELVIN。明示的な湧き出し)
    [[nodiscard]] ProbeCommand MakePokeCommand(uint64_t targetTick, uint32_t sequence, uint32_t x, uint32_t y,
                                               uint32_t z);

    // 光線で物を押す(T-0098。PROBE_COMMAND_TYPE_PUSH)。origin は物理の座標(2^-20 m)、direction は長さ 1 の Q1.30、力積は mN·s
    [[nodiscard]] ProbeCommand MakePushCommand(uint64_t targetTick, uint32_t sequence,
                                               const std::array<int64_t, 3>& origin,
                                               const std::array<int32_t, 3>& direction,
                                               uint32_t impulseMillinewtonSeconds);

    // --- GPU から戻ってくるもの ---

    struct ProbeEvent {
        uint64_t tick = 0;
        uint32_t type = 0;   // PROBE_EVENT_*
        uint32_t place = 0;  // つつき: x | y << 8 | z << 16(ProbePokePlace)。遅れたコマンド: コマンドの種類

        [[nodiscard]] uint32_t PokeX() const { return place & 0xFFu; }
        [[nodiscard]] uint32_t PokeY() const { return (place >> 8) & 0xFFu; }
        [[nodiscard]] uint32_t PokeZ() const { return (place >> 16) & 0xFFu; }
        friend bool operator==(const ProbeEvent&, const ProbeEvent&) = default;
    };

    // 刻み tick の始めの状態 S(tick) の要約(ハッシュの表の欄。probe_sim.hlsli)
    struct ProbeTickHash {
        uint64_t tick = 0;
        uint64_t hash = 0;    // ProbeStateHash
        uint64_t energy = 0;  // エネルギーの合計(ProbeEnergySum。mJ の和の mod 2^64)
        uint64_t sourceEnergy =
            0;  // S(tick) を作った刻み(tick − 1)のつつきが足したエネルギー(mJ)。energy(t) = energy(t − 1) + これ
        uint32_t scheduledBlocks = 0;  // S(tick) を作った刻み(tick − 1)で伝導と反応を計算したブロックの数
        uint64_t bodyHash = 0;         // 物の状態のハッシュ(PhysicsWorld::StateHash。物理なしなら 0。T-0098)

        // 世界全体の要約(セル + 物。再生ファイルに入れて突き合わせる値)
        [[nodiscard]] uint64_t WorldHash() const { return hash ^ bodyHash; }
    };

    struct ProbeFrameReadback {
        // --- 世界の結果 ---
        std::vector<ProbeEvent> events;     // (刻み, 種類, 場所) の順(刻みの最後に GPU が並べる。分け方に依存しない)
        uint32_t droppedEventCount = 0;     // 容量を超えて書けなかった数(刻みの一時置き場 + リング)
        std::vector<ProbeTickHash> hashes;  // このフレームで終えた刻みの状態(刻みの順)

        // --- GPU の時間 ---
        std::vector<uint64_t> unitGpuTicks;  // 投げた i 番目の単位の GPU 時間(タイムスタンプの刻み)
        uint32_t firstUnit = 0;              // unitGpuTicks[0] の単位の、刻みの中の番号
        uint64_t gpuBeginTimestamp = 0;      // リスト全体(抽出と読み戻しを含む)の始めと終わり
        uint64_t gpuEndTimestamp = 0;

        // --- デバッグ ---
        uint32_t debugAssertCount = 0;  // シェーダーの assert の数(中身はログに出る)

        // 伝導のグラフのノードごとのカウンタ(このフレームの全部の刻みの合計。T-0008)
        gpu::GraphStatsSnapshot graphStats;
        uint32_t graphFindingCount = 0;  // 上限に当たった・近づいたものの数(中身は Warning でログに出る)

        // 伝導の連鎖のトレース(範囲を有効にしたときだけ。atomic の順。T-0087。組み立ては sim/probe_trace.h)
        std::vector<gpu::GraphTraceRecord> trace;
        uint32_t droppedTraceCount = 0;  // 容量を越えて書けなかった数(> 0 ならトレースは欠けている)
    };

    // 抽出の後のフック(ProbeFrameInput::afterExtract)に渡すもの(T-0096)
    struct ProbeExtractContext {
        uint64_t tick = 0;                        // 抽出した刻みの境界(S(tick)。セルの世代は tick & 1)
        ID3D12Resource* cells = nullptr;          // 世界のセル(UAV の状態。読むだけ)
        ID3D12Resource* extraction = nullptr;     // 書き終えた抽出(UAV の状態)
        D3D12_GPU_VIRTUAL_ADDRESS debugRing = 0;  // シミュのデバッグのリング(FX_ASSERT の出力。ReadFrame でログへ出る)
    };

    // 保存点を使わない(ProbeFrameInput::saveTo・restoreFrom の既定)
    inline constexpr uint32_t NO_SAVE_POINT = UINT32_MAX;

    struct ProbeFrameInput {
        uint64_t firstTick = 0;  // 最初の単位の刻み
        uint32_t firstUnit = 0;  // 最初の単位の、刻みの中の番号(0〜UnitsPerTick()-1)
        uint32_t unitCount = 0;  // 投げる単位の数(0〜MAX_UNITS_PER_FRAME。0 なら抽出だけ)

        bool extract = false;           // 単位の後ろで、刻みの境界の状態を抽出へ写すか
        uint32_t extractionTarget = 0;  // 抽出の書き先(0〜PROBE_EXTRACTION_COUNT-1)

        // GPU のキューへ足す新しいコマンド(最大 PROBE_MAX_COMMANDS。約束はファイルの先頭)
        std::span<const ProbeCommand> commands;

        // 物理の物と統計を、フレームの終わりに読み戻す(テスト用。T-0098)。読み戻しの置き場は 1 つなので、そのリストが終わってから
        // 次に読み戻すリストを投げるまでに Physics()->ReadBodies() / ReadStats() を呼ぶ。このフレームに単位があること
        bool readPhysics = false;

        // 抽出の後に同じリストへ記録するもの(覗き窓 sim/probe_peek。T-0096)。抽出するフレームだけ呼ぶ。
        // 約束: 世界のバッファは読むだけ、抽出は覗きの欄だけに書く(世界の結果を変えない。D-403)
        std::function<void(ID3D12GraphicsCommandList10* list, const ProbeExtractContext& context)> afterExtract;

        // --- 巻き戻し(T-0143。sim/probe_save_point.cpp)。どちらも刻みの境界から始まるフレーム(firstUnit == 0)だけ・同時には使えない ---
        // 単位の前に、S(firstTick) をこの番号の保存点へ写す(GPU → GPU。CreateSavePoints の後)
        uint32_t saveTo = NO_SAVE_POINT;
        // 単位の前に、世界をこの番号の保存点の状態へ戻す(firstTick は保存点の刻み)。GPU のキューで待っていたコマンドは捨てる
        // (呼ぶ側が firstTick 以降のコマンドを、このフレームの commands から足し直す)
        uint32_t restoreFrom = NO_SAVE_POINT;
    };

    // 重さの試験(R-LOOP-2)。世界の結果には入らない
    struct ProbeSimOptions {
        // 1 刻みに足す繰り返しの合計(0 なら重さの単位は無し。上限 PROBE_BUSY_ITERATIONS_LIMIT)
        uint32_t busyIterations = 0;
        uint32_t busyPieces = 1;  // それを何個の単位に分けるか(1〜PROBE_MAX_BUSY_PIECES)

        // 伝導の連鎖のトレースの最初の範囲(既定は無効。場所の箱はブロックの座標。セルからは ProbeTraceFilterForCells。T-0087)
        gpu::GraphTraceFilter trace;

        // トレースの容量(1 フレームに書ける記録の上限)。実行中に SetTraceFilter で範囲を変えるなら上限ぶんを渡す(T-0088)。
        // 0 なら trace.capacity だけ
        uint32_t traceCapacity = 0;

        // 物理の場面(T-0098。nullptr なら物理の単位は無い)。物の 1 刻みを伝導の後ろの単位に入れ、押すコマンドを受け付ける。
        // 呼んだ後は持たなくてよい
        const PhysicsScene* physicsScene = nullptr;
        GpuPhysicsOptions
            physicsOptions;  // 物理の解き方(既定は T-0092 で測った形: 広域と接触は Work Graph・色ごとの解は Compute)
        physics::PxParameters physicsParameters = physics::PxDefaultParameters();

        // 初めの世界(PROBE_CELL_COUNT 個。空なら MakeProbeInitialWorld。試験用。T-0122)。作る時だけ読む(呼んだ後は持たなくてよい)
        std::span<const reaction::RxCell> initialWorld;
    };

    // --- GPU で走らせる ---

    class ProbeSim {
    public:
        static constexpr uint32_t FRAME_SLOT_COUNT = 4;  // 同時に GPU にあってよいフレームのリストの数
        static constexpr uint32_t MAX_UNITS_PER_FRAME = 256;

        // listType: フレームのリストを投げるキューの種類(シミュは compute。06 §4)
        // table: 反応の表(ベイクしたもの)。GPU に写し、初めの世界(MakeProbeInitialWorld)も作る。呼んだ後は持たなくてよい
        [[nodiscard]] static std::expected<ProbeSim, std::string> Create(ID3D12Device5* device,
                                                                         D3D12_COMMAND_LIST_TYPE listType,
                                                                         const BakedReactionTable& table,
                                                                         const ProbeSimOptions& options = {});

        // 1 刻みの単位の数(適用・伝導・ハッシュ + 重さの単位)
        [[nodiscard]] uint32_t UnitsPerTick() const {
            return PROBE_FIXED_UNITS_PER_TICK + PhysicsUnitCount() + BusyUnitCount();
        }

        [[nodiscard]] uint32_t HashUnit() const { return UnitsPerTick() - 1; }

        // 次に記録する単位が (tick, unit) のとき、まだ記録していない最初の適用の単位の刻み。
        // そのフレームに足すコマンドの targetTick はこれ以上でなければならない(でなければ適用に間に合わない)
        [[nodiscard]] static uint64_t NextApplyTick(uint64_t tick, uint32_t unit) {
            return unit == 0 ? tick : tick + 1;
        }

        // GPU のコマンドキューの空き(記録した適用の単位で取り出される分を引いた、次のフレームに足せる数)
        [[nodiscard]] uint32_t FreeCommandSlots() const { return PROBE_COMMAND_QUEUE_CAPACITY - m_queuedCommandCount; }

        // slot のアップロードのバッファに入力を書き、slot のリストに単位を記録して返す。
        // 呼ぶ側の約束: slot の前のリストを GPU が終えている。入力が範囲外・記録の失敗なら nullptr(理由はログ)
        [[nodiscard]] ID3D12CommandList* RecordFrame(uint32_t slot, const ProbeFrameInput& input);

        // slot のリストを GPU が終えた後に呼ぶ(待たない。終わったかどうかは呼ぶ側がフェンスで見る)。
        // ノードのカウンタの要約と上限の Warning もここでログへ出す(同じ Warning を繰り返しすぎないように状態を持つので const でない)
        [[nodiscard]] ProbeFrameReadback ReadFrame(uint32_t slot);

        // 伝導の連鎖のトレースの範囲を変える。次に記録するフレームから効く(容量は作った時の容量までに切り詰める。T-0088)
        void SetTraceFilter(const gpu::GraphTraceFilter& filter) { m_graphTrace.SetFilter(filter); }
        [[nodiscard]] const gpu::GraphTraceFilter& TraceFilter() const { return m_graphTrace.Filter(); }

        // 伝導のグラフのカウンタの名前と上限(フレームのループが要約をまとめて出すときに使う)
        [[nodiscard]] const gpu::GraphStatsLayout& ConductStatsLayout() const { return m_graphStats.Layout(); }

        // 描画用の抽出(0〜PROBE_EXTRACTION_COUNT-1。全部のセルの温度と見る物質 3 つの量 + ブロックの活性の印、PROBE_EXTRACTION_WORDS 個。
        // 並びは probe_sim.hlsli)。描画は読むだけ
        [[nodiscard]] ID3D12Resource* Extraction(uint32_t target) const { return m_extractions[target].Get(); }

        // 世界のセル(2 世代 × PROBE_CELL_COUNT × RxCell。S(t) は世代 t & 1)。テストが読み戻すだけ(フレームの間は COMMON)
        [[nodiscard]] ID3D12Resource* Cells() const { return m_cells.Get(); }

        // 伝導の Work Graph の裏のメモリ(ドライバが決める。docs/perf.md に残す)
        [[nodiscard]] uint64_t ConductBackingMemoryBytes() const { return m_conductGraph->BackingMemoryBytes(); }

        // 物理(物理なしなら nullptr。T-0098)。テストが読み戻しを読むだけ(記録はフレームのリストの中で ProbeSim がする)
        [[nodiscard]] const GpuPhysics* Physics() const { return m_physics.get(); }

        // --- 巻き戻し(保存点 + 再生。T-0143・ADR-0036。sim/probe_save_point.cpp)---
        // 保存点(刻みの境界の状態の写し。VRAM に置く)を count 個作る。作り直すと前の保存点は消える
        [[nodiscard]] bool CreateSavePoints(ID3D12Device5* device, uint32_t count);
        [[nodiscard]] uint32_t SavePointCount() const { return static_cast<uint32_t>(m_savePoints.size()); }
        // その保存点が持つ状態の刻み(まだ写していなければ UINT64_MAX)。写すリストを記録した時に決まる(GPU が終えたかは呼ぶ側がフェンスで見る)
        [[nodiscard]] uint64_t SavePointTick(uint32_t index) const {
            return index < m_savePoints.size() ? m_savePoints[index].tick : UINT64_MAX;
        }
        // tick より後の刻みの保存点を空にする(巻き戻した後、その先の流れが変わりうるので)
        void DiscardSavePointsAfter(uint64_t tick) {
            for (SavePoint& savePoint : m_savePoints) {
                if (savePoint.tick != UINT64_MAX && savePoint.tick > tick)
                    savePoint.tick = UINT64_MAX;
            }
        }
        // 保存点 1 つの大きさ(docs/perf.md)
        [[nodiscard]] uint64_t SavePointBytes() const;

    private:
        struct FrameSlot {
            // --- 記録 ---
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList10> list;  // 毎フレーム記録し直す

            // --- 入力 ---
            ComPtr<ID3D12Resource> input;      // アップロード(PROBE_INPUT_BYTES)
            std::byte* mappedInput = nullptr;  // Map したまま

            // --- 読み戻し ---
            ComPtr<ID3D12Resource> timestampReadback;  // (MAX_UNITS_PER_FRAME + 2) × 8 バイト
            ComPtr<ID3D12Resource> hashReadback;       // PROBE_HASH_BYTES

            // 最後に記録した範囲(ReadFrame が、どの刻みのハッシュとどの単位の時間かを知るため)
            uint64_t firstTick = 0;
            uint32_t firstUnit = 0;
            uint32_t unitCount = 0;
        };

        ProbeSim(const ProbeSimOptions& options, gpu::ReadbackRing&& events, gpu::DebugRing&& debugRing,
                 gpu::WorkGraphStats&& graphStats, gpu::GraphTrace&& graphTrace)
            : m_options(options),
              m_events(std::move(events)),
              m_debugRing(std::move(debugRing)),
              m_graphStats(std::move(graphStats)),
              m_graphTrace(std::move(graphTrace)) {}

        [[nodiscard]] uint32_t BusyUnitCount() const { return m_options.busyIterations > 0 ? m_options.busyPieces : 0; }
        [[nodiscard]] uint32_t PhysicsUnitCount() const { return m_physics ? 1 : 0; }

        // --- 作る ---
        [[nodiscard]] bool CreatePipelines(ID3D12Device5* device);
        [[nodiscard]] bool CreateConductGraph(ID3D12Device5* device);
        [[nodiscard]] bool CreateBuffers(ID3D12Device5* device);
        [[nodiscard]] bool CreateWorld(ID3D12Device5* device, const BakedReactionTable& table,
                                       std::span<const reaction::RxCell> initialWorld);
        void RecordInitialization(ID3D12GraphicsCommandList10* list);
        [[nodiscard]] bool CreateFrameSlots(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType);
        [[nodiscard]] std::expected<void, std::string> CreatePhysics(ID3D12Device5* device);

        // --- 入力とコマンド ---
        [[nodiscard]] bool ValidateInput(uint32_t slot, const ProbeFrameInput& input) const;
        [[nodiscard]] bool ValidateCommands(const ProbeFrameInput& input) const;
        void WriteInput(FrameSlot& frame, const ProbeFrameInput& input) const;
        void RecordEnqueue(ID3D12GraphicsCommandList10* list, uint32_t commandCount) const;
        void TrackCommands(std::span<const ProbeCommand> commands, uint64_t nextApplyTick);

        // --- 単位の記録 ---
        void BindRootArguments(ID3D12GraphicsCommandList10* list, ID3D12Resource* input) const;
        void BindRootViews(ID3D12GraphicsCommandList10* list, ID3D12Resource* input) const;
        void RecordUnit(ID3D12GraphicsCommandList10* list, ID3D12Resource* input, uint64_t tick, uint32_t unit);
        void RecordConduct(ID3D12GraphicsCommandList10* list, ID3D12Resource* input, uint64_t tick);
        void RecordPhysics(ID3D12GraphicsCommandList10* list, ID3D12Resource* input, uint64_t tick);
        void RecordPhysicsInitialization(ID3D12GraphicsCommandList10* list, ID3D12Resource* input);
        void RecordActiveListStates(ID3D12GraphicsCommandList10* list, D3D12_RESOURCE_STATES before,
                                    D3D12_RESOURCE_STATES after) const;
        void RecordExtract(ID3D12GraphicsCommandList10* list, uint64_t tick, uint32_t target) const;
        void RecordExtractAndHook(ID3D12GraphicsCommandList10* list, uint64_t tick, const ProbeFrameInput& input);

        // --- 読み戻し ---
        void RecordReadbacks(ID3D12GraphicsCommandList10* list, uint32_t slot, bool hasHash) const;

        // --- 巻き戻し(sim/probe_save_point.cpp)---
        // 保存点に写す世界の部分。generationBytes > 0 のもの(セル・熱)は S(tick) の世代だけを写し、戻すときは 2 世代に写す
        // (予定していないブロックは 2 世代とも S(t) と同じ値。予定したブロックは刻み t がもう一方の世代を全部書き直す。probe_sim.hlsli)
        struct StateRegion {
            ID3D12Resource* resource = nullptr;
            uint64_t offset = 0;
            uint64_t bytes = 0;
            uint64_t generationBytes = 0;
        };

        struct SavePoint {
            uint64_t tick = UINT64_MAX;                   // UINT64_MAX = 空
            std::vector<ComPtr<ID3D12Resource>> buffers;  // StateRegions の順
            GpuPhysicsCursor physics;
        };

        [[nodiscard]] std::vector<StateRegion> StateRegions(uint64_t tick) const;
        [[nodiscard]] bool ValidateSavePoints(const ProbeFrameInput& input) const;
        void RecordSave(ID3D12GraphicsCommandList10* list, uint32_t index, uint64_t tick);
        void RecordRestore(ID3D12GraphicsCommandList10* list, uint32_t index);
        void ResetCommandMirror();
        [[nodiscard]] std::vector<ProbeTickHash> ReadHashes(const FrameSlot& frame) const;

        ProbeSimOptions m_options;

        // --- パイプライン ---
        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_enqueuePipeline;
        ComPtr<ID3D12PipelineState> m_applyPipeline;
        ComPtr<ID3D12PipelineState> m_wakeDuePipeline;  // 起こす刻みの来たブロックを一覧へ(待ちの丸め。T-0122)
        ComPtr<ID3D12PipelineState> m_busyPipeline;
        ComPtr<ID3D12PipelineState> m_hashCellsPipeline;
        ComPtr<ID3D12PipelineState> m_flushEventsPipeline;
        ComPtr<ID3D12PipelineState> m_extractPipeline;

        // --- 伝導の Work Graph ---
        std::unique_ptr<gpu::WorkGraph> m_conductGraph;  // 伝導(shaders/sim/probe_conduct.hlsl)
        uint32_t m_conductEntrypoint = 0;                // WakeBlocks の入口の番号
        bool m_conductInitialized = false;               // 裏のメモリを初期化するリストを記録したか(最初の 1 回だけ)

        // --- 世界と抽出 ---
        ComPtr<ID3D12Resource> m_cells;    // 2 世代 × PROBE_CELL_COUNT × RxCell
        ComPtr<ID3D12Resource> m_thermal;  // 2 世代 × PROBE_CELL_COUNT × HcThermalCache
        // 反応の表(物質・規則・索引・速度。既定のヒープ)と、初めの世界・表のアップロード(最初のフレームで写す。以後は使わない)
        std::array<ComPtr<ID3D12Resource>, 4> m_reactionTable;
        // 表 4 つ・セル・キャッシュ(1 世代ぶん。2 世代に写す)・予定の印(初めの起こす刻みの印。ProbeInitialScheduleWords)
        std::array<ComPtr<ID3D12Resource>, 7> m_initialUploads;
        bool m_initialized = false;
        std::array<uint32_t, PROBE_VIEW_SPECIES_COUNT> m_viewSpecies{};  // 抽出に写す物質(O2・CO2・炭)
        std::array<ComPtr<ID3D12Resource>, PROBE_EXTRACTION_COUNT> m_extractions;
        ComPtr<ID3D12Resource> m_busySink;

        // --- 物理(T-0098)---
        std::unique_ptr<GpuPhysics> m_physics;
        int64_t m_physicsRate = 0;  // 物理の 1 秒あたりの小刻みの数(押す力積の計算。フレームの入力の見出しで渡す)
        bool m_physicsInitialized = false;

        // --- 刻みの道具(表・キュー・イベント・活性)---
        ComPtr<ID3D12Resource> m_hashes;        // ハッシュの表(PROBE_HASH_BYTES)
        ComPtr<ID3D12Resource> m_commandQueue;  // GPU のコマンドキュー(PROBE_COMMAND_QUEUE_BYTES)
        ComPtr<ID3D12Resource> m_tickEvents;    // 刻みの中のイベントの一時置き場(PROBE_TICK_EVENT_BYTES)

        // 活性の一覧(刻みの偶奇で 2 組。PROBE_ACTIVE_LIST_BYTES)。フレームの中では UAV、伝導の間だけ入力の組を GPU の入力の状態にする
        std::array<ComPtr<ID3D12Resource>, 2> m_activeLists;
        ComPtr<ID3D12Resource> m_blockSchedule;  // 予定の印(PROBE_SCHEDULE_BYTES)

        // --- 読み戻しとフレームの枠 ---
        gpu::ReadbackRing m_events;
        gpu::DebugRing m_debugRing;
        gpu::WorkGraphStats m_graphStats;      // 伝導のグラフのノードのカウンタ(T-0008)
        gpu::GraphTrace m_graphTrace;          // 伝導の連鎖のトレース(T-0087)
        ComPtr<ID3D12QueryHeap> m_timestamps;  // slot ごとに MAX_UNITS_PER_FRAME + 2
        std::array<FrameSlot, FRAME_SLOT_COUNT> m_slots;

        // --- 巻き戻し(T-0143)---
        std::vector<SavePoint> m_savePoints;
        ComPtr<ID3D12Resource> m_zeroQueueHeader;  // 0 の 16 バイト(戻すときにコマンドキューを空にする)

        // --- GPU のコマンドキューの CPU 側の控え(足すのは CPU だけなので、末尾と待っている数を CPU が知っている)---
        struct QueuedTick {
            uint64_t targetTick = 0;
            uint32_t count = 0;
        };

        uint32_t m_commandTail = 0;             // 足した総数(GPU の末尾と同じ。2^32 で一周)
        uint32_t m_queuedCommandCount = 0;      // まだ適用の単位を記録していないコマンドの数
        std::vector<QueuedTick> m_queuedTicks;  // その内訳(targetTick の昇順)
        bool m_hasEnqueued = false;
        ProbeCommand m_lastEnqueued;  // 最後に足したコマンド(並びの確認)
    };

    // --- 初めの世界(T-0089)---

    // 1 世代ぶんのセル(PROBE_CELL_COUNT 個)。全体が 300 K の空気(N2・O2、1 気圧)、中央に木箱
    // (8³ セル = 4 m 角、壁の厚さ 1 セル、中は空気。壁のセルは体積の 1 割がセルロースで、残りは孔の中の空気)
    [[nodiscard]] std::vector<reaction::RxCell> MakeProbeInitialWorld(const BakedReactionTable& table);

    // 抽出に写す物質(O2・CO2・炭の順。描画の色分け)
    [[nodiscard]] std::array<uint32_t, PROBE_VIEW_SPECIES_COUNT> ProbeViewSpecies(const BakedReactionTable& table);

    // --- CPU リファレンス(GPU とビット一致するはずのもの。D-307・CLAUDE.md 原則 4)---

    // 全部のセルを毎刻み計算する(活性を使わない)。GPU は活性のブロックだけを計算するので、一致すれば活性の取り方も正しい
    // (眠っているブロックは、計算しても変わらないことが規則で決まっている。probe_sim.hlsli の「活性」)。
    // 予定のブロックの数は、変わった・まだ進めるブロックの記録から GPU と同じ規則で予想する
    class ProbeReference {
    public:
        // initialWorld が空なら MakeProbeInitialWorld(ProbeSimOptions::initialWorld と同じ)
        explicit ProbeReference(const BakedReactionTable& table, std::span<const reaction::RxCell> initialWorld = {});

        // 刻み tick を 1 つ進める(targetTick == tick のコマンドを並びの順に適用 → 伝導と反応)
        void Advance(uint64_t tick, std::span<const ProbeCommand> commands);

        // 刻み tick の始めの状態 S(tick)(= tick 回進めた後)
        [[nodiscard]] std::span<const reaction::RxCell> State(uint64_t tick) const;

        // 同じく、熱のキャッシュ(抽出の温度を作るのに使う)
        [[nodiscard]] std::span<const reaction::HcThermalCache> Caches(uint64_t tick) const;

        // 最後の Advance のつつきが足したエネルギー(mJ。GPU の表の sourceEnergy と同じ)
        [[nodiscard]] uint64_t SourceEnergy() const { return m_sourceEnergy; }

        // 最後の Advance で GPU が伝導と反応を計算するはずのブロックの数
        [[nodiscard]] uint32_t ScheduledBlocks() const { return m_scheduledBlocks; }

        // 最後の Advance で、待ちが来て起きたブロック(前の刻みに計算しなかったのに評価が要る。GPU の WakeDueBlocks が一覧に足す数)
        [[nodiscard]] uint32_t WokenBlocks() const { return m_wokenBlocks; }

        // 最後の Advance の、ブロックごとの結果(PROBE_BLOCK_COUNT 個の PROBE_BLOCK_FLAG_* の組み合わせ。0 でなければ次の刻みの予定の種。
        // トレースの予想に使う。sim/probe_trace.h)
        [[nodiscard]] std::span<const uint8_t> BlockFlags() const { return m_blockFlags; }

    private:
        const BakedReactionTable* m_table;
        std::vector<reaction::RxCell> m_cells;           // 2 世代 × PROBE_CELL_COUNT(GPU と同じ並び)
        std::vector<reaction::HcThermalCache> m_caches;  // 同じ並びの熱のキャッシュ
        std::vector<uint8_t> m_blockFlags;               // 前の刻みのブロックごとの結果(PROBE_BLOCK_COUNT)
        std::vector<uint64_t> m_changedMarks;  // ブロックが最後に変わった刻みの印(tc。待ちの丸め。PROBE_BLOCK_COUNT)
        std::vector<uint8_t> m_scheduled;      // 最後の Advance で予定したブロック(PROBE_BLOCK_COUNT)
        uint64_t m_sourceEnergy = 0;
        uint32_t m_scheduledBlocks = 0;
        uint32_t m_wokenBlocks = 0;
    };

    // ブロックごとの初めの起こす刻みの印(待ちの丸め。ADR-0018・T-0122): 刻み 0 を tc = 0 で計算してみて、変わるブロックは刻み 0 の印、
    // 変わらないブロックはセルの次に評価の要る刻みの最小(刻み 0 を計算しなくても同じ結果になる)
    [[nodiscard]] std::vector<uint64_t> ProbeInitialBlockWakes(const BakedReactionTable& table,
                                                               std::span<const reaction::RxCell> cells);

    // 予定の印のバッファ(PROBE_SCHEDULE_BYTES)の初めの中身: 予定と tc は 0、起こす刻みは ProbeInitialBlockWakes
    [[nodiscard]] std::vector<uint32_t> ProbeInitialScheduleWords(const BakedReactionTable& table,
                                                                  std::span<const reaction::RxCell> cells);

    // 状態のハッシュ = Σ ProbeCellHash(セルの番号, セル)(mod 2^64)。GPU のハッシュの単位と同じ値になる
    [[nodiscard]] uint64_t ProbeStateHash(std::span<const reaction::RxCell> cells);

    // エネルギーの合計(Σ mJ の mod 2^64。GPU の表のエネルギーの合計と同じ値になる)
    [[nodiscard]] uint64_t ProbeEnergySum(std::span<const reaction::RxCell> cells);

    // 抽出のセルの部分(PROBE_CELL_COUNT × PROBE_EXTRACTION_CELL_WORDS 語: 温度と見る物質 3 つ)を CPU で作る。GPU の Extract と同じ値
    [[nodiscard]] std::vector<uint32_t> MakeProbeExtractionCells(
        std::span<const reaction::RxCell> cells, std::span<const reaction::HcThermalCache> caches,
        const std::array<uint32_t, PROBE_VIEW_SPECIES_COUNT>& viewSpecies);

    // 抽出の語の要約(比べるため。順番に依存する FNV 風の混ぜ方)
    [[nodiscard]] uint64_t ProbeExtractionHash(std::span<const uint32_t> words);

}  // namespace bicameral::sim
