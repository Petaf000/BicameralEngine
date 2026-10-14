// coupling_package.h — パッケージの分類 drags(引きずり)と machines(押す構造)を、1 つの読み手で sim::CouplingDefinition に読む
// (T-0536・ADR-0238。形は ADR-0220・ADR-0228。書き方は仮〔ユーザー未確認。QUESTIONS Q66・Q70〕)。
//
// データの流れ: パッケージのフォルダ → LoadPackages(合わせた「分類 → 鍵 → 値」)→ ReadCouplings → sim::CouplingDefinition
//   (ReactionTableDefinition::couplings)→ sim::BakeCouplings(物質を ID に・|q| > 1 を警告して押さえる)→ 反応表のパネルの「結合」。
// 書き方(1 つの鍵 = 1 つの結合。Mod は鍵を足して増やす。流れは "heat"・"work"・"gas"・"species:<物質>"):
//   drags = {
//       salt_thermal = { flow = "heat", drags = "species:salt", coupling = "0.3",   -- q(−1〜1)
//                        within = "water" },                                        -- 効く物質(省けばどこでも)
//   }
//   machines = {
//       pump = { store = "work", push = "gas", coupling = "0.9",
//                cost_j_per_mol = 3000,   -- w: 1 mol 押すのに使う蓄え(J/mol)
//                speed_w = 3000 },        -- P: 負荷のない時に蓄えを使う速さ(W)
//   }
// coupling は 10 進の文字列("0.9"・"-0.25"。誤差なしで読み、Q16 に 0 の向きへ切り捨てる)か整数(−1・0・1)。
// |q| > 1 もここでは通す(押さえて警告するのはベイク。D-487 ②)。|q| が 1000 を超える・小数の数(0.9)は形の誤り。
// 両方の分類が同じ型・同じベイク・同じ検査を通る(書き方が 2 つに見えるだけ。ADR-0238)。
// 合わせた表の分類なので表の版(TableVersion)に入り、ホットリロードで読み直される。
// 欄を変えたら、型の定義 data/types/bicameral.d.luau の Drag・Machine も変える。ここは CPU だけ(原則 2)。
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "script/luau_package.h"
#include "sim/coupling_table.h"

namespace bicameral::script {

    constexpr std::string_view DRAGS_CATEGORY = "drags";
    constexpr std::string_view MACHINES_CATEGORY = "machines";

    // 結合の強さの書き方("0.9"・"-1.5"・1)を Q16 に(0 の向きに切り捨て。|q| ≤ 1000 だけ)
    [[nodiscard]] std::expected<int64_t, std::string> ReadCouplingQ16(const ScriptValue& value, std::string_view path);

    // 分類 drags と machines を読む(どちらも無ければ空。ブランクの世界。D-455)。誤りは「パッケージ X: 分類.鍵.欄: 何が違うか」。
    // ReadReactionTableDefinition がこれを呼ぶ(T-0398 の共有の決め事)
    [[nodiscard]] std::expected<std::vector<sim::CouplingDefinition>, std::string> ReadCouplings(
        const PackageSetResult& packages);

}  // namespace bicameral::script
