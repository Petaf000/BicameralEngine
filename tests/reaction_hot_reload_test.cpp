// reaction_hot_reload_test.cpp — 反応表のホットリロード(T-0139・ADR-0047)の CPU の部分。
//
// 試験の表のパッケージ(data/packages/combustion_test)を一時フォルダに写して書き換え、
//   - 変わっていなければ何もしない・書いた直後は待つ(次の Poll で同じなら読む)・コメントだけの変更は同じ版
//   - 速度を変えると新しい版の表になる(規則の速度の表が変わる)
//   - 型の誤り・文法の誤り・物質を足す Mod は落ちて、今の表のまま(誤りにファイルの名前か理由)
//   - 直せばまた読める
//   - 指紋はフォルダの場所に依らない(相対パスと中身だけ)
// を確かめる。GPU は使わない(世界への当て方は gpu_probe_sim_test の --table-swap)。
#include "script/reaction_hot_reload.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

    using namespace bicameral;
    using namespace bicameral::script;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr std::string_view BASE_PACKAGE = "combustion_test";

    std::string ReadText(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);

        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    void WriteText(const fs::path& path, std::string_view text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    // text の中の from(1 か所)を to に替えた文字列
    std::string ReplaceOnce(std::string text, std::string_view from, std::string_view to) {
        const size_t position = text.find(from);
        EXPECT(position != std::string::npos);
        if (position != std::string::npos)
            text.replace(position, from.size(), to);

        return text;
    }

    bool Contains(std::string_view text, std::string_view part) {
        return text.find(part) != std::string_view::npos;
    }

    // 2 回 Poll する(1 回目は書いたのを見て待つ、2 回目で読む)
    HotReloadResult PollTwice(ReactionTableHotReload& reload) {
        const HotReloadResult first = reload.Poll();
        EXPECT(first.state == HotReloadState::Waiting);

        return reload.Poll();
    }

    void TestReload(const fs::path& root) {
        auto initial = LoadReactionTable({.packageRoot = root});
        EXPECT(initial.has_value());
        if (!initial) {
            std::printf("  読めない: %s\n", initial.error().c_str());
            return;
        }

        const uint64_t initialVersion = initial->tableVersion;
        const std::vector<uint64_t> initialRates = initial->table.rates;
        ReactionTableHotReload reload({.packageRoot = root},
                                      std::make_shared<const LoadedReactionTable>(std::move(*initial)));
        EXPECT(reload.Poll().state == HotReloadState::Unchanged);

        // --- 速度を変える → 新しい版 ---
        const fs::path reactions = root / BASE_PACKAGE / "reactions.luau";
        const std::string original = ReadText(reactions);
        const std::string faster = ReplaceOnce(original, "a = \"3e8\"", "a = \"6e8\"");
        WriteText(reactions, faster);
        const HotReloadResult reloaded = PollTwice(reload);
        EXPECT(reloaded.state == HotReloadState::Reloaded);
        EXPECT(reloaded.table && reloaded.table->tableVersion != initialVersion);
        EXPECT(reloaded.table && reloaded.table->table.rates != initialRates);
        EXPECT(reloaded.table && reloaded.table->table.speciesNames == reload.Current()->table.speciesNames);
        EXPECT(reload.Current()->tableVersion != initialVersion);
        EXPECT(reload.Poll().state == HotReloadState::Unchanged);
        const uint64_t fasterVersion = reload.Current()->tableVersion;

        // --- コメントだけ → 同じ版(何もしない)---
        WriteText(reactions, faster + "\n-- コメントだけ足した\n");
        EXPECT(PollTwice(reload).state == HotReloadState::Unchanged);
        EXPECT(reload.Current()->tableVersion == fasterVersion);

        // --- 型の誤り → 落ちて今の表のまま(誤りにファイルの名前)---
        WriteText(reactions, ReplaceOnce(faster, "activation_energy_j_per_mol = 160000",
                                         "activation_energy_j_per_mol = \"160000x\""));
        const HotReloadResult typeError = PollTwice(reload);
        std::printf("  型の誤り → %s\n", typeError.message.c_str());
        EXPECT(typeError.state == HotReloadState::Failed);
        EXPECT(Contains(typeError.message, "reactions.luau"));
        EXPECT(reload.Current()->tableVersion == fasterVersion);

        // --- 文法の誤り ---
        WriteText(reactions, "return {");
        const HotReloadResult syntaxError = PollTwice(reload);
        std::printf("  文法の誤り → %s\n", syntaxError.message.c_str());
        EXPECT(syntaxError.state == HotReloadState::Failed);
        EXPECT(reload.Current()->tableVersion == fasterVersion);

        // --- 直す → 読める(最初の表に戻る)---
        WriteText(reactions, original);
        const HotReloadResult restored = PollTwice(reload);
        EXPECT(restored.state == HotReloadState::Reloaded);
        EXPECT(reload.Current()->tableVersion == initialVersion);

        // --- 物質を足す Mod → 今の世界には当てられない ---
        WriteText(root / "adds_hydrogen" / "package.luau", "return { format = 1, depends = { \"combustion_test\" } }");
        WriteText(root / "adds_hydrogen" / "init.luau",
                  "return { species = { hydrogen = { composition = { H = 2 }, formation_enthalpy_j_per_mol = 0, "
                  "heat_capacity_mj_per_mol_k = 28836, thermal_conductivity_mw_per_m_k = 900000 } } }");
        const HotReloadResult added = PollTwice(reload);
        std::printf("  物質を足した → %s\n", added.message.c_str());
        EXPECT(added.state == HotReloadState::Failed);
        EXPECT(Contains(added.message, "hydrogen"));
        EXPECT(reload.Current()->tableVersion == initialVersion);
    }

    // 指紋は場所に依らず、中身が変われば変わる
    void TestFingerprint(const fs::path& root) {
        const fs::path first = root / "first";
        const fs::path second = root / "elsewhere" / "second";
        fs::create_directories(first);
        fs::create_directories(second);
        fs::copy(fs::path(BICAMERAL_DATA_PACKAGES_DIR) / BASE_PACKAGE, first / BASE_PACKAGE,
                 fs::copy_options::recursive);
        fs::copy(fs::path(BICAMERAL_DATA_PACKAGES_DIR) / BASE_PACKAGE, second / BASE_PACKAGE,
                 fs::copy_options::recursive);

        EXPECT(DirectoryFingerprint(first) == DirectoryFingerprint(second));
        EXPECT(DirectoryFingerprint(first) == DirectoryFingerprint(first));
        EXPECT(DirectoryFingerprint(root / "missing") != DirectoryFingerprint(first));

        WriteText(second / BASE_PACKAGE / "extra.luau", "return {}");
        EXPECT(DirectoryFingerprint(first) != DirectoryFingerprint(second));
    }

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "bicameral_reaction_hot_reload_test";
    std::error_code ignored;
    fs::remove_all(root, ignored);

    const fs::path packages = root / "packages";
    fs::create_directories(packages);
    fs::copy(fs::path(BICAMERAL_DATA_PACKAGES_DIR) / BASE_PACKAGE, packages / BASE_PACKAGE,
             fs::copy_options::recursive);

    TestReload(packages);
    TestFingerprint(root / "fingerprint");
    fs::remove_all(root, ignored);

    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("reaction_hot_reload: すべて通過\n");

    return 0;
}
