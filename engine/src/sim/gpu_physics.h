// gpu_physics.h — 整数の AVBD の 1 刻み(08 §2)を GPU の Compute で走らせる(T-0090)。
// パスは shaders/sim/physics_step.hlsl、物ごと・組ごとの手順は shaders/common/physics_step.hlsli(CPU の sim::PhysicsWorld と共通)。
// CPU の世界とビット一致することを tests/gpu_physics_test.cpp が確かめる。Work Graphs 版と測って比べるのは T-0092。
//
// 使い方:
//   auto physics = GpuPhysics::Create(device, scene, PxDefaultParameters());
//   physics->RecordInitialize(list, debugRingAddress);       // 初めの 1 回
//   physics->RecordStep(list, debugRingAddress);             // 1 刻み(何刻みでも続けて記録してよい)
//   physics->RecordReadback(list);  → 投げて待つ →  physics->ReadBodies() / ReadStats()
// debugRingAddress は debug のビルドでシェーダーの FX_ASSERT が書くデバッグのリング(gpu::DebugRing。必ず結ぶ)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "sim/physics_world.h"

namespace bicameral::sim {

    // 数は固定(M1 の原理の確認。足りなければ GpuPhysicsStats::overflow に印が付く)
    struct GpuPhysicsOptions {
        uint32_t slotsPerBody = 16;     // 持ち主の物 1 つあたりの組の枠
        uint32_t incidentPerBody = 24;  // 動く物 1 つあたりの、入っている組の数
        uint32_t colorRounds = 24;      // 彩色(Jones-Plassmann)の回数
    };

    // shaders/sim/physics_step.hlsl の統計の並び(g_stats)
    struct GpuPhysicsStats {
        int64_t maxPenetration = 0;  // 2^-20 m
        uint32_t contactCount = 0;
        uint32_t colorCount = 0;
        uint32_t solveFailures = 0;
        uint32_t overflow = 0;  // 1 枠 / 2 一覧 / 4 塗れなかった物 / 8 色が多すぎる。0 でなければ結果は信用できない
    };
    static_assert(sizeof(GpuPhysicsStats) == 24);

    class GpuPhysics {
    public:
        [[nodiscard]] static std::expected<GpuPhysics, std::string> Create(ID3D12Device* device,
                                                                           const PhysicsScene& scene,
                                                                           const physics::PxParameters& parameters,
                                                                           const GpuPhysicsOptions& options = {});

        void RecordInitialize(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing);
        void RecordStep(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing);
        void RecordReadback(ID3D12GraphicsCommandList* list) const;

        // RecordReadback を含むリストが終わってから
        [[nodiscard]] std::vector<PhysicsBody> ReadBodies() const;
        [[nodiscard]] GpuPhysicsStats ReadStats() const;

        [[nodiscard]] uint64_t Tick() const { return m_tick; }

        // 物の状態のハッシュ(PhysicsWorld::StateHash と同じ式)
        [[nodiscard]] static uint64_t StateHash(uint64_t tick, const std::vector<PhysicsBody>& bodies);

    private:
        // shaders/sim/physics_step.hlsl の入口(.cso の名前と同じ順)
        enum class Pass : uint8_t {
            Initialize,
            BeginSubstep,
            Broadphase,
            Narrowphase,
            PrepareManifolds,
            ColorRound,
            FinishColoring,
            Recollide,
            SolveColor,
            UpdateDuals,
            UpdateVelocity,
            Finish,
            Count,
        };

        // b0 のルート定数(physics_step.hlsl の RootConstants と同じ順)
        struct Constants {
            uint32_t bodyCount = 0;
            uint32_t slotsPerBody = 0;
            uint32_t incidentPerBody = 0;
            uint32_t currentHalf = 0;
            uint32_t tickLow = 0;
            uint32_t tickHigh = 0;
            uint32_t frictionQ16 = 0;
            int32_t color = 0;
            uint32_t alphaQ16 = 0;
            uint32_t colorIn = 0;
            uint32_t resetStats = 0;
        };

        void RecordSubstep(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint32_t substep);
        void RecordColoring(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, Constants constants);
        void RecordIterations(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                              Constants constants);
        void RecordPass(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, Pass pass,
                        const Constants& constants, uint32_t threadCount) const;
        [[nodiscard]] Constants BaseConstants() const;

        physics::PxParameters m_parameters{};
        GpuPhysicsOptions m_options;
        uint32_t m_bodyCount = 0;
        uint32_t m_frictionQ16 = 0;
        uint64_t m_tick = 0;
        uint32_t m_half = 1;  // 今の小刻みの組の枠(小刻みの初めに入れ替える)

        ComPtr<ID3D12RootSignature> m_rootSignature;
        std::array<ComPtr<ID3D12PipelineState>, (size_t)Pass::Count> m_pipelines;
        std::array<ComPtr<ID3D12Resource>, 10> m_uavs;  // u0〜u9(physics_step.hlsl の結び付け)
        std::array<ComPtr<ID3D12Resource>, 2> m_srvs;   // t0 パラメータ・t1 初めの物
        ComPtr<ID3D12Resource> m_bodiesReadback;
        ComPtr<ID3D12Resource> m_statsReadback;
    };

}  // namespace bicameral::sim
