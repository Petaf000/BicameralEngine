// coupling_table.h — 流れどうしの結合(引きずり drags と押す構造 machines)の定義とベイク(T-0536・ADR-0238。
// 形は ADR-0220〔交差・結合の強さ q〕と ADR-0228〔押す構造 = 交差を構造が持つ〕。docs/design/19 §9・§13)。
//
// 一つの芯: 引きずりも押す構造も「流れ X と流れ Y の 2 × 2 の結合」で、作り手が書く結合の強さ q(−1〜1)と、
// エンジンが作る交差の係数 L_XY = q √(L_XX L_YY) で表す。違いは係数の出どころだけ(引きずり = 物質の伝導率・拡散、
// 押す構造 = 構造の費用 w と速さ P)。だから定義の型・ベイク・検査は 1 つ(CouplingDefinition・BakeCouplings・ClampCouplingQ16)。
// パッケージの書き方は 2 つの分類(drags・machines。script/coupling_package)に見えるが、同じ読み手が同じ型に読む。
//
// データの流れ: パッケージ(分類 drags・machines)→ script::ReadCouplings → CouplingDefinition(ReactionTableDefinition::couplings)
//   → BakeCouplings(BakeReactionTable の最後。物質の名前を ID に・|q| > 1 を警告して ±1 に押さえる・費用と速さを刻みの単位に)
//   → Coupling(BakedReactionTable::couplings。エディタの反応表のパネルの「結合」の欄)。
// 世界の刻みへの組み込み(気体の中の成分の拡散・構造の置き場所)はまだ(T-0186・T-0210・T-0555)。今は CPU だけの表。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace bicameral::sim {

    struct BakedReactionTable;

    constexpr int64_t COUPLING_ONE_Q16 = 65536;  // 結合の強さ q の 1(Q16)

    // どちらの分類で書いたか(見せ方の違いだけ。型・ベイク・検査は同じ)
    enum class CouplingForm : uint8_t {
        Drag,     // 分類 drags: 物質の中で、流れ X が量 Y を引きずる(どこでも効く法則)
        Machine,  // 分類 machines: 構造が蓄え X を下らせて Y を押す(置いた所だけで効く。置き場所は T-0555)
    };

    // 結合する流れの種類
    enum class CouplingQuantityKind : uint8_t {
        Heat,     // 熱の流れ(坂 = 温度。X なら熱機関: 1 − T_低 ÷ T_高)
        Work,     // 構造に蓄えた仕事(坂 = 1。押す構造の蓄え X だけ)
        Gas,      // 気体そのもの(坂 = 圧力。押す構造の Y だけ。引きずりの「運ぶ」は書かない形なので drags には無い)
        Species,  // 成分の流れ(坂 = 化学ポテンシャル)
    };

    // --- 定義(人が書く値。名前のまま)---

    struct CouplingQuantityDefinition {
        CouplingQuantityKind kind = CouplingQuantityKind::Heat;
        std::string species;  // Species のときの物質の名前
    };

    struct CouplingDefinition {
        std::string key;  // 分類の鍵(分類ごとに一意)
        CouplingForm form = CouplingForm::Drag;

        CouplingQuantityDefinition driver;  // X: drags の flow・machines の store
        CouplingQuantityDefinition driven;  // Y: drags の drags・machines の push

        // 結合の強さ q(Q16。書いた値を 0 の向きに切り捨てた値。|q| > 1 もここでは通す。押さえるのはベイク)
        int64_t couplingQ16 = 0;

        // --- drags だけ ---
        std::string withinSpecies;  // どの物質の中で効くか(空 = どこでも)

        // --- machines だけ ---
        int64_t costJoulesPerMole = 0;  // w: Y を 1 mol 押すのに使う蓄え(J/mol)
        int64_t speedWatts = 0;         // P: 負荷のない時に蓄えを使う速さ(W)
    };

    // --- ベイクしたもの(物質は ID。0 = 物質でない)---

    struct CouplingQuantity {
        CouplingQuantityKind kind = CouplingQuantityKind::Heat;
        uint32_t species = 0;
        friend bool operator==(const CouplingQuantity&, const CouplingQuantity&) = default;
    };

    struct Coupling {
        std::string key;
        CouplingForm form = CouplingForm::Drag;
        CouplingQuantity driver;
        CouplingQuantity driven;
        uint32_t withinSpecies = 0;  // drags: 効く物質(0 = どこでも)

        int64_t writtenCouplingQ16 = 0;  // 書いた q(パネルに「書いた値 → 効く値」を出す)
        int64_t couplingQ16 = 0;         // 効く q(|q| ≤ 1 に押さえた値)
        int64_t peakEfficiencyQ16 = 0;   // 効率の上限 η_max(q) = q² ÷ (1 + √(1 − q²))²(Q16。Kedem–Caplan)

        // --- machines だけ(刻みの単位。押す構造の模型 tests/push_model_test と同じ単位)---
        int64_t costQ16 = 0;       // w: Y を 1 µmol 押すのに使う蓄え(mJ。Q16)
        int64_t speedPerTick = 0;  // P: 負荷のない時に蓄えを使う速さ(mJ/刻み)
        bool disabled = false;     // 費用 ≤ 0・速さ < 0 で止めた構造(警告あり)
        friend bool operator==(const Coupling&, const Coupling&) = default;
    };

    struct BakedCouplings {
        std::vector<Coupling> couplings;    // 分類(drags → machines)・鍵の順
        std::vector<std::string> warnings;  // 押さえた値・止めた構造(BakedReactionTable::warnings へ足す)
    };

    // --- ベイクの検査(引きずりと押す構造に共通の 1 つ。ADR-0220・ADR-0228)---

    struct ClampedCoupling {
        int64_t couplingQ16 = 0;
        bool clamped = false;
    };

    // |q| > 1 は結合の行列が半正定でなくなる(坂を上る組・効率 > 1 の状態がある = 永久機関)。±1 に押さえる(D-487 ②「警告して動かす」)
    [[nodiscard]] ClampedCoupling ClampCouplingQ16(int64_t couplingQ16);

    // 効率の上限 η_max = (1 − s) ÷ (1 + s)、s = √(1 − q²)(Q16。|q| ≤ 1 に押さえた値を渡す)
    [[nodiscard]] int64_t PeakEfficiencyQ16(int64_t couplingQ16);

    // 分類と鍵の順に並べ、物質の名前を ID に直し(知らない物質は誤り)、組み合わせを確かめ(X = Y・分類で使えない種類は誤り)、
    // q を押さえ、費用と速さを刻みの単位にする。誤りは「分類.鍵: 何が違うか」
    [[nodiscard]] std::expected<BakedCouplings, std::string> BakeCouplings(
        const std::vector<CouplingDefinition>& definitions, const BakedReactionTable& table);

    // 表示の名前("heat"・"work"・"gas"・"species:<名前>"。書き方と同じ)
    [[nodiscard]] std::string CouplingQuantityName(const CouplingQuantity& quantity, const BakedReactionTable& table);

}  // namespace bicameral::sim
