// reaction_slope.h — 反応の坂(標準ギブズエネルギー ΔG°)のベイクの検査(案 A。D-487 ①②・T-0492・docs/design/19 §3)。
//
// 坂の原理(D-466)の P2「散逸の流れは坂を下る向きにだけ進む」を、反応表のベイクで規則ごとに調べる。
// 反応の向きは作り手が規則ごとに書く(案 A)。エンジンは坂を上る規則を**警告するだけで、表はそのまま当てる**(D-487 ②)。
//
// データの流れ:
//   物質の標準エントロピー(任意の欄 standard_entropy_mj_per_mol_k → BakedReactionTable::standardEntropies)と、
//   ベイクした物質の h0・比熱・規則の係数・速度の表
//   → CheckReactionSlopes: 規則ごと・温度 1 K ごとに ΔG°(T) = Σ ν (H(T) − T S(T))(H = h0 + Cp T・S = S° + Cp ln(T / 298.15)。
//     エンジンのエネルギーと同じ比熱一定の近似。触媒は両辺で消える)を整数で数え、速さが 0 でない温度だけを見る
//   → RuleSlope(BakedReactionTable::ruleSlopes。パネルの「坂」の欄)と警告の文(BakedReactionTable::warnings。頭に SLOPE_WARNING_PREFIX)
//   → ログ(起動・ホットリロード)とエディタの反応表のパネル(ホットリロードで表が変われば作り直す)。
// 捕まえられるのは「標準の状態で坂を上る温度がある」規則まで。成分しだいで釣り合いを越える行き過ぎ(T-0491 の boudouard の
// 1645〜1738 K)は刻みごとの数え上げ(開発ビルドの計器。T-0518)が要る。ここは「釣り合いに近い(成分しだいで上りうる)」の注意を出すだけ。
// GPU の表とハッシュは変えない(CPU だけ)。浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bicameral::reaction {
    struct RxRule;
}

namespace bicameral::reaction {
    struct RxRule;
}

namespace bicameral::sim {

    struct BakedReactionTable;

    // 坂の警告の文の頭(ほかのベイクの警告と見分けるため。試験とパネルが使う)
    inline constexpr std::string_view SLOPE_WARNING_PREFIX = "坂: ";

    enum class RuleSlopeState : uint8_t {
        Downhill,     // 速さが 0 でないどの温度でも標準で坂を下る
        Uphill,       // 速さが 0 でない温度のどこかで ΔG° > 0(警告)
        Unevaluated,  // 調べられない(正味の物質に標準エントロピーが無い・値が大きすぎる)
    };

    // 温度の範囲(1 K ごと。count == 0 なら範囲なし)
    struct KelvinRange {
        uint32_t lowKelvin = 0;
        uint32_t highKelvin = 0;
        uint32_t count = 0;
    };

    struct RuleSlope {
        RuleSlopeState state = RuleSlopeState::Unevaluated;
        KelvinRange uphill;  // 速さが 0 でなく ΔG° > 0 の温度
        KelvinRange
            nearBalance;  // 速さが 0 でなく坂を下るが |ΔG°| < RT ln 10^6(成分の比が 10^6 倍で逆転しうる。注意だけ)
        uint32_t balanceKelvin = 0;        // ΔG° の符号が変わる最初の温度(2〜4095 K。無ければ 0)
        int64_t gibbs300MilliJoules = 0;   // ΔG°(300 K)(mJ/mol。表示用)
        int64_t gibbs1000MilliJoules = 0;  // ΔG°(1000 K)

        // --- Unevaluated の理由 ---
        std::vector<std::string> missingEntropy;  // 標準エントロピーの無い物質(正味の係数が 0 でないもの)
        bool tooLarge = false;                    // 値が大きすぎて 64bit で数えられない
    };

    // 規則の正味の係数(生成物 − 反応物。触媒は両辺で消えるので入らない)
    struct RuleNetTerm {
        uint32_t species = 0;
        int64_t coefficient = 0;
    };

    [[nodiscard]] std::vector<RuleNetTerm> RuleNetTerms(const reaction::RxRule& rule);

    // 規則の標準の坂 ΔG°(T)(mJ/mol。添字 = K、0 K は 0)。CheckReactionSlopes と同じ値(坂の計器 sim/slope_gauge が使う)。
    // 調べられない規則(正味の物質に標準エントロピーが無い・値が大きすぎる)は空
    [[nodiscard]] std::vector<int64_t> RuleStandardGibbs(const BakedReactionTable& baked, uint32_t ruleId);

    // 規則ごとの坂(添字 = 規則 ID)。baked の物質・規則・速度の表・standardEntropies を読む
    [[nodiscard]] std::vector<RuleSlope> CheckReactionSlopes(const BakedReactionTable& baked);

    // 坂の警告の文(坂を上る規則ごとに 1 行・調べられない規則をまとめて 1 行)。頭は SLOPE_WARNING_PREFIX
    [[nodiscard]] std::vector<std::string> SlopeWarnings(const BakedReactionTable& baked,
                                                         const std::vector<RuleSlope>& slopes);

}  // namespace bicameral::sim
