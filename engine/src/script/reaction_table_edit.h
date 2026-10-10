// reaction_table_edit.h — エディタの反応表のパネルの中身: 表の一覧・保存則の検査・値の Luau への書き戻し(T-0219・D-449・14 §2)。
//
// データの流れ:
//   世界が使っている表(LoadedReactionTable。起動時の表か、ホットリロードで当てた表)
//   → BuildReactionTableDocument: 表の中身(TableBytes)から定義を読み直し(Luau は走らせない)、元素・物質・規則の行と、
//     規則ごとの保存則の検査(元素の釣り合い・生成エンタルピーの差と書いた反応熱)を作る。
//     書き戻せる値は、パッケージのフォルダの .luau の中のリテラルの位置を探しておく(script/luau_literal_edit)。
//   → パネルで値を変える → WriteReactionValue: そのリテラルの字面だけを差し替えてファイルへ書く
//   → ホットリロード(ADR-0047)が読み直し、型検査・ベイクの検査を通れば刻みの境界で世界に当てる → 次の表でまた作る。
// 書き戻すのはパッケージのフォルダ(--packages。無ければ exe の横の data/packages)の中のファイル。
// ここは CPU だけ(エディタ。原則 2)。
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/aliases.h"
#include "script/reaction_table_loader.h"

namespace bicameral::script {

    enum class ReactionValueKind : uint8_t {
        Integer,  // 整数のリテラル(J/mol・mJ/(mol·K) など)
        Decimal,  // 10 進の数を文字列で書く(頻度因子 A。"2.8e19")
    };

    // パネルで変えられる 1 つの値
    struct ReactionValue {
        std::string label;                 // 欄の名前(例 "rate.a")
        std::vector<std::string> keyPath;  // 分類からの鍵の並び(例 reactions・cellulose_combustion・rate・a)
        ReactionValueKind kind = ReactionValueKind::Integer;
        int64_t minimum = 0;  // Integer の範囲(読み手 reaction_package.cpp と同じ)
        int64_t maximum = 0;
        std::string text;  // 今の値(表の中身から。A は "28e18" のような 仮数e指数)

        // --- 書き戻す所(file が空なら書き戻せない。理由は readOnly)---
        fs::path file;
        std::vector<std::string> sourceKeyPath;  // ソースの中で探す鍵の並び(keyPath か、分類を除いたもの)
        std::string readOnly;
    };

    struct ElementRow {
        std::string name;
        ReactionValue atomicMass;
    };

    struct SpeciesRow {
        std::string name;
        std::string composition;  // "C6 H10 O5"
        ReactionValue formationEnthalpy;
        ReactionValue heatCapacity;
        ReactionValue thermalConductivity;
    };

    struct RuleRow {
        std::string name;
        std::string equation;  // "cellulose + 6 oxygen → 6 carbon_dioxide + 5 water_vapor"
        ReactionValue preExponential;
        ReactionValue activationEnergy;
        std::optional<ReactionValue> declaredEnthalpy;  // 書いていなければ無し
        std::vector<ReactionValue> coefficients;        // 反応物 → 生成物の係数

        // --- 速さ(表示の計算用。表の中身と同じ値)---
        uint64_t preExponentialMantissa = 0;
        int32_t preExponentialExponent10 = 0;
        int64_t activationEnergyJoulesPerMol = 0;

        // --- 保存則の検査 ---
        std::vector<std::string> elementImbalance;  // 釣り合わない元素(例 "C: 6 → 5")。空なら釣り合う
        int64_t enthalpyJoulesPerMol = 0;           // 生成エンタルピーの差(生成物 − 反応物。負なら発熱)
    };

    struct ReactionTableDocument {
        fs::path packageRoot;  // 書き戻すフォルダ(空なら書き戻せない: 再生ファイルの表など)
        uint64_t version = 0;
        std::vector<ElementRow> elements;
        std::vector<SpeciesRow> species;
        std::vector<RuleRow> rules;
        std::vector<std::string> warnings;  // ベイクの警告(文献の反応熱との食い違いなど)
    };

    // 表から一覧を作る。書き戻す所はパッケージのフォルダのファイルから探す(読んだ順の後ろのパッケージから)
    [[nodiscard]] std::expected<ReactionTableDocument, std::string> BuildReactionTableDocument(
        const LoadedReactionTable& table);

    // text が value の欄として正しいか(整数・範囲・10 進の数)。正しければ書くリテラル
    [[nodiscard]] std::expected<std::string, std::string> ReactionValueLiteral(const ReactionValue& value,
                                                                               std::string_view text);

    // value のリテラルだけを text に替えてファイルへ書く(ほかのバイトは変えない)。同じ値なら書かない
    [[nodiscard]] std::expected<void, std::string> WriteReactionValue(const ReactionValue& value,
                                                                      std::string_view text);

    // 表の中の値を、鍵の並び(keyPath)で探す(テストと自動の確認用)
    [[nodiscard]] const ReactionValue* FindReactionValue(const ReactionTableDocument& document,
                                                         std::span<const std::string> keyPath);

}  // namespace bicameral::script
