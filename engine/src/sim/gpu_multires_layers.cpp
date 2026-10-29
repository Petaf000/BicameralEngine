// gpu_multires_layers.cpp — GPU の層の置き場の像を作る・戻す(T-1221・T-1240)。何をするかは gpu_multires_layers.h。
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

        // 頁 page のセルの層を層の頁 layerPage の組に書く(0 でない層だけ。前から詰める)
        void WriteLayerPage(const MultiresNest& nest, uint32_t page, uint32_t capacity, uint32_t layerPage,
                            std::span<uint32_t> words) {
            const auto pageCount = static_cast<uint32_t>(nest.cells.pages.size());
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const RxCell cell = nest_detail::LoadPageCell(nest, page, index);
                const MrCellLayers layers = nest_detail::LoadPageCellLayers(nest, page, index);
                uint32_t k = 0;
                for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                    if (layers.amounts[i] == 0)
                        continue;

                    const uint32_t address = MrLayerEntryAddress(pageCount, capacity, layerPage, index, k);
                    words[address] = cell.species[i];
                    words[address + 1] = static_cast<uint32_t>(layers.amounts[i]);
                    words[address + 2] = static_cast<uint32_t>(layers.amounts[i] >> 32);
                    ++k;
                }
            }
        }

        // 層の頁 layerPage のセル index の層を、今のセルの成分の並びに結ぶ
        MrCellLayers ReadLayerCell(const RxCell& cell, std::span<const uint32_t> words, uint32_t pageCount,
                                   uint32_t capacity, uint32_t layerPage, uint32_t index) {
            MrCellLayers layers;
            for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
                const uint32_t address = MrLayerEntryAddress(pageCount, capacity, layerPage, index, k);
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

        // 空の置き場(頁の索引・持ち主は無し・空きのスタックは全部。上から 0, 1, … の順に取れる並び)
        std::vector<uint32_t> EmptyLayerStore(uint32_t pageCount, uint32_t capacity) {
            std::vector<uint32_t> words(MrLayerStoreWords(pageCount, capacity), 0);
            std::fill_n(words.begin(), pageCount + capacity, MR_LAYER_NO_PAGE);
            for (uint32_t position = 0; position < capacity; ++position)
                words[MrLayerFreeAddress(pageCount, capacity, position)] = capacity - 1 - position;

            words[MrLayerFreeCountAddress(pageCount, capacity)] = capacity;

            return words;
        }

    }  // namespace

    std::expected<GpuLayerImage, std::string> MakeGpuLayerImage(const MultiresNest& nest, uint32_t capacity) {
        const auto pageCount = static_cast<uint32_t>(nest.cells.pages.size());
        GpuLayerImage image;
        image.words = EmptyLayerStore(pageCount, capacity);

        // --- まだの所: 一様の枠の層は写さない(T-1240 の続き)---
        if (!nest.cells.uniformLayers.Empty())
            return std::unexpected("一様の枠の層はまだ GPU に写せない(T-1240)");

        for (uint32_t page = 0; page < pageCount; ++page) {
            if (!nest_detail::PageHasLayers(nest, page))
                continue;

            if (nest.wideCells)
                return std::unexpected("溢れを使う世界の層はまだ GPU に写せない(T-1240)");

            if (image.used >= capacity)
                return std::unexpected(std::format("層の頁が足りない(層のある頁が {} を超える。T-1221)", capacity));

            // 空きのスタックの上から取る(EmptyLayerStore の並びで層の頁 0, 1, …)
            const uint32_t layerPage = image.used;
            image.words[MrLayerIndexAddress(page)] = layerPage;
            image.words[MrLayerOwnerAddress(pageCount, layerPage)] = page;
            WriteLayerPage(nest, page, capacity, layerPage, image.words);
            ++image.used;
        }

        image.words[MrLayerFreeCountAddress(pageCount, capacity)] = capacity - image.used;

        return image;
    }

    void ApplyGpuLayerImage(MultiresNest& nest, std::span<const uint32_t> words, uint32_t capacity) {
        const auto pageCount = static_cast<uint32_t>(nest.cells.pages.size());
        FX_ASSERT(words.size() == MrLayerStoreWords(pageCount, capacity));

        // --- GPU の層が真実: CPU の頁の層を一度全部消してから、索引が層の頁を指す頁へ写す ---
        for (uint32_t page = 0; page < pageCount; ++page)
            nest_detail::ClearPageLayers(nest, page);

        for (uint32_t page = 0; page < pageCount; ++page) {
            const uint32_t layerPage = words[MrLayerIndexAddress(page)];
            if (layerPage >= capacity || words[MrLayerOwnerAddress(pageCount, layerPage)] != page)
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const RxCell cell = nest_detail::LoadPageCell(nest, page, index);
                const MrCellLayers layers = ReadLayerCell(cell, words, pageCount, capacity, layerPage, index);
                if (AnyLayer(layers))
                    nest_detail::StorePageCellWithLayers(nest, page, index, cell, layers);
            }
        }
    }

}  // namespace bicameral::sim
