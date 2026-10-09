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

        void RecordReadback(ID3D12GraphicsCommandList10* list) { m_nest.RecordReadback(list); }
        [[nodiscard]] bool Read(MultiresNest& nest) const { return m_nest.Read(nest); }

    private:
        explicit GpuLabBox(GpuMultires nest) : m_nest(std::move(nest)) {}

        [[nodiscard]] bool RecordCommandUpload(ID3D12GraphicsCommandList10* list, std::span<const Command> commands);

        GpuMultires m_nest;
        ComPtr<ID3D12PipelineState> m_applyPipeline;
        ComPtr<ID3D12Resource> m_commands;       // この刻みのコマンドの列(u4)
        ComPtr<ID3D12Resource> m_commandUpload;  // CPU が書く写し
        uint32_t m_speciesCount = 0;
    };

}  // namespace bicameral::sim
