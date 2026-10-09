// replay_file.cpp — 再生ファイルの読み書き。形と約束は replay_file.h。
#include "save/replay_file.h"

#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>

#include "core/aliases.h"
#include "core/unicode.h"

namespace bicameral::save {
    namespace {

        static_assert(std::endian::native == std::endian::little, "再生ファイルはリトルエンディアンで書く");

        constexpr std::array<char, 4> MAGIC = {'B', 'C', 'R', 'P'};
        constexpr uint64_t HEADER_BYTES = 64;
        constexpr uint64_t HASH_BYTES = 16;
        constexpr uint64_t TABLE_HEADER_BYTES = 32;
        constexpr uint64_t ALIGNMENT = 8;
        // 壊れたファイルで巨大な確保をしないための上限(1 GiB)
        constexpr uint64_t MAX_FILE_BYTES = uint64_t{1} << 30;
        // 表の数・1 つの表のパッケージの数の上限(壊れた見出しで長い繰り返しをしない)
        constexpr uint32_t MAX_TABLES = 4096;
        constexpr uint32_t MAX_PACKAGES = 4096;
        constexpr uint32_t TABLE_FLAG_MODIFIED_WORLD = 1;

        // ファイルの先頭 64 バイト(並びは replay_file.h の先頭の表)
        struct Header {
            // --- 形の確認 ---
            std::array<char, 4> magic = MAGIC;               // "BCRP"
            uint32_t formatVersion = REPLAY_FORMAT_VERSION;  // 形式の版

            // --- どの世界の、どの状態から始まるか ---
            uint32_t world = 0;         // ReplayWorld
            uint32_t tableCount = 0;    // 表の数(版 1 は予約で 0。後ろの u64 を 8 バイトの境界に揃える役も)
            uint64_t tableVersion = 0;  // 初期状態の反応表の版
            uint64_t seed = 0;          // 世界の生成のシード
            uint64_t startTick = 0;     // 初期状態の刻み

            // --- 後ろに続くものの大きさ ---
            uint64_t deltaBytes = 0;    // 初期状態の差分のバイト数
            uint64_t commandCount = 0;  // コマンドの数
            uint64_t hashCount = 0;     // ハッシュの数
        };

        static_assert(sizeof(Header) == HEADER_BYTES);

        // 表 1 つの見出し 32 バイト(replay_file.h の先頭の表)
        struct TableHeader {
            uint64_t version = 0;       // 表の版
            uint32_t flags = 0;         // TABLE_FLAG_*
            uint32_t packageCount = 0;  // 読んだパッケージの数
            uint64_t contentBytes = 0;  // 中身のバイト数
            uint64_t namesBytes = 0;    // 名前のバイト数(埋める前)
        };

        static_assert(sizeof(TableHeader) == TABLE_HEADER_BYTES);

