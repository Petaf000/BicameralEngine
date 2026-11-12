// gpu_multires_fields.cpp — GPU の欄の置き場の像を作る・戻す(T-1252 段 2)。何をするかは gpu_multires_fields.h。
// 64bit の値は下位・上位の 2 語(multires_field_pages.hlsli の並び)。
#include "sim/gpu_multires_fields.h"

#include <format>

#include "common/multires_field_pages.hlsli"

using namespace bicameral::multires;
using namespace bicameral::state_field;

namespace bicameral::sim {

    namespace {

        void StorePair(std::span<uint32_t> words, size_t address, int64_t value) {
            words[address] = static_cast<uint32_t>(static_cast<uint64_t>(value));
            words[address + 1] = static_cast<uint32_t>(static_cast<uint64_t>(value) >> 32);
        }

        int64_t LoadPair(std::span<const uint32_t> words, size_t address) {
            return static_cast<int64_t>((uint64_t{words[address + 1]} << 32) | uint64_t{words[address]});
        }

        size_t PageValueCount(const MultiresFieldStore& store) {
            return size_t{MR_BLOCK_CELLS} * store.components;
        }

    }  // namespace

    std::expected<std::vector<uint32_t>, std::string> MakeGpuFieldImage(const MultiresNest& nest, uint32_t field,
                                                                        uint32_t capacity) {
        if (field >= nest.fields.size() || !SfOnCells(nest.fields[field].desc))
            return std::unexpected("セル・面の欄でない");

        const MultiresFieldStore& store = nest.fields[field].store;
        if (!store.sparse)
            return std::unexpected("密の欄は GPU の置き場に写さない(疎だけ。T-1252 段 2)");

        const auto top = static_cast<uint32_t>(store.pages.size() / PageValueCount(store));
        if (top > capacity)
            return std::unexpected(std::format("使った欄の頁 {} が容量 {} を超える", top, capacity));

        const uint32_t cellPages = store.cellPages;
        std::vector<uint32_t> words(MfStoreWords(cellPages, capacity, store.components), 0);
        words[MF_HEADER_CELL_PAGES] = cellPages;
        words[MF_HEADER_CAPACITY] = capacity;
        words[MF_HEADER_COMPONENTS] = store.components;
        words[MF_HEADER_BUDGET] = store.pageBudget;
        words[MF_HEADER_FREE_COUNT] = static_cast<uint32_t>(store.freePages.size());
        words[MF_HEADER_TOP] = top;
        for (uint32_t page = 0; page < cellPages; ++page)
            words[MfPageOfAddress(page)] = store.pageOf[page];

        for (uint32_t i = 0; i < store.freePages.size(); ++i)
            words[MfFreeAddress(cellPages, i)] = store.freePages[i];

        // 使った高さまでの頁(空きの頁の古い値も。CPU の頁の並びと同じ像)
        for (size_t value = 0; value < store.pages.size(); ++value)
            StorePair(words, MfValueAddress(cellPages, capacity, store.components, 0, 0, 0) + (value * 2),
                      store.pages[value]);

        return words;
    }

    void ApplyGpuFieldImage(MultiresNest& nest, uint32_t field, std::span<const uint32_t> words) {
        MultiresFieldStore& store = nest.fields[field].store;
        const uint32_t cellPages = words[MF_HEADER_CELL_PAGES];
        const uint32_t capacity = words[MF_HEADER_CAPACITY];
        FX_ASSERT(cellPages == store.cellPages && words[MF_HEADER_COMPONENTS] == store.components);
        for (uint32_t page = 0; page < cellPages; ++page)
            store.pageOf[page] = words[MfPageOfAddress(page)];

        store.freePages.resize(words[MF_HEADER_FREE_COUNT]);
        for (uint32_t i = 0; i < store.freePages.size(); ++i)
            store.freePages[i] = words[MfFreeAddress(cellPages, i)];

        store.pages.resize(size_t{words[MF_HEADER_TOP]} * PageValueCount(store));
        const size_t base = MfValueAddress(cellPages, capacity, store.components, 0, 0, 0);
        for (size_t value = 0; value < store.pages.size(); ++value)
            store.pages[value] = LoadPair(words, base + (value * 2));
    }

    NestGatherReport GpuFieldGatherReport(std::span<const uint32_t> words) {
        return {.blocks = words[MF_HEADER_WRITTEN],
                .pagesTaken = words[MF_HEADER_TAKEN],
                .expanded = words[MF_HEADER_WANTED],
                .pageShortage = words[MF_HEADER_SHORTAGE],
                .cellPageShortage = 0};
    }

    std::vector<uint32_t> MakeGpuGatherWorkImage(const NestGatherWork& work) {
        const auto worldBlocks = static_cast<uint32_t>(work.blockOf.size());
        const auto workBlocks = static_cast<uint32_t>(work.slots.size());
        std::vector<uint32_t> words(MfWorkWords(worldBlocks, workBlocks, work.components), 0);
        for (uint32_t slot = 0; slot < worldBlocks; ++slot)
            words[MfWorkBlockOfAddress(slot)] = work.blockOf[slot];

        const size_t base = MfWorkValueAddress(worldBlocks, work.components, 0, 0, 0);
        for (size_t value = 0; value < work.values.size(); ++value)
            StorePair(words, base + (value * 2), work.values[value]);

        return words;
    }

    void ApplyGpuGatherWorkImage(NestGatherWork& work, std::span<const uint32_t> words,
                                 std::vector<uint32_t>& results) {
        const auto worldBlocks = static_cast<uint32_t>(work.blockOf.size());
        results.resize(worldBlocks);
        for (uint32_t slot = 0; slot < worldBlocks; ++slot)
            results[slot] = words[MfWorkStateAddress(worldBlocks, slot)];

        const size_t base = MfWorkValueAddress(worldBlocks, work.components, 0, 0, 0);
        for (size_t value = 0; value < work.values.size(); ++value)
            work.values[value] = LoadPair(words, base + (value * 2));
    }

}  // namespace bicameral::sim
