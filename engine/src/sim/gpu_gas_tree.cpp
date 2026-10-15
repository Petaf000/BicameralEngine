// gpu_gas_tree.cpp — 気体の多重解像度の木の 1 刻みを GPU の Compute で走らせる(gpu_gas_tree.h・T-0209・T-0356)。段を呼ぶ順は shaders/sim/gas_tree_step.hlsl の
// 先頭の表と同じで、CPU リファレンス(gas_tree.cpp の MicroStep)の段を面の段とセルの段に分けて並べたもの。段の間は全部の UAV の書き込みを待つ。
// 1 刻み = いちばん細かい小刻み substeps × 2^σmax 回 × 9 段。どのレベルの小刻みが始まる・終わるかは段の中でルート定数の小刻みの番号から決める。
// 木の形(面の一覧・セルの場所・セルごとの面の並び)は Create で一度だけ作って写す(木は作った後に変えない。T-0208)。
// レベルごとの係数と層の基準(環境から決まる t0〜t3)は、環境の値のホットリロードで RecordEnvironment が写し直す(T-0470・ADR-0200)。
#include "sim/gpu_gas_tree.h"

#include <cstring>

#include "common/gas_tree_gpu.hlsli"
#include "gpu/resources.h"
#include "sim/gpu_gas_internal.h"

namespace bicameral::sim {

    namespace {

        constexpr uint32_t THREADS_PER_GROUP = 64;  // gas_tree_step.hlsl の numthreads
        constexpr uint32_t ROOT_CONSTANT_COUNT = 8;
        constexpr uint32_t MAX_GROUPS = 65535;  // Dispatch の 1 次元の上限

        // u0〜u10 / b0 / デバッグのリング / t0〜t6
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = 11, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = 7};

        // shaders/CMakeLists.txt で入口ごとに作る .cso(Pass の順)
        constexpr std::array<const char*, 9> SHADER_NAMES = {
            "sim/gas_tree_derive.cso",    "sim/gas_tree_face_force.cso", "sim/gas_tree_apply_force.cso",
            "sim/gas_tree_face_flow.cso", "sim/gas_tree_outflow.cso",    "sim/gas_tree_face_moved.cso",
            "sim/gas_tree_moved_sum.cso", "sim/gas_tree_face_final.cso", "sim/gas_tree_transfer.cso"};

        // 面の段か(面の一覧の 1 つごとに 1 スレッド)
        constexpr std::array<bool, 9> FACE_PASS = {false, true, false, true, false, true, false, true, false};

        static_assert(sizeof(GasTreeGpuFace) == 64);
        static_assert(sizeof(GasTreeGpuLevel) == 40);
        static_assert(sizeof(GasTreeGpuPlace) == 16);
        static_assert(sizeof(GasTreeGpuCarry) == GAS_TREE_GPU_CARRY_BYTES);
        static_assert(sizeof(GasTreeGpuFraction) == GAS_TREE_GPU_FRACTION_BYTES);
        static_assert(sizeof(GasQ64) == GAS_TREE_GPU_LEDGER_STRIDE);
        static_assert((uint32_t)GasTreeSideKind::Cell == GAS_TREE_GPU_SIDE_CELL &&
                      (uint32_t)GasTreeSideKind::Ghost == GAS_TREE_GPU_SIDE_GHOST &&
                      (uint32_t)GasTreeSideKind::Wall == GAS_TREE_GPU_SIDE_WALL &&
                      (uint32_t)GasTreeSideKind::Mixed == GAS_TREE_GPU_SIDE_MIXED);

        template <typename T>
        void WriteRecord(std::span<std::byte> bytes, size_t offset, const T& value) {
            std::memcpy(bytes.data() + offset, &value, sizeof(T));
        }

        template <typename T>
        T ReadRecord(std::span<const std::byte> bytes, size_t offset) {
            T value;
            std::memcpy(&value, bytes.data() + offset, sizeof(T));
            return value;
        }