        constexpr uint64_t AlignUp(uint64_t value) {
            return (value + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT;
        }

        // 見出しの数から、表の前までの大きさ(桁あふれしないように、上限で先に切る)
        std::expected<uint64_t, std::string> FixedBytes(const Header& header) {
            if (header.deltaBytes > MAX_FILE_BYTES || header.commandCount > MAX_FILE_BYTES / sim::COMMAND_BYTES ||
                header.hashCount > MAX_FILE_BYTES / HASH_BYTES) {
                return std::unexpected("再生ファイルの見出しの数が大きすぎる");
            }

            return HEADER_BYTES + AlignUp(header.deltaBytes) + header.commandCount * sim::COMMAND_BYTES +
                   header.hashCount * HASH_BYTES;
        }

        // 表の約束: 版の昇順(重なりなし)・中身がある・数が上限の中・表があれば初期状態の表を含む
        std::expected<void, std::string> CheckTables(const ReplayFile& replay) {
            if (replay.tables.size() > MAX_TABLES)
                return std::unexpected(std::format("表の数 {} が多すぎる(上限 {})", replay.tables.size(), MAX_TABLES));

            for (size_t index = 0; index < replay.tables.size(); ++index) {
                const ReplayTable& table = replay.tables[index];
                if (table.content.empty() || table.loadOrder.size() > MAX_PACKAGES)
                    return std::unexpected(std::format("表 {} の中身が空か、パッケージの数が多すぎる", index));

                if (index > 0 && replay.tables[index - 1].version >= table.version)
                    return std::unexpected(std::format("表 {} が版の昇順になっていない", index));
            }

            if (!replay.tables.empty() && replay.FindTable(replay.tableVersion) == nullptr) {
                return std::unexpected(
                    std::format("初期状態の表(版 {:016x})の中身が再生ファイルに無い", replay.tableVersion));
            }

            return {};
        }

        // 並びの約束: コマンドは (targetTick, sequence) の昇順、ハッシュは刻みの昇順。どちらも初期状態の刻みから
        std::expected<void, std::string> CheckOrder(const ReplayFile& replay) {
            for (size_t index = 0; index < replay.commands.size(); ++index) {
                const sim::Command& command = replay.commands[index];
                if (command.targetTick < replay.startTick) {
                    return std::unexpected(std::format("コマンド {} の刻み {} が初期状態の刻み {} より前", index,
                                                       command.targetTick, replay.startTick));
                }

                if (index > 0 && !sim::CommandPrecedes(replay.commands[index - 1], command))
                    return std::unexpected(std::format("コマンド {} が (刻み, 番号) の昇順になっていない", index));
            }

            for (size_t index = 1; index < replay.tickHashes.size(); ++index) {
                if (replay.tickHashes[index - 1].tick >= replay.tickHashes[index].tick)
                    return std::unexpected(std::format("ハッシュ {} が刻みの昇順になっていない", index));
            }

            return CheckTables(replay);
        }

        // ログ用のパス(fs::path::string は日本語で例外になりうるので UTF-16 から変換する)
        std::string PathText(const fs::path& path) {
            return ToUtf8(path.wstring());
        }

        void Append(std::vector<std::byte>& bytes, std::span<const std::byte> source) {
            bytes.insert(bytes.end(), source.begin(), source.end());
        }

        void AppendPadding(std::vector<std::byte>& bytes) {
            bytes.resize(AlignUp(bytes.size()), std::byte{0});
        }

        // 表 1 つ: 見出し → 名前(長さ u32 + バイト)→ 中身。名前と中身はそれぞれ 8 バイトの境界まで埋める
        void AppendTable(std::vector<std::byte>& bytes, const ReplayTable& table) {
            std::vector<std::byte> names;
            for (const std::string& name : table.loadOrder) {
                const auto length = static_cast<uint32_t>(name.size());
                Append(names, std::as_bytes(std::span(&length, 1)));
                Append(names, std::as_bytes(std::span(name)));
            }

            const TableHeader header{.version = table.version,
                                     .flags = table.modifiedWorld ? TABLE_FLAG_MODIFIED_WORLD : 0,
                                     .packageCount = static_cast<uint32_t>(table.loadOrder.size()),
                                     .contentBytes = table.content.size(),
                                     .namesBytes = names.size()};

            Append(bytes, std::as_bytes(std::span(&header, 1)));
            Append(bytes, names);
            AppendPadding(bytes);
            Append(bytes, std::as_bytes(std::span(table.content)));
            AppendPadding(bytes);
        }

        // 読む位置を進めながら、残りの大きさを確かめて取り出す(壊れたファイルで外へ出ない)
        class ByteReader {
        public:
            ByteReader(std::span<const std::byte> bytes, uint64_t offset) : m_bytes(bytes), m_offset(offset) {}

            [[nodiscard]] uint64_t Remaining() const { return m_bytes.size() - m_offset; }
            [[nodiscard]] bool AtEnd() const { return m_offset == m_bytes.size(); }

            // size バイトを取り出す(足りなければ空の optional)
            [[nodiscard]] std::optional<std::span<const std::byte>> Take(uint64_t size) {
                if (size > Remaining())
                    return std::nullopt;

                const auto taken = m_bytes.subspan(static_cast<size_t>(m_offset), static_cast<size_t>(size));
                m_offset += size;

                return taken;
            }

            // 8 バイトの境界まで飛ばす
            [[nodiscard]] bool SkipPadding() { return Take(AlignUp(m_offset) - m_offset).has_value(); }

        private:
            std::span<const std::byte> m_bytes;
            uint64_t m_offset = 0;
        };

        // パッケージの名前の列(長さ u32 + バイト)を namesBytes ちょうどで読む
        std::expected<std::vector<std::string>, std::string> ParseNames(std::span<const std::byte> names,
                                                                        uint32_t packageCount) {
            std::vector<std::string> loadOrder;
            ByteReader reader(names, 0);
            for (uint32_t index = 0; index < packageCount; ++index) {
                const auto lengthBytes = reader.Take(sizeof(uint32_t));
                uint32_t length = 0;
                if (lengthBytes)
                    std::memcpy(&length, lengthBytes->data(), sizeof(length));

                const auto name = lengthBytes ? reader.Take(length) : std::nullopt;
                if (!name)
                    return std::unexpected("表のパッケージの名前が途中で切れている");

                loadOrder.emplace_back(reinterpret_cast<const char*>(name->data()), name->size());
            }

            if (!reader.AtEnd())
                return std::unexpected("表のパッケージの名前の大きさが合わない");

            return loadOrder;
        }

        std::expected<ReplayTable, std::string> ParseTable(ByteReader& reader) {
            const auto headerBytes = reader.Take(TABLE_HEADER_BYTES);
            if (!headerBytes)
                return std::unexpected("表の見出しが途中で切れている");

            TableHeader header;
            std::memcpy(&header, headerBytes->data(), TABLE_HEADER_BYTES);
            if (header.packageCount > MAX_PACKAGES || (header.flags & ~TABLE_FLAG_MODIFIED_WORLD) != 0)
                return std::unexpected(std::format("表(版 {:016x})の見出しが壊れている", header.version));

            const auto names = reader.Take(header.namesBytes);
            if (!names || !reader.SkipPadding())
                return std::unexpected("表のパッケージの名前が途中で切れている");

            auto loadOrder = ParseNames(*names, header.packageCount);
            if (!loadOrder)
                return std::unexpected(loadOrder.error());

            const auto content = reader.Take(header.contentBytes);
            if (!content || !reader.SkipPadding())
                return std::unexpected(std::format("表(版 {:016x})の中身が途中で切れている", header.version));

            return ReplayTable{.version = header.version,
                               .loadOrder = std::move(*loadOrder),
                               .modifiedWorld = (header.flags & TABLE_FLAG_MODIFIED_WORLD) != 0,
                               .content = std::string(reinterpret_cast<const char*>(content->data()), content->size())};
        }

        // offset から values の大きさだけ写す(大きさは呼ぶ前に確かめてある)
        template <typename T>
        void CopyOut(std::span<const std::byte> bytes, uint64_t offset, std::span<T> values) {
            if (values.empty())
                return;

            std::memcpy(values.data(), bytes.data() + offset, values.size_bytes());
        }

    }  // namespace

