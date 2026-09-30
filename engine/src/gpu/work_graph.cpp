// work_graph.cpp — Work Graph の状態オブジェクト・裏のメモリ・起動(T-0013。GPU の入力からの起動は T-0005)。
// 手順は D3D12 Work Graphs の仕様(DirectX-Specs の "Work Graphs")の「Program setup」の順:
//   状態オブジェクト(DXIL ライブラリ + グローバルのルート署名 + ワークグラフ)→ 必要な裏のメモリの大きさを聞いて確保
//   → SetProgram(最初は INITIALIZE)→ DispatchGraph
#include "gpu/work_graph.h"

#include "core/hresult.h"
#include "core/log.h"
#include "core/unicode.h"
#include "gpu/resources.h"

using Microsoft::WRL::ComPtr;

namespace bicameral::gpu {
    namespace {

        ComPtr<ID3D12StateObject> CreateStateObject(ID3D12Device5* device, ID3D12RootSignature* globalRootSignature,
                                                    std::span<const std::byte> library, const std::wstring& name) {
            CD3DX12_STATE_OBJECT_DESC desc(D3D12_STATE_OBJECT_TYPE_EXECUTABLE);
            const CD3DX12_SHADER_BYTECODE bytecode(library.data(), library.size());
            desc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>()->SetDXILLibrary(&bytecode);
            desc.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>()->SetRootSignature(globalRootSignature);
            auto* workGraph = desc.CreateSubobject<CD3DX12_WORK_GRAPH_SUBOBJECT>();
            workGraph->IncludeAllAvailableNodes();
            workGraph->SetProgramName(name.c_str());

            ComPtr<ID3D12StateObject> stateObject;
            if (!BICAMERAL_CHECK_HR(Channel::WorkGraph, device->CreateStateObject(desc, IID_PPV_ARGS(&stateObject)))) {
                return nullptr;
            }
            return stateObject;
        }

    }  // namespace

    std::expected<WorkGraph, std::string> WorkGraph::Create(ID3D12Device5* device,
                                                            ID3D12RootSignature* globalRootSignature,
                                                            std::span<const std::byte> library,
                                                            std::wstring_view programName) {
        const std::wstring name(programName);
        WorkGraph graph;
        graph.m_stateObject = CreateStateObject(device, globalRootSignature, library, name);
        if (!graph.m_stateObject) return std::unexpected("ワークグラフの状態オブジェクトを作れない");

        ComPtr<ID3D12StateObjectProperties1> stateProperties;
        if (FAILED(graph.m_stateObject.As(&stateProperties)) || FAILED(graph.m_stateObject.As(&graph.m_properties))) {
            return std::unexpected("状態オブジェクトから Work Graphs のプロパティを取れない(ランタイムが古い)");
        }
        graph.m_programIdentifier = stateProperties->GetProgramIdentifier(name.c_str());
        graph.m_graphIndex = graph.m_properties->GetWorkGraphIndex(name.c_str());

        // --- 裏のメモリ(ノード間のレコードの置き場所。ドライバが大きさを決める)---
        D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS requirements{};
        graph.m_properties->GetWorkGraphMemoryRequirements(graph.m_graphIndex, &requirements);
        graph.m_backingMemoryBytes = requirements.MaxSizeInBytes;
        if (graph.m_backingMemoryBytes > 0) {
            graph.m_backingMemory = CreateBuffer(device, graph.m_backingMemoryBytes, BufferKind::UnorderedAccess);
            if (!graph.m_backingMemory) return std::unexpected("ワークグラフの裏のメモリを確保できない");
        }
        Log(Channel::WorkGraph, Level::Info, "ワークグラフ {}: 裏のメモリ {} バイト(最小 {})", ToUtf8(name),
            requirements.MaxSizeInBytes, requirements.MinSizeInBytes);
        return graph;
    }

    uint32_t WorkGraph::EntrypointIndex(std::wstring_view nodeName) const {
        const std::wstring name(nodeName);
        return m_properties->GetEntrypointIndex(m_graphIndex, {.Name = name.c_str(), .ArrayIndex = 0});
    }

    void WorkGraph::SetProgram(ID3D12GraphicsCommandList10* list, bool initialize) const {
        D3D12_SET_PROGRAM_DESC desc{.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH};
        desc.WorkGraph = {
            .ProgramIdentifier = m_programIdentifier,
            .Flags = initialize ? D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE : D3D12_SET_WORK_GRAPH_FLAG_NONE,
            .BackingMemory = {.StartAddress = m_backingMemory ? m_backingMemory->GetGPUVirtualAddress() : 0,
                              .SizeInBytes = m_backingMemoryBytes},
        };
        list->SetProgram(&desc);
    }

    void WorkGraph::DispatchFromCpu(ID3D12GraphicsCommandList10* list, uint32_t entrypointIndex, const void* records,
                                    uint32_t recordCount, uint64_t recordStrideBytes) {
        D3D12_DISPATCH_GRAPH_DESC desc{.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT};
        desc.NodeCPUInput = {
            .EntrypointIndex = entrypointIndex,
            .NumRecords = recordCount,
            .pRecords = records,
            .RecordStrideInBytes = recordStrideBytes,
        };
        list->DispatchGraph(&desc);
    }

    void WorkGraph::DispatchFromGpu(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS nodeGpuInput) {
        D3D12_DISPATCH_GRAPH_DESC desc{.Mode = D3D12_DISPATCH_MODE_NODE_GPU_INPUT};
        desc.NodeGPUInput = nodeGpuInput;
        list->DispatchGraph(&desc);
    }

}  // namespace bicameral::gpu