        // 端数 ↔ 頭の並び + 成分ごとの面(セルと同じ形)
        std::vector<std::byte> PackFractions(std::span<const GasTreeFraction> fractions, uint32_t speciesStride,
                                             uint32_t speciesCount) {
            const auto count = (uint32_t)fractions.size();
            std::vector<std::byte> bytes((size_t)count * GasGpuCellStride(speciesStride));
            for (uint32_t i = 0; i < count; ++i) {
                const GasTreeFraction& fraction = fractions[i];
                WriteRecord(
                    std::span(bytes), (size_t)i * GAS_TREE_GPU_FRACTION_BYTES,
                    GasTreeGpuFraction{.energy = fraction.energy,
                                       .momentum = {fraction.momentum[0], fraction.momentum[1], fraction.momentum[2]}});
                for (uint32_t s = 0; s < speciesCount; ++s) {
                    WriteRecord(std::span(bytes), GasGpuPlaneAddress(GAS_TREE_GPU_FRACTION_BYTES, count, i, s),
                                fraction.amounts[s]);
                }
            }

            return bytes;
        }

        std::vector<GasTreeFraction> UnpackFractions(std::span<const std::byte> bytes, uint32_t count,
                                                     uint32_t speciesCount) {
            std::vector<GasTreeFraction> fractions(count);
            for (uint32_t i = 0; i < count; ++i) {
                const auto head = ReadRecord<GasTreeGpuFraction>(bytes, (size_t)i * GAS_TREE_GPU_FRACTION_BYTES);
                GasTreeFraction& fraction = fractions[i];
                for (uint32_t s = 0; s < speciesCount; ++s) {
                    fraction.amounts[s] = ReadRecord<uint64_t>(
                        bytes, GasGpuPlaneAddress(GAS_TREE_GPU_FRACTION_BYTES, count, i, s));
                }

                fraction.energy = head.energy;
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                    fraction.momentum[axis] = head.momentum[axis];
            }

            return fractions;
        }

        // 帳簿(gas_tree_gpu.hlsli の GAS_TREE_GPU_LEDGER_*)
        std::vector<std::byte> PackLedger(const GasTreeTotals& ledger, uint32_t speciesStride, uint32_t speciesCount) {
            std::vector<std::byte> bytes(GasTreeGpuLedgerBytes(speciesStride));
            WriteRecord(std::span(bytes), GAS_TREE_GPU_LEDGER_ENERGY, ledger.energy);
            WriteRecord(std::span(bytes), GAS_TREE_GPU_LEDGER_MOMENTUM, ledger.momentum);
            for (uint32_t s = 0; s < speciesCount; ++s) {
                WriteRecord(std::span(bytes), GAS_TREE_GPU_LEDGER_AMOUNTS + (s * GAS_TREE_GPU_LEDGER_STRIDE),
                            ledger.amounts[s]);
            }

            return bytes;
        }