    const ReplayTable* ReplayFile::FindTable(uint64_t version) const {
        const auto found = rng::find(tables, version, &ReplayTable::version);

        return found == tables.end() ? nullptr : &*found;
    }

    // --- 書く ---

    std::expected<std::vector<std::byte>, std::string> SerializeReplay(const ReplayFile& replay) {
        if (auto order = CheckOrder(replay); !order)
            return std::unexpected(order.error());

        const Header header{.world = static_cast<uint32_t>(replay.world),
                            .tableCount = static_cast<uint32_t>(replay.tables.size()),
                            .tableVersion = replay.tableVersion,
                            .seed = replay.seed,
                            .startTick = replay.startTick,
                            .deltaBytes = replay.initialDelta.size(),
                            .commandCount = replay.commands.size(),
                            .hashCount = replay.tickHashes.size()};

        const auto fixed = FixedBytes(header);
        if (!fixed)
            return std::unexpected(fixed.error());

        std::vector<std::byte> bytes;
        bytes.reserve(*fixed);
        Append(bytes, std::as_bytes(std::span(&header, 1)));
        Append(bytes, replay.initialDelta);
        bytes.resize(HEADER_BYTES + AlignUp(replay.initialDelta.size()), std::byte{0});
        Append(bytes, std::as_bytes(std::span(replay.commands)));

        for (const ReplayTickHash& entry : replay.tickHashes) {
            const std::array<uint64_t, 2> words = {entry.tick, entry.hash};
            Append(bytes, std::as_bytes(std::span(words)));
        }

        for (const ReplayTable& table : replay.tables)
            AppendTable(bytes, table);

        if (bytes.size() > MAX_FILE_BYTES)
            return std::unexpected("再生ファイルが大きすぎる");

        return bytes;
    }

