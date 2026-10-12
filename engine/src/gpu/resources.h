// resources.h — バッファ・ルート署名・パイプライン・シェーダーのファイルを作る小さな関数(T-0013)。
// 失敗したら理由をログ(Channel::Gpu)に出し、nullptr か unexpected を返す。
// シェーダーはビルドが bin/shaders/<sim|render>/*.cso に置く(shaders/CMakeLists.txt)。exe の横から読む。
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

    enum class BufferKind : uint8_t {
        UnorderedAccess,  // GPU だけが読み書きする(既定のヒープ)。COMMON で作るので、最初の使用で UAV に暗黙に昇格する
        Readback,         // GPU → CPU の読み戻し。COPY_DEST のまま使う
        Upload,           // CPU → GPU(アップロードのヒープ)。GENERIC_READ のまま使い、CPU は Map したまま書く(T-0004)
    };

    [[nodiscard]] ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, uint64_t sizeBytes, BufferKind kind);

    // 「ルートの UAV」とルート定数だけのルート署名の形。記述子のヒープが要らない一番簡単な形。
    // ルートの番号は並び順: u0..u(uavCount-1)(space0)→ b0 のルート定数(あれば)→ デバッグのリング(あれば)
    //   → Work Graphs のカウンタ(あれば)→ 連鎖のトレース(あれば)→ t0..t(srvCount-1)(space0。ルートの SRV。T-0004)
    // 見える範囲は全部のステージ(compute でも描画でも同じ形を使う)。
    struct RootSignatureLayout {
        uint32_t uavCount = 0;           // space0 の u0 から
        uint32_t rootConstantCount = 0;  // b0 の 32bit の値の数。0 なら無し
        bool debugRing = false;          // u0 space1 のデバッグのリング(shaders/common/debug_ring.hlsli、T-0003)
        bool graphStats = false;  // u1 space1 の Work Graphs のカウンタ(shaders/common/work_graph_stats.hlsli、T-0008)
        bool graphTrace = false;  // u2 space1 の連鎖のトレース(shaders/common/graph_trace.hlsli、T-0087)
        uint32_t srvCount = 0;    // space0 の t0 から(バッファだけ)

        [[nodiscard]] uint32_t RootConstantIndex() const { return uavCount; }
        [[nodiscard]] uint32_t DebugRingIndex() const { return uavCount + (rootConstantCount > 0 ? 1 : 0); }
        [[nodiscard]] uint32_t GraphStatsIndex() const { return DebugRingIndex() + (debugRing ? 1 : 0); }
        [[nodiscard]] uint32_t GraphTraceIndex() const { return GraphStatsIndex() + (graphStats ? 1 : 0); }
        [[nodiscard]] uint32_t SrvIndex(uint32_t shaderRegister) const {
            return GraphTraceIndex() + (graphTrace ? 1 : 0) + shaderRegister;
        }
    };

    [[nodiscard]] ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device,
                                                                  const RootSignatureLayout& layout);

    [[nodiscard]] ComPtr<ID3D12PipelineState> CreateComputePipeline(ID3D12Device* device,
                                                                    ID3D12RootSignature* rootSignature,
                                                                    std::span<const std::byte> bytecode);

    // exe の横の shaders/<relativePath> を読む。relativePath の例: "sim/fixed_selftest.cso"
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> LoadShader(std::string_view relativePath);

    // 読み戻しのバッファの先頭から destination の大きさだけ写す
    [[nodiscard]] bool ReadBuffer(ID3D12Resource* readback, std::span<std::byte> destination);

    // リソース全体の状態の遷移
    [[nodiscard]] D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                                    D3D12_RESOURCE_STATES after);

    // UAV の書き込みを次の読み書きより前に終わらせる
    [[nodiscard]] D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource);

    // UAV で書いたバッファを読み戻しのバッファへ写すコマンドを記録する(UAV → COPY_SOURCE の遷移つき)
    void RecordCopyToReadback(ID3D12GraphicsCommandList* list, ID3D12Resource* source, ID3D12Resource* readback);

    // --- シェーダーが毎段読む変わらない表(T-0410)---
    // アップロードのヒープは CPU のメモリで、シェーダーが直接読むと毎回 PCIe 越しになる(L2 に収まらない大きさや、追い出される負荷で、
    // セルの数に対して超線形に遅くなる。T-0374 で 32³ が約 30 倍)。変わらない表は既定のヒープの写しを読む。
    // 毎フレーム CPU が書く小さな定数(フレームの定数・トレースの絞り込み・覗き窓の入力)はアップロードのヒープのままでよい。
    struct ResidentBuffer {
        ComPtr<ID3D12Resource> buffer;  // 既定のヒープ(シェーダーが読む。COMMON で作る)
        ComPtr<ID3D12Resource> upload;  // 写しの元(写すリストが GPU で終わるまで持つ)

        [[nodiscard]] bool Valid() const { return buffer && upload; }
    };

    // bytes で埋めたアップロードのバッファを作る(作れなければ nullptr)
    [[nodiscard]] ComPtr<ID3D12Resource> CreateFilledUpload(ID3D12Device* device, std::span<const std::byte> bytes);

    // bytes を持つ表の写しの組を作る。中身が既定のヒープへ入るのは RecordResidentUploads を記録したリストが走った後
    [[nodiscard]] ResidentBuffer CreateResidentBuffer(ID3D12Device* device, std::span<const std::byte> bytes);

    // 写しを記録し、シェーダーが読める状態(NON_PIXEL_SHADER_RESOURCE)にする。表は COMMON にある前提
    //(作った直後か、前のリストの終わりで COMMON に戻った後。同じリストで読んだ後に写し直さない)
    void RecordResidentUploads(ID3D12GraphicsCommandList* list, std::span<const ResidentBuffer> buffers);

}  // namespace bicameral::gpu