        GasTreeTotals UnpackLedger(std::span<const std::byte> bytes, uint32_t speciesCount) {
            GasTreeTotals ledger;
            ledger.energy = ReadRecord<GasQ64>(bytes, GAS_TREE_GPU_LEDGER_ENERGY);
            ledger.momentum = ReadRecord<std::array<GasQ64, GAS_AXES>>(bytes, GAS_TREE_GPU_LEDGER_MOMENTUM);
            for (uint32_t s = 0; s < speciesCount; ++s) {
                ledger.amounts[s] = ReadRecord<GasQ64>(bytes,
                                                       GAS_TREE_GPU_LEDGER_AMOUNTS + (s * GAS_TREE_GPU_LEDGER_STRIDE));
            }

            return ledger;
        }

        ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, std::span<const std::byte> bytes) {
            ComPtr<ID3D12Resource> buffer = gpu::CreateBuffer(device, bytes.size(), gpu::BufferKind::Upload);
            void* mapped = nullptr;
            const D3D12_RANGE noRead{.Begin = 0, .End = 0};
            if (!buffer || FAILED(buffer->Map(0, &noRead, &mapped)))
                return nullptr;

            std::memcpy(mapped, bytes.data(), bytes.size());
            buffer->Unmap(0, nullptr);

            return buffer;
        }

        template <typename T>
        gpu::ResidentBuffer CreateResident(ID3D12Device* device, std::span<const T> data) {
            return gpu::CreateResidentBuffer(device, std::as_bytes(data));
        }

        uint32_t GroupsFor(uint64_t threadCount) {
            return (uint32_t)((threadCount + THREADS_PER_GROUP - 1) / THREADS_PER_GROUP);
        }

        // --- 木の形を GPU の表へ ---

        // レベルごとの層の表の始め(層の基準・開いた境界の外をレベル 0 から続けて並べる)
        std::vector<uint32_t> LayerBegins(const GasTree& tree) {
            std::vector<uint32_t> begins;
            uint32_t begin = 0;
            for (const GasTreeLevel& level : tree.levels) {
                begins.push_back(begin);
                begin += (uint32_t)level.reference.size();
            }

            return begins;
        }

        constexpr size_t ENVIRONMENT_SRV_COUNT = 4;  // t0〜t3(設定・レベル・層の基準・層の外)は環境から決まる

        // 環境から決まる表(t0〜t3): 設定・レベルごとの係数と σ・層の基準の導く値・層の外。Create とホットリロード(T-0470)が同じここを通る
        std::array<gpu::ResidentBuffer, ENVIRONMENT_SRV_COUNT> CreateEnvironmentBuffers(ID3D12Device* device,
                                                                                        const GasTree& tree) {
            const std::vector<uint32_t> layerBegins = LayerBegins(tree);
            std::vector<GasTreeGpuLevel> levels;
            const uint32_t speciesStride = GasGpuSpeciesStride(tree.config.speciesCount);
            std::vector<GasGpuDerived> referenceDerived;
            std::vector<GasCell> ghost;
            for (uint32_t k = 0; k < tree.levels.size(); ++k) {
                const GasTreeLevel& level = tree.levels[k];
                const GasCoefficients& coefficients = level.coefficients;
                levels.push_back({.centralFlow = coefficients.centralFlow,
                                  .pressureFlow = coefficients.pressureFlow,
                                  .pressureImpulse = coefficients.pressureImpulse,
                                  .gravityImpulse = coefficients.gravityImpulse,
                                  .layerBegin = layerBegins[k],
                                  .subcycleExponent = level.subcycleExponent});
                for (const GasDerived& layer : level.referenceDerived)
                    referenceDerived.push_back(ToGasGpuDerived(layer));

                ghost.insert(ghost.end(), level.ghost.begin(), level.ghost.end());
            }

            return {gpu::CreateResidentBuffer(
                        device, PackGasGpuParameters(tree.config, tree.levels[0].coefficients, speciesStride)),
                    CreateResident(device, std::span<const GasTreeGpuLevel>(levels)),
                    CreateResident(device, std::span<const GasGpuDerived>(referenceDerived)),
                    gpu::CreateResidentBuffer(device, PackGasGpuCellRecords(ghost, speciesStride))};
        }

        // 側の番号: セルはセルの番号、外は層の表の番号、壁と Mixed は 0
        uint32_t SideIndex(const GasTreeFaceSide& side, const std::vector<uint32_t>& layerBegins) {
            if (side.kind == GasTreeSideKind::Cell)
                return side.index;

            if (side.kind == GasTreeSideKind::Ghost)
                return layerBegins[side.level] + side.index;

            return 0;
        }

        GasTreeGpuFace ToGpuFace(const GasTreeFaceLayout& face, const std::vector<uint32_t>& layerBegins) {
            return {.leftKind = (uint32_t)face.left.kind,
                    .leftIndex = SideIndex(face.left, layerBegins),
                    .leftLevel = face.left.level,
                    .rightKind = (uint32_t)face.right.kind,
                    .rightIndex = SideIndex(face.right, layerBegins),
                    .rightLevel = face.right.level,
                    .leftOuterKind = (uint32_t)face.leftOuter.kind,
                    .leftOuterIndex = SideIndex(face.leftOuter, layerBegins),
                    .rightOuterKind = (uint32_t)face.rightOuter.kind,
                    .rightOuterIndex = SideIndex(face.rightOuter, layerBegins),
                    .axis = face.axis,
                    .level = face.level,
                    .id = face.id,
                    .unused0 = 0,
                    .unused1 = 0,
                    .unused2 = 0};
        }

        // セルごとの面の並び(面の番号 × 2 + 自分が右側なら 1)と、セルの場所(並びの範囲)
        void BuildCellFaces(const GasTree& tree, const std::vector<GasTreeFaceLayout>& faces,
                            const std::vector<uint32_t>& layerBegins, std::vector<GasTreeGpuPlace>& places,
                            std::vector<uint32_t>& cellFaces) {
            std::vector<std::vector<uint32_t>> perCell(tree.cells.size());
            for (uint32_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
                const GasTreeFaceLayout& face = faces[faceIndex];
                if (face.left.kind == GasTreeSideKind::Cell)
                    perCell[face.left.index].push_back(faceIndex * 2);

                if (face.right.kind == GasTreeSideKind::Cell)
                    perCell[face.right.index].push_back((faceIndex * 2) + 1);
            }

            places.clear();
            cellFaces.clear();
            for (uint32_t index = 0; index < tree.cells.size(); ++index) {
                const GasTreePlace& place = tree.places[index];
                const auto begin = (uint32_t)cellFaces.size();
                cellFaces.insert(cellFaces.end(), perCell[index].begin(), perCell[index].end());
                places.push_back({.level = place.level,
                                  .layer = layerBegins[place.level] + place.coordinate[2],
                                  .faceBegin = begin,
                                  .faceEnd = (uint32_t)cellFaces.size()});
            }
        }

    }  // namespace

    std::expected<GpuGasTree, std::string> GpuGasTree::Create(ID3D12Device5* device, const GasTree& tree) {
        // 成分の数に上限は無い(1 件 = 頭 + 欄 × m_speciesStride。T-0598)
        GpuGasTree result;
        result.m_cellCount = (uint32_t)tree.cells.size();
        result.m_speciesCount = tree.config.speciesCount;
        result.m_speciesStride = GasGpuSpeciesStride(tree.config.speciesCount);
        result.m_substeps = tree.config.substeps;
        result.m_maxExponent = tree.maxSubcycleExponent;
        result.m_levelCount = (uint32_t)tree.levels.size();
        result.m_tick = tree.tick;
        if (auto created = result.CreatePipelines(device); !created)
            return std::unexpected(created.error());

        if (auto created = result.CreateLayout(device, tree); !created)
            return std::unexpected(created.error());

        if (GroupsFor(result.m_faceCount) > MAX_GROUPS || GroupsFor(result.m_cellCount) > MAX_GROUPS)
            return std::unexpected("気体の木が大きすぎる(Dispatch の上限)");

        if ((uint64_t)result.m_faceCount * GasGpuFaceStride(result.m_speciesStride) > UINT32_MAX)
            return std::unexpected("気体の木が大きすぎる(面の件の番地が 32bit を超える)");

        if (auto created = result.CreateBuffers(device, tree); !created)
            return std::unexpected(created.error());

        return result;
    }

    std::expected<void, std::string> GpuGasTree::CreatePipelines(ID3D12Device5* device) {
        m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!m_rootSignature)
            return std::unexpected("気体の木のルート署名を作れない");

        for (size_t pass = 0; pass < SHADER_NAMES.size(); ++pass) {
            const auto bytecode = gpu::LoadShader(SHADER_NAMES[pass]);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            m_pipelines[pass] = gpu::CreateComputePipeline(device, m_rootSignature.Get(), *bytecode);
            if (!m_pipelines[pass])
                return std::unexpected(std::format("パイプラインを作れない: {}", SHADER_NAMES[pass]));
        }

        return {};
    }

    // 木の形(t0〜t6): 設定・レベルごとの係数・層の基準と外・面の一覧・セルの場所・セルごとの面の並び
    std::expected<void, std::string> GpuGasTree::CreateLayout(ID3D12Device5* device, const GasTree& tree) {
        const std::vector<uint32_t> layerBegins = LayerBegins(tree);
        const std::vector<GasTreeFaceLayout> faces = ListGasTreeFaces(tree);
        m_faceCount = (uint32_t)faces.size();

        std::vector<GasTreeGpuFace> gpuFaces;
        gpuFaces.reserve(faces.size());
        for (const GasTreeFaceLayout& face : faces)
            gpuFaces.push_back(ToGpuFace(face, layerBegins));

        std::vector<GasTreeGpuPlace> places;
        std::vector<uint32_t> cellFaces;
        BuildCellFaces(tree, faces, layerBegins, places, cellFaces);

        std::array<gpu::ResidentBuffer, ENVIRONMENT_SRV_COUNT> environment = CreateEnvironmentBuffers(device, tree);
        m_srvs = {std::move(environment[0]),
                  std::move(environment[1]),
                  std::move(environment[2]),
                  std::move(environment[3]),
                  CreateResident(device, std::span<const GasTreeGpuFace>(gpuFaces)),
                  CreateResident(device, std::span<const GasTreeGpuPlace>(places)),
                  CreateResident(device, std::span<const uint32_t>(cellFaces))};
        // 読むのは既定のヒープの写しから(アップロードのヒープは CPU のメモリで、毎段 PCIe 越しに読むと L2 に収まらない大きさで
        // セルの数に対して超線形に遅くなる。32³ で 1 刻み 約 1300 ms。T-0374)。写すのは RecordUpload
        if (!std::ranges::all_of(m_srvs, &gpu::ResidentBuffer::Valid))
            return std::unexpected("気体の木の形のバッファを作れない");

        return {};
    }

    // 状態と作業のバッファ(gas_tree_step.hlsl の結び付けの順)・初めの状態の写し・読み戻し
    std::expected<void, std::string> GpuGasTree::CreateBuffers(ID3D12Device5* device, const GasTree& tree) {
        const uint64_t n = m_cellCount;
        const uint64_t cellBytes = n * GasGpuCellStride(m_speciesStride);      // セルと端数の件は同じ大きさ
        const uint64_t sumBytes = n * GasGpuSumsStride(m_speciesStride);       // セルごとの合計と出した量も
        const std::array<uint64_t, 11> uavBytes = {cellBytes,                  // u0 セル
                                                   cellBytes,                  // u1 端数
                                                   cellBytes,                  // u2 力を当てた後のセル
                                                   cellBytes,                  // u3 その端数
                                                   n * sizeof(GasGpuDerived),  // u4 導く値
                                                   (uint64_t)m_faceCount * GasGpuFaceStride(m_speciesStride),  // u5 面
                                                   sumBytes,                                // u6 セルごとの合計
                                                   GasTreeGpuLedgerBytes(m_speciesStride),  // u7 帳簿
                                                   cellBytes,                               // u8 小刻みの終わりの値
                                                   cellBytes,                               // u9 その端数
                                                   sumBytes};                               // u10 出した量
        for (size_t i = 0; i < uavBytes.size(); ++i) {
            m_uavs[i] = gpu::CreateBuffer(device, uavBytes[i], gpu::BufferKind::UnorderedAccess);
            if (!m_uavs[i])
                return std::unexpected("気体の木のバッファを作れない");
        }

        // --- 初めの状態 ---
        m_uploads = {CreateUploadBuffer(device, PackGasGpuCellPlanes(tree.cells, m_speciesStride)),
                     CreateUploadBuffer(device, PackFractions(tree.fractions, m_speciesStride, m_speciesCount)),
                     CreateUploadBuffer(device, PackLedger(tree.ledger, m_speciesStride, m_speciesCount))};
        m_readbacks = {gpu::CreateBuffer(device, uavBytes[0], gpu::BufferKind::Readback),
                       gpu::CreateBuffer(device, uavBytes[1], gpu::BufferKind::Readback),
                       gpu::CreateBuffer(device, uavBytes[7], gpu::BufferKind::Readback)};
        for (size_t i = 0; i < m_uploads.size(); ++i) {
            if (!m_uploads[i] || !m_readbacks[i])
                return std::unexpected("気体の木のアップロード・読み戻しのバッファを作れない");
        }

        return {};
    }

    void GpuGasTree::RecordUpload(ID3D12GraphicsCommandList10* list) {
        // バッファは COMMON から COPY_DEST へ暗黙に上がる(リストの終わりに COMMON へ戻る)
        constexpr std::array<size_t, 3> STATE_UAVS = {0, 1, 7};  // セル・端数・帳簿
        std::array<D3D12_RESOURCE_BARRIER, 3> toUav{};
        for (size_t i = 0; i < STATE_UAVS.size(); ++i) {
            ID3D12Resource* target = m_uavs[STATE_UAVS[i]].Get();
            list->CopyResource(target, m_uploads[i].Get());
            toUav[i] = gpu::Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        list->ResourceBarrier((uint32_t)toUav.size(), toUav.data());

        // --- 木の形(t0〜t6)を既定のヒープへ(T-0374)---
        gpu::RecordResidentUploads(list, m_srvs);
    }

    // 環境の値のホットリロード(T-0470・ADR-0200。GpuGas::RecordEnvironment と同じ形): 新しい t0〜t3 を作って写し、前のものは呼ぶ側に持たせる
    // (記録済みの刻みがまだ読む)。新しいバッファは作った直後で COMMON なので、同じリストで前の刻みが読んだ後でも写せる。t4〜t6(木の形)はそのまま
    bool GpuGasTree::RecordEnvironment(ID3D12GraphicsCommandList10* list, const GasTree& tree,
                                       std::vector<ComPtr<ID3D12Resource>>& keepAlive) {
        const bool sameShape = (uint32_t)tree.cells.size() == m_cellCount && tree.config.substeps == m_substeps &&
                               tree.config.speciesCount == m_speciesCount &&
                               tree.maxSubcycleExponent == m_maxExponent &&
                               (uint32_t)tree.levels.size() == m_levelCount;
        if (!sameShape)
            return false;

        ComPtr<ID3D12Device> device;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
            return false;

        std::array<gpu::ResidentBuffer, ENVIRONMENT_SRV_COUNT> buffers = CreateEnvironmentBuffers(device.Get(), tree);
        if (!std::ranges::all_of(buffers, &gpu::ResidentBuffer::Valid))
            return false;

        gpu::RecordResidentUploads(list, buffers);
        for (size_t i = 0; i < buffers.size(); ++i) {
            keepAlive.push_back(std::move(m_srvs[i].buffer));
            keepAlive.push_back(std::move(m_srvs[i].upload));
            keepAlive.push_back(buffers[i].upload);
            m_srvs[i] = std::move(buffers[i]);
        }

        return true;
    }

    // 1 つの段を投げ、全部の UAV の書き込みを次の段より前に終わらせる
    void GpuGasTree::RecordPass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, Pass pass,
                                const Constants& constants) const {
        static_assert(sizeof(Constants) == ROOT_CONSTANT_COUNT * sizeof(uint32_t));
        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_pipelines[(size_t)pass].Get());
        for (uint32_t i = 0; i < m_uavs.size(); ++i)
            list->SetComputeRootUnorderedAccessView(i, m_uavs[i]->GetGPUVirtualAddress());

        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &constants, 0);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing);
        for (uint32_t i = 0; i < m_srvs.size(); ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i), m_srvs[i].buffer->GetGPUVirtualAddress());

        const uint32_t threads = FACE_PASS[(size_t)pass] ? m_faceCount : m_cellCount;
        list->Dispatch(GroupsFor(threads), 1, 1);
        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
    }

    // 1 刻み = いちばん細かい小刻み substeps × 2^σmax 回(gas_tree.cpp の StepGasTree)。乱数の丸めの小刻みの通し番号はレベルごとに段の中で作る
    void GpuGasTree::RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) {
        const uint32_t micros = m_substeps << m_maxExponent;
        if (m_passTimestamps)
            list->EndQuery(m_passTimestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

        for (uint32_t micro = 0; micro < micros; ++micro) {
            const Constants constants{.tickLow = (uint32_t)m_tick,
                                      .tickHigh = (uint32_t)(m_tick >> 32),
                                      .micro = micro,
                                      .substeps = m_substeps,
                                      .maxExponent = m_maxExponent,
                                      .cellCount = m_cellCount,
                                      .faceCount = m_faceCount,
                                      .speciesStride = m_speciesStride};
            for (size_t pass = 0; pass < (size_t)Pass::Count; ++pass) {
                RecordPass(list, debugRing, (Pass)pass, constants);
                if (m_passTimestamps) {
                    const uint32_t slot = 1 + (micro * (uint32_t)Pass::Count) + (uint32_t)pass;
                    list->EndQuery(m_passTimestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot);
                }
            }
        }

        ++m_tick;
    }

    void GpuGasTree::RecordReadback(ID3D12GraphicsCommandList10* list) const {
        gpu::RecordCopyToReadback(list, m_uavs[0].Get(), m_readbacks[0].Get());
        gpu::RecordCopyToReadback(list, m_uavs[1].Get(), m_readbacks[1].Get());
        gpu::RecordCopyToReadback(list, m_uavs[7].Get(), m_readbacks[2].Get());
    }

    bool GpuGasTree::Read(std::vector<GasCell>& cells, std::vector<GasTreeFraction>& fractions,
                          GasTreeTotals& ledger) const {
        const size_t stride = GasGpuCellStride(m_speciesStride);
        std::vector<std::byte> cellBytes((size_t)m_cellCount * stride);
        std::vector<std::byte> fractionBytes((size_t)m_cellCount * stride);
        std::vector<std::byte> ledgerBytes(GasTreeGpuLedgerBytes(m_speciesStride));
        if (!gpu::ReadBuffer(m_readbacks[0].Get(), cellBytes) ||
            !gpu::ReadBuffer(m_readbacks[1].Get(), fractionBytes) ||
            !gpu::ReadBuffer(m_readbacks[2].Get(), ledgerBytes))
            return false;

        cells = UnpackGasGpuCellPlanes(cellBytes, m_cellCount, m_speciesCount);
        fractions = UnpackFractions(fractionBytes, m_cellCount, m_speciesCount);
        ledger = UnpackLedger(ledgerBytes, m_speciesCount);

        return true;
    }

    // --- 計測(gpu_gas.cpp と同じ形。T-0358)---

    bool GpuGasTree::EnableTiming(ID3D12Device* device) {
        const D3D12_QUERY_HEAP_DESC queryDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 2, .NodeMask = 0};
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestamps))))
            return false;

        m_timestampReadback = gpu::CreateBuffer(device, 2 * sizeof(uint64_t), gpu::BufferKind::Readback);

        return m_timestampReadback != nullptr;
    }

    void GpuGasTree::RecordTimestamp(ID3D12GraphicsCommandList10* list, uint32_t slot) const {
        if (m_timestamps)
            list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot);
    }

    void GpuGasTree::RecordTimingReadback(ID3D12GraphicsCommandList10* list) const {
        if (m_timestamps)
            list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, m_timestampReadback.Get(), 0);
    }

    std::optional<uint64_t> GpuGasTree::TimingTicks() const {
        std::array<uint64_t, 2> ticks = {};
        if (!m_timestamps || !gpu::ReadBuffer(m_timestampReadback.Get(), std::as_writable_bytes(std::span(ticks))))
            return std::nullopt;

        return ticks[1] - ticks[0];
    }

    // --- 段ごとの計測(T-0374)。段の後のバリアの後に打つので、差 = その段の Dispatch + バリア ---

    bool GpuGasTree::EnablePassTiming(ID3D12Device* device) {
        static_assert(PASS_COUNT == (size_t)Pass::Count);
        const uint32_t count = 1 + PassesPerTick();
        const D3D12_QUERY_HEAP_DESC queryDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = count, .NodeMask = 0};
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_passTimestamps))))
            return false;

        m_passTimestampReadback = gpu::CreateBuffer(device, count * sizeof(uint64_t), gpu::BufferKind::Readback);

        return m_passTimestampReadback != nullptr;
    }

    void GpuGasTree::RecordPassTimingReadback(ID3D12GraphicsCommandList10* list) const {
        if (m_passTimestamps)
            list->ResolveQueryData(m_passTimestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 1 + PassesPerTick(),
                                   m_passTimestampReadback.Get(), 0);
    }

    std::optional<std::array<uint64_t, GpuGasTree::PASS_COUNT>> GpuGasTree::PassTimingTicks() const {
        std::vector<uint64_t> ticks(1 + PassesPerTick());
        if (!m_passTimestamps ||
            !gpu::ReadBuffer(m_passTimestampReadback.Get(), std::as_writable_bytes(std::span(ticks))))
            return std::nullopt;

        std::array<uint64_t, PASS_COUNT> totals = {};
        for (size_t slot = 1; slot < ticks.size(); ++slot)
            totals[(slot - 1) % PASS_COUNT] += ticks[slot] - ticks[slot - 1];

        return totals;
    }

}  // namespace bicameral::sim
