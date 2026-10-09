// replay_file_test.cpp — save/replay_file(再生ファイルの形。T-0086、15 §2)を CPU だけで確かめる。
// 書いて読むと同じものに戻ること、壊れたファイル(先頭・版・大きさ・並び)を理由つきで拒否することを見る。
// 版 2(T-0170・T-0193): 反応表の中身と読んだパッケージの一覧が往復すること・表の壊れ方を拒否すること・版 1 のファイルを表の無い形で読むこと。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include "save/replay_file.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "core/aliases.h"

namespace {

    using namespace bicameral;
    using save::ReplayFile;
    using sim::Command;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    Command MakeCommand(uint64_t targetTick, uint32_t sequence, uint32_t value) {
        Command command{.targetTick = targetTick, .sequence = sequence, .type = 1, .size = 8};
        command.payload[0] = value;
        command.payload[1] = value * 3;
        command.payload[11] = 0xDEADBEEFu;  // payload の最後まで残るか

        return command;
    }

    ReplayFile MakeReplay() {
        ReplayFile replay{.tableVersion = 0x0123456789ABCDEFull, .seed = 42, .startTick = 3};
        replay.initialDelta = {std::byte{1}, std::byte{2}, std::byte{3}};  // 8 の倍数でない(埋めの確認)
        replay.commands = {MakeCommand(3, 0, 10), MakeCommand(3, 1, 11), MakeCommand(9, 2, 12)};
        replay.tickHashes = {{.tick = 4, .hash = 0xAAu}, {.tick = 5, .hash = 0xBBu}, {.tick = 12, .hash = ~0ull}};

        return replay;
    }

    // 表を 2 つ持つ記録(初期状態の表 + 差し替えた表。版の昇順)
    ReplayFile MakeReplayWithTables() {
        ReplayFile replay = MakeReplay();
        replay.tables = {{.version = replay.tableVersion,
                          .loadOrder = {"combustion_test", "mod-a"},
                          .modifiedWorld = true,
                          .content = "13-bytes-here"},
                         {.version = 0xFEDC000000000000ull, .content = "x"}};

        return replay;
    }

    // --- 往復 ---

    void TestRoundTrip() {
        const ReplayFile replay = MakeReplay();
        const auto bytes = save::SerializeReplay(replay);
        EXPECT(bytes.has_value());
        if (!bytes)
            return;

        EXPECT(bytes->size() == 64 + 8 + 3 * 64 + 3 * 16);
        const auto parsed = save::ParseReplay(*bytes);
        EXPECT(parsed.has_value() && *parsed == replay);

        // 空(コマンドもハッシュも無い)も往復する
        const ReplayFile empty;
        const auto emptyBytes = save::SerializeReplay(empty);
        EXPECT(emptyBytes.has_value() && emptyBytes->size() == 64);
        if (emptyBytes)
            EXPECT(save::ParseReplay(*emptyBytes) == empty);
    }

    void TestTablesRoundTrip() {
        const ReplayFile replay = MakeReplayWithTables();
        const auto bytes = save::SerializeReplay(replay);
        EXPECT(bytes.has_value());
        if (!bytes)
            return;

        // 表 1: 見出し 32 + 名前 (4 + 15) + (4 + 5) = 28 → 32 + 中身 13 → 16。表 2: 見出し 32 + 名前 0 + 中身 1 → 8
        EXPECT(bytes->size() == 64 + 8 + 3 * 64 + 3 * 16 + (32 + 32 + 16) + (32 + 8));
        const auto parsed = save::ParseReplay(*bytes);
        EXPECT(parsed.has_value() && *parsed == replay);
        if (!parsed)
            return;

        EXPECT(parsed->FindTable(replay.tableVersion) != nullptr &&
               parsed->FindTable(replay.tableVersion)->modifiedWorld);
        EXPECT(parsed->FindTable(0xFEDC000000000000ull) != nullptr && parsed->FindTable(1) == nullptr);
    }

    void TestFile() {
        const fs::path path = fs::temp_directory_path() / "bicameral_replay_file_test.bcreplay";
        const ReplayFile replay = MakeReplay();
        EXPECT(save::WriteReplayFile(path, replay).has_value());
        const auto read = save::ReadReplayFile(path);
        EXPECT(read.has_value() && *read == replay);
        std::error_code ignored;
        fs::remove(path, ignored);
        EXPECT(!save::ReadReplayFile(path).has_value());  // 無いファイル
    }

    // --- 拒否 ---

