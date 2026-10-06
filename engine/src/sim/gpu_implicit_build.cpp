// gpu_implicit_build.cpp — 細かいレベルの熱の陰解法の系を GPU の木から作る(T-0129)。何をするかは gpu_implicit_build.h、
// 段の中身と番号の付け方は shaders/sim/implicit_build.hlsl。段の間は GpuMultires::RecordExternalDispatch が入れる UAV のバリア。
#include "sim/gpu_implicit_build.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <span>
#include <string_view>
#include <utility>

#include "gpu/resources.h"

using namespace bicameral::multires;

namespace bicameral::sim {

    namespace {

        constexpr uint32_t THREADS = 64;  // implicit_build.hlsl の BUILD_THREADS
        constexpr uint32_t HEADER_WORDS = 16;
        constexpr uint32_t HEADER_UNKNOWNS = 0;
        constexpr uint32_t HEADER_BOUNDARY = 1;
        constexpr uint32_t HEADER_FACES = 2;
        constexpr uint32_t HEADER_OVERFLOW = 3;
        constexpr uint32_t ENTRY_WORDS = 4;      // 面の候補ごと: 先・印・面の番号・境のセルの番号
        constexpr uint32_t CELL_WORDS = 3;       // セルごと: 番地・面の数・面の一覧の始まり
        constexpr uint32_t SCAN_THREADS = 1024;  // implicit_build.hlsl の SCAN_THREADS(接頭和の 1 グループ)
        constexpr uint32_t FROZEN_MARKS_BIT = 1u << 24;

        static_assert(sizeof(ImGpuCell) == 40 && sizeof(ImGpuFace) == 32,
                      "implicit_build.hlsl の CELL_BYTES・FACE_BYTES");

        // implicit_build.hlsl の入口(段の順)
        enum Pass : uint8_t {
            PassClear,
            PassCountBlocks,
            PassScanBlocks,
            PassNumberUnknowns,
            PassFaceEntries,
            PassScanEntriesLocal,
            PassScanEntriesGroups,
            PassScanEntriesAdd,
            PassBoundary,
            PassCells,
            PassFaces,
            PassScanCellsLocal,
            PassScanCellsGroups,
            PassScanCellsAdd,
            PassFillLists,
            PassSortLists,
            PassCount
        };

        // implicit_build.hlsl の入口から Build を除いたもの(Pass の順)
        constexpr std::array<const char*, PassCount> PASS_NAMES = {"Clear",
                                                                   "CountBlocks",
                                                                   "ScanBlocks",
                                                                   "NumberUnknowns",
                                                                   "FaceEntries",
                                                                   "ScanEntriesLocal",
                                                                   "ScanEntriesGroups",
                                                                   "ScanEntriesAdd",
                                                                   "Boundary",
                                                                   "Cells",
                                                                   "Faces",
                                                                   "ScanCellsLocal",
                                                                   "ScanCellsGroups",
                                                                   "ScanCellsAdd",
                                                                   "FillLists",
                                                                   "SortLists"};

        // シェーダーのファイル名(CMake の bicameral_add_shader と同じ: 入口の名前の大文字の前に _ を入れて小文字に)
        std::string ShaderFile(std::string_view name) {
            std::string file = "sim/implicit_build_";
            for (size_t i = 0; i < name.size(); ++i) {
                const char letter = name[i];
                const bool upper = letter >= 'A' && letter <= 'Z';
                if (upper && i > 0)
                    file += '_';

                file += upper ? static_cast<char>(letter - 'A' + 'a') : letter;
            }

            return file + ".cso";
        }

        uint32_t Groups(uint64_t threads) {
            return static_cast<uint32_t>((threads + THREADS - 1) / THREADS);
        }

        // UAV のバッファの先頭 bytes を読み戻しへ写す(UAV → COPY_SOURCE → UAV)
        void CopyToReadback(ID3D12GraphicsCommandList* list, ID3D12Resource* source, ID3D12Resource* readback,
                            uint64_t bytes) {
            D3D12_RESOURCE_BARRIER barrier = gpu::Transition(source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &barrier);
            list->CopyBufferRegion(readback, 0, source, 0, bytes);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(1, &barrier);
        }

        template <typename T>
        std::vector<T> Slice(std::span<const std::byte> bytes, uint64_t offset, uint32_t count) {
            std::vector<T> values(count);
            if (count != 0)
                std::memcpy(values.data(), bytes.data() + offset, size_t{count} * sizeof(T));

            return values;
        }

    }  // namespace

