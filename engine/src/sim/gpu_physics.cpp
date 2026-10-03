// gpu_physics.cpp — 整数の AVBD の 1 刻みを GPU の Compute で走らせる(gpu_physics.h)。
// パスを呼ぶ順番は CPU の PhysicsWorld::Substep と同じ。パスの間は全部の UAV の書き込みを待つ(グローバルな UAV バリア)。
// 色の数と彩色の回数は GPU の中で決まるので、読み戻さずに固定の回数(PX_GPU_MAX_COLORS・colorRounds)だけ投げる
// (空の色・塗る物の無い回は何もしない)。回数を詰めるのは T-0092(Work Graphs・間接の起動)。
#include "sim/gpu_physics.h"

#include "gpu/resources.h"

namespace bicameral::sim {

    namespace {

        using namespace bicameral::physics;

        constexpr uint32_t THREADS_PER_GROUP = 64;  // physics_step.hlsl の numthreads
        constexpr uint32_t ROOT_CONSTANT_COUNT = 14;
        constexpr uint32_t STATS_BYTES = sizeof(GpuPhysicsStats);

        // u0〜u13 / b0 / デバッグのリング / t0〜t1(ルート署名の大きさ 48 / 64 語)
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = 14, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = 2};

        constexpr uint32_t COLOR_PREDICATE_UAV = 13;  // u13 色ごとの「その色の物がいる」(述語。T-0094)

        // shaders/CMakeLists.txt で入口ごとに作る .cso(Pass の順)
        constexpr std::array<const char*, 13> SHADER_NAMES = {
            "sim/physics_initialize.cso",      "sim/physics_begin_substep.cso",     "sim/physics_broadphase.cso",
            "sim/physics_narrowphase.cso",     "sim/physics_prepare_manifolds.cso", "sim/physics_color_round.cso",
            "sim/physics_finish_coloring.cso", "sim/physics_recollide.cso",         "sim/physics_solve_color.cso",
            "sim/physics_update_duals.cso",    "sim/physics_update_velocity.cso",   "sim/physics_finish.cso",
            "sim/physics_build_manifolds.cso"};

        // 計測の表示名(Pass の順)
        constexpr std::array<const char*, 15> PASS_NAMES = {
            "Initialize",     "BeginSubstep",   "Broadphase",     "Narrowphase",     "PrepareManifolds",
            "ColorRound",     "FinishColoring", "Recollide",      "SolveColor",      "UpdateDuals",
            "UpdateVelocity", "Finish",         "BuildManifolds", "BroadphaseGraph", "SolveColorGraph"};

        constexpr auto COLOR_COUNT = (uint64_t)PX_GPU_MAX_COLORS;

        // physics_bindings.hlsli の COLOR_INPUT_BYTES(色ごとの GPU の入力の間隔)
        static_assert(sizeof(D3D12_NODE_GPU_INPUT) == 24);

        // physics_graph.hlsl の BroadphaseNode の NumThreads と NodeMaxDispatchGrid
        constexpr uint32_t BROADPHASE_THREADS = 16;
        constexpr uint32_t BROADPHASE_MAX_GROUPS = 1024;

        // 1 つのリストで打てるタイムスタンプの数(60 刻み × 約 230 パス + 余り)
        constexpr uint32_t MAX_TIMESTAMPS = 1u << 16;

        // physics_step.hlsl の ManifoldHeader(組の点を除いた部分)
        constexpr uint64_t MANIFOLD_HEADER_BYTES = 80;

