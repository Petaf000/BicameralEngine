// pipeline_cache.h — 同じデバイスで同じシェーダーのパイプラインを何度も作る所(試験の場面ごとの作り直し)で、
// 作ったルート署名・Compute のパイプライン・Work Graph のプログラム(状態オブジェクト)を使い回す(T-0427)。
// ドライバのシェーダーのキャッシュに当たらない時、多重解像度の Compute のパイプラインの作成は debug で 1 回 約 55 秒かかった
// (docs/perf.md)。使い回すのはコンパイルの結果だけで、裏のメモリ・バッファ・状態は使う側がそれぞれ持つので、計算の結果は変わらない。
//
//   gpu::PipelineCache cache;   // デバイスより先に消す(持っているパイプラインがデバイスを参照する)
//   auto pipeline = cache.ComputePipeline(root, "sim/x.cso", [&] { return CreateX(...); });
//
// 1 つのキャッシュは 1 つのデバイスだけで使う。同時に複数のスレッドから呼ばない。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "gpu/com_ptr.h"

namespace bicameral::gpu {

    class PipelineCache {
    public:
        template <class T>
        using Creator = std::function<std::expected<ComPtr<T>, std::string>()>;

        // name(呼ぶ側が決める名前)のルート署名。無ければ create で作って覚える(失敗は覚えない)
        [[nodiscard]] std::expected<ComPtr<ID3D12RootSignature>, std::string> RootSignature(
            std::string_view name, const Creator<ID3D12RootSignature>& create);

        // ルート署名とシェーダーの .cso(LoadShader の相対パス)の組の Compute のパイプライン
        [[nodiscard]] std::expected<ComPtr<ID3D12PipelineState>, std::string> ComputePipeline(
            ID3D12RootSignature* rootSignature, std::string_view shaderPath,
            const Creator<ID3D12PipelineState>& create);

        // ルート署名・DXIL ライブラリの .cso・プログラム名の組の Work Graph の状態オブジェクト
        [[nodiscard]] std::expected<ComPtr<ID3D12StateObject>, std::string> WorkGraphProgram(
            ID3D12RootSignature* rootSignature, std::string_view libraryPath, std::wstring_view programName,
            const Creator<ID3D12StateObject>& create);

        // 使い回した回数と作った回数(試験のログ用)
        [[nodiscard]] uint32_t HitCount() const { return m_hitCount; }
        [[nodiscard]] uint32_t MissCount() const { return m_missCount; }

    private:
        template <class T>
        std::expected<ComPtr<T>, std::string> FindOrCreate(std::map<std::string, ComPtr<T>, std::less<>>& entries,
                                                           const std::string& key, const Creator<T>& create);

        // --- 覚えたもの(キーはルート署名のアドレス + パス + 名前)---
        std::map<std::string, ComPtr<ID3D12RootSignature>, std::less<>> m_rootSignatures;
        std::map<std::string, ComPtr<ID3D12PipelineState>, std::less<>> m_computePipelines;
        std::map<std::string, ComPtr<ID3D12StateObject>, std::less<>> m_workGraphPrograms;

        // --- 数 ---
        uint32_t m_hitCount = 0;
        uint32_t m_missCount = 0;
    };

}  // namespace bicameral::gpu
