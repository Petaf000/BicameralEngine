// multires_cell_store.cpp — 多重解像度の世界のセルの置き場(multires_nest.h の MultiresCellStore。T-0931・ADR-0417・20 §10.1)。
// 頁のセルを「エネルギー・成分の数 + 頁の幅ぶんの成分」で持つ(前は RxCell 112 B 固定 = インライン 8 成分を毎セル)。
// 読み書きは RxCell の値で行い、読む値・GPU と同じ並びの列(DenseCellImage)・状態の要約は前と同じ(試験 multires_sparse)。
// 頁の幅は書く時に足りなければ広げ、眠る判定(FoldExact の前)で頁の中の成分の数の最大まで詰める。並びは値だけで決まり、書いた順に依らない。
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
        }

        // 頁の幅を width に並べ直す(成分の数の外は 0。width は頁の中の成分の数の最大以上)
        void RelayoutPage(MultiresCellPage& page, uint32_t width) {
            std::vector<uint32_t> species(size_t{MR_BLOCK_CELLS} * width, 0);
            std::vector<uint64_t> amounts(size_t{MR_BLOCK_CELLS} * width, 0);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                FX_ASSERT(page.counts[index] <= width);
                for (uint32_t i = 0; i < page.counts[index]; ++i) {
                    species[(size_t{index} * width) + i] = page.species[(size_t{index} * page.width) + i];
                    amounts[(size_t{index} * width) + i] = page.amounts[(size_t{index} * page.width) + i];
                }
            }

            page.width = width;
            page.species = std::move(species);
            page.amounts = std::move(amounts);
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

            // --- 書く(成分の数の外は 0 に。並びが値だけで決まるように)---
            cells.energy[index] = cell.energy;
            cells.counts[index] = static_cast<uint8_t>(cell.speciesCount);
            const size_t base = size_t{index} * cells.width;
            for (uint32_t i = 0; i < cells.width; ++i) {
                const bool used = i < cell.speciesCount;
                cells.species[base + i] = used ? cell.species[i] : 0;
                cells.amounts[base + i] = used ? cell.amounts[i] : 0;
            }
        }

        void NarrowPageCells(MultiresNest& nest, uint32_t page) {
            MultiresCellPage& cells = nest.cells.pages[page];
            if (cells.energy.empty())
                return;

            const uint32_t width = *std::ranges::max_element(cells.counts);
            if (width < cells.width)
                RelayoutPage(cells, width);
        }

    }  // namespace nest_detail

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
