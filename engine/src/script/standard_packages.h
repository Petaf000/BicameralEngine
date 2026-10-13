// standard_packages.h — 標準パッケージの棚(data/standard)と、環境ミキサーで世界に足すパッケージの一覧(T-0373・ADR-0183・D-457)。
//
// 標準パッケージ = 現実の元素・物質・反応・地球の環境などを、それぞれ独立に足せるようにした Luau のパッケージ(13 §2.6)。
// 世界の既定の読み込み(data/packages)とは別の棚にあり、足さなければ読まれない(ブランクの世界は空のまま。D-455)。
// エンジンは棚を読む・依存をたどる・足す一覧を作る仕組みだけを持ち、中身(何があるか)は持たない(D-454)。
//
// データの流れ: <exe の横>/data/standard(ビルドがリポジトリの data/standard を写す)→ ReadStandardCatalog(マニフェストだけ読む)
//   → 足す・外す(MixAdd・MixRemove。依存も一緒に)→ 足す一覧(名前)→ MixFolders → ReactionTableSource::addedPackages
//   → LoadReactionTable(ほかのパッケージと同じ読み込み・同じ表の版)。
// 足す一覧は名前の順に持つ(読む順は依存で決まる。LoadPackages)。ここは CPU だけ(原則 2)。
#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/aliases.h"

namespace bicameral::script {

    struct StandardPackage {
        std::string name;                  // フォルダの名前 = パッケージの名前
        std::string title;                 // 人に見せる名前(マニフェストの title。無ければ名前)
        std::vector<std::string> depends;  // 依存するパッケージ
        fs::path folder;
    };

    struct StandardCatalog {
        fs::path root;
        std::vector<StandardPackage> packages;  // 名前の順

        [[nodiscard]] const StandardPackage* Find(std::string_view name) const;
    };

    // 既定の棚: <exe のフォルダ>/data/standard
    [[nodiscard]] fs::path DefaultStandardPackageRoot();

    // 棚の直下のフォルダを 1 つずつ標準パッケージとして読む(マニフェストだけ。表は読まない)。
    // 棚が無い・マニフェストが読めないパッケージがあれば誤り(どのパッケージか付き)
    [[nodiscard]] std::expected<StandardCatalog, std::string> ReadStandardCatalog(const fs::path& root);

    // 足す: name と、その依存(棚の中で再帰的に)を mix に加えた一覧(名前の順・重なりなし)。棚に無い名前・依存は誤り
    [[nodiscard]] std::expected<std::vector<std::string>, std::string> MixAdd(const StandardCatalog& catalog,
                                                                              std::span<const std::string> mix,
                                                                              std::string_view name);

    // 外す: name と、mix の中でそれに依存するもの(再帰的に)を除いた一覧
    [[nodiscard]] std::vector<std::string> MixRemove(const StandardCatalog& catalog, std::span<const std::string> mix,
                                                     std::string_view name);

    // mix の中で name に依存するもの(外すと一緒に外れるもの。name は含めない。名前の順)
    [[nodiscard]] std::vector<std::string> MixDependents(const StandardCatalog& catalog,
                                                         std::span<const std::string> mix, std::string_view name);

    // 足す一覧 → 読むフォルダ(棚に無い名前・依存が足りない一覧は誤り。「a は b が要る」の形)
    [[nodiscard]] std::expected<std::vector<fs::path>, std::string> MixFolders(const StandardCatalog& catalog,
                                                                               std::span<const std::string> mix);

    // "a,b,c" → 名前の一覧(--mix。空の要素は誤り)
    [[nodiscard]] std::expected<std::vector<std::string>, std::string> ParseMixList(std::string_view text);

}  // namespace bicameral::script
