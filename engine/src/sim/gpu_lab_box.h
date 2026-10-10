// gpu_lab_box.h — 実験室の箱(sim/lab_box.h)を GPU で刻む(T-0142)。多重解像度の木(sim/gpu_multires)に根 1 つの箱を置き、
// 刻みの前にコマンド(common/lab_box.hlsli)を shaders/sim/lab_box.hlsl で当ててから RecordStep(反応 + 熱の伝導)。
// CPU が書くのはコマンドの列の写しとルート定数だけ(箱のセルには触れない。D-107)。初めの箱は RecordUpload で 1 度だけ写す。
//
// 使い方(1 本のリストに 1 刻みまで。コマンドの写しの置き場が 1 つなので、前のリストが終わってから次を記録する):
//   auto box = GpuLabBox::Create(device, table);
//   box->RecordUpload(list, MakeLabBoxNest(table));        // 最初だけ
//   box->RecordTick(list, ring, tick, この刻みのコマンド);  box->RecordReadback(list);  → 投げて待つ → box->Read(nest)
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "sim/command.h"
#include "sim/gpu_multires.h"
#include "sim/lab_box.h"

namespace bicameral::sim {

    class GpuLabBox {
    public:
        [[nodiscard]] static std::expected<GpuLabBox, std::string> Create(ID3D12Device5* device,
                                                                          const BakedReactionTable& table);

        // 初めの箱(MakeLabBoxNest)を GPU へ写す
        [[nodiscard]] bool RecordUpload(ID3D12GraphicsCommandList10* list, const MultiresNest& nest);

        // 刻み tick: コマンド(LAB_MAX_COMMANDS_PER_TICK まで。(targetTick, sequence) の昇順)を当てて、1 刻み
        [[nodiscard]] bool RecordTick(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                      uint64_t tick, std::span<const Command> commands);

        // 反応表を替える(T-0194。GpuMultires::ReplaceTable。返す前の表は、それを読んだリストが終わるまで呼ぶ側が持つ)。
        // 物質の一覧が変わる表なら、続けて RecordSpeciesRemap で箱のセルを付け替える(T-0242)
        [[nodiscard]] std::expected<std::vector<ComPtr<ID3D12Resource>>, std::string> ReplaceTable(
            const BakedReactionTable& table) {
            auto retired = m_nest.ReplaceTable(table);
            if (retired)
                m_speciesCount = static_cast<uint32_t>(table.species.size());

            return retired;
        }

        // 箱の全部のセル(cellCount 個。MultiresNest::cells の数)を付け替える(T-0242・ADR-0065。shaders/sim/lab_box.hlsl の
        // RemapLabSpecies。CPU リファレンスは RemapLabBox)。remapWords は sim::PackSpeciesRemap(今の表 → 新しい表)。
        // 新しい表は ReplaceTable で先に結んでおく。写しのバッファはこのリストが終わるまで持つ(次の付け替えで捨てる)
        [[nodiscard]] bool RecordSpeciesRemap(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                              std::span<const uint32_t> remapWords, uint32_t cellCount);

        void RecordReadback(ID3D12GraphicsCommandList10* list) { m_nest.RecordReadback(list); }
        [[nodiscard]] bool Read(MultiresNest& nest) const { return m_nest.Read(nest); }

    private:
        explicit GpuLabBox(GpuMultires nest) : m_nest(std::move(nest)) {}

        [[nodiscard]] bool RecordCommandUpload(ID3D12GraphicsCommandList10* list, std::span<const Command> commands);

        GpuMultires m_nest;
        ComPtr<ID3D12PipelineState> m_applyPipeline;
        ComPtr<ID3D12PipelineState> m_remapPipeline;
        ComPtr<ID3D12Resource> m_remap;          // 付け替えの表(u4。T-0242)
        ComPtr<ID3D12Resource> m_remapUpload;    // その写し
        ComPtr<ID3D12Resource> m_commands;       // この刻みのコマンドの列(u4)
        ComPtr<ID3D12Resource> m_commandUpload;  // CPU が書く写し
        uint32_t m_speciesCount = 0;
    };

}  // namespace bicameral::sim
