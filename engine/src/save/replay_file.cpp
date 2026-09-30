// replay_file.cpp — 再生ファイルの読み書き。形と約束は replay_file.h。
#include "save/replay_file.h"

#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <fstream>

#include "core/aliases.h"
#include "core/unicode.h"

namespace bicameral::save {
    namespace {

        static_assert(std::endian::native == std::endian::little, "再生ファイルはリトルエンディアンで書く");

        constexpr std::array<char, 4> MAGIC = {'B', 'C', 'R', 'P'};
        constexpr uint64_t HEADER_BYTES = 64;
        constexpr uint64_t HASH_BYTES = 16;
        constexpr uint64_t ALIGNMENT = 8;
        // 壊れたファイルで巨大な確保をしないための上限(1 GiB)
        constexpr uint64_t MAX_FILE_BYTES = uint64_t{1} << 30;

        // ファイルの先頭 64 バイト(replay_file.h の表)
        // ファイルの先頭 64 バイト(並びは replay_file.h の先頭の表)
        struct Header {
            // --- 形の確認 ---
            std::array<char, 4> magic = MAGIC;               // "BCRP"
            uint32_t formatVersion = REPLAY_FORMAT_VERSION;  // 形式の版

            // --- どの世界の、どの状態から始まるか ---
            uint32_t world = 0;         // ReplayWorld
            uint32_t reserved = 0;      // 0(後ろの u64 を 8 バイトの境界に揃える)
            uint64_t tableVersion = 0;  // 反応表などの版
            uint64_t seed = 0;          // 世界の生成のシード
            uint64_t startTick = 0;     // 初期状態の刻み

            // --- 後ろに続くものの大きさ ---
            uint64_t deltaBytes = 0;    // 初期状態の差分のバイト数
            uint64_t commandCount = 0;  // コマンドの数
            uint64_t hashCount = 0;     // ハッシュの数
        };

        static_assert(sizeof(Header) == HEADER_BYTES);

        constexpr uint64_t AlignUp(uint64_t value) {
            return (value + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT;
        }

        // 見出しの数から全体の大きさ(桁あふれしないように、上限で先に切る)
        std::expected<uint64_t, std::string> ExpectedBytes(const Header& header) {
            if (header.deltaBytes > MAX_FILE_BYTES || header.commandCount > MAX_FILE_BYTES / sim::COMMAND_BYTES ||
                header.hashCount > MAX_FILE_BYTES / HASH_BYTES) {
                return std::unexpected("再生ファイルの見出しの数が大きすぎる");
            }

            return HEADER_BYTES + AlignUp(header.deltaBytes) + header.commandCount * sim::COMMAND_BYTES +
                   header.hashCount * HASH_BYTES;
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

            return {};
        }

        // ログ用のパス(fs::path::string は日本語で例外になりうるので UTF-16 から変換する)
        std::string PathText(const fs::path& path) {
            return ToUtf8(path.wstring());
        }

        void Append(std::vector<std::byte>& bytes, std::span<const std::byte> source) {
            bytes.insert(bytes.end(), source.begin(), source.end());
        }

        // offset から values の大きさだけ写す(大きさは呼ぶ前に確かめてある)
        template <typename T>
        void CopyOut(std::span<const std::byte> bytes, uint64_t offset, std::span<T> values) {
            if (values.empty())
                return;

            std::memcpy(values.data(), bytes.data() + offset, values.size_bytes());
        }

    }  // namespace

    // --- 書く ---

    std::expected<std::vector<std::byte>, std::string> SerializeReplay(const ReplayFile& replay) {
        if (auto order = CheckOrder(replay); !order)
            return std::unexpected(order.error());

        const Header header{.world = static_cast<uint32_t>(replay.world),
                            .tableVersion = replay.tableVersion,
                            .seed = replay.seed,
                            .startTick = replay.startTick,
                            .deltaBytes = replay.initialDelta.size(),
                            .commandCount = replay.commands.size(),
                            .hashCount = replay.tickHashes.size()};

        const auto total = ExpectedBytes(header);
        if (!total)
            return std::unexpected(total.error());

        std::vector<std::byte> bytes;
        bytes.reserve(*total);
        Append(bytes, std::as_bytes(std::span(&header, 1)));
        Append(bytes, replay.initialDelta);
        bytes.resize(HEADER_BYTES + AlignUp(replay.initialDelta.size()), std::byte{0});
        Append(bytes, std::as_bytes(std::span(replay.commands)));

        for (const ReplayTickHash& entry : replay.tickHashes) {
            const std::array<uint64_t, 2> words = {entry.tick, entry.hash};
            Append(bytes, std::as_bytes(std::span(words)));
        }

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

        if (header.formatVersion != REPLAY_FORMAT_VERSION) {
            return std::unexpected(std::format("再生ファイルの形式の版 {} は読めない(読めるのは {})",
                                               header.formatVersion, REPLAY_FORMAT_VERSION));
        }

        if (header.world != static_cast<uint32_t>(ReplayWorld::Probe))
            return std::unexpected(std::format("知らない世界の種類 {}", header.world));

        const auto total = ExpectedBytes(header);
        if (!total)
            return std::unexpected(total.error());

        if (*total != bytes.size())
            return std::unexpected(
                std::format("再生ファイルの大きさが合わない: 見出しから {} 実際 {}", *total, bytes.size()));

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
