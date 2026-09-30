// readback_ring.h — GPU が追記するバッファを、CPU が待たずに読む仕組み(T-0004、docs/design/06-simulation-loop.md §3)。
//
// データの流れ:
//   GPU のバッファ(既定のヒープ)の先頭に見出しがあり、[0] が「書こうとした数」。シェーダーは atomic で空きを取って追記する。
//   コマンドリストの終わりに RecordReadbackAndReset(list, slot) が、バッファを丸ごと読み戻しのバッファ[slot] へ写し、見出しを 0 に戻す。
//   CPU は slot のリストが終わったことをフェンスで知ってから Read(slot) する。終わっていなければ読まずに次のフレームへ(待たない)。
// 読み戻しのバッファを slot(フレームやバッチの番号 % 数)ごとに持つので、GPU が次を書いている間に前の分を読める。
// イベント(sim/probe_sim)とシェーダーの printf / assert(gpu/debug_ring)が使う。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gpu/com_ptr.h"

namespace bicameral::gpu {

    class ReadbackRing {
    public:
        // bytes: バッファ全体の大きさ。headerBytes: 見出しの大きさ(RecordReadbackAndReset がここだけ 0 に戻す)
        [[nodiscard]] static std::expected<ReadbackRing, std::string> Create(ID3D12Device* device, uint64_t bytes,
                                                                             uint64_t headerBytes, uint32_t slotCount,
                                                                             std::wstring_view name);

        [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS GpuAddress() const { return m_buffer->GetGPUVirtualAddress(); }
        [[nodiscard]] uint32_t SlotCount() const { return static_cast<uint32_t>(m_readbacks.size()); }
        [[nodiscard]] uint64_t Bytes() const { return m_bytes; }
        [[nodiscard]] ID3D12Resource* Buffer() const {
            return m_buffer.Get();
        }  // シェーダーが書くバッファ(見出しの一部を別に書く人向け)

        // シェーダーが書く前に: COMMON → UNORDERED_ACCESS
        void RecordBegin(ID3D12GraphicsCommandList* list) const;

        // シェーダーが書いた後に: 読み戻しのバッファ[slot] へ先頭から copyBytes(既定は全体)だけ写し、見出しを 0 に戻し、COMMON に戻す
        void RecordReadbackAndReset(ID3D12GraphicsCommandList* list, uint32_t slot,
                                    uint64_t copyBytes = UINT64_MAX) const;

        // slot のリストを GPU が終えた後に呼ぶ。先頭から destination の大きさだけ写す
        [[nodiscard]] bool Read(uint32_t slot, std::span<std::byte> destination) const;

    private:
        ReadbackRing() = default;

        ComPtr<ID3D12Resource> m_buffer;                  // シェーダーが書く(作った時は 0)
        ComPtr<ID3D12Resource> m_zeros;                   // 見出しを 0 に戻すための写し元(0 のまま使う)
        std::vector<ComPtr<ID3D12Resource>> m_readbacks;  // CPU が読む(slot ごと)
        uint64_t m_bytes = 0;
        uint64_t m_headerBytes = 0;
    };

}  // namespace bicameral::gpu