    void TestRejectsBrokenBytes() {
        const auto bytes = save::SerializeReplay(MakeReplay());
        if (!bytes)
            return;

        std::vector<std::byte> badMagic = *bytes;
        badMagic[0] = std::byte{'X'};
        EXPECT(!save::ParseReplay(badMagic).has_value());

        std::vector<std::byte> badVersion = *bytes;
        const uint32_t futureVersion = save::REPLAY_FORMAT_VERSION + 1;
        std::memcpy(badVersion.data() + 4, &futureVersion, 4);
        EXPECT(!save::ParseReplay(badVersion).has_value());

        std::vector<std::byte> truncated = *bytes;
        truncated.pop_back();
        EXPECT(!save::ParseReplay(truncated).has_value());
        EXPECT(!save::ParseReplay(std::span(*bytes).first(10)).has_value());

        std::vector<std::byte> hugeCount = *bytes;
        const uint64_t huge = ~0ull;
        std::memcpy(hugeCount.data() + 48, &huge, 8);  // コマンドの数
        EXPECT(!save::ParseReplay(hugeCount).has_value());
    }

    // 表の壊れ方: 途中で切れる・余りがある・見出しの数が大きすぎる
    void TestRejectsBrokenTables() {
        const auto bytes = save::SerializeReplay(MakeReplayWithTables());
        if (!bytes)
            return;

        std::vector<std::byte> truncated = *bytes;
        truncated.resize(truncated.size() - 8);
        EXPECT(!save::ParseReplay(truncated).has_value());

        std::vector<std::byte> trailing = *bytes;
        trailing.resize(trailing.size() + 8, std::byte{0});
        EXPECT(!save::ParseReplay(trailing).has_value());

        std::vector<std::byte> hugeTables = *bytes;
        const uint32_t huge = ~0u;
        std::memcpy(hugeTables.data() + 12, &huge, 4);  // 表の数
        EXPECT(!save::ParseReplay(hugeTables).has_value());

        std::vector<std::byte> hugeContent = *bytes;
        const uint64_t hugeBytes = ~0ull;
        std::memcpy(hugeContent.data() + 64 + 8 + 3 * 64 + 3 * 16 + 16, &hugeBytes, 8);  // 表 1 の中身のバイト数
        EXPECT(!save::ParseReplay(hugeContent).has_value());

        ReplayFile outOfOrder = MakeReplayWithTables();
        std::swap(outOfOrder.tables[0], outOfOrder.tables[1]);
        EXPECT(!save::SerializeReplay(outOfOrder).has_value());

        ReplayFile noInitial = MakeReplayWithTables();
        noInitial.tables.erase(noInitial.tables.begin());  // 初期状態の表が無い
        EXPECT(!save::SerializeReplay(noInitial).has_value());

        ReplayFile emptyContent = MakeReplayWithTables();
        emptyContent.tables[1].content.clear();
        EXPECT(!save::SerializeReplay(emptyContent).has_value());
    }

    // 古い形式(版 1。T-0086〜T-0139): 見出しの [12] が予約(0)で表が無い。表の無い形として読む(15 §1 の変換)
    void TestReadsVersion1() {
        const ReplayFile replay = MakeReplay();
        auto bytes = save::SerializeReplay(replay);
        if (!bytes)
            return;

        const uint32_t version1 = 1;
        std::memcpy(bytes->data() + 4, &version1, 4);
        const auto parsed = save::ParseReplay(*bytes);
        EXPECT(parsed.has_value() && *parsed == replay && parsed->tables.empty());

        // 版 1 の予約の欄が 0 でない・版 0 は読まない
        std::vector<std::byte> reserved = *bytes;
        const uint32_t one = 1;
        std::memcpy(reserved.data() + 12, &one, 4);
        EXPECT(!save::ParseReplay(reserved).has_value());

        std::vector<std::byte> version0 = *bytes;
        const uint32_t zero = 0;
        std::memcpy(version0.data() + 4, &zero, 4);
        EXPECT(!save::ParseReplay(version0).has_value());
    }

    void TestRejectsBadOrder() {
        ReplayFile commandsOutOfOrder = MakeReplay();
        std::swap(commandsOutOfOrder.commands[0], commandsOutOfOrder.commands[2]);
        EXPECT(!save::SerializeReplay(commandsOutOfOrder).has_value());

        ReplayFile sameKey = MakeReplay();
        sameKey.commands[1].sequence = sameKey.commands[0].sequence;  // 同じ (刻み, 番号) は並びが決まらない
        EXPECT(!save::SerializeReplay(sameKey).has_value());

        ReplayFile beforeStart = MakeReplay();
        beforeStart.commands[0].targetTick = beforeStart.startTick - 1;
        EXPECT(!save::SerializeReplay(beforeStart).has_value());

        ReplayFile hashesOutOfOrder = MakeReplay();
        hashesOutOfOrder.tickHashes[1].tick = hashesOutOfOrder.tickHashes[0].tick;
        EXPECT(!save::SerializeReplay(hashesOutOfOrder).has_value());
    }

}  // namespace

int main() {
    TestRoundTrip();
    TestTablesRoundTrip();
    TestFile();
    TestRejectsBrokenBytes();
    TestRejectsBrokenTables();
    TestReadsVersion1();
    TestRejectsBadOrder();
    std::printf("replay_file_test: %s\n", failureCount == 0 ? "OK" : "FAILED");

    return failureCount == 0 ? 0 : 1;
}
