// gpu_physics.h — 整数の AVBD の 1 刻み(08 §2)を GPU の Compute で走らせる(T-0090)。
// パスは shaders/sim/physics_step.hlsl、物ごと・組ごとの手順は shaders/common/physics_step.hlsli(CPU の sim::PhysicsWorld と共通)。
// CPU の世界とビット一致することを tests/gpu_physics_test.cpp が確かめる。Work Graphs 版と測って比べるのは T-0092。
//
// 使い方:
//   auto physics = GpuPhysics::Create(device, scene, PxDefaultParameters());
//   physics->RecordInitialize(list, debugRingAddress);       // 初めの 1 回
//   physics->RecordStep(list, debugRingAddress);             // 1 刻み(何刻みでも続けて記録してよい)
//   physics->RecordReadback(list);  → 投げて待つ →  physics->ReadBodies() / ReadStats()
// 計測(T-0092): EnableProfiling の後は、パスごとにタイムスタンプを打ち、RecordReadback で読み戻しに入れる。
//   リストが終わってから AccumulateProfile → ProfileResult でパスの種類ごとの GPU 時間。
// debugRingAddress は debug のビルドでシェーダーの FX_ASSERT が書くデバッグのリング(gpu::DebugRing。必ず結ぶ)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "gpu/work_graph.h"
#include "sim/physics_world.h"

namespace bicameral::sim {

    // 色ごとの解き方(T-0092 で測って Compute にした。ADR-0002「計測」)
    enum class GpuPhysicsSolver : uint8_t {
        Compute,  // 色ごとに Dispatch、1 グループ = 1 物、1 スレッド = 1 接触点(physics_step.hlsl の SolveColor)
        Graph,    // 同じ解き方を、色ごとの物の一覧を入力にした Work Graph で(physics_graph.hlsl の SolveBodyNode)
    };

    // 数は固定(M1 の原理の確認。足りなければ GpuPhysicsStats::overflow に印が付く)
    struct GpuPhysicsOptions {
        uint32_t slotsPerBody = 16;     // 持ち主の物 1 つあたりの組の枠(PX_GPU_MAX_SLOTS_PER_BODY まで)
        uint32_t incidentPerBody = 24;  // 動く物 1 つあたりの、入っている組の数
        uint32_t colorRounds = 24;      // 彩色(Jones-Plassmann)の回数

        // 広域の選別 → 接触の幾何を Work Graph(physics_graph.hlsl)で走らせる。false なら Compute の 2 パス(T-0092。ADR-0002「計測」)
        bool broadphaseGraph = true;
        GpuPhysicsSolver solver = GpuPhysicsSolver::Compute;
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

    // パスの種類ごとの GPU 時間の合計(計測。T-0092)。時間はタイムスタンプの刻み(キューの GetTimestampFrequency で割る。
    // シミュのコードに浮動小数点を置かないため、ミリ秒にするのは呼ぶ側)
    struct GpuPhysicsPassTime {
        const char* name = "";
        uint64_t count = 0;  // 投げた回数
        uint64_t timestampTicks = 0;
    };

    class GpuPhysics {
    public:
        [[nodiscard]] static std::expected<GpuPhysics, std::string> Create(ID3D12Device5* device,
                                                                           const PhysicsScene& scene,
                                                                           const physics::PxParameters& parameters,
                                                                           const GpuPhysicsOptions& options = {});

        void RecordInitialize(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing);
        void RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing);
        void RecordReadback(ID3D12GraphicsCommandList10* list) const;

        // RecordReadback を含むリストが終わってから
        [[nodiscard]] std::vector<PhysicsBody> ReadBodies() const;
        [[nodiscard]] GpuPhysicsStats ReadStats() const;

        [[nodiscard]] uint64_t Tick() const { return m_tick; }

        // --- 計測(T-0092)---
        [[nodiscard]] bool EnableProfiling(ID3D12Device* device);
        void AccumulateProfile();
        [[nodiscard]] std::vector<GpuPhysicsPassTime> ProfileResult() const;

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
            BuildManifolds,   // Work Graph の後で組を作る(broadphaseGraph のとき)
            BroadphaseGraph,  // Work Graph(Pipeline は無い。m_graph)
            SolveColorGraph,  // Work Graph(GpuPhysicsSolver::Graph)
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
            uint32_t solveEntry = 0;
            uint32_t colorBodiesLow = 0;
            uint32_t colorBodiesHigh = 0;
        };

        [[nodiscard]] std::expected<void, std::string> CreatePipelines(ID3D12Device5* device);
        [[nodiscard]] std::expected<void, std::string> CreateBuffers(ID3D12Device5* device, const PhysicsScene& scene);
        void RecordSubstep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint32_t substep);
        void RecordColoring(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                            Constants constants);
        void RecordIterations(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                              Constants constants);
        void RecordPass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, Pass pass,
                        const Constants& constants, uint32_t threadCount);
        void RecordSolveColor(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                              const Constants& constants);
        void RecordSolveColorGraph(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                   const Constants& constants, int32_t color);
        void RecordColorListStates(ID3D12GraphicsCommandList10* list, bool toGraphInput) const;
        void SetGraphProgram(ID3D12GraphicsCommandList10* list);
        void RecordBroadphaseGraph(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                   const Constants& constants);
        void BindRoot(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                      const Constants& constants) const;
        void RecordProfileStart(ID3D12GraphicsCommandList10* list) const;
        void RecordTimestamp(ID3D12GraphicsCommandList10* list, Pass pass);
        [[nodiscard]] Constants BaseConstants() const;

        physics::PxParameters m_parameters{};
        GpuPhysicsOptions m_options;
        uint32_t m_bodyCount = 0;
        uint32_t m_frictionQ16 = 0;
        uint64_t m_tick = 0;
        uint32_t m_half = 1;  // 今の小刻みの組の枠(小刻みの初めに入れ替える)

        ComPtr<ID3D12RootSignature> m_rootSignature;
        std::array<ComPtr<ID3D12PipelineState>, (size_t)Pass::Count> m_pipelines;  // BroadphaseGraph は空
        std::unique_ptr<gpu::WorkGraph> m_graph;  // broadphaseGraph か solver == Graph のときだけ(physics_graph.hlsl)
        uint32_t m_broadphaseEntry = 0;
        uint32_t m_solveEntry = 0;
        bool m_graphInitialized = false;                // 裏のメモリを初期化する SetProgram を記録したか
        bool m_graphProgramSet = false;                 // 今のリストの状態がグラフか(Compute のパスが PSO に戻す)
        std::array<ComPtr<ID3D12Resource>, 13> m_uavs;  // u0〜u12(physics_bindings.hlsli の結び付け)
        std::array<ComPtr<ID3D12Resource>, 2> m_srvs;   // t0 パラメータ・t1 初めの物
        ComPtr<ID3D12Resource> m_bodiesReadback;
        ComPtr<ID3D12Resource> m_statsReadback;

        // --- 計測(T-0092)---
        ComPtr<ID3D12QueryHeap> m_timestamps;  // [0] リストの始め、[1 + n] n 番目のパスの後
        ComPtr<ID3D12Resource> m_timestampReadback;
        std::vector<Pass> m_profiledPasses;  // 今のリストで打ったパスの順
        std::array<uint64_t, (size_t)Pass::Count> m_profileCounts{};
        std::array<uint64_t, (size_t)Pass::Count> m_profileTicks{};
    };

}  // namespace bicameral::sim