        template <typename T>
        ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, std::span<const T> data) {
            const auto bytes = std::as_bytes(data);
            ComPtr<ID3D12Resource> buffer = gpu::CreateBuffer(device, bytes.size(), gpu::BufferKind::Upload);
            void* mapped = nullptr;
            const D3D12_RANGE noRead{.Begin = 0, .End = 0};
            if (!buffer || FAILED(buffer->Map(0, &noRead, &mapped)))
                return nullptr;

            std::memcpy(mapped, bytes.data(), bytes.size());
            buffer->Unmap(0, nullptr);

            return buffer;
        }

        uint32_t GroupsFor(uint32_t threadCount) {
            return (threadCount + THREADS_PER_GROUP - 1) / THREADS_PER_GROUP;
        }

    }  // namespace

    std::expected<GpuPhysics, std::string> GpuPhysics::Create(ID3D12Device5* device, const PhysicsScene& scene,
                                                              const PxParameters& parameters,
                                                              const GpuPhysicsOptions& options) {
        GpuPhysics result;
        result.m_parameters = parameters;
        result.m_options = options;
        result.m_bodyCount = (uint32_t)scene.bodies.size();
        result.m_frictionQ16 = scene.frictionQ16;

        if (options.slotsPerBody > PX_GPU_MAX_SLOTS_PER_BODY)
            return std::unexpected(std::format("slotsPerBody は {} まで", PX_GPU_MAX_SLOTS_PER_BODY));

        if (auto created = result.CreatePipelines(device); !created)
            return std::unexpected(created.error());

        if (auto created = result.CreateBuffers(device, scene); !created)
            return std::unexpected(created.error());

        return result;
    }

    // ルート署名・Compute のパス・Work Graph(使うときだけ)
    std::expected<void, std::string> GpuPhysics::CreatePipelines(ID3D12Device5* device) {
        m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!m_rootSignature)
            return std::unexpected("物理のルート署名を作れない");

        for (size_t pass = 0; pass < SHADER_NAMES.size(); ++pass) {
            const auto bytecode = gpu::LoadShader(SHADER_NAMES[pass]);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            m_pipelines[pass] = gpu::CreateComputePipeline(device, m_rootSignature.Get(), *bytecode);
            if (!m_pipelines[pass])
                return std::unexpected(std::format("パイプラインを作れない: {}", SHADER_NAMES[pass]));
        }

        if (m_options.broadphaseGraph || m_options.solver == GpuPhysicsSolver::Graph) {
            if ((m_bodyCount + BROADPHASE_THREADS - 1) / BROADPHASE_THREADS > BROADPHASE_MAX_GROUPS)
                return std::unexpected("物が多すぎる(広域の選別の Work Graph の NodeMaxDispatchGrid)");

            const auto library = gpu::LoadShader("sim/physics_graph.cso");
            if (!library)
                return std::unexpected(library.error());

            auto graph = gpu::WorkGraph::Create(device, m_rootSignature.Get(), *library, L"PhysicsBroadphase");
            if (!graph)
                return std::unexpected(graph.error());

            m_graph = std::make_unique<gpu::WorkGraph>(std::move(*graph));
            m_broadphaseEntry = m_graph->EntrypointIndex(L"BroadphaseNode");
            m_solveEntry = m_graph->EntrypointIndex(L"SolveBodyNode");
            if (m_broadphaseEntry == UINT32_MAX || m_solveEntry == UINT32_MAX)
                return std::unexpected("physics_graph.cso に BroadphaseNode か SolveBodyNode が無い");
        }

        return {};
    }

    // バッファ(physics_bindings.hlsli の結び付けの順)・アップロード・読み戻し
    std::expected<void, std::string> GpuPhysics::CreateBuffers(ID3D12Device5* device, const PhysicsScene& scene) {
        const uint64_t n = m_bodyCount;
        const uint64_t slots = n * m_options.slotsPerBody;
        const std::array<uint64_t, 14> uavBytes = {
            n * sizeof(PhysicsBody),                                       // u0 物
            2 * slots * MANIFOLD_HEADER_BYTES,                             // u1 組の見出し
            2 * n * sizeof(uint32_t),                                      // u2 持ち主の枠の数
            slots * sizeof(uint32_t),                                      // u3 枠の相手
            n * m_options.incidentPerBody * sizeof(uint32_t),              // u4 物の組の一覧
            n * sizeof(uint32_t),                                          // u5 一覧の数
            n * sizeof(uint64_t),                                          // u6 触れている質量
            2 * n * sizeof(int32_t),                                       // u7 彩色の回ごとの色
            STATS_BYTES,                                                   // u8 統計
            2 * slots * PX_MANIFOLD_POINTS * sizeof(PhysicsContactPoint),  // u9 組の点
            slots * sizeof(PxContactGeometry),  // u10 接触の幾何(GPU だけが読み書き。HLSL の間隔は C++ の大きさ以下)
            COLOR_COUNT * sizeof(D3D12_NODE_GPU_INPUT),  // u11 色ごとの GPU の入力
            COLOR_COUNT * n * sizeof(uint32_t),          // u12 色ごとの物の一覧
            COLOR_COUNT * sizeof(uint64_t)};             // u13 その色の物がいる(述語)
        for (size_t i = 0; i < uavBytes.size(); ++i) {
            m_uavs[i] = gpu::CreateBuffer(device, uavBytes[i], gpu::BufferKind::UnorderedAccess);
            if (!m_uavs[i])
                return std::unexpected("物理のバッファを作れない");
        }

        std::vector<PhysicsBody> initialBodies;
        initialBodies.reserve(scene.bodies.size());
        const int64_t rate = PxStepRate(m_parameters);
        for (const PhysicsSceneBody& source : scene.bodies)
            initialBodies.push_back(MakePhysicsBody(source, rate));

        const std::array<PxParameters, 1> parameterArray = {m_parameters};
        m_srvs = {CreateUploadBuffer(device, std::span<const PxParameters>(parameterArray)),
                  CreateUploadBuffer(device, std::span<const PhysicsBody>(initialBodies))};
        m_bodiesReadback = gpu::CreateBuffer(device, uavBytes[0], gpu::BufferKind::Readback);
        m_statsReadback = gpu::CreateBuffer(device, STATS_BYTES, gpu::BufferKind::Readback);
        if (!m_srvs[0] || !m_srvs[1] || !m_bodiesReadback || !m_statsReadback)
            return std::unexpected("物理のアップロード・読み戻しのバッファを作れない");

        return {};
    }

    GpuPhysics::Constants GpuPhysics::BaseConstants() const {
        return {.bodyCount = m_bodyCount,
                .slotsPerBody = m_options.slotsPerBody,
                .incidentPerBody = m_options.incidentPerBody,
                .currentHalf = m_half,
                .tickLow = (uint32_t)m_tick,
                .tickHigh = (uint32_t)(m_tick >> 32),
                .frictionQ16 = m_frictionQ16,
                .solveEntry = m_solveEntry,
                .colorBodiesLow = (uint32_t)m_uavs[12]->GetGPUVirtualAddress(),
                .colorBodiesHigh = (uint32_t)(m_uavs[12]->GetGPUVirtualAddress() >> 32)};
    }

    // 1 つのパスを投げ、全部の UAV の書き込みを次のパスより前に終わらせる
    void GpuPhysics::RecordPass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, Pass pass,
                                const Constants& constants, uint32_t threadCount) {
        RecordProfileStart(list);
        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_pipelines[(size_t)pass].Get());
        m_graphProgramSet = false;
        BindRoot(list, debugRing, constants);
        list->Dispatch(GroupsFor(threadCount), 1, 1);
        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
        RecordTimestamp(list, pass);
    }

    // 広域の選別 → 接触の生成の Work Graph(Broadphase・Narrowphase の 2 パスの代わり)。CPU の入力 1 件 = グループの数
    void GpuPhysics::RecordBroadphaseGraph(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                           const Constants& constants) {
        RecordProfileStart(list);
        SetGraphProgram(list);
        BindRoot(list, debugRing, constants);

        const uint32_t groupCount = (m_bodyCount + BROADPHASE_THREADS - 1) / BROADPHASE_THREADS;
        gpu::WorkGraph::DispatchFromCpu(list, m_broadphaseEntry, &groupCount, 1, sizeof(groupCount));
        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
        RecordTimestamp(list, Pass::BroadphaseGraph);
    }

    // 色 color の物の一覧(FinishColoring が作った)を GPU の入力にして、その色の物だけを 1 グループずつ解く
    void GpuPhysics::RecordSolveColorGraph(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                           const Constants& constants, int32_t color) {
        RecordProfileStart(list);
        SetGraphProgram(list);
        BindRoot(list, debugRing, constants);

        const D3D12_GPU_VIRTUAL_ADDRESS input = m_uavs[11]->GetGPUVirtualAddress() +
                                                (uint64_t)color * sizeof(D3D12_NODE_GPU_INPUT);
        gpu::WorkGraph::DispatchFromGpu(list, input);
        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
        RecordTimestamp(list, Pass::SolveColorGraph);
    }

    // 色ごとの GPU の入力と物の一覧は、DispatchGraph が読む間 NON_PIXEL_SHADER_RESOURCE(仕様の D3D12_NODE_GPU_INPUT)。
    // 反復の前に UAV から移し、反復の後で戻す(FinishColoring と次の小刻みの BeginSubstep が UAV で書く)
    void GpuPhysics::RecordColorListStates(ID3D12GraphicsCommandList10* list, bool toGraphInput) const {
        const D3D12_RESOURCE_STATES uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const D3D12_RESOURCE_STATES input = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const D3D12_RESOURCE_STATES before = toGraphInput ? uav : input;
        const D3D12_RESOURCE_STATES after = toGraphInput ? input : uav;
        const std::array<D3D12_RESOURCE_BARRIER, 2> barriers = {gpu::Transition(m_uavs[11].Get(), before, after),
                                                                gpu::Transition(m_uavs[12].Get(), before, after)};
        list->ResourceBarrier((uint32_t)barriers.size(), barriers.data());
    }

    // グラフを設定する(Compute のパスの後だけ)。裏のメモリの初期化は最初の 1 回だけ(リストは記録した順に実行される)。
    // 呼んだ後でルートの引数を結ぶ
    void GpuPhysics::SetGraphProgram(ID3D12GraphicsCommandList10* list) {
        if (m_graphProgramSet)
            return;

        list->SetComputeRootSignature(m_rootSignature.Get());
        m_graph->SetProgram(list, !m_graphInitialized);
        m_graphInitialized = true;
        m_graphProgramSet = true;
    }

    // ルートの引数(u0〜u13・b0・デバッグのリング・t0〜t1)
    void GpuPhysics::BindRoot(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                              const Constants& constants) const {
        static_assert(sizeof(Constants) == ROOT_CONSTANT_COUNT * sizeof(uint32_t));
        static_assert(std::tuple_size_v<decltype(m_uavs)> == ROOT_LAYOUT.uavCount);
        for (uint32_t i = 0; i < m_uavs.size(); ++i)
            list->SetComputeRootUnorderedAccessView(i, m_uavs[i]->GetGPUVirtualAddress());

        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &constants, 0);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing);
        for (uint32_t i = 0; i < m_srvs.size(); ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i), m_srvs[i]->GetGPUVirtualAddress());
    }

    // --- 計測(T-0092)---

    bool GpuPhysics::EnableProfiling(ID3D12Device* device) {
        const D3D12_QUERY_HEAP_DESC queryDesc{
            .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = MAX_TIMESTAMPS + 1, .NodeMask = 0};
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestamps))))
            return false;

        m_timestampReadback = gpu::CreateBuffer(device, (uint64_t)(MAX_TIMESTAMPS + 1) * sizeof(uint64_t),
                                                gpu::BufferKind::Readback);

        return m_timestampReadback != nullptr;
    }

    // リストの最初のパスの前に 1 個
    void GpuPhysics::RecordProfileStart(ID3D12GraphicsCommandList10* list) const {
        if (m_timestamps && m_profiledPasses.empty())
            list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    }

    // パスの後ろ(UAV バリアの後)に打つ
    void GpuPhysics::RecordTimestamp(ID3D12GraphicsCommandList10* list, Pass pass) {
        if (!m_timestamps || m_profiledPasses.size() >= MAX_TIMESTAMPS)
            return;

        m_profiledPasses.push_back(pass);
        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (uint32_t)m_profiledPasses.size());
    }

    // RecordReadback を含むリストが終わってから。打ったパスの時間を種類ごとに足す
    void GpuPhysics::AccumulateProfile() {
        if (!m_timestamps || m_profiledPasses.empty())
            return;

        std::vector<uint64_t> ticks(m_profiledPasses.size() + 1);
        if (!gpu::ReadBuffer(m_timestampReadback.Get(), std::as_writable_bytes(std::span(ticks))))
            return;

        for (size_t n = 0; n < m_profiledPasses.size(); ++n) {
            const auto pass = (size_t)m_profiledPasses[n];
            m_profileCounts[pass] += 1;
            m_profileTicks[pass] += ticks[n + 1] - ticks[n];
        }

        m_profiledPasses.clear();
    }

    std::vector<GpuPhysicsPassTime> GpuPhysics::ProfileResult() const {
        static_assert(PASS_NAMES.size() == (size_t)Pass::Count);
        static_assert(SHADER_NAMES.size() + 2 ==
                      (size_t)Pass::Count);  // BroadphaseGraph・SolveColorGraph は Work Graph

        std::vector<GpuPhysicsPassTime> result;
        for (size_t pass = 0; pass < PASS_NAMES.size(); ++pass) {
            if (m_profileCounts[pass] > 0)
                result.push_back(
                    {.name = PASS_NAMES[pass], .count = m_profileCounts[pass], .timestampTicks = m_profileTicks[pass]});
        }

        return result;
    }

    void GpuPhysics::RecordInitialize(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) {
        m_tick = 0;
        m_half = 1;
        RecordPass(list, debugRing, Pass::Initialize, BaseConstants(), m_bodyCount);
    }

    void GpuPhysics::RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) {
        m_graphProgramSet = false;  // リストが変わったかもしれない
        for (uint32_t substep = 0; substep < m_parameters.substeps; ++substep)
            RecordSubstep(list, debugRing, substep);

        ++m_tick;
    }

    // PhysicsWorld::Substep と同じ順
    void GpuPhysics::RecordSubstep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                   uint32_t substep) {
        m_half = 1 - m_half;
        Constants constants = BaseConstants();
        const uint32_t slotCount = m_bodyCount * m_options.slotsPerBody;

        constants.resetStats = substep == 0 ? 1 : 0;
        RecordPass(list, debugRing, Pass::BeginSubstep, constants, m_bodyCount);
        constants.resetStats = 0;

        if (m_graph) {
            RecordBroadphaseGraph(list, debugRing, constants);
            RecordPass(list, debugRing, Pass::BuildManifolds, constants, slotCount);
        } else {
            RecordPass(list, debugRing, Pass::Broadphase, constants, m_bodyCount);
            RecordPass(list, debugRing, Pass::Narrowphase, constants, slotCount);
        }

        RecordPass(list, debugRing, Pass::PrepareManifolds, constants, slotCount);
        RecordColoring(list, debugRing, constants);
        if (SkipsEmptyColors())
            RecordColorPredicateStates(list, true);  // FinishColoring が書いた「その色の物がいる」を述語に

        RecordIterations(list, debugRing, constants);
        if (SkipsEmptyColors())
            RecordColorPredicateStates(list, false);

        RecordPass(list, debugRing, Pass::Finish, constants, m_bodyCount);
    }

    // 彩色: 回ごとに色の組を入れ替え、最後の回の結果を物に移す
    void GpuPhysics::RecordColoring(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                    Constants constants) {
        for (uint32_t colorRound = 0; colorRound < m_options.colorRounds; ++colorRound) {
            constants.colorIn = colorRound % 2;
            RecordPass(list, debugRing, Pass::ColorRound, constants, m_bodyCount);
        }

        constants.colorIn = m_options.colorRounds % 2;
        RecordPass(list, debugRing, Pass::FinishColoring, constants, m_bodyCount);
    }

    // 本反復は α = 1、最後の 1 回は α = 0。途中で接触を探し直し、最後の本反復の後で速度を決める
    void GpuPhysics::RecordIterations(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                      Constants constants) {
        const uint32_t slotCount = m_bodyCount * m_options.slotsPerBody;
        const uint32_t iterations = m_parameters.iterations;
        const bool graph = m_options.solver == GpuPhysicsSolver::Graph;
        if (graph)
            RecordColorListStates(list, true);

        for (uint32_t iteration = 0; iteration <= iterations; ++iteration) {
            if (iteration == m_parameters.recollideIteration)
                RecordPass(list, debugRing, Pass::Recollide, constants, slotCount);

            constants.alphaQ16 = iteration < iterations ? (uint32_t)PX_ALPHA_ONE_Q16 : 0;
            RecordSolveColors(list, debugRing, constants);

            if (iteration < iterations)
                RecordPass(list, debugRing, Pass::UpdateDuals, constants, slotCount);

            if (iteration + 1 == iterations)
                RecordPass(list, debugRing, Pass::UpdateVelocity, constants, m_bodyCount);
        }

        if (graph)
            RecordColorListStates(list, false);
    }

    // 1 回の反復の色ごとの解。skipEmptyColors なら「その色の物がいる」(u13)の述語つき。
    // 述語をかけるときの色ごとの解は Compute(述語と DispatchGraph は組み合わせない。SkipsEmptyColors)
    void GpuPhysics::RecordSolveColors(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                       Constants constants) {
        const bool colorPredicates = SkipsEmptyColors();
        for (int32_t color = 0; color < PX_GPU_MAX_COLORS; ++color) {
            constants.color = color;
            if (colorPredicates) {
                list->SetPredication(m_uavs[COLOR_PREDICATE_UAV].Get(), (uint64_t)color * sizeof(uint64_t),
                                     D3D12_PREDICATION_OP_EQUAL_ZERO);  // 0 ならその色の解を飛ばす
            }

            if (m_options.solver == GpuPhysicsSolver::Graph)
                RecordSolveColorGraph(list, debugRing, constants, color);
            else
                RecordPass(list, debugRing, Pass::SolveColor, constants,
                           m_bodyCount * THREADS_PER_GROUP);  // 1 グループ = 1 物
        }

        if (colorPredicates)
            list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);  // 戻さないと後ろのパスが黙って飛ぶ
    }

    // 述語のバッファ(u13)の状態: BeginSubstep が UAV で 0 にし、FinishColoring が UAV で書く → 反復の間だけ PREDICATION →
    // 小刻みの終わりに UAV へ戻す(リストの終わりで COMMON に戻り、次のリストの BeginSubstep で UAV に昇格する)
    void GpuPhysics::RecordColorPredicateStates(ID3D12GraphicsCommandList10* list, bool toPredication) const {
        const D3D12_RESOURCE_STATES unordered = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const D3D12_RESOURCE_STATES predication = D3D12_RESOURCE_STATE_PREDICATION;
        const D3D12_RESOURCE_BARRIER barrier = gpu::Transition(m_uavs[COLOR_PREDICATE_UAV].Get(),
                                                               toPredication ? unordered : predication,
                                                               toPredication ? predication : unordered);
        list->ResourceBarrier(1, &barrier);
    }

    void GpuPhysics::RecordReadback(ID3D12GraphicsCommandList10* list) const {
        gpu::RecordCopyToReadback(list, m_uavs[0].Get(), m_bodiesReadback.Get());
        gpu::RecordCopyToReadback(list, m_uavs[8].Get(), m_statsReadback.Get());
        if (m_timestamps && !m_profiledPasses.empty()) {
            list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,
                                   (uint32_t)m_profiledPasses.size() + 1, m_timestampReadback.Get(), 0);
        }
    }

    std::vector<PhysicsBody> GpuPhysics::ReadBodies() const {
        std::vector<PhysicsBody> bodies(m_bodyCount);
        if (!gpu::ReadBuffer(m_bodiesReadback.Get(), std::as_writable_bytes(std::span(bodies))))
            bodies.clear();

        return bodies;
    }

    GpuPhysicsStats GpuPhysics::ReadStats() const {
        GpuPhysicsStats stats;
        if (!gpu::ReadBuffer(m_statsReadback.Get(), std::as_writable_bytes(std::span(&stats, 1))))
            stats.overflow = 0xFFFFFFFFu;

        return stats;
    }

    uint64_t GpuPhysics::StateHash(uint64_t tick, const std::vector<PhysicsBody>& bodies) {
        uint64_t hash = fx::FxMix64(tick);
        for (const PhysicsBody& body : bodies)
            hash = PxHashBody(hash, body);

        return hash;
    }

}  // namespace bicameral::sim
