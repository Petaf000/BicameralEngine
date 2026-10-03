// gpu_physics_islands.cpp — 物理の 1 小刻みを「島ごとに 1 グループ」で解く方式の記録(T-0094。gpu_physics.h の GpuPhysicsIslands)。
// シェーダーは shaders/sim/physics_islands.hlsli(FindIslands・SolveIslands)。全体の方式(gpu_physics.cpp の RecordColoring・
// RecordIterations)と同じ順に解くので CPU とビット一致のまま。大きな島(物の数が islandBodyLimit を超える)は全体の方式のパスが解き、
// そのパスは述語(SetPredication)で「大きな島が無ければ」「その色を大きな島の物が使わなければ」飛ばす(CPU は島の大きさを知らない。D-107)。
//
// 述語のバッファ(u14・u15)の状態: BeginSubstep が UAV で 0 にする → FindIslands・FinishColoring が UAV で書く →
// 述語に使う間 PREDICATION → 小刻みの終わりに UAV へ戻す(リストの終わりで COMMON に戻り、次のリストの BeginSubstep で UAV に昇格する)。
#include "sim/gpu_physics.h"

#include "gpu/resources.h"

namespace bicameral::sim {

    namespace {

        constexpr uint32_t THREADS_PER_GROUP = 64;  // gpu_physics.cpp の RecordPass の 1 グループ
        constexpr uint32_t PREDICATE_LARGE = 14;    // u14 大きな島がある
        constexpr uint32_t PREDICATE_COLORS = 15;   // u15 大きな島の物が使う色

        // physics_bindings.hlsli の島の並びの、物ごとの印の区画(見出し 4 語の後ろ)
        constexpr uint64_t ISLAND_LABEL_OFFSET_BYTES = 4 * sizeof(uint32_t);

    }  // namespace

    // 物の数が上限を超える島がありうるか(無ければ全体の方式のパスを記録しない)
    bool GpuPhysics::HasLargeIslandPath() const {
        return m_options.islandBodyLimit < m_bodyCount;
    }

    // 島分け → (大きな島の彩色)→ 島ごとの解 → (大きな島の反復)。全体の方式のパスは大きな島の物だけを扱う(islandMode = 1)
    void GpuPhysics::RecordIslandSolve(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                       Constants constants) {
        constants.islandMode = 1;
        RecordPass(list, debugRing, Pass::FindIslands, constants, 1);  // 1 グループ

        const bool large = HasLargeIslandPath();
        if (large) {
            RecordPredicateStates(list, PREDICATE_LARGE, true);
            SetPredicate(list, true, PREDICATE_LARGE, 0);
            RecordColoring(list, debugRing, constants);
            list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
            RecordPredicateStates(list, PREDICATE_COLORS, true);
        }

        // 1 グループ = 1 島。島の数は GPU の中で決まるので、上限(物の数)だけ投げ、島の無いグループはすぐ終わる
        RecordPass(list, debugRing, Pass::SolveIslands, constants, m_bodyCount * THREADS_PER_GROUP);

        if (!large)
            return;

        RecordIterations(list, debugRing, constants, true);
        list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        RecordPredicateStates(list, PREDICATE_LARGE, false);
        RecordPredicateStates(list, PREDICATE_COLORS, false);
    }

    void GpuPhysics::RecordPredicateStates(ID3D12GraphicsCommandList10* list, uint32_t uav, bool toPredication) const {
        const D3D12_RESOURCE_STATES unordered = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const D3D12_RESOURCE_STATES predication = D3D12_RESOURCE_STATE_PREDICATION;
        const D3D12_RESOURCE_BARRIER barrier = gpu::Transition(
            m_uavs[uav].Get(), toPredication ? unordered : predication, toPredication ? predication : unordered);
        list->ResourceBarrier(1, &barrier);
    }

    // 次のパスから、uav の offset の 64bit が 0 ならそのパスを飛ばす(predicated = false なら何もしない)
    void GpuPhysics::SetPredicate(ID3D12GraphicsCommandList10* list, bool predicated, uint32_t uav,
                                  uint64_t offset) const {
        if (predicated)
            list->SetPredication(m_uavs[uav].Get(), offset, D3D12_PREDICATION_OP_EQUAL_ZERO);
    }

    std::vector<uint32_t> GpuPhysics::ReadIslandLabels() const {
        if (!m_islandsReadback)
            return {};

        std::vector<uint32_t> words(ISLAND_LABEL_OFFSET_BYTES / sizeof(uint32_t) + m_bodyCount);
        if (!gpu::ReadBuffer(m_islandsReadback.Get(), std::as_writable_bytes(std::span(words))))
            return {};

        return {words.begin() + ISLAND_LABEL_OFFSET_BYTES / sizeof(uint32_t), words.end()};
    }

}  // namespace bicameral::sim
