// work_graph.h — Work Graph のプログラムを作り、コマンドリストに設定して起動する(T-0013)。
// DXIL ライブラリ(lib_6_8)の中の全部のノードを 1 つのグラフにし、グラフが要る裏のメモリ(バッキングメモリ)も持つ。
// ルート署名はグローバルの 1 つだけ(ノードごとのローカルのルート署名は使わない)。
//
//   auto graph = WorkGraph::Create(device, rootSignature, library, L"Probe");
//   list->SetComputeRootSignature(rootSignature);
//   graph->SetProgram(list, true);           // 最初の 1 回は裏のメモリを初期化する
//   list->SetComputeRootUnorderedAccessView(0, ...);
//   graph->DispatchFromCpu(list, graph->EntrypointIndex(L"Root"), &record, 1, sizeof(record));
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace bicameral::gpu {

    class WorkGraph {
    public:
        [[nodiscard]] static std::expected<WorkGraph, std::string> Create(ID3D12Device5* device,
                                                                          ID3D12RootSignature* globalRootSignature,
                                                                          std::span<const std::byte> library,
                                                                          std::wstring_view programName);

        // 入口のノードの番号。名前が無ければ UINT32_MAX
        [[nodiscard]] uint32_t EntrypointIndex(std::wstring_view nodeName) const;

        // グラフをコマンドリストに設定する。initialize は裏のメモリを初期化するか(作ってから最初の 1 回は true)
        void SetProgram(ID3D12GraphicsCommandList10* list, bool initialize) const;

        // CPU が持つレコードを入口のノードへ渡して起動する(D3D12_DISPATCH_MODE_NODE_CPU_INPUT)
        static void DispatchFromCpu(ID3D12GraphicsCommandList10* list, uint32_t entrypointIndex, const void* records,
                                    uint32_t recordCount, uint64_t recordStrideBytes);

        [[nodiscard]] uint64_t BackingMemoryBytes() const { return m_backingMemoryBytes; }

    private:
        WorkGraph() = default;

        Microsoft::WRL::ComPtr<ID3D12StateObject> m_stateObject;
        Microsoft::WRL::ComPtr<ID3D12WorkGraphProperties> m_properties;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_backingMemory;
        D3D12_PROGRAM_IDENTIFIER m_programIdentifier{};
        uint32_t m_graphIndex = 0;
        uint64_t m_backingMemoryBytes = 0;
    };

}  // namespace bicameral::gpu
