// debug_ring.h — シェーダーの printf / assert のリング(shaders/common/debug_ring.hlsli)を CPU 側で持つ(T-0003、16 §1)。
//
// データの流れ: シェーダーが DEBUG_PRINT / DEBUG_ASSERT でリングに書く → RecordReadbackAndReset() が読み戻しのバッファへ写して
//   リングを空にする → GPU が終わった後に Drain() が書式(shaders/common/debug_formats.hlsli)に当てはめてログ(ADR-0006)へ出す。
//
// 1 本のコマンドリストの中での使い方:
//   ring.RecordBegin(list);                                        // リングを UAV にする
//   list->SetComputeRootUnorderedAccessView(layout.DebugRingIndex(), ring.GpuAddress());
//   ... Dispatch / DispatchGraph ...
//   ring.RecordReadbackAndReset(list);                             // 読み戻して空にする(リストの終わりで COMMON に戻る)
//   queue.ExecuteAndWait();  const auto contents = ring.Drain();
// 読み戻しのバッファは 1 つなので、1 度に 1 本のリストだけで使う(フレームを重ねるときは T-0004 でフレームごとに持たせる)。
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/debug_ring.hlsli"
#include "core/log.h"

namespace bicameral::gpu {

    // 読み戻した 1 レコード
    struct DebugMessage {
        DebugFormat format = DebugFormat::Count;  // 知らない番号なら Count
        bool isAssert = false;
        uint32_t line = 0;               // シェーダーのソースの行(__LINE__)
        Channel channel = Channel::Gpu;  // 書式の一覧のチャンネル
        std::string_view where;          // 書式の一覧の「場所」(ファイル名かノード名)
        std::string text;                // 書式に引数を当てはめたもの
    };

    // 1 レコード(16 語)を読む。知らない書式の番号や、書式と引数が合わないときも、生の値を並べた文字列にする
    [[nodiscard]] DebugMessage DecodeDebugRecord(std::span<const uint32_t, DEBUG_RECORD_WORDS> record);

    struct DebugRingContents {
        uint32_t requestedCount = 0;         // シェーダーが書こうとした数
        uint32_t droppedCount = 0;           // 容量(DEBUG_RING_CAPACITY)を超えて書けなかった数
        uint32_t assertCount = 0;            // 読めたレコードの中の assert の数
        std::vector<DebugMessage> messages;  // 並びは atomic で空きを取った順(毎回同じとは限らない)
    };

    class DebugRing {
    public:
        // 1 回の Drain() でログに出す行の数の既定。残りは件数だけ出す(リングは毎フレーム数千件になりうる)
        static constexpr uint32_t DEFAULT_MAX_LOGGED_MESSAGES = 32;

        [[nodiscard]] static std::expected<DebugRing, std::string> Create(ID3D12Device* device);

        // ルートの UAV(u0 space1)に渡すアドレス
        [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS GpuAddress() const { return m_ring->GetGPUVirtualAddress(); }

        // シェーダーが書く前に: COMMON → UNORDERED_ACCESS
        void RecordBegin(ID3D12GraphicsCommandList* list) const;

        // シェーダーが書いた後に: 読み戻しのバッファへ写し、見出し(書こうとした数)を 0 に戻す。最後は COMMON に戻す
        void RecordReadbackAndReset(ID3D12GraphicsCommandList* list) const;

        // GPU が RecordReadbackAndReset() まで終えた後に呼ぶ。読んだものをログに出し(最大 maxLoggedMessages 行)、返す
        DebugRingContents Drain(uint32_t maxLoggedMessages = DEFAULT_MAX_LOGGED_MESSAGES) const;

    private:
        DebugRing() = default;

        Microsoft::WRL::ComPtr<ID3D12Resource> m_ring;      // シェーダーが書く(既定のヒープ。作った時は 0)
        Microsoft::WRL::ComPtr<ID3D12Resource> m_zeros;     // 見出しを 0 に戻すための写し元(0 のまま使う)
        Microsoft::WRL::ComPtr<ID3D12Resource> m_readback;  // CPU が読む
    };

}  // namespace bicameral::gpu