    std::expected<GpuImplicitBuild, std::string> GpuImplicitBuild::Create(ID3D12Device5* device,
                                                                          const GpuMultires& multires,
                                                                          const MultiresCapacity& capacity,
                                                                          const GpuImplicitBuildLimits& limits) {
        if (limits.unknowns == 0 || limits.cells < limits.unknowns)
            return std::unexpected("陰解法の系の上限が正しくない");

        GpuImplicitBuild result;
        result.m_limits = limits;
        result.m_worldBlocks = capacity.worldBlocks;

        // --- 作業場: 見出し・番号の表・最初の面の候補・枠の始まり・面の候補・セル(implicit_build.hlsl の番地の関数)---
        const uint64_t mapWords = uint64_t{capacity.worldBlocks} * MR_BLOCK_CELLS;
        // 続けて、接頭和のグループの和(面の候補 2 本・セル)・仮の面の一覧とその位置のセル
        const uint64_t entries = uint64_t{MR_FACES} * limits.unknowns;
        const uint64_t entryGroups = (entries + SCAN_THREADS - 1) / SCAN_THREADS;
        const uint64_t cellGroups = (uint64_t{limits.cells} + SCAN_THREADS - 1) / SCAN_THREADS;
        const uint64_t workWords = HEADER_WORDS + (2 * mapWords) + capacity.worldBlocks + 1 + (ENTRY_WORDS * entries) +
                                   (uint64_t{CELL_WORDS} * limits.cells) + 1 + (2 * (entryGroups + 1)) +
                                   (cellGroups + 1) + (2 * 2 * entries);
        result.m_workBytes = workWords * sizeof(uint32_t);
        result.m_systemBytes = result.ListsOffset() + (uint64_t{2} * MR_FACES * limits.unknowns * sizeof(uint32_t));

        for (const char* name : PASS_NAMES) {
            const std::string shader = ShaderFile(name);
            const auto bytecode = gpu::LoadShader(shader);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            auto pipeline = gpu::CreateComputePipeline(device, multires.RootSignature(), *bytecode);
            if (!pipeline)
                return std::unexpected(std::format("系を作る段のパイプラインを作れない({})", shader));

            result.m_pipelines.push_back(std::move(pipeline));
        }

        result.m_work = gpu::CreateBuffer(device, result.m_workBytes, gpu::BufferKind::UnorderedAccess);
        result.m_system = gpu::CreateBuffer(device, result.m_systemBytes, gpu::BufferKind::UnorderedAccess);
        result.m_workReadback = gpu::CreateBuffer(device, uint64_t{HEADER_WORDS} * 4, gpu::BufferKind::Readback);
        result.m_systemReadback = gpu::CreateBuffer(device, result.m_systemBytes, gpu::BufferKind::Readback);
        result.m_timestampReadback = gpu::CreateBuffer(device, uint64_t{MAX_TIMESTAMPS} * sizeof(uint64_t),
                                                       gpu::BufferKind::Readback);
        if (!result.m_work || !result.m_system || !result.m_workReadback || !result.m_systemReadback ||
            !result.m_timestampReadback)
            return std::unexpected("系を作る段のバッファを作れない");

        const D3D12_QUERY_HEAP_DESC queryDesc{
            .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = MAX_TIMESTAMPS, .NodeMask = 0};
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&result.m_timestamps))))
            return std::unexpected("タイムスタンプのヒープを作れない");

        return result;
    }

    uint64_t GpuImplicitBuild::FacesOffset() const {
        return uint64_t{m_limits.cells} * sizeof(ImGpuCell);
    }

    uint64_t GpuImplicitBuild::ListsOffset() const {
        return FacesOffset() + (uint64_t{MR_FACES} * m_limits.unknowns * sizeof(ImGpuFace));
    }

    void GpuImplicitBuild::RecordBuild(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                       GpuMultires& multires, const MultiresStepOptions& options, bool useFrozenMarks) {
        const auto base = static_cast<uint16_t>(static_cast<int16_t>(options.subcycleBaseLevel));
        const uint32_t packed = uint32_t{base} | (std::min(options.implicitMaxGap, 255u) << 16) |
                                (useFrozenMarks ? FROZEN_MARKS_BIT : 0u);
        const std::array<uint32_t, 4> external = {packed, m_limits.unknowns, m_limits.cells, 0};

        // --- 段ごとのグループの数(数は GPU が決めるので上限から。超えたグループは何もしない)---
        const uint64_t mapWords = uint64_t{m_worldBlocks} * MR_BLOCK_CELLS;
        const uint64_t entries = uint64_t{MR_FACES} * m_limits.unknowns;
        const auto scanGroups = [](uint64_t count) {
            return static_cast<uint32_t>((count + SCAN_THREADS - 1) / SCAN_THREADS);
        };
        const std::array<uint32_t, PassCount> groups = {Groups(HEADER_WORDS + (2 * mapWords)),
                                                        m_worldBlocks,
                                                        1,
                                                        m_worldBlocks,
                                                        Groups(m_limits.unknowns),
                                                        scanGroups(entries),
                                                        1,
                                                        Groups(entries),
                                                        Groups(entries),
                                                        Groups(m_limits.cells),
                                                        Groups(entries),
                                                        scanGroups(m_limits.cells),
                                                        1,
                                                        Groups(m_limits.cells),
                                                        Groups(entries),
                                                        Groups(2 * entries)};

        multires.SetExternalViews(m_work->GetGPUVirtualAddress(), m_system->GetGPUVirtualAddress());
        for (uint32_t pass = 0; pass < PassCount; ++pass) {
            if (m_stampPasses)
                RecordTimestamp(list, pass);

            multires.RecordExternalDispatch(list, debugRing, m_pipelines[pass].Get(), groups[pass], external);
        }

        if (m_stampPasses)
            RecordTimestamp(list, PassCount);

        multires.SetExternalViews(0, 0);
    }

    void GpuImplicitBuild::RecordCopyTo(ID3D12GraphicsCommandList* list, GpuImplicit& implicit) {
        D3D12_RESOURCE_BARRIER barrier = gpu::Transition(m_system.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        implicit.RecordCopySystem(list, m_system.Get(), 0, FacesOffset(), ListsOffset());
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
    }

    uint32_t GpuImplicitBuild::StageCount() {
        return PassCount;
    }

    // 段の名前(計測の表に使う。implicit_build.hlsl の入口から Build を除いたもの)
    const char* GpuImplicitBuild::StageName(uint32_t pass) {
        return pass < PASS_NAMES.size() ? PASS_NAMES[pass] : "?";
    }

    void GpuImplicitBuild::RecordTimestamp(ID3D12GraphicsCommandList* list, uint32_t index) {
        if (index >= MAX_TIMESTAMPS)
            return;

        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
        m_timestampCount = std::max(m_timestampCount, index + 1);
    }

    void GpuImplicitBuild::RecordReadback(ID3D12GraphicsCommandList* list) {
        CopyToReadback(list, m_work.Get(), m_workReadback.Get(), uint64_t{HEADER_WORDS} * 4);
        CopyToReadback(list, m_system.Get(), m_systemReadback.Get(), m_systemBytes);
        if (m_timestampCount > 0) {
            list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, m_timestampCount,
                                   m_timestampReadback.Get(), 0);
        }
    }

    std::expected<GpuImplicitSystem, std::string> GpuImplicitBuild::Read() const {
        std::array<uint32_t, HEADER_WORDS> header{};
        std::vector<std::byte> bytes(m_systemBytes);
        if (!gpu::ReadBuffer(m_workReadback.Get(), std::as_writable_bytes(std::span(header))) ||
            !gpu::ReadBuffer(m_systemReadback.Get(), bytes))
            return std::unexpected("作った系を読み戻せない");

        GpuImplicitSystem system;
        system.unknowns = header[HEADER_UNKNOWNS];
        system.boundary = header[HEADER_BOUNDARY];
        system.faces = header[HEADER_FACES];
        system.overflow = header[HEADER_OVERFLOW] != 0;
        if (system.overflow)
            return system;

        system.cells = Slice<ImGpuCell>(bytes, 0, system.unknowns + system.boundary);
        system.faceList = Slice<ImGpuFace>(bytes, FacesOffset(), system.faces);
        system.lists = Slice<uint32_t>(bytes, ListsOffset(), 2 * system.faces);

        return system;
    }

    std::vector<uint64_t> GpuImplicitBuild::ReadTimestamps(uint32_t count) const {
        std::vector<uint64_t> ticks(std::min(count, m_timestampCount));
        if (!gpu::ReadBuffer(m_timestampReadback.Get(), std::as_writable_bytes(std::span(ticks))))
            ticks.clear();

        return ticks;
    }

}  // namespace bicameral::sim
