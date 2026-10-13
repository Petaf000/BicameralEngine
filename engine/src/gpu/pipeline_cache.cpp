// pipeline_cache.cpp — 作ったパイプラインを名前で覚えて使い回す(T-0427)。作り方は呼ぶ側が渡す(このファイルは覚えるだけ)。
#include "gpu/pipeline_cache.h"

#include <format>

#include "core/unicode.h"

namespace bicameral::gpu {

    template <class T>
    std::expected<ComPtr<T>, std::string> PipelineCache::FindOrCreate(
        std::map<std::string, ComPtr<T>, std::less<>>& entries, const std::string& key, const Creator<T>& create) {
        if (const auto found = entries.find(key); found != entries.end()) {
            ++m_hitCount;
            return found->second;
        }

        auto created = create();
        if (!created)
            return std::unexpected(created.error());

        if (!*created)
            return std::unexpected(std::format("パイプラインを作れない({})", key));

        ++m_missCount;
        entries.emplace(key, *created);

        return created;
    }

    std::expected<ComPtr<ID3D12RootSignature>, std::string> PipelineCache::RootSignature(
        std::string_view name, const Creator<ID3D12RootSignature>& create) {
        return FindOrCreate(m_rootSignatures, std::string(name), create);
    }

    std::expected<ComPtr<ID3D12PipelineState>, std::string> PipelineCache::ComputePipeline(
        ID3D12RootSignature* rootSignature, std::string_view shaderPath, const Creator<ID3D12PipelineState>& create) {
        const std::string key = std::format("{}|{}", static_cast<const void*>(rootSignature), shaderPath);
        return FindOrCreate(m_computePipelines, key, create);
    }

    std::expected<ComPtr<ID3D12StateObject>, std::string> PipelineCache::WorkGraphProgram(
        ID3D12RootSignature* rootSignature, std::string_view libraryPath, std::wstring_view programName,
        const Creator<ID3D12StateObject>& create) {
        const std::string key = std::format("{}|{}|{}", static_cast<const void*>(rootSignature), libraryPath,
                                            ToUtf8(programName));
        return FindOrCreate(m_workGraphPrograms, key, create);
    }

}  // namespace bicameral::gpu
