// time_step_span_file.h — 時間を飛ばす試作(sim/time_step_tree)の列の系(EquilibriumSystem)を写すファイル(T-1111)。
// time_step_tree_test の span-dump が火事の途中の系を書き、gpu_time_step_span_test が読んで同じ系を CPU と GPU で解き比べる。
// 並び(リトルエンディアン・欄ごと): 見出し(印 'TSSP'・版・系の数)→ 系ごとに e・歩の初め・V の回数・割り方・節・面・固定した面・右辺の項。
#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "sim/equilibrium.h"

namespace bicameral::test {

    struct TimeStepSpanRecord {
        int32_t exponent = 0;
        uint64_t start = 0;
        uint32_t cycles = 0;
        sim::EquilibriumSystem system;
    };

    namespace span_file {

        constexpr uint32_t MAGIC = 0x50535354u;  // 'TSSP'
        constexpr uint32_t VERSION = 1;

        template <typename T>
        void Put(std::ofstream& out, T value) {
            out.write(reinterpret_cast<const char*>(&value), sizeof(T));
        }

        template <typename T>
        bool Get(std::ifstream& in, T& value) {
            in.read(reinterpret_cast<char*>(&value), sizeof(T));
            return static_cast<bool>(in);
        }

        inline void PutWide(std::ofstream& out, fx::FxU128 value) {
            Put(out, value.hi);
            Put(out, value.lo);
        }

        inline bool GetWide(std::ifstream& in, fx::FxU128& value) {
            return Get(in, value.hi) && Get(in, value.lo);
        }

    }  // namespace span_file

    inline bool WriteTimeStepSpans(const std::string& path, const std::vector<TimeStepSpanRecord>& records) {
        using namespace span_file;
        std::ofstream out(path, std::ios::binary);
        Put(out, MAGIC);
        Put(out, VERSION);
        Put(out, static_cast<uint32_t>(records.size()));
        for (const TimeStepSpanRecord& record : records) {
            const sim::EquilibriumSystem& system = record.system;
            Put(out, record.exponent);
            Put(out, record.start);
            Put(out, record.cycles);
            Put(out, system.rounding);
            Put(out, static_cast<uint32_t>(system.nodes.size()));
            Put(out, static_cast<uint32_t>(system.faces.size()));
            Put(out, static_cast<uint32_t>(system.fixedFaces.size()));
            Put(out, static_cast<uint32_t>(system.sources.size()));
            for (const sim::EquilibriumNode& node : system.nodes) {
                Put(out, node.level);
                Put(out, node.x);
                Put(out, node.y);
                Put(out, node.z);
                Put(out, node.capacity);
            }

            for (const sim::EquilibriumFace& face : system.faces) {
                Put(out, face.fine);
                Put(out, face.coarse);
                Put(out, face.gap);
                PutWide(out, face.coefficient);
            }

            for (const sim::EquilibriumFixedFace& fixed : system.fixedFaces) {
                Put(out, fixed.node);
                PutWide(out, fixed.coefficient);
                Put(out, fixed.value);
            }

            for (const sim::EquilibriumSource& source : system.sources) {
                Put(out, source.node);
                Put(out, source.value);
                PutWide(out, source.factor);
            }
        }

        return static_cast<bool>(out);
    }

    inline std::optional<std::vector<TimeStepSpanRecord>> ReadTimeStepSpans(const std::string& path) {
        using namespace span_file;
        std::ifstream in(path, std::ios::binary);
        uint32_t magic = 0;
        uint32_t version = 0;
        uint32_t count = 0;
        if (!Get(in, magic) || !Get(in, version) || !Get(in, count) || magic != MAGIC || version != VERSION)
            return std::nullopt;

        std::vector<TimeStepSpanRecord> records(count);
        for (TimeStepSpanRecord& record : records) {
            sim::EquilibriumSystem& system = record.system;
            uint32_t nodes = 0;
            uint32_t faces = 0;
            uint32_t fixedFaces = 0;
            uint32_t sources = 0;
            if (!Get(in, record.exponent) || !Get(in, record.start) || !Get(in, record.cycles) ||
                !Get(in, system.rounding) || !Get(in, nodes) || !Get(in, faces) || !Get(in, fixedFaces) ||
                !Get(in, sources))
                return std::nullopt;

            system.nodes.resize(nodes);
            for (sim::EquilibriumNode& node : system.nodes) {
                if (!Get(in, node.level) || !Get(in, node.x) || !Get(in, node.y) || !Get(in, node.z) ||
                    !Get(in, node.capacity))
                    return std::nullopt;
            }

            system.faces.resize(faces);
            for (sim::EquilibriumFace& face : system.faces) {
                if (!Get(in, face.fine) || !Get(in, face.coarse) || !Get(in, face.gap) ||
                    !GetWide(in, face.coefficient))
                    return std::nullopt;
            }

            system.fixedFaces.resize(fixedFaces);
            for (sim::EquilibriumFixedFace& fixed : system.fixedFaces) {
                if (!Get(in, fixed.node) || !GetWide(in, fixed.coefficient) || !Get(in, fixed.value))
                    return std::nullopt;
            }

            system.sources.resize(sources);
            for (sim::EquilibriumSource& source : system.sources) {
                if (!Get(in, source.node) || !Get(in, source.value) || !GetWide(in, source.factor))
                    return std::nullopt;
            }
        }

        return records;
    }

}  // namespace bicameral::test
