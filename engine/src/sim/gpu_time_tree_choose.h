// gpu_time_tree_choose.h — 時間を飛ばす試作(sim/time_step_tree)の刻みを選ぶ段の GPU 版(T-1112。D-669 の窓・隣の差 1 段)。
// CPU リファレンスは time_step_tree.cpp の WindowCaps・ChooseExponents(既定の形: 単位はセル・窓 2 m・退く道/緩衝/揃える相手なし)。
// 持つもの: 木の形の表(TimeTreeChooseTables: 窓の格子の単位・隣。既定のヒープ)・出来事ごとの選ぶ前の詰めた記録(2 B/単位。
//   試験の間は CPU が書いた記録を並べて写す。本物の GPU の早送りでは前の出来事の後の記録がそのまま次の入力)・作業場(格子の 2 面・
//   単位の上限と今の e)・出来事の後の詰めた記録。シェーダーは shaders/sim/time_tree_choose.hlsl。
// 1 出来事 = 初め → 窓(置く・軸ごとの最小 × 3・集める)→ 掃く × MAX_SWEEPS(前の回に変わった単位が無ければ空回り)→ 詰める。
// データの流れ: CPU の記録(試験)→ 写し(既定のヒープ)→ この段 → 窓の上限・今の e・後の記録 → 読み戻し(試験)。
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "gpu/resources.h"
#include "sim/time_step_tree.h"

namespace bicameral::sim {

    // 1 出来事の GPU の答え(読み戻し)
    struct GpuTimeTreeChoice {
        std::vector<uint32_t> windowCaps;  // 単位ごと
        std::vector<int32_t> next;         // 単位ごと(-1 = 歩の途中のまま)
        std::vector<uint16_t> after;       // 単位ごと
        uint32_t sweeps = 0;               // 変わった単位があった掃く回の数
        bool settled = false;              // 最後の掃く回に変わった単位が無い(不動点に着いた)
    };

    class GpuTimeTreeChoose {
    public:
        static constexpr uint32_t MAX_SWEEPS = 128;  // time_tree_choose.hlsl の TTC_MAX_PASSES
        // 初めのこの回数だけはいつも積み、残りは「最後の回に変わった単位があった」時だけ走らせる(述語〔SetPredication〕で
        // 飛ばす。空の Dispatch と壁が 1 回約 1 µs で、128 回では 1 出来事の大半になった。打ち切りが連なる出来事だけ 59 回まで要る)
        static constexpr uint32_t HEAD_SWEEPS = 4;
        static constexpr uint32_t MAX_TIMESTAMPS = 1024;

        // befores = 出来事ごとの選ぶ前の詰めた記録を並べたもの(出来事 × units)。predicate = false なら残りの回も述語なしで全部積む(比べの測り)
        [[nodiscard]] static std::expected<GpuTimeTreeChoose, std::string> Create(ID3D12Device5* device,
                                                                                  const TimeTreeChooseTables& tables,
                                                                                  std::span<const uint16_t> befores,
                                                                                  bool predicate = true);

        // 表と記録の写し(最初のリストの初め。バッファは COMMON の時)
        void RecordUpload(ID3D12GraphicsCommandList* list) const;

        // 出来事 event(befores の何番目か)の選ぶ段を積む。cap = その刻みで始めてよい最も長い e
        void RecordChoose(ID3D12GraphicsCommandList* list, uint64_t debugRing, uint32_t event, int32_t cap) const;

        void RecordReadback(ID3D12GraphicsCommandList* list) const;
        [[nodiscard]] bool Read(GpuTimeTreeChoice& choice) const;

        // --- 時刻印(GPU の時計) ---
        void RecordTimestamp(ID3D12GraphicsCommandList* list, uint32_t index) const;
        void RecordTimestampReadback(ID3D12GraphicsCommandList* list, uint32_t count) const;
        [[nodiscard]] std::vector<uint64_t> ReadTimestamps(uint32_t count) const;

        [[nodiscard]] uint32_t Units() const { return m_units; }
        [[nodiscard]] uint64_t DeviceBytes() const;

    private:
        GpuTimeTreeChoose() = default;

        void Bind(ID3D12GraphicsCommandList* list, uint64_t debugRing) const;
        void RecordPredicate(ID3D12GraphicsCommandList* list) const;
        void Dispatch(ID3D12GraphicsCommandList* list, uint32_t pipeline, uint32_t threads, uint32_t pass,
                      uint32_t event, int32_t cap) const;

        uint32_t m_units = 0;
        uint32_t m_gridCells = 0;
        uint32_t m_wordsPerEvent = 0;
        uint32_t m_events = 0;
        bool m_usePredicate = true;
        TimeTreeChooseTables m_shape;  // 大きさだけ(表の中身は写しの中)

        ComPtr<ID3D12RootSignature> m_rootSignature;
        std::vector<ComPtr<ID3D12PipelineState>> m_pipelines;  // 段の順(Init・Scatter・MinAxis・Gather・Sweep・Pack)
        gpu::ResidentBuffer m_before;
        gpu::ResidentBuffer m_gridUnits;
        gpu::ResidentBuffer m_neighborStarts;
        gpu::ResidentBuffer m_neighbors;
        ComPtr<ID3D12Resource> m_caps;
        ComPtr<ID3D12Resource> m_state;
        ComPtr<ID3D12Resource> m_after;
        ComPtr<ID3D12Resource> m_counters;
        ComPtr<ID3D12Resource> m_predicate;  // 初めの回の最後の印の写し(64bit。0 なら残りを飛ばす)
        ComPtr<ID3D12Resource> m_windowA;
        ComPtr<ID3D12Resource> m_windowB;
        ComPtr<ID3D12Resource> m_capsReadback;
        ComPtr<ID3D12Resource> m_stateReadback;
        ComPtr<ID3D12Resource> m_afterReadback;
        ComPtr<ID3D12Resource> m_countersReadback;
        ComPtr<ID3D12QueryHeap> m_timestamps;
        ComPtr<ID3D12Resource> m_timestampReadback;
    };

}  // namespace bicameral::sim
