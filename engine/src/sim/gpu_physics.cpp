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
        constexpr uint32_t ROOT_CONSTANT_COUNT = 11;
        constexpr uint32_t STATS_BYTES = sizeof(GpuPhysicsStats);

        // u0〜u9 / b0 / デバッグのリング / t0〜t1
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = 10, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = 2};

        // shaders/CMakeLists.txt で入口ごとに作る .cso(Pass の順)
        constexpr std::array<const char*, 12> SHADER_NAMES = {
            "sim/physics_initialize.cso",      "sim/physics_begin_substep.cso",     "sim/physics_broadphase.cso",
            "sim/physics_narrowphase.cso",     "sim/physics_prepare_manifolds.cso", "sim/physics_color_round.cso",
            "sim/physics_finish_coloring.cso", "sim/physics_recollide.cso",         "sim/physics_solve_color.cso",
            "sim/physics_update_duals.cso",    "sim/physics_update_velocity.cso",   "sim/physics_finish.cso"};

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

    std::expected<GpuPhysics, std::string> GpuPhysics::Create(ID3D12Device* device, const PhysicsScene& scene,
                                                              const PxParameters& parameters,
                                                              const GpuPhysicsOptions& options) {
        GpuPhysics result;
        result.m_parameters = parameters;
        result.m_options = options;
        result.m_bodyCount = (uint32_t)scene.bodies.size();
        result.m_frictionQ16 = scene.frictionQ16;

        // --- パイプライン ---
        result.m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!result.m_rootSignature)
            return std::unexpected("物理のルート署名を作れない");

        for (size_t pass = 0; pass < SHADER_NAMES.size(); ++pass) {
            const auto bytecode = gpu::LoadShader(SHADER_NAMES[pass]);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            result.m_pipelines[pass] = gpu::CreateComputePipeline(device, result.m_rootSignature.Get(), *bytecode);
            if (!result.m_pipelines[pass])
                return std::unexpected(std::format("パイプラインを作れない: {}", SHADER_NAMES[pass]));
        }

        // --- バッファ(physics_step.hlsl の結び付けの順)---
        const uint64_t n = result.m_bodyCount;
        const uint64_t slots = n * options.slotsPerBody;
        const std::array<uint64_t, 10> uavBytes = {
            n * sizeof(PhysicsBody),                                        // u0 物
            2 * slots * MANIFOLD_HEADER_BYTES,                              // u1 組の見出し
            2 * n * sizeof(uint32_t),                                       // u2 持ち主の枠の数
            slots * sizeof(uint32_t),                                       // u3 枠の相手
            n * options.incidentPerBody * sizeof(uint32_t),                 // u4 物の組の一覧
            n * sizeof(uint32_t),                                           // u5 一覧の数
            n * sizeof(uint64_t),                                           // u6 触れている質量
            2 * n * sizeof(int32_t),                                        // u7 彩色の回ごとの色
            STATS_BYTES,                                                    // u8 統計
            2 * slots * PX_MANIFOLD_POINTS * sizeof(PhysicsContactPoint)};  // u9 組の点
        for (size_t i = 0; i < uavBytes.size(); ++i) {
            result.m_uavs[i] = gpu::CreateBuffer(device, uavBytes[i], gpu::BufferKind::UnorderedAccess);
            if (!result.m_uavs[i])
                return std::unexpected("物理のバッファを作れない");
        }

        std::vector<PhysicsBody> initialBodies;
        initialBodies.reserve(scene.bodies.size());
        const int64_t rate = PxStepRate(parameters);
        for (const PhysicsSceneBody& source : scene.bodies)
            initialBodies.push_back(MakePhysicsBody(source, rate));

        const std::array<PxParameters, 1> parameterArray = {parameters};
        result.m_srvs = {CreateUploadBuffer(device, std::span<const PxParameters>(parameterArray)),
                         CreateUploadBuffer(device, std::span<const PhysicsBody>(initialBodies))};
        result.m_bodiesReadback = gpu::CreateBuffer(device, uavBytes[0], gpu::BufferKind::Readback);
        result.m_statsReadback = gpu::CreateBuffer(device, STATS_BYTES, gpu::BufferKind::Readback);
        if (!result.m_srvs[0] || !result.m_srvs[1] || !result.m_bodiesReadback || !result.m_statsReadback)
            return std::unexpected("物理のアップロード・読み戻しのバッファを作れない");

        return result;
    }

    GpuPhysics::Constants GpuPhysics::BaseConstants() const {
        return {.bodyCount = m_bodyCount,
                .slotsPerBody = m_options.slotsPerBody,
                .incidentPerBody = m_options.incidentPerBody,
                .currentHalf = m_half,
                .tickLow = (uint32_t)m_tick,
                .tickHigh = (uint32_t)(m_tick >> 32),
                .frictionQ16 = m_frictionQ16};
    }

    // 1 つのパスを投げ、全部の UAV の書き込みを次のパスより前に終わらせる
    void GpuPhysics::RecordPass(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, Pass pass,
                                const Constants& constants, uint32_t threadCount) const {
        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_pipelines[(size_t)pass].Get());
        for (uint32_t i = 0; i < m_uavs.size(); ++i)
            list->SetComputeRootUnorderedAccessView(i, m_uavs[i]->GetGPUVirtualAddress());

        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &constants, 0);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing);
        for (uint32_t i = 0; i < m_srvs.size(); ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i), m_srvs[i]->GetGPUVirtualAddress());

        list->Dispatch(GroupsFor(threadCount), 1, 1);
        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
    }

    void GpuPhysics::RecordInitialize(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) {
        m_tick = 0;
        m_half = 1;
        RecordPass(list, debugRing, Pass::Initialize, BaseConstants(), m_bodyCount);
    }

    void GpuPhysics::RecordStep(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) {
        for (uint32_t substep = 0; substep < m_parameters.substeps; ++substep)
            RecordSubstep(list, debugRing, substep);

        ++m_tick;
    }

    // PhysicsWorld::Substep と同じ順
    void GpuPhysics::RecordSubstep(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                   uint32_t substep) {
        m_half = 1 - m_half;
        Constants constants = BaseConstants();
        const uint32_t slotCount = m_bodyCount * m_options.slotsPerBody;

        constants.resetStats = substep == 0 ? 1 : 0;
        RecordPass(list, debugRing, Pass::BeginSubstep, constants, m_bodyCount);
        constants.resetStats = 0;

        RecordPass(list, debugRing, Pass::Broadphase, constants, m_bodyCount);
        RecordPass(list, debugRing, Pass::Narrowphase, constants, slotCount);
        RecordPass(list, debugRing, Pass::PrepareManifolds, constants, slotCount);
        RecordColoring(list, debugRing, constants);
        RecordIterations(list, debugRing, constants);
        RecordPass(list, debugRing, Pass::Finish, constants, m_bodyCount);
    }

    // 彩色: 回ごとに色の組を入れ替え、最後の回の結果を物に移す
    void GpuPhysics::RecordColoring(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                    Constants constants) {
        for (uint32_t colorRound = 0; colorRound < m_options.colorRounds; ++colorRound) {
            constants.colorIn = colorRound % 2;
            RecordPass(list, debugRing, Pass::ColorRound, constants, m_bodyCount);
        }

        constants.colorIn = m_options.colorRounds % 2;
        RecordPass(list, debugRing, Pass::FinishColoring, constants, m_bodyCount);
    }

    // 本反復は α = 1、最後の 1 回は α = 0。途中で接触を探し直し、最後の本反復の後で速度を決める
    void GpuPhysics::RecordIterations(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                      Constants constants) {
        const uint32_t slotCount = m_bodyCount * m_options.slotsPerBody;
        const uint32_t iterations = m_parameters.iterations;
        for (uint32_t iteration = 0; iteration <= iterations; ++iteration) {
            if (iteration == m_parameters.recollideIteration)
                RecordPass(list, debugRing, Pass::Recollide, constants, slotCount);

            constants.alphaQ16 = iteration < iterations ? (uint32_t)PX_ALPHA_ONE_Q16 : 0;
            for (int32_t color = 0; color < PX_GPU_MAX_COLORS; ++color) {
                constants.color = color;
                RecordPass(list, debugRing, Pass::SolveColor, constants, m_bodyCount);
            }

            if (iteration < iterations)
                RecordPass(list, debugRing, Pass::UpdateDuals, constants, slotCount);

            if (iteration + 1 == iterations)
                RecordPass(list, debugRing, Pass::UpdateVelocity, constants, m_bodyCount);
        }
    }

    void GpuPhysics::RecordReadback(ID3D12GraphicsCommandList* list) const {
        gpu::RecordCopyToReadback(list, m_uavs[0].Get(), m_bodiesReadback.Get());
        gpu::RecordCopyToReadback(list, m_uavs[8].Get(), m_statsReadback.Get());
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
