// time_step_choose_file.h — 時間を飛ばす試作(sim/time_step_tree)の刻みを選ぶ段の入力と CPU の答えを写すファイル(T-1112)。
// time_step_tree_test の choose-dump が火事の出来事から書き、gpu_time_step_choose_test が読んで同じ入力を GPU で選び直して比べる。
// 並び(リトルエンディアン・欄ごと): 見出し(印 'TSCH'・版)→ 木の形の表(sim::TimeTreeChooseTables の単位の数・窓の格子・半径・
//   maxExponent・格子の単位・隣の始まり・隣)→ 出来事の数 → 出来事ごとに 刻み・上限 cap・選ぶ前の詰めた記録・窓の上限・選んだ e・
//   出来事の後の詰めた記録(どれも単位の数だけ)。
#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "sim/time_step_tree.h"

namespace bicameral::test {

    struct TimeStepChooseEvent {
        uint64_t tick = 0;
        int32_t cap = 0;
        std::vector<uint16_t> before;
        std::vector<uint8_t> windowCaps;
        std::vector<int8_t> next;  // -1 = 歩の途中のまま
        std::vector<uint16_t> after;
    };

    struct TimeStepChooseFile {
        sim::TimeTreeChooseTables tables;
        std::vector<TimeStepChooseEvent> events;
    };

    namespace choose_file {

        constexpr uint32_t MAGIC = 0x48435354u;  // 'TSCH'
        constexpr uint32_t VERSION = 2;          // 2: 案 (c) の looseExponents(T-1271。1 も読める = 空)

        template <typename T>
        void Put(std::ofstream& out, T value) {
            out.write(reinterpret_cast<const char*>(&value), sizeof(T));
        }

        template <typename T>
        bool Get(std::ifstream& in, T& value) {
            in.read(reinterpret_cast<char*>(&value), sizeof(T));
            return static_cast<bool>(in);
        }

        template <typename T>
        void PutVector(std::ofstream& out, const std::vector<T>& values) {
            Put(out, static_cast<uint64_t>(values.size()));
            out.write(reinterpret_cast<const char*>(values.data()),
                      static_cast<std::streamsize>(values.size() * sizeof(T)));
        }

        template <typename T>
        bool GetVector(std::ifstream& in, std::vector<T>& values) {
            uint64_t count = 0;
            if (!Get(in, count) || count > (uint64_t{1} << 32))
                return false;

            values.resize(count);
            in.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(count * sizeof(T)));
            return static_cast<bool>(in);
        }

    }  // namespace choose_file

    inline bool WriteTimeStepChoose(const std::string& path, const TimeStepChooseFile& file) {
        using namespace choose_file;
        std::ofstream out(path, std::ios::binary);
        const sim::TimeTreeChooseTables& tables = file.tables;
        Put(out, MAGIC);
        Put(out, VERSION);
        Put(out, tables.units);
        for (const uint32_t size : tables.gridSize)
            Put(out, size);

        Put(out, tables.radius);
        Put(out, tables.maxExponent);
        PutVector(out, tables.gridUnits);
        PutVector(out, tables.neighborStarts);
        PutVector(out, tables.neighbors);
        PutVector(out, tables.looseExponents);
        Put(out, static_cast<uint32_t>(file.events.size()));
        for (const TimeStepChooseEvent& event : file.events) {
            Put(out, event.tick);
            Put(out, event.cap);
            PutVector(out, event.before);
            PutVector(out, event.windowCaps);
            PutVector(out, event.next);
            PutVector(out, event.after);
        }

        return static_cast<bool>(out);
    }

    inline std::optional<TimeStepChooseFile> ReadTimeStepChoose(const std::string& path) {
        using namespace choose_file;
        std::ifstream in(path, std::ios::binary);
        uint32_t magic = 0;
        uint32_t version = 0;
        if (!Get(in, magic) || !Get(in, version) || magic != MAGIC || version < 1 || version > VERSION)
            return std::nullopt;

        TimeStepChooseFile file;
        sim::TimeTreeChooseTables& tables = file.tables;
        tables.supported = true;
        bool good = Get(in, tables.units);
        for (uint32_t& size : tables.gridSize)
            good = good && Get(in, size);

        uint32_t events = 0;
        good = good && Get(in, tables.radius) && Get(in, tables.maxExponent) && GetVector(in, tables.gridUnits) &&
               GetVector(in, tables.neighborStarts) && GetVector(in, tables.neighbors) &&
               (version < 2 || GetVector(in, tables.looseExponents)) && Get(in, events);
        if (!good)
            return std::nullopt;

        file.events.resize(events);
        for (TimeStepChooseEvent& event : file.events) {
            if (!Get(in, event.tick) || !Get(in, event.cap) || !GetVector(in, event.before) ||
                !GetVector(in, event.windowCaps) || !GetVector(in, event.next) || !GetVector(in, event.after))
                return std::nullopt;
        }

        return file;
    }

}  // namespace bicameral::test
