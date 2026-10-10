// reaction_hot_reload.cpp — 反応表のホットリロード(ファイルの指紋 → 読み直し → 今の世界に当てられるか。T-0139・ADR-0047)。
#include "script/reaction_hot_reload.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

#include "script/luau_type_check.h"
#include "sim/species_remap.h"

namespace bicameral::script {

    namespace {

        // FNV-1a(64bit)。指紋は同じ PC の中で比べるだけ(ファイルにも世界にも入らない)
        constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
        constexpr uint64_t FNV_PRIME = 0x100000001b3ULL;

        uint64_t MixBytes(uint64_t hash, std::string_view bytes) {
            for (const char byte : bytes) {
                hash ^= static_cast<uint8_t>(byte);
                hash *= FNV_PRIME;
            }

            return hash;
        }

        // 中身の区切り(パスと中身が混ざって別のファイルの組と同じ指紋にならないように、長さも混ぜる)
        uint64_t MixField(uint64_t hash, std::string_view bytes) {
            hash = MixBytes(hash, std::to_string(bytes.size()));
            hash = MixBytes(hash, ":");

            return MixBytes(hash, bytes);
        }

        std::string ReadWhole(const fs::path& path, bool& ok) {
            std::ifstream file(path, std::ios::binary);
            ok = static_cast<bool>(file);
            if (!ok)
                return {};

            return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        }

        std::string NamesNotIn(const std::vector<std::string>& names, const std::vector<std::string>& other) {
            std::string text;
            for (size_t index = 1; index < names.size(); ++index) {  // 0 は使わない物質
                if (std::ranges::contains(other, names[index]))
                    continue;

                text += (text.empty() ? "" : "・") + names[index];
            }

            return text.empty() ? "なし" : text;
        }

    }  // namespace

    uint64_t DirectoryFingerprint(const fs::path& root) {
        std::error_code error;
        if (!fs::is_directory(root, error))
            return MixField(FNV_OFFSET, "フォルダが無い");

        std::vector<std::pair<std::string, fs::path>> files;
        for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            std::error_code entryError;
            if (!it->is_regular_file(entryError))
                continue;

            const std::u8string relative = it->path().lexically_relative(root).generic_u8string();
            files.emplace_back(std::string(relative.begin(), relative.end()), it->path());
        }

        if (error)
            return MixField(FNV_OFFSET, "フォルダを読めない");

        std::ranges::sort(files);
        uint64_t hash = FNV_OFFSET;
        for (const auto& [relative, path] : files) {
            bool ok = false;
            const std::string contents = ReadWhole(path, ok);
            hash = MixField(hash, relative);
            hash = MixField(hash, ok ? contents : std::string("読めない"));
        }

        return hash;
    }

    std::expected<void, std::string> CheckHotReloadCompatible(const sim::BakedReactionTable& current,
                                                              const sim::BakedReactionTable& next,
                                                              SpeciesChangePolicy policy) {
        // 付け替えられる世界: 消す物質を元素に分けて戻せるか(元素の一覧・組み立ての変化も名前で付け替える)
        if (policy == SpeciesChangePolicy::Remap) {
            const auto remap = sim::BuildSpeciesRemap(current, next);
            if (!remap)
                return std::unexpected(remap.error());

            return {};
        }

        if (current.speciesNames != next.speciesNames) {
            return std::unexpected(
                std::format("物質の一覧が変わったので今の世界には当てられない(足した: {} / 消した: "
                            "{})。物質を足す・消すときは起動し直す",
                            NamesNotIn(next.speciesNames, current.speciesNames),
                            NamesNotIn(current.speciesNames, next.speciesNames)));
        }

        if (current.elementNames != next.elementNames || current.speciesElements != next.speciesElements) {
            return std::unexpected(
                "物質の元素の組み立て(または元素の一覧)が変わったので今の世界には当てられない("
                "セルの元素が保存されなくなる)。起動し直す");
        }

        return {};
    }

    ReactionTableHotReload::ReactionTableHotReload(ReactionTableSource source,
                                                   std::shared_ptr<const LoadedReactionTable> current,
                                                   SpeciesChangePolicy policy)
        : m_source(std::move(source)), m_current(std::move(current)), m_policy(policy) {
        m_seen = Fingerprint();
        m_pending = m_seen;
    }

    // パッケージのフォルダと型の定義(型の定義を直しても読み直す)
    uint64_t ReactionTableHotReload::Fingerprint() const {
        const fs::path root = m_source.packageRoot.empty() ? DefaultPackageRoot() : m_source.packageRoot;
        const fs::path definitions = m_source.typeDefinitions.empty() ? DefaultTypeDefinitionsPath()
                                                                      : m_source.typeDefinitions;
        bool ok = false;
        const std::string definitionText = ReadWhole(definitions, ok);

        return MixField(DirectoryFingerprint(root), ok ? definitionText : std::string("型の定義を読めない"));
    }

    HotReloadResult ReactionTableHotReload::Poll() {
        const uint64_t fingerprint = Fingerprint();
        if (fingerprint == m_seen) {
            m_pending = fingerprint;
            return {};
        }

        // 書きかけ(保存の途中)を読まないよう、同じ指紋を 2 回続けて見てから読む
        if (fingerprint != m_pending) {
            m_pending = fingerprint;
            return {.state = HotReloadState::Waiting};
        }

        m_seen = fingerprint;

        return Reload();
    }

    HotReloadResult ReactionTableHotReload::Reload() {
        auto loaded = LoadReactionTable(m_source);
        if (!loaded)
            return {.state = HotReloadState::Failed, .message = std::move(loaded.error())};

        if (loaded->tableVersion == m_current->tableVersion)
            return {};  // コメント・空白・並びだけの変更(同じ法則)

        if (const auto compatible = CheckHotReloadCompatible(m_current->table, loaded->table, m_policy); !compatible)
            return {.state = HotReloadState::Failed, .message = compatible.error()};

        std::string message;
        for (const PackageRejection& rejection : loaded->rejected)
            message += std::format("{}パッケージ {} を読まなかった: {}", message.empty() ? "" : " / ",
                                   rejection.package, rejection.reason);

        m_current = std::make_shared<const LoadedReactionTable>(std::move(*loaded));

        return {.state = HotReloadState::Reloaded, .table = m_current, .message = std::move(message)};
    }

}  // namespace bicameral::script