    std::expected<void, std::string> WriteReplayFile(const fs::path& path, const ReplayFile& replay) {
        const auto bytes = SerializeReplay(replay);
        if (!bytes)
            return std::unexpected(bytes.error());

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        if (!file)
            return std::unexpected(std::format("再生ファイルを書けない: {}", PathText(path)));

        return {};
    }

    // --- 読む ---

    std::expected<ReplayFile, std::string> ParseReplay(std::span<const std::byte> bytes) {
        Header header;
        if (bytes.size() < HEADER_BYTES)
            return std::unexpected("再生ファイルが見出しより短い");

        std::memcpy(&header, bytes.data(), HEADER_BYTES);
        if (header.magic != MAGIC)
            return std::unexpected("再生ファイルではない(先頭が BCRP でない)");

        if (header.formatVersion < REPLAY_OLDEST_FORMAT_VERSION || header.formatVersion > REPLAY_FORMAT_VERSION) {
            return std::unexpected(std::format("再生ファイルの形式の版 {} は読めない(読めるのは {}〜{})",
                                               header.formatVersion, REPLAY_OLDEST_FORMAT_VERSION,
                                               REPLAY_FORMAT_VERSION));
        }

        // 版 1 の [12] は予約(0)。表は無い形として読む(変換。15 §1)
        if (header.formatVersion == 1 && header.tableCount != 0)
            return std::unexpected("形式の版 1 の予約の欄が 0 でない");

        if (header.tableCount > MAX_TABLES)
            return std::unexpected(std::format("表の数 {} が多すぎる(上限 {})", header.tableCount, MAX_TABLES));

        if (header.world != static_cast<uint32_t>(ReplayWorld::Probe))
            return std::unexpected(std::format("知らない世界の種類 {}", header.world));

        const auto fixed = FixedBytes(header);
        if (!fixed)
            return std::unexpected(fixed.error());

        // 表が無ければ大きさはちょうど、あれば表の分だけ後ろに続く
        if (header.tableCount == 0 ? *fixed != bytes.size() : *fixed > bytes.size())
            return std::unexpected(
                std::format("再生ファイルの大きさが合わない: 見出しから {} 実際 {}", *fixed, bytes.size()));

        ReplayFile replay{.world = static_cast<ReplayWorld>(header.world),
                          .tableVersion = header.tableVersion,
                          .seed = header.seed,
                          .startTick = header.startTick};

        replay.initialDelta.resize(header.deltaBytes);
        replay.commands.resize(header.commandCount);
        std::vector<std::array<uint64_t, 2>> hashes(header.hashCount);
        uint64_t offset = HEADER_BYTES;
        CopyOut(bytes, offset, std::span(replay.initialDelta));
        offset += AlignUp(header.deltaBytes);
        CopyOut(bytes, offset, std::span(replay.commands));
        offset += header.commandCount * sim::COMMAND_BYTES;
        CopyOut(bytes, offset, std::span(hashes));
        replay.tickHashes.reserve(hashes.size());
        for (const auto& [tick, hash] : hashes)
            replay.tickHashes.push_back({.tick = tick, .hash = hash});

        // --- 表(版 2〜)---
        ByteReader reader(bytes, *fixed);
        for (uint32_t index = 0; index < header.tableCount; ++index) {
            auto table = ParseTable(reader);
            if (!table)
                return std::unexpected(std::format("表 {}: {}", index, table.error()));

            replay.tables.push_back(std::move(*table));
        }

        if (!reader.AtEnd())
            return std::unexpected(std::format("再生ファイルの後ろに余りが {} バイトある", reader.Remaining()));

        if (auto order = CheckOrder(replay); !order)
            return std::unexpected(order.error());

        return replay;
    }

    std::expected<ReplayFile, std::string> ReadReplayFile(const fs::path& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return std::unexpected(std::format("再生ファイルを開けない: {}", PathText(path)));

        const std::streamoff size = file.tellg();
        if (size < 0 || static_cast<uint64_t>(size) > MAX_FILE_BYTES)
            return std::unexpected(std::format("再生ファイルの大きさが扱えない: {}", PathText(path)));

        std::vector<std::byte> bytes(static_cast<size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!file)
            return std::unexpected(std::format("再生ファイルを読めない: {}", PathText(path)));

        return ParseReplay(bytes);
    }

}  // namespace bicameral::save
