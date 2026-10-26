// gpu_time_tree_choose.h — 時間を飛ばす試作(sim/time_step_tree)の刻みを選ぶ段の GPU 版(T-1112・T-1213。D-669 の窓・隣の差 1 段)。
// CPU リファレンスは time_step_tree.cpp の WindowCaps・ChooseExponents(既定の形: 単位はセル・窓 2 m・退く道/緩衝/揃える相手なし)。
// 持つもの: 木の形の表(TimeTreeChooseTables: 窓の格子の単位・隣。既定のヒープ)・出来事ごとの選ぶ前の詰めた記録(2 B/単位。
//   試験の間は CPU が書いた記録を並べて写す。本物の GPU の早送りでは前の出来事の後の記録がそのまま次の入力)・作業場(格子の 2 面・
//   単位の上限と今の e)・出来事の後の詰めた記録。シェーダーは shaders/sim/time_tree_choose.hlsl。
// 1 出来事 = 初めと窓の x → 窓の y → 窓の z と集める → 掃く(区画 = TILE_UNITS 単位を 1 グループが groupshared で不動点まで回し、
//   後の記録を詰める)。単位が 1 区画に収まれば 4 Dispatch で終わる(T-1213。前の形は 135 Dispatch・0.107 ms)。区画が 2 つ以上なら
//   掃く回を HEAD_SWEEPS 回(並べて)積み、最後に仕上げ(1 グループが最後の回に変わった区画の隣から 1 区画ずつ不動点まで)を 1 回積む。
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
        uint32_t sweeps = 0;               // 変わった区画があった掃く回の数
        uint32_t fixupTiles = 0;           // 仕上げで回した区画の数
        uint32_t innerSweeps = 0;          // 区画の中で回した回の最大(変わらないと分かった回を含む)
        bool settled = false;              // どの区画も上限の内に回し切った(不動点に着いた)
    };

    class GpuTimeTreeChoose {
    public:
        static constexpr uint32_t
            TILE_UNITS = 4096;  // time_tree_choose.hlsl の TTC_TILE_UNITS(1 グループ 1,024 スレッド)
        static constexpr uint32_t MAX_TILES = 4096;  // time_tree_choose.hlsl の TTC_MAX_TILES(1,677 万単位まで)
        // 区画が 2 つ以上の時に並べて積む掃く回(変わった区画が無ければ空回り)。残りは仕上げの 1 グループが必ず不動点まで回す
        // (T-1213: 前の形は 128 回を積んで述語で飛ばし、飛ばした回の壁だけで 1 出来事 0.086 ms・述語の写し 1 回 4 µs かかった)
        static constexpr uint32_t HEAD_SWEEPS = 3;           // time_tree_choose.hlsl の TTC_HEAD_PASSES
        static constexpr uint32_t PASSES = HEAD_SWEEPS + 1;  // 掃く回 + 仕上げ
        // 回ごと + 回し切れなかった区画 + 区画の中の回の最大 + 仕上げの区画の数
        static constexpr uint32_t COUNTERS = PASSES + 3;
        static constexpr uint32_t MAX_TIMESTAMPS = 1024;

        // befores = 出来事ごとの選ぶ前の詰めた記録を並べたもの(出来事 × units)
        [[nodiscard]] static std::expected<GpuTimeTreeChoose, std::string> Create(ID3D12Device5* device,
                                                                                  const TimeTreeChooseTables& tables,
                                                                                  std::span<const uint16_t> befores);

        // 表と記録の写し(最初のリストの初め。バッファは COMMON の時)
        void RecordUpload(ID3D12GraphicsCommandList* list) const;

        // 出来事 event(befores の何番目か)の選ぶ段を積む。cap = その刻みで始めてよい最も長い e。
        // stamp を渡すと、初めと段のまとまり(PhaseNames の順)の後ごとに時刻印を打って進める(固定費の内訳の測り。T-1213)
        void RecordChoose(ID3D12GraphicsCommandList* list, uint64_t debugRing, uint32_t event, int32_t cap,
                          uint32_t* stamp = nullptr) const;

        // stamp を渡した RecordChoose が時刻印で区切る段のまとまりの名前(時刻印は名前の数 + 1)
        [[nodiscard]] static std::span<const char* const> PhaseNames();

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
        void Dispatch(ID3D12GraphicsCommandList* list, uint32_t pipeline, uint32_t groups, uint32_t pass,
                      uint32_t event, int32_t cap) const;

        uint32_t m_units = 0;
        uint32_t m_gridCells = 0;
        uint32_t m_wordsPerEvent = 0;
        uint32_t m_events = 0;
        uint32_t m_tiles = 0;
        TimeTreeChooseTables m_shape;  // 大きさだけ(表の中身は写しの中)

        ComPtr<ID3D12RootSignature> m_rootSignature;
        std::vector<ComPtr<ID3D12PipelineState>> m_pipelines;  // 段の順(WindowX・WindowY・WindowZ・Sweep・Fixup)
        gpu::ResidentBuffer m_before;
        gpu::ResidentBuffer m_gridUnits;
        gpu::ResidentBuffer m_neighborStarts;
        gpu::ResidentBuffer m_neighbors;
        gpu::ResidentBuffer m_tileNeighborStarts;  // 区画ごとの隣の区画(仕上げが印を付ける)
        gpu::ResidentBuffer m_tileNeighbors;
        ComPtr<ID3D12Resource> m_caps;
        ComPtr<ID3D12Resource> m_state;
        ComPtr<ID3D12Resource> m_after;
        ComPtr<ID3D12Resource> m_counters;
        ComPtr<ID3D12Resource> m_tileChanged;  // 区画ごと: 最後に変わった回 + 1
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
