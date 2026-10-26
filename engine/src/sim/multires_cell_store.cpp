// multires_cell_store.cpp — 多重解像度の世界のセルの置き場(multires_nest.h の MultiresCellStore。T-0931・ADR-0417・20 §10.1)。
// 頁のセルを「エネルギー・成分の数 + 頁の幅ぶんの成分」で持つ(前は RxCell 112 B 固定 = インライン 8 成分を毎セル)。
// 読み書きは RxCell の値で行い、読む値・GPU と同じ並びの列(DenseCellImage)・状態の要約は前と同じ(試験 multires_sparse)。
// 頁の幅は書く時に足りなければ広げ、眠る判定(FoldExact の前)で頁の中の成分の数の最大まで詰める。並びは値だけで決まり、書いた順に依らない。
// 成分ごとの層の量(T-1210。ADR-0481 の 5): 層のある頁だけ成分と同じ並びの層を持つ。成分の量だけを書く道(StorePageCell)は、
// 書く前の成分と物質で結んで 層 = min(前の層, 新しい量) に付け替える(増えた分は残りへ)。層を書くのは StorePageCellWithLayers だけ。
#include <algorithm>

#include "sim/multires_nest.h"
#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        bool IsEmptyCell(const RxCell& cell) {
            return cell.energy == 0 && cell.speciesCount == 0;
        }

        // 頁を持つ形にする(エネルギー・成分の数の並びを取る。成分は幅 0 から)
        void AllocatePage(MultiresCellPage& page) {
            page.energy.assign(MR_BLOCK_CELLS, 0);
            page.counts.assign(MR_BLOCK_CELLS, 0);
            page.width = 0;
            page.species.clear();
            page.amounts.clear();
            page.layers.clear();
        }

        // 頁の幅を width に並べ直す(成分の数の外は 0。width は頁の中の成分の数の最大以上)
        void RelayoutPage(MultiresCellPage& page, uint32_t width) {
            const bool layered = !page.layers.empty();
            std::vector<uint32_t> species(size_t{MR_BLOCK_CELLS} * width, 0);
            std::vector<uint64_t> amounts(size_t{MR_BLOCK_CELLS} * width, 0);
            std::vector<uint64_t> layers(layered ? size_t{MR_BLOCK_CELLS} * width : 0, 0);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                FX_ASSERT(page.counts[index] <= width);
                for (uint32_t i = 0; i < page.counts[index]; ++i) {
                    const size_t to = (size_t{index} * width) + i;
                    const size_t from = (size_t{index} * page.width) + i;
                    species[to] = page.species[from];
                    amounts[to] = page.amounts[from];
                    if (layered)
                        layers[to] = page.layers[from];
                }
            }

            page.width = width;
            page.species = std::move(species);
            page.amounts = std::move(amounts);
            page.layers = std::move(layers);
        }

        // 頁のセル index に成分を書く(成分の数の外は 0 に。並びが値だけで決まるように)。頁の幅は足りていること
        void WriteCell(MultiresCellPage& page, uint32_t index, const RxCell& cell) {
            page.energy[index] = cell.energy;
            page.counts[index] = static_cast<uint8_t>(cell.speciesCount);
            const size_t base = size_t{index} * page.width;
            for (uint32_t i = 0; i < page.width; ++i) {
                const bool used = i < cell.speciesCount;
                page.species[base + i] = used ? cell.species[i] : 0;
                page.amounts[base + i] = used ? cell.amounts[i] : 0;
            }
        }

        // --- 層(T-1210)---

        // 書く前の頁のセル index の層を、新しいセルの成分へ物質で結ぶ: 層 = min(前の層, 新しい量)。前に無かった物質の層は 0
        MrCellLayers RebindLayers(const MultiresCellPage& page, uint32_t index, const RxCell& cell) {
            MrCellLayers layers;
            const size_t base = size_t{index} * page.width;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                for (uint32_t j = 0; j < page.counts[index]; ++j) {
                    if (page.species[base + j] != cell.species[i])
                        continue;

                    layers.amounts[i] = std::min(page.layers[base + j], cell.amounts[i]);
                }
            }

            return layers;
        }

        // 頁のセル index の層を書く(成分の数 count の外は 0。頁は層の並びを持つこと)
        void WriteLayers(MultiresCellPage& page, uint32_t index, uint32_t count, const MrCellLayers& layers) {
            const size_t base = size_t{index} * page.width;
            for (uint32_t i = 0; i < page.width; ++i)
                page.layers[base + i] = i < count ? layers.amounts[i] : 0;
        }

        bool AnyLayer(const MrCellLayers& layers) {
            return std::ranges::any_of(layers.amounts, [](uint64_t layer) { return layer != 0; });
        }

    }  // namespace

    namespace nest_detail {

        RxCell LoadPageCell(const MultiresNest& nest, uint32_t page, uint32_t index) {
            const MultiresCellPage& cells = nest.cells.pages[page];
            RxCell cell = RxMakeEmptyCell(0);
            if (cells.energy.empty())
                return cell;

            cell.energy = cells.energy[index];
            cell.speciesCount = cells.counts[index];
            const size_t base = size_t{index} * cells.width;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                cell.species[i] = cells.species[base + i];
                cell.amounts[i] = cells.amounts[base + i];
            }

            return cell;
        }

        void StorePageCell(MultiresNest& nest, uint32_t page, uint32_t index, const RxCell& cell) {
            FX_ASSERT(cell.speciesCount <= RX_MAX_CELL_SPECIES);
            MultiresCellPage& cells = nest.cells.pages[page];

            // --- 持たない頁に空のセルを書くなら何もしない(読むと同じ)---
            if (cells.energy.empty()) {
                if (IsEmptyCell(cell))
                    return;

                AllocatePage(cells);
            }

            if (cell.speciesCount > cells.width)
                RelayoutPage(cells, cell.speciesCount);

            // --- 層の無い頁は成分だけ書く(前と同じ道)---
            if (cells.layers.empty()) {
                WriteCell(cells, index, cell);
                return;
            }

            // --- 層を物質で結んで付け替える(書く前の成分から。T-1210)---
            const MrCellLayers layers = RebindLayers(cells, index, cell);
            WriteCell(cells, index, cell);
            WriteLayers(cells, index, cell.speciesCount, layers);
        }

        void NarrowPageCells(MultiresNest& nest, uint32_t page) {
            MultiresCellPage& cells = nest.cells.pages[page];
            if (cells.energy.empty())
                return;

            const uint32_t width = *std::ranges::max_element(cells.counts);
            if (width < cells.width)
                RelayoutPage(cells, width);

            if (!PageHasLayers(nest, page))
                cells.layers.clear();  // 層が全部 0 になった頁は層の並びを持たない(読む値は同じ)
        }

        MrCellLayers LoadPageCellLayers(const MultiresNest& nest, uint32_t page, uint32_t index) {
            const MultiresCellPage& cells = nest.cells.pages[page];
            MrCellLayers layers;
            if (cells.layers.empty())
                return layers;

            const size_t base = size_t{index} * cells.width;
            for (uint32_t i = 0; i < cells.counts[index]; ++i)
                layers.amounts[i] = cells.layers[base + i];

            return layers;
        }

        void StorePageCellWithLayers(MultiresNest& nest, uint32_t page, uint32_t index, const RxCell& cell,
                                     const MrCellLayers& layers) {
            FX_ASSERT(MrLayersFit(cell, layers));
            StorePageCell(nest, page, index, cell);

            // --- 層の無い頁に 0 でない層を書く時だけ層の並びを取る ---
            MultiresCellPage& cells = nest.cells.pages[page];
            if (cells.layers.empty()) {
                if (!AnyLayer(layers))
                    return;

                cells.layers.assign(cells.amounts.size(), 0);
            }

            WriteLayers(cells, index, cell.speciesCount, layers);
        }

        bool PageHasLayers(const MultiresNest& nest, uint32_t page) {
            const std::vector<uint64_t>& layers = nest.cells.pages[page].layers;

            return std::ranges::any_of(layers, [](uint64_t layer) { return layer != 0; });
        }

        void ClearPageLayers(MultiresNest& nest, uint32_t page) {
            nest.cells.pages[page].layers.clear();
        }

        uint64_t HashCellLayers(uint64_t hash, const MultiresNest& nest, uint32_t page, uint32_t index,
                                uint64_t address) {
            const MultiresCellPage& cells = nest.cells.pages[page];
            if (cells.layers.empty())
                return hash;

            // 0 でない層だけ(層の無い世界の要約はビット単位で今と同じ)。物質で書く(頁の幅に依らない)
            const size_t base = size_t{index} * cells.width;
            for (uint32_t i = 0; i < cells.counts[index]; ++i) {
                const uint64_t layer = cells.layers[base + i];
                if (layer == 0)
                    continue;

                hash = FxHashCombine(hash, address);
                hash = FxHashCombine(hash, cells.species[base + i]);
                hash = FxHashCombine(hash, layer);
            }

            return hash;
        }

    }  // namespace nest_detail

    // --- 層(T-1210)---

    bool MrLayersFit(const RxCell& cell, const MrCellLayers& layers) {
        for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
            const bool fits = i < cell.speciesCount ? layers.amounts[i] <= cell.amounts[i] : layers.amounts[i] == 0;
            if (!fits)
                return false;
        }

        return true;
    }

    MrCellLayers LoadNestCellLayers(const MultiresNest& nest, uint32_t slot, uint32_t index) {
        const MrBlock& block = nest.blocks[slot];
        if (MrIsUniform(block))
            return {};  // 一様の枠の層は 0(1 段目。T-1220)

        return nest_detail::LoadPageCellLayers(nest, block.page, index);
    }

    void StoreNestCellWithLayers(MultiresNest& nest, uint32_t slot, uint32_t index, const RxCell& cell,
                                 const MrCellLayers& layers) {
        FX_ASSERT(!nest.wideCells);  // 溢れの成分の層は T-1220
        FX_ASSERT(!MrIsUniform(nest.blocks[slot]));
        nest_detail::StorePageCellWithLayers(nest, nest.blocks[slot].page, index, cell, layers);
    }

    uint64_t CountLayerMisfits(const MultiresNest& nest) {
        uint64_t misfits = 0;
        for (const MultiresCellPage& cells : nest.cells.pages) {
            if (cells.layers.empty())
                continue;

            if (cells.layers.size() != cells.amounts.size()) {
                misfits += 1;
                continue;
            }

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const size_t base = size_t{index} * cells.width;
                for (uint32_t i = 0; i < cells.width; ++i) {
                    const uint64_t layer = cells.layers[base + i];
                    const bool fits = i < cells.counts[index] ? layer <= cells.amounts[base + i] : layer == 0;
                    misfits += fits ? 0u : 1u;
                }
            }
        }

        return misfits;
    }

    uint64_t MeasureCellPageBytes(const MultiresCellPage& page) {
        if (page.energy.empty())
            return 0;

        const uint64_t cellBytes = uint64_t{MR_BLOCK_CELLS} * (sizeof(int64_t) + sizeof(uint8_t));
        const uint64_t componentBytes = page.species.size() * sizeof(uint32_t) + page.amounts.size() * sizeof(uint64_t);

        return cellBytes + componentBytes + (page.layers.size() * sizeof(uint64_t));
    }

    size_t DenseCellCount(const MultiresNest& nest) {
        return nest_detail::CellAddressCount(nest);
    }

    std::vector<RxCell> DenseCellImage(const MultiresNest& nest) {
        std::vector<RxCell> image = nest.cells.uniform;
        image.reserve(DenseCellCount(nest));
        for (uint32_t page = 0; page < nest.cells.pages.size(); ++page) {
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                image.push_back(nest_detail::LoadPageCell(nest, page, index));
        }

        return image;
    }

    void StoreDenseCellImage(MultiresNest& nest, std::span<const RxCell> image) {
        FX_ASSERT(image.size() == DenseCellCount(nest));
        const size_t uniformCount = nest.cells.uniform.size();
        std::copy_n(image.begin(), uniformCount, nest.cells.uniform.begin());
        for (uint32_t page = 0; page < nest.cells.pages.size(); ++page) {
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const RxCell& cell = image[uniformCount + (size_t{page} * MR_BLOCK_CELLS) + index];
                nest_detail::StorePageCell(nest, page, index, cell);
            }
        }
    }

}  // namespace bicameral::sim
