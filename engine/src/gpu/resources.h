// resources.h — バッファ・ルート署名・パイプライン・シェーダーのファイルを作る小さな関数(T-0013)。
// 失敗したら理由をログ(Channel::Gpu)に出し、nullptr か std::unexpected を返す。
// シェーダーはビルドが bin/shaders/<sim|render>/*.cso に置く(shaders/CMakeLists.txt)。exe の横から読む。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bicameral::gpu {

    enum class BufferKind : uint8_t {
        UnorderedAccess,  // GPU だけが読み書きする(既定のヒープ)。COMMON で作るので、最初の使用で UAV に暗黙に昇格する
        Readback,         // GPU → CPU の読み戻し。COPY_DEST のまま使う
    };

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, uint64_t sizeBytes,
                                                                      BufferKind kind);

    // u0 から uavCount 個の「ルートの UAV」を持つルート署名。記述子のヒープが要らない一番簡単な形。
    // rootConstantCount > 0 なら、その後ろ(ルートの番号 uavCount)に b0 のルート定数(32bit × rootConstantCount)を足す
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateRootUavSignature(ID3D12Device* device,
                                                                                     uint32_t uavCount,
                                                                                     uint32_t rootConstantCount = 0);

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12PipelineState> CreateComputePipeline(
        ID3D12Device* device, ID3D12RootSignature* rootSignature, std::span<const std::byte> bytecode);

    // exe の横の shaders/<relativePath> を読む。relativePath の例: "sim/fixed_selftest.cso"
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> LoadShader(std::string_view relativePath);

    // 読み戻しのバッファの先頭から destination の大きさだけ写す
    [[nodiscard]] bool ReadBuffer(ID3D12Resource* readback, std::span<std::byte> destination);

    // UAV で書いたバッファを読み戻しのバッファへ写すコマンドを記録する(UAV → COPY_SOURCE の遷移つき)
    void RecordCopyToReadback(ID3D12GraphicsCommandList* list, ID3D12Resource* source, ID3D12Resource* readback);

}  // namespace bicameral::gpu
