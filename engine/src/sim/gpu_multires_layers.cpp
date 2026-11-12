// gpu_multires_layers.cpp — GPU の層の置き場の像を作る・戻す(T-1221・T-1240)。何をするかは gpu_multires_layers.h。
// 1 セルの組は成分の並びの位置ではなく物質で結ぶ(層の頁に物質も持つ)ので、GPU の段がセルの並びを変えても層は物質についていく。
// 溢れを使う世界(T-1240 の 7 の 5)も同じ組: 溢れの成分の層(MultiresOverflowArea::layers)も物質で組に詰める。組は 1 セル・一様の枠 1 つに
// RX_MAX_CELL_SPECIES まで(0 でない層を持つ物質が 9 種以上のセル・一様の枠は写さずに理由を返す。仮【エンジン】T-1240 の 7 の 5 の詰め)。
#include "sim/gpu_multires_layers.h"

#include <algorithm>
#include <format>

#include "common/multires_layers.hlsli"
#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        // 頁 page のセル index の成分(上限の無い形)と層(同じ位置・同じ数)。溢れを使わない世界はインラインのまま
        RxWideCell LoadCellOf(const MultiresNest& nest, uint32_t page, uint32_t index) {
            if (nest.wideCells)
                return nest_detail::LoadWidePageCell(nest, page, index);

            return RxWidenCell(nest_detail::LoadPageCell(nest, page, index));
        }

        std::vector<uint64_t> LoadLayersOf(const MultiresNest& nest, uint32_t page, uint32_t index, uint32_t count) {
            if (nest.wideCells)
                return nest_detail::LoadWidePageCellLayers(nest, page, index);

            const MrCellLayers layers = nest_detail::LoadPageCellLayers(nest, page, index);

            return {layers.amounts.begin(), layers.amounts.begin() + count};
        }

        // 頁 page のセルの層を層の頁 layerPage の組に書く(0 でない層だけ。前から詰める)。組に入らないセルがあれば偽
        bool WriteLayerPage(const MultiresNest& nest, uint32_t page, uint32_t capacity, uint32_t layerPage,
                            std::span<uint32_t> words) {
            const auto pageCount = static_cast<uint32_t>(nest.cells.pages.size());
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const RxWideCell cell = LoadCellOf(nest, page, index);
                const std::vector<uint64_t> layers = LoadLayersOf(nest, page, index, cell.speciesCount);
                uint32_t k = 0;
                for (uint32_t i = 0; i < cell.speciesCount && i < layers.size(); ++i) {
                    if (layers[i] == 0)
                        continue;

                    if (k >= RX_MAX_CELL_SPECIES)
                        return false;

                    const uint32_t address = MrLayerEntryAddress(pageCount, capacity, layerPage, index, k);
                    words[address] = cell.species[i];
                    words[address + 1] = static_cast<uint32_t>(layers[i]);
                    words[address + 2] = static_cast<uint32_t>(layers[i] >> 32);
                    ++k;
                }
            }

            return true;
        }

        // 層の頁 layerPage のセル index の層を、今のセルの成分の並びに結ぶ(溢れの成分まで。位置は cell の成分と同じ)
        std::vector<uint64_t> ReadLayerCell(const RxWideCell& cell, std::span<const uint32_t> words, uint32_t pageCount,
                                            uint32_t capacity, uint32_t layerPage, uint32_t index) {
            std::vector<uint64_t> layers(cell.speciesCount, 0);
            for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
                const uint32_t address = MrLayerEntryAddress(pageCount, capacity, layerPage, index, k);
                const uint64_t layer = (uint64_t{words[address + 2]} << 32) | words[address + 1];
                if (layer == 0)
                    continue;

                for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                    if (cell.species[i] == words[address])
                        layers[i] = std::min(layer, cell.amounts[i]);
                }
            }

            return layers;
        }

        // 一様な枠 slot の層(CPU の組。物質で結ぶ生の値・0 でない層だけ)を置き場の組に書く(前から詰める)。組は 8 つまで:
        // 溢れを使う世界で 9 種以上の物質が層を持てば偽
        bool WriteUniformLayers(const MultiresNest& nest, uint32_t slot, uint32_t capacity, std::span<uint32_t> words) {
            const MultiresUniformLayers* stored = nest.cells.uniformLayers.Find(slot);
            const auto pageCount = static_cast<uint32_t>(nest.cells.pages.size());
            FX_ASSERT(stored != nullptr);
            if (stored->species.size() > RX_MAX_CELL_SPECIES)
                return false;

            for (uint32_t k = 0; k < stored->species.size(); ++k) {
                const uint32_t address = MrLayerUniformAddress(pageCount, capacity, slot, k);
                words[address] = stored->species[k];
                words[address + 1] = static_cast<uint32_t>(stored->layers[k]);
                words[address + 2] = static_cast<uint32_t>(stored->layers[k] >> 32);
            }

            return true;
        }

        // 置き場の一様な枠 slot の層を、今の一様の値の成分の並びに結ぶ(0 でない組だけ。物質がセルに無ければ 0)
        std::vector<uint64_t> ReadUniformLayers(const RxWideCell& value, std::span<const uint32_t> words,
                                                uint32_t pageCount, uint32_t capacity, uint32_t slot) {
            std::vector<uint64_t> layers(value.speciesCount, 0);
            for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
                const uint32_t address = MrLayerUniformAddress(pageCount, capacity, slot, k);
                const uint64_t layer = (uint64_t{words[address + 2]} << 32) | words[address + 1];
                if (layer == 0)
                    continue;

                for (uint32_t i = 0; i < value.speciesCount; ++i) {
                    if (value.species[i] == words[address])
                        layers[i] = std::min(layer, value.amounts[i]);
                }
            }

            return layers;
        }

        bool AnyLayer(std::span<const uint64_t> layers) {
            return std::ranges::any_of(layers, [](uint64_t layer) { return layer != 0; });
        }

        // 空の置き場(頁の索引・持ち主は無し・空きのスタックは全部。上から 0, 1, … の順に取れる並び)
        std::vector<uint32_t> EmptyLayerStore(uint32_t pageCount, uint32_t capacity, uint32_t slotCount) {
            std::vector<uint32_t> words(MrLayerStoreWords(pageCount, capacity, slotCount), 0);
            std::fill_n(words.begin(), pageCount + capacity, MR_LAYER_NO_PAGE);
            for (uint32_t position = 0; position < capacity; ++position)
                words[MrLayerFreeAddress(pageCount, capacity, position)] = capacity - 1 - position;

            words[MrLayerFreeCountAddress(pageCount, capacity)] = capacity;

            return words;
        }

    }  // namespace

    std::expected<GpuLayerImage, std::string> MakeGpuLayerImage(const MultiresNest& nest, uint32_t capacity) {
        const auto pageCount = static_cast<uint32_t>(nest.cells.pages.size());
        const auto slotCount = static_cast<uint32_t>(nest.blocks.size());
        GpuLayerImage image;
        image.words = EmptyLayerStore(pageCount, capacity, slotCount);
        if (capacity > 0 && nest.cellLayers.enabled)
            image.words[MrLayerFlagsAddress(pageCount, capacity)] = MR_LAYER_FLAG_GRAIN;  // 粒の残りは舞う粒(T-1240)

        // --- 一様の枠の層(T-1240 の 4 段目。枠ごとに決まった場所)---
        for (uint32_t slot = 0; slot < slotCount && !nest.cells.uniformLayers.Empty(); ++slot) {
            if (nest.cells.uniformLayers.Find(slot) == nullptr)
                continue;

            if (capacity == 0)
                return std::unexpected("層の頁を持たない GPU には一様の枠の層を写せない(T-1240)");

            if (!WriteUniformLayers(nest, slot, capacity, image.words))
                return std::unexpected(std::format("一様の枠 {} の層を持つ物質が {} 種を超える(T-1240 の 7 の 5)", slot,
                                                   RX_MAX_CELL_SPECIES));

            ++image.uniformSlots;
        }

        for (uint32_t page = 0; page < pageCount; ++page) {
            if (!nest_detail::PageHasLayers(nest, page))
                continue;

            if (image.used >= capacity)
                return std::unexpected(std::format("層の頁が足りない(層のある頁が {} を超える。T-1221)", capacity));

            // 空きのスタックの上から取る(EmptyLayerStore の並びで層の頁 0, 1, …)
            const uint32_t layerPage = image.used;
            image.words[MrLayerIndexAddress(page)] = layerPage;
            image.words[MrLayerOwnerAddress(pageCount, layerPage)] = page;
            if (!WriteLayerPage(nest, page, capacity, layerPage, image.words))
                return std::unexpected(std::format("頁 {} に層を持つ物質が {} 種を超えるセルがある(T-1240 の 7 の 5)",
                                                   page, RX_MAX_CELL_SPECIES));

            ++image.used;
        }

        image.words[MrLayerFreeCountAddress(pageCount, capacity)] = capacity - image.used;

        return image;
    }

    void ApplyGpuLayerImage(MultiresNest& nest, std::span<const uint32_t> words, uint32_t capacity) {
        const auto pageCount = static_cast<uint32_t>(nest.cells.pages.size());
        const auto slotCount = static_cast<uint32_t>(nest.blocks.size());
        FX_ASSERT(words.size() == MrLayerStoreWords(pageCount, capacity, slotCount));

        // --- 一様の枠の層: 一様な枠だけ、今の一様の値と結んで置き換える(頁を持つ枠・空いた枠は層なし)---
        for (uint32_t slot = 0; slot < slotCount && capacity > 0; ++slot) {
            nest_detail::ClearUniformLayers(nest, slot);
            if (!MrIsUniform(nest.blocks[slot]))
                continue;

            const RxWideCell value = nest.wideCells ? LoadWideUniform(nest, slot)
                                                    : RxWidenCell(nest.cells.uniform[slot]);
            const std::vector<uint64_t> layers = ReadUniformLayers(value, words, pageCount, capacity, slot);
            if (AnyLayer(layers))
                nest_detail::SetUniformLayers(nest, slot, value, layers);
        }

        // --- GPU の層が真実: CPU の頁の層を一度全部消してから、索引が層の頁を指す頁へ写す ---
        for (uint32_t page = 0; page < pageCount; ++page) {
            nest_detail::ClearPageLayers(nest, page);
            if (nest.wideCells && page < nest.cellOverflow.size())
                nest.cellOverflow[page].layers.clear();  // 溢れの成分の層も(T-1240 の 7 の 5)
        }

        for (uint32_t page = 0; page < pageCount; ++page) {
            const uint32_t layerPage = words[MrLayerIndexAddress(page)];
            if (layerPage >= capacity || words[MrLayerOwnerAddress(pageCount, layerPage)] != page)
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const RxWideCell cell = LoadCellOf(nest, page, index);
                const std::vector<uint64_t> layers = ReadLayerCell(cell, words, pageCount, capacity, layerPage, index);
                if (!AnyLayer(layers))
                    continue;

                if (nest.wideCells) {
                    nest_detail::StoreWidePageCellWithLayers(nest, page, index, cell, layers);
                    continue;
                }

                MrCellLayers narrow;
                std::ranges::copy(layers, narrow.amounts.begin());
                nest_detail::StorePageCellWithLayers(nest, page, index, nest_detail::LoadPageCell(nest, page, index),
                                                     narrow);
            }
        }
    }

}  // namespace bicameral::sim
