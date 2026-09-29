// resources.cpp — バッファ・ルート署名・パイプライン・シェーダーのファイル(T-0013)。
#include "gpu/resources.h"

#include "core/hresult.h"
#include "core/log.h"
#include "core/unicode.h"

using Microsoft::WRL::ComPtr;

namespace bicameral::gpu {
    namespace {

        std::filesystem::path ExecutableDirectory() {
            std::wstring path(MAX_PATH, L'\0');
            for (;;) {
                const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
                if (length < path.size()) {
                    path.resize(length);
                    break;
                }
                path.resize(path.size() * 2);  // 長いパスでは切り詰められるので広げて取り直す
            }
            return std::filesystem::path(path).parent_path();
        }

    }  // namespace

    // --- バッファ ---

    ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, uint64_t sizeBytes, BufferKind kind) {
        const bool isReadback = kind == BufferKind::Readback;
        const D3D12_HEAP_PROPERTIES heap{.Type = isReadback ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC desc{
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = sizeBytes,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = DXGI_FORMAT_UNKNOWN,
            .SampleDesc = {.Count = 1},
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            .Flags = isReadback ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        };
        const D3D12_RESOURCE_STATES state = isReadback ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
        ComPtr<ID3D12Resource> buffer;
        if (!BICAMERAL_CHECK_HR(Channel::Gpu, device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                                              nullptr, IID_PPV_ARGS(&buffer)))) {
            return nullptr;
        }
        return buffer;
    }

    bool ReadBuffer(ID3D12Resource* readback, std::span<std::byte> destination) {
        const D3D12_RANGE readRange{.Begin = 0, .End = destination.size()};
        void* mapped = nullptr;
        if (!BICAMERAL_CHECK_HR(Channel::Gpu, readback->Map(0, &readRange, &mapped))) return false;
        std::memcpy(destination.data(), mapped, destination.size());
        const D3D12_RANGE writtenRange{};  // CPU は書いていない
        readback->Unmap(0, &writtenRange);
        return true;
    }

    void RecordCopyToReadback(ID3D12GraphicsCommandList* list, ID3D12Resource* source, ID3D12Resource* readback) {
        const D3D12_RESOURCE_BARRIER barrier{
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Transition = {.pResource = source,
                           .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE},
        };
        list->ResourceBarrier(1, &barrier);
        list->CopyResource(readback, source);
    }

    // --- ルート署名とパイプライン ---

    ComPtr<ID3D12RootSignature> CreateRootUavSignature(ID3D12Device* device, uint32_t uavCount) {
        std::vector<D3D12_ROOT_PARAMETER1> parameters(uavCount);
        for (uint32_t index = 0; index < uavCount; ++index) {
            parameters[index] = {.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV,
                                 .Descriptor = {.ShaderRegister = index,
                                                .RegisterSpace = 0,
                                                .Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE},
                                 .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL};
        }
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{.Version = D3D_ROOT_SIGNATURE_VERSION_1_1};
        desc.Desc_1_1 = {.NumParameters = uavCount, .pParameters = parameters.data()};

        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> error;
        if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error))) {
            const std::string_view message = error ? static_cast<const char*>(error->GetBufferPointer()) : "";
            Log(Channel::Gpu, Level::Error, "ルート署名をシリアライズできない: {}", message);
            return nullptr;
        }
        ComPtr<ID3D12RootSignature> rootSignature;
        if (!BICAMERAL_CHECK_HR(Channel::Gpu,
                                device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                            IID_PPV_ARGS(&rootSignature)))) {
            return nullptr;
        }
        return rootSignature;
    }

    ComPtr<ID3D12PipelineState> CreateComputePipeline(ID3D12Device* device, ID3D12RootSignature* rootSignature,
                                                      std::span<const std::byte> bytecode) {
        const D3D12_COMPUTE_PIPELINE_STATE_DESC desc{
            .pRootSignature = rootSignature,
            .CS = {.pShaderBytecode = bytecode.data(), .BytecodeLength = bytecode.size()},
        };
        ComPtr<ID3D12PipelineState> pipeline;
        if (!BICAMERAL_CHECK_HR(Channel::Gpu, device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)))) {
            return nullptr;
        }
        return pipeline;
    }

    // --- シェーダーのファイル ---

    std::expected<std::vector<std::byte>, std::string> LoadShader(std::string_view relativePath) {
        const std::filesystem::path path = ExecutableDirectory() / "shaders" / ToWide(relativePath);
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return std::unexpected("シェーダーを開けない: " + ToUtf8(path.wstring()));
        std::vector<std::byte> bytes(static_cast<size_t>(file.tellg()));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) return std::unexpected("シェーダーを読めない: " + ToUtf8(path.wstring()));
        return bytes;
    }

}  // namespace bicameral::gpu
