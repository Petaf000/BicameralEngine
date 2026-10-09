// reaction_replay_table_test.cpp — 再生ファイルに残した反応表(T-0170・T-0193・ADR-0050)だけで、元のパッケージのフォルダが無くても
// 同じ表で再生でき、刻みごとのハッシュ列が一致することを CPU リファレンス(sim::ProbeReference。GPU とのビット一致は別のテスト)で確かめる。
//
// 流れ: 試験の表のパッケージを一時フォルダに写して読む(初期状態の表)→ 速度と熱容量を書き換えて読み直す(差し替える表)
//   → 記録: 木箱の壁に火をつけ、刻み SWAP_TICK に差し替えの印(PROBE_COMMAND_TYPE_TABLE)を入れて進め、ハッシュと表の中身を再生ファイルに書く
//   → パッケージのフォルダを消す(読めなくなったことも確かめる)
//   → 再生: 再生ファイルの表の中身だけから表を作り直し(script::RebuildReactionTable)、ReplayPlayer でコマンドを流してハッシュを突き合わせる。
// ほかに: 作り直した表が元の表とバイトで同じ・差し替えが結果を変えている(試験になっている)・壊れた中身と違う版は拒否する。
// 失敗すると失敗した条件を表示して 1 を返す(ctest が落ちる)。GPU は使わない。
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/aliases.h"
#include "save/replay_session.h"
#include "script/reaction_table_loader.h"
#include "sim/probe_sim.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    constexpr std::string_view BASE_PACKAGE = "combustion_test";
    constexpr uint64_t TOTAL_TICKS = 40;
    constexpr uint64_t SWAP_TICK = 12;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    std::string ReadText(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);

        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    // path の中の from(1 か所)を to に替える
    bool ReplaceInFile(const fs::path& path, std::string_view from, std::string_view to) {
        std::string text = ReadText(path);
        const size_t position = text.find(from);
        if (position == std::string::npos)
            return false;

        text.replace(position, from.size(), to);
        std::ofstream(path, std::ios::binary) << text;

        return true;
    }

    // 物質の一覧は同じまま、木の燃焼を 100 倍速く・窒素の熱容量を 1 割大きくする(ホットリロードで当てられる変更。
    // gpu_probe_sim_test の MakeSwappedTable と同じ考え。差し替えの後の世界が必ず変わる)
    bool ChangeTable(const fs::path& package) {
        return ReplaceInFile(package / "reactions.luau", "a = \"2e10\"", "a = \"2e12\"") &&
               ReplaceInFile(package / "species.luau", "heat_capacity_mj_per_mol_k = 29124",
                             "heat_capacity_mj_per_mol_k = 32036");
    }

    // frame_loop の --auto-ignite と同じ木箱の壁に火をつけ、刻み SWAP_TICK の始めに表を差し替える
    std::vector<ProbeCommand> MakeCommands(uint64_t swapVersion) {
        std::vector<ProbeCommand> commands = {
            MakePokeCommand(0, 0, 28, 32, PROBE_VIEW_Z), MakePokeCommand(0, 1, 28, 33, PROBE_VIEW_Z),
            MakePokeCommand(7, 2, 28, 32, PROBE_VIEW_Z), MakeTableCommand(SWAP_TICK, 3, swapVersion)};
        rng::sort(commands, CommandPrecedes);

        return commands;
    }

    // 版 → 表(差し替えの印から引く)
    using TableByVersion = std::map<uint64_t, const BakedReactionTable*>;

    // 刻みごとに commands を当てて進め、S(1)〜S(TOTAL_TICKS) のハッシュを返す。差し替えの印の版の表を tables から引く
    std::vector<save::ReplayTickHash> Run(const BakedReactionTable& initial, std::span<const ProbeCommand> commands,
                                          const TableByVersion& tables) {
        ProbeReference reference(initial);
        std::vector<save::ReplayTickHash> hashes;
        for (uint64_t tick = 0; tick < TOTAL_TICKS; ++tick) {
            const BakedReactionTable* newTable = nullptr;
            for (const ProbeCommand& command : commands) {
                if (command.targetTick != tick || command.type != PROBE_COMMAND_TYPE_TABLE)
                    continue;

                const auto found = tables.find(TableCommandVersion(command));
                EXPECT(found != tables.end());
                if (found != tables.end())
                    newTable = found->second;
            }

            reference.Advance(tick, commands, newTable);
            hashes.push_back({.tick = tick + 1, .hash = ProbeStateHash(reference.State(tick + 1))});
        }

        return hashes;
    }

    save::ReplayTable ToReplayTable(const script::LoadedReactionTable& loaded) {
        return {.version = loaded.tableVersion,
                .loadOrder = loaded.loadOrder,
                .modifiedWorld = loaded.modifiedWorld,
                .content = loaded.tableBytes};
    }

    // --- 記録 ---

    bool Record(const fs::path& packages, const fs::path& replayPath, std::vector<save::ReplayTickHash>& recorded) {
        const auto initial = script::LoadReactionTable({.packageRoot = packages});
        EXPECT(initial.has_value() && ChangeTable(packages / BASE_PACKAGE));
        const auto swapped = script::LoadReactionTable({.packageRoot = packages});
        EXPECT(swapped.has_value());
        if (!initial || !swapped) {
            std::printf("  表を読めない: %s\n", initial ? swapped.error().c_str() : initial.error().c_str());
            return false;
        }

        EXPECT(swapped->tableVersion != initial->tableVersion && !initial->tableBytes.empty());
        EXPECT(swapped->table.speciesNames == initial->table.speciesNames &&
               swapped->table.rates != initial->table.rates);

        const std::vector<ProbeCommand> commands = MakeCommands(swapped->tableVersion);
        recorded = Run(initial->table, commands, {{swapped->tableVersion, &swapped->table}});

        // 差し替えが結果を変えている(差し替えの印を除いて進めると、差し替えの後のハッシュが違う)
        std::vector<ProbeCommand> withoutSwap = commands;
        std::erase_if(withoutSwap,
                      [](const ProbeCommand& command) { return command.type == PROBE_COMMAND_TYPE_TABLE; });
        const std::vector<save::ReplayTickHash> unswapped = Run(initial->table, withoutSwap, {});
        EXPECT(unswapped[SWAP_TICK - 1] == recorded[SWAP_TICK - 1] && unswapped.back() != recorded.back());
        std::printf("  表の中身 %zu バイト・%zu バイト。差し替えなし S(%llu) = %016llx\n", initial->tableBytes.size(),
                    swapped->tableBytes.size(), static_cast<unsigned long long>(unswapped.back().tick),
                    static_cast<unsigned long long>(unswapped.back().hash));

        save::ReplayRecorder recorder;
        recorder.AddCommands(commands);
        for (const save::ReplayTickHash& entry : recorded)
            recorder.AddHash(entry.tick, entry.hash);

        // frame_loop の AttachReplayTables と同じ: 初期状態の表の版と、表の中身を版の昇順に
        save::ReplayFile replay = recorder.Build();
        replay.tableVersion = initial->tableVersion;
        replay.tables = {ToReplayTable(*initial), ToReplayTable(*swapped)};
        rng::sort(replay.tables, {}, &save::ReplayTable::version);

        const auto written = save::WriteReplayFile(replayPath, replay);
        EXPECT(written.has_value());

        return written.has_value();
    }

    // --- 再生(パッケージのフォルダ無し)---

    void Replay(const fs::path& replayPath, const std::vector<save::ReplayTickHash>& recorded) {
        auto player = save::ReplayPlayer::Load(replayPath);
        EXPECT(player.has_value());
        if (!player)
            return;

        const save::ReplayFile& file = player->File();
        EXPECT(file.tables.size() == 2 && file.FindTable(file.tableVersion) != nullptr);

        std::vector<script::LoadedReactionTable> rebuilt;
        TableByVersion tables;
        for (const save::ReplayTable& stored : file.tables) {
            auto table = script::RebuildReactionTable(stored.content, stored.version);
            EXPECT(table.has_value());
            if (!table) {
                std::printf("  作り直せない: %s\n", table.error().c_str());
                return;
            }

            EXPECT(table->tableVersion == stored.version && table->tableBytes == stored.content);
            EXPECT(stored.loadOrder == std::vector<std::string>{std::string(BASE_PACKAGE)} && !stored.modifiedWorld);
            rebuilt.push_back(std::move(*table));
        }

        for (const script::LoadedReactionTable& table : rebuilt)
            tables.emplace(table.tableVersion, &table.table);

        // 再生ファイルのコマンドを ReplayPlayer から刻みに間に合うように取り出して進め、ハッシュを突き合わせる
        const BakedReactionTable& initial = *tables.at(file.tableVersion);
        std::vector<ProbeCommand> commands;
        for (uint64_t tick = 0; tick < TOTAL_TICKS; ++tick) {
            const std::vector<ProbeCommand> taken = player->TakeCommands(tick, 64);
            commands.insert(commands.end(), taken.begin(), taken.end());
        }

        const std::vector<save::ReplayTickHash> replayed = Run(initial, commands, tables);
        for (const save::ReplayTickHash& entry : replayed)
            player->CheckHash(entry.tick, entry.hash);

        std::printf("  再生: 一致 %llu / 不一致 %llu / 全部 %zu・S(%llu) = %016llx\n",
                    static_cast<unsigned long long>(player->Matches()),
                    static_cast<unsigned long long>(player->Mismatches()), player->HashCount(),
                    static_cast<unsigned long long>(replayed.back().tick),
                    static_cast<unsigned long long>(replayed.back().hash));
        EXPECT(player->Passed() && replayed == recorded);
    }

    // --- 壊れた中身・違う版は拒否する ---

    void TestRejects(const fs::path& replayPath) {
        const auto file = save::ReadReplayFile(replayPath);
        if (!file)
            return;

        const save::ReplayTable& stored = *file->FindTable(file->tableVersion);
        std::string broken = stored.content;
        broken[broken.size() / 2] = static_cast<char>(broken[broken.size() / 2] ^ 0x01);
        const auto brokenTable = script::RebuildReactionTable(broken, stored.version);
        EXPECT(!brokenTable.has_value());
        if (!brokenTable)
            std::printf("  壊れた中身 → %s\n", brokenTable.error().c_str());

        EXPECT(!script::RebuildReactionTable(stored.content, stored.version + 1).has_value());
        EXPECT(!script::RebuildReactionTable("", 0).has_value());
    }

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "bicameral_reaction_replay_table_test";
    const fs::path packages = root / "packages";
    const fs::path replayPath = root / "table.bcreplay";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(packages, ignored);
    fs::copy(fs::path(BICAMERAL_DATA_PACKAGES_DIR) / BASE_PACKAGE, packages / BASE_PACKAGE, fs::copy_options::recursive,
             ignored);

    std::vector<save::ReplayTickHash> recorded;
    if (Record(packages, replayPath, recorded)) {
        // 元のパッケージのフォルダを消してから再生する
        fs::remove_all(packages, ignored);
        EXPECT(!fs::exists(packages) && !script::LoadReactionTable({.packageRoot = packages}).has_value());
        Replay(replayPath, recorded);
        TestRejects(replayPath);
    }

    fs::remove_all(root, ignored);
    std::printf("reaction_replay_table_test: %s\n", failureCount == 0 ? "OK" : "FAILED");

    return failureCount == 0 ? 0 : 1;
}
