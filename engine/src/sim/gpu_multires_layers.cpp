// gpu_multires_layers.cpp — GPU の層の置き場の像を作る・戻す(T-1221)。何をするかは gpu_multires_layers.h。
// 1 セルの組は成分の並びの位置ではなく物質で結ぶ(層の頁に物質も持つ)ので、GPU の段がセルの並びを変えても層は物質についていく。
#include "sim/gpu_multires_layers.h"

#include <algorithm>
#include <format>

#include "common/multires_layers.hlsli"
#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        // 頁 page を持つ枠(無ければ MR_LAYER_NO_PAGE)
        uint32_t PageOwner(const MultiresNest& nest, uint32_t page) {
            for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
                const MrBlock& block = nest.blocks[slot];
                if (block.kind != MR_BLOCK_UNUSED && block.page == page)
                    return slot;
            }

            return MR_LAYER_NO_PAGE;
        }

        // 頁 page のセルの層を層の頁 layerPage の組に書く(0 でない層だけ。前から詰める)
        void WriteLayerPage(const MultiresNest& nest, uint32_t page, uint32_t capacity, uint32_t layerPage,
                            std::span<uint32_t> words) {
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const RxCell cell = nest_detail::LoadPageCell(nest, page, index);
                const MrCellLayers layers = nest_detail::LoadPageCellLayers(nest, page, index);
                uint32_t k = 0;
                for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                    if (layers.amounts[i] == 0)
                        continue;

                    const uint32_t address = MrLayerEntryAddress(capacity, layerPage, index, k);
                    words[address] = cell.species[i];
                    words[address + 1] = static_cast<uint32_t>(layers.amounts[i]);
                    words[address + 2] = static_cast<uint32_t>(layers.amounts[i] >> 32);
                    ++k;
                }
            }
        }

        // 層の頁 layerPage のセル index の層を、今のセルの成分の並びに結ぶ
        MrCellLayers ReadLayerCell(const RxCell& cell, std::span<const uint32_t> words, uint32_t capacity,
                                   uint32_t layerPage, uint32_t index) {
            MrCellLayers layers;
            for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
                const uint32_t address = MrLayerEntryAddress(capacity, layerPage, index, k);
                const uint64_t layer = (uint64_t{words[address + 2]} << 32) | words[address + 1];
                if (layer == 0)
                    continue;

                for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                    if (cell.species[i] == words[address])
                        layers.amounts[i] = std::min(layer, cell.amounts[i]);
                }
            }

            return layers;
        }

        bool AnyLayer(const MrCellLayers& layers) {
            return std::ranges::any_of(layers.amounts, [](uint64_t layer) { return layer != 0; });
        }

    }  // namespace

    std::expected<GpuLayerImage, std::string> MakeGpuLayerImage(const MultiresNest& nest, uint32_t capacity) {
        GpuLayerImage image;
        image.words.assign(MrLayerStoreWords(capacity), 0);
        for (uint32_t layerPage = 0; layerPage < capacity; ++layerPage)
            image.words[MrLayerHeaderAddress(layerPage)] = MR_LAYER_NO_PAGE;

        // --- 1 段目の限り: 一様の枠の層は写さない(T-1240)---
        if (!nest.cells.uniformLayers.Empty())
            return std::unexpected("一様の枠の層はまだ GPU に写せない(T-1240)");

        for (uint32_t page = 0; page < nest.cells.pages.size(); ++page) {
            if (!nest_detail::PageHasLayers(nest, page))
                continue;

            if (nest.wideCells)
                return std::unexpected("溢れを使う世界の層はまだ GPU に写せない(T-1240)");

            if (image.used >= capacity)
                return std::unexpected(std::format("層の頁が足りない(層のある頁が {} を超える。T-1221)", capacity));

            const uint32_t header = MrLayerHeaderAddress(image.used);
            image.words[header] = page;
            image.words[header + 1] = PageOwner(nest, page);
            WriteLayerPage(nest, page, capacity, image.used, image.words);
            ++image.used;
        }

        return image;
    }

    void ApplyGpuLayerImage(MultiresNest& nest, std::span<const uint32_t> words, uint32_t capacity) {
        // --- GPU の層が真実: CPU の頁の層を一度全部消してから、GPU の層の頁を写す ---
        for (uint32_t page = 0; page < nest.cells.pages.size(); ++page)
            nest_detail::ClearPageLayers(nest, page);

        for (uint32_t layerPage = 0; layerPage < capacity; ++layerPage) {
            const uint32_t header = MrLayerHeaderAddress(layerPage);
            const uint32_t page = words[header];
            const uint32_t owner = words[header + 1];
            if (page == MR_LAYER_NO_PAGE || page >= nest.cells.pages.size() || owner >= nest.blocks.size())
                continue;

            const MrBlock& block = nest.blocks[owner];
            if (block.kind == MR_BLOCK_UNUSED || block.page != page)
                continue;  // 頁を手放した持ち主の層は 0(GPU の段と同じ)

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const RxCell cell = nest_detail::LoadPageCell(nest, page, index);
                const MrCellLayers layers = ReadLayerCell(cell, words, capacity, layerPage, index);
                if (AnyLayer(layers))
                    nest_detail::StorePageCellWithLayers(nest, page, index, cell, layers);
            }
        }
    }

}  // namespace bicameral::sim
