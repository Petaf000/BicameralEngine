// support_law.h — 支えの法則(docs/design/22-support-law.md §3.2。研究 R-B1・T-0871)の試作: 固い物が硬さでたわんで釣り合う式を、
// 釣り合う家族の共通部品(sim/equilibrium。C-0・ADR-0397)の使い手として解く。CPU だけ・浮動小数点なし(04 §4)。
// 範囲(1 チャット目): 縦の面(x と上)の平面・奥行 1 セル・1 つの細かさのセル(セルの大きさ cellMm で 0.25 / 0.5 / 2 m を作る)・
//   横の縮み方 0(ν = 0。22 §3.2 ① の「この宇宙の法則として宣言」の仮)。3 次元(変位 3 + 回り 3)は T-0944。
// 違う大きさのセル(T-1120): 場面の fine の印のセルを一辺 cellMm ÷ 2 の 2 × 2 に分ける(板の厚さ = 奥行は cellMm のまま)。
//   違う大きさのセルの間は「対の面」(MixedBeam::FacePair。粗いセルの面に並ぶ 2 つの細かいセルを 1 つの面としてつなぎ、ばねは面の点に置く。
//   T-1191・ADR-0485)。成分を持つ節の系では粗いセルがレベル 0・細かいセルがレベル 1(細かいほど大きい。ADR-0462。T-1104 で直した)。Coupled だけ
// 2 つの形(どちらも節 = セルの中心):
//   - PerComponent: 変位の成分ごとに独立に解く(今の C-0 の口のまま。面ごとに伸び縮みとずれのばね。22 §3.2 ①)
//   - BeamLattice: 節が変位 2 成分と回り φ(= 角 × セルの大きさ ÷ 2。長さの単位)を持ち、面ごとに梁のばね(伸び・ずれ・曲げ)を置く。
//     解き方は 2 つ(SupportSolve):
//       - Coupled(既定。T-0919): 節が 3 成分を持つ C-0 の口(equilibrium_block)で 1 つの系として解く。面の係数は 3 × 3(ずれ ⇄ 回り・
//         段の斜めの棒の x ⇄ 上の非対角も面の中)。延ばし方は剛体の動き(粗い節の変位 + 回り × 位置の差)
//       - Lagged(T-0871。比べ用): 成分ごとに C-0(スカラー)で解き、成分の間の結合は前の値を右辺の項へ遅らせる
//         (刻みごとに x → 上 → 回り の順に 1 回ずつ。釣り合いに着くまで刻みが要る)
// 単位: 変位 2^-16 nm(int64)・力 mN・係数 2^-32 mN/nm(係数 × 変位 ÷ 2^48 = mN。ImFaceFlow と同じ桁)。
// データの流れ: SupportScene(セル・固定した面・硬さ・密度)→ 成分ごとの EquilibriumSystem(節・面・固定した面)と刻みごとの右辺の項
//   (重さ・遅らせた結合)→ SolveEquilibrium → 変位(次の刻みの温めた始まり)→ 帳簿(重さ・固定した面が支える力・セルに残る力)
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "sim/equilibrium.h"
#include "sim/equilibrium_block.h"

namespace bicameral::sim {

    // セルの固定した面(地面・壁に固定した面。その向こうは動かない)
    inline constexpr uint8_t SUPPORT_CLAMP_LEFT = 1;    // −x
    inline constexpr uint8_t SUPPORT_CLAMP_RIGHT = 2;   // +x
    inline constexpr uint8_t SUPPORT_CLAMP_BOTTOM = 4;  // 下

    inline constexpr uint32_t SUPPORT_X = 0;     // 成分: 横の変位
    inline constexpr uint32_t SUPPORT_UP = 1;    // 成分: 上の変位
    inline constexpr uint32_t SUPPORT_TURN = 2;  // 成分: 回り φ(BeamLattice だけ)
    inline constexpr uint32_t SUPPORT_COMPONENTS = 3;

    // 面の残りの硬さ(Q16)の「割れていない」(R-B2・T-0872)
    inline constexpr uint32_t SUPPORT_INTACT_FULL = 65536;

    enum class SupportForm : uint8_t {
        PerComponent,  // 成分ごと(結合なし)
        BeamLattice,   // 格子の梁(回りを持つ。結合は遅らせる)
    };

    // 違う大きさのセルの間の面(T-1120 の 2 本・T-1191 の 2 本)
    enum class MixedBeam : uint8_t {
        // ④ 対のずれ(T-1191・ADR-0485。既定): ③ の面の点のばねのうち、粗いセルの面に並ぶ 2 つの細かいセルの対を、ずれは対の平均で
        //   粗いセルにつなぎ(対の横の伸び = 粗いセルが表せない歪みを止めない)、伸びは対の平均のずれの歪みを引いて読む(一様なずれで
        //   2 つの細かいセルに逆向きの偽の伸びを出さない)。曲げの力の回りは ③ のまま 2 つの細かいセルの伸びの対で運ぶ。
        //   つぎはぎの検査(一様な歪み 3 つ・回り・一様な曲げ)で面の力が全部細かい塊と同じになる。対の中の面が割れていく(残りの硬さ β)と ③ へ移る。
        //   曲げも対の平均で粗いセルにつなぐ(T-1207・ADR-0501): 対の 2 つの細かいセルの回りの差(面に沿う回りの勾配 = 曲げの梁の曲率)は
        //   粗いセルが表せないので粗いセルに掛けない(上の面の移り目が梁の中立軸に沿う所の偽の曲げ 10 % → 5 % 以内)
        FacePair,
        // ③ 面の点のばね(T-1191 の切り分け用): 伸び・ずれ・曲げのばねを細かい側の面の中心の点に置く(両側のセルの剛体の動きで面の点へ)。
        //   ずれの力は面の点で運ぶので、セルの回りの釣り合いに効くずれの力の腕は中心から面まで(同じ大きさの面と同じ)。
        //   硬さは同じ大きさの面と同じ規則: 伸びとずれ = E × 面積 ÷ 中心の間の長さ・曲げ = E I ÷ 中心の間の長さ
        FaceSpring,
        FaceLever,  // ③' ずれは面の点で運ぶが、ずれの硬さは ② の梁のまま(12 E I / L³。T-1191 の切り分け用)
        FaceOffset,  // ② 細かい側の面ごとの梁: 粗い側の端は粗いセルの中心から横へ ±細かいセル ÷ 2 の点(剛体の動き。T-1120 の既定・比べ用)
        CenterLine,     // ① 中心どうしの梁: 粗い側の端は粗いセルの中心(横のずれを見ない。比べ用)
        FacePairShear,  // ④' 対のずれだけ(曲げは面ごと。T-1191〜T-1206 の既定・T-1207 の比べ用)
    };

    // 違う大きさのセルが絡む段の斜めの棒の硬さ(T-1133・ADR-0504)。棒は段の角を挟む 2 つのセル(違う大きさ)の間に 1 本・向きは 45°。
    // 棒の端を粗いセルの中心に置く(①)か、粗いセルの角に近い細かい場所(剛体の動き)に置く(②)かは、その場所が中心から棒の向きに
    // ずれるだけなので回りは伸びに入らず、違うのは長さ = 硬さだけ(E A ÷ L。A = 一番細かいセルの棒の断面)
    enum class MixedBrace : uint8_t {
        // ① 既定: 端はセルの中心。硬さ = 同じ大きさの棒 × 細かいセルの中心の間 ÷ 中心の間(片方の端が粗いセル 2/3・両端が粗いセル 1/2)。
        //   一様な歪みで、全部細かい場面の段の角の棒と同じ力
        Center,
        Corner,  // ② 端は粗いセルの角に近い細かい場所: 長さ = 細かいセルの棒なので同じ係数(一様な歪みで力が 1.5・2 倍)
        None,    // 比べ用: 置かない(T-1133 の前とビット一致)
    };

    enum class SupportSolve : uint8_t {
        Coupled,  // 成分を持つ節で 1 つの系(T-0919。BeamLattice だけ。PerComponent は結合が無いので Lagged と同じ)
        Lagged,   // 成分ごと + 結合を遅らせる(T-0871)
    };

    struct SupportScene {
        uint32_t width = 0;   // x のセルの数
        uint32_t height = 0;  // 上のセルの数

        std::vector<uint8_t> solid;   // [x + width × up]
        std::vector<uint8_t> clamps;  // 同じ並び。SUPPORT_CLAMP_* の組

        // --- 物質(1 つ)---
        uint64_t cellMm = 500;      // セルの一辺
        uint64_t stiffnessKpa = 0;  // 硬さ(ヤング率)。x の向きの面と左右の固定した面
        uint64_t
            stiffnessUpKpa = 0;  // 上の向きの面と下の固定した面の硬さ(0 = stiffnessKpa。向きごとの硬さ Q143 の材料)
        uint64_t densityKgPerM3 = 0;  // 重さの元
        uint64_t gravityUmPerS2 = 9806650;

        // --- 割れと荷(R-B2・T-0872。割れの法則 sim/support_fracture が書く)---
        // 面の残りの硬さ(Q16。SUPPORT_INTACT_FULL = 割れていない・0 = 割れた面 = 係数 0)。[x + width × up] のセルと +軸の隣の間の面。
        // 空 = 全部割れていない(今までと同じ答え)
        std::vector<uint32_t> intactX;
        std::vector<uint32_t> intactUp;
        std::vector<int64_t> loadMn;  // セルに足す下向きの力(mN。荷を載せる)。空 = 0

        // --- 違う大きさのセル(T-1120)---
        // 同じ並び。1 = このセルを一辺 cellMm ÷ 2 の 2 × 2 のセル(全部ある)に分ける。空 = 分けない(今までとビット一致)。
        // 奥行は分けても cellMm(板の厚さは場面で 1 つ)。荷は 4 つに分ける。固定した面は向きの側の細かいセルへ。
        // 分けたセルの中の面・分けたセルどうしの面・違う大きさの面の残りの硬さは intactFineX / intactFineUp(空なら元のセルの面の値)
        std::vector<uint8_t> fine;
        // 細かい格子(一辺 cellMm ÷ 2・[fx + 2 width × fup])の面の残りの硬さ(Q16。T-1067)。細かい座標のセルと +軸の隣の間の面で、
        // どちらかが細かいセルの面(分けたセルの中・分けたセルどうし・違う大きさの面)だけが読む。空 = 分けたセルの中は割れていない・
        // ほかは元のセルの面(intactX / intactUp)の値(T-1120 とビット一致)
        std::vector<uint32_t> intactFineX;
        std::vector<uint32_t> intactFineUp;
        // 同じ細かい並び。1 = 分けたセルのうち、この細かいセルは無い(割れて分かれた。T-1067)。空 = 全部ある
        std::vector<uint8_t> fineAbsent;
    };

    struct SupportOptions {
        SupportForm form = SupportForm::BeamLattice;
        EquilibriumOptions equilibrium;  // 成分 1 つの解き(V の回数は法則の定数)
        // 段の斜めの棒(T-0944 の 2 本目。BeamLattice だけ): 角だけで接する斜めの隣のうち、間の 2 セルの片方だけがある所(階段の段)に、
        // 2 つのセルの中心を結ぶ軸の棒(係数 ka/12。斜めの向きの伸び縮みだけ)を足す。斜めの階段(アーチ・傾いた柱)が回りでしか
        // つながらず柔らかすぎる分(内径 4 m のアーチで +14 %)を埋める。真っすぐな物(段が無い)では何も変わらない。
        // 既定 true は司令塔の仮(2026-10-21・ADR-0415。係数 ka/12 は合わせ込みの値なので T-0919 の後に測り直す)
        bool stairBraces = true;
        // 棒の硬さ = ka × braceNumerator ÷ braceDenominator(既定 1/12。T-0919 で測り直す値)
        uint64_t braceNumerator = 1;
        uint64_t braceDenominator = 12;
        MixedBrace mixedBrace = MixedBrace::Center;  // 違う大きさのセルが絡む段の棒(T-1133)

        // --- 解き方(T-0919)---
        SupportSolve solve = SupportSolve::Coupled;
        // Coupled の V の回数など(法則の定数)。既定は V 2 回 + V を前処理とする共役勾配・向きを刻みをまたいで持つ
        // (D-649・T-1086・ADR-0455。持つのは支えの解きに入っている節の向きと残差 = 48 B/セル。系の形・荷が変わったら捨てる)
        EquilibriumBlockOptions blocks{.cycles = 2, .lineSearch = true, .conjugateAcrossTicks = true};

        // 違う大きさのセルの間の梁(T-1120)
        MixedBeam mixedBeam = MixedBeam::FacePair;

        // 変位と回り(温めた始まり)を C-1 の温めた始まりの欄に置く(T-0922。値の数 ≥ 成分の数。節 = セル (x, 上, 0)・レベル 0)。
        // Coupled は blocks.warmStart、Lagged は成分 c を値 component + c に。既定は置かない(法則の控えだけ)
        EquilibriumWarmStart warmStart;
        // 刻みをまたぐ共役勾配の向きと残差を C-1 の温めた始まりの欄に置く(D-649。値の数 ≥ 成分の数。Coupled だけ)。
        // 置けば保存点とハッシュに入る。既定は置かない(法則の控えだけ)。欄は眠る時に捨てる印で登録する
        // (RegisterWarmStartField の discardOnSleep = true。起きた刻みは形が変わるので使わない。ADR-0469 の 2-3・T-1159)
        EquilibriumWarmStart conjugateDirection;
        EquilibriumWarmStart conjugateResidual;

        // 掘る・割れるなどで系が変わった後も、値で固定して閉じずに全部を解く(ADR-0480。遠くは粗いセルのまま一緒に解く。
        // T-1171 の 1 本目の値で固定する閉じた領域は T-1192 で外した。C-0 の閉じた領域〔T-1087〕は土台に残る)
    };

    // 帳簿(上向きの力。mN)
    struct SupportLedger {
        int64_t weightMn = 0;    // 重さの和(下向きを正で数える)
        int64_t reactionMn = 0;  // 固定した面が支える力の和(上向きが正)
        int64_t netMn = 0;  // セルごとの上向きの力(重さ・面の力・固定した面の力)の和。面の力は両側に同じ値で当たるので
                            // netMn == reactionMn − weightMn が解の精度によらず成り立つ(帳簿)
        uint64_t worstCellMn = 0;  // セルに残る上向きの力の絶対値の最大(釣り合いの誤差)
    };

    // 面の力(面 1 つ。セル (x, up) と +軸の隣の間。mN)。表面の引きの応力(割れの判定 R-B2 が読む)は側の力 ÷ 面積 dx²
    // (梁の N/A ± 6M/(A·dx)。M = 曲げの力の回り。1 セルの厚さで梁の式と一致・T-0944)
    struct SupportFaceForces {
        int64_t axialMn = 0;     // 軸の力 N(引きが正)
        int64_t shearMn = 0;     // ずれの力(横の向きの正へ +軸の側のセルを押し返す向き)
        int64_t lowSideMn = 0;   // −横の側の縁の力(x の面 = 下の縁・上の面 = −x の縁)。N ± 6M/dx
        int64_t highSideMn = 0;  // +横の側の縁の力(x の面 = 上の縁・上の面 = +x の縁)
        int64_t energyPj = 0;    // 面の梁に蓄えたたわみのエネルギー(pJ。伸び・ずれ・曲げの和。割れの帳簿 R-B2 が読む)
        uint32_t intact = 0;     // 面の残りの硬さ(Q16。力とエネルギーはこの硬さでの値)
    };

    // 世界のデータの V の回数(環境の balance_cycles.support。T-0955)を 1 つの系(blocks)と成分ごとの解き(equilibrium)の両方へ
    inline void ApplySupportCycles(uint32_t cycles, SupportOptions& options) {
        options.blocks.cycles = cycles;
        options.equilibrium.solver.cycles = cycles;
    }

    class SupportLaw {
    public:
        SupportLaw(const SupportScene& scene, const SupportOptions& options);

        // 掘った後など: before と同じ座標のセルの値を温めた始まりにする(無いセルは 0)。温めた始まりを欄に置く時(warmStart)は
        // 欄が答えの元なので使わない(掘った所の欄は木の操作が持つ。T-0922)。刻みをまたぐ共役勾配の向きは捨てる(D-649)
        void CarryFrom(const SupportLaw& before);

        // 1 刻み(成分ごとに 1 回ずつ解く)
        void Step();

        // --- 読む ---
        // セル (x, up) の成分 component の値(2^-16 nm)。セルが無ければ 0(分けたセルも 0。細かいセルは FineValue)
        int64_t Value(uint32_t x, uint32_t up, uint32_t component) const;
        // 細かい座標 (fx, fup)(一辺 cellMm ÷ 2 の格子)にあるセルの値: 分けたセルの中なら細かいセルの値、分けていないセルなら
        // そのセルの値(中心の値。回り φ はそのセルの単位)。無ければ 0(T-1120)
        int64_t FineValue(uint32_t fx, uint32_t fup, uint32_t component) const;
        // 細かい座標のセルが細かいセルか(分けたセルの中)
        bool IsFine(uint32_t fx, uint32_t fup) const;
        bool Mixed() const { return m_mixed; }
        SupportLedger Ledger() const;
        // セル (x, up) と +軸(0 = x・1 = 上)の隣の間の面の力。面が無い(どちらかのセルが無い)・成分ごとの形なら全部 0
        SupportFaceForces FaceForces(uint32_t x, uint32_t up, uint32_t axis) const;
        // 細かい座標 (fx, fup) のセルと +軸の隣の間の面の力(T-1067)。どちらかが細かいセルなら細かい面(幅 cellMm ÷ 2。
        // 違う大きさの面は細かい側の面の位置の切り口)、両方が分けていない違うセルなら粗い面(FaceForces と同じ。幅 cellMm)、
        // 同じ分けていないセルの中・面が無いなら全部 0
        SupportFaceForces FineFaceForces(uint32_t fx, uint32_t fup, uint32_t axis) const;
        uint64_t Hash() const;
        // 前の Step で節の値を解いた回数(全部の段。Lagged は成分ごとの和。費用の比べ)
        uint64_t LastNodeUpdates() const { return m_lastNodeUpdates; }
        // 前の Step で未知数にした節の数(支えは閉じない〔ADR-0480〕ので節の全部)
        uint32_t LastUnknowns() const { return m_lastUnknowns; }
        uint32_t CellCount() const { return (uint32_t)m_cells.size(); }

        // 成分を持つ節の系と段と今の値(Coupled の時。GPU 版〔gpu_equilibrium_block。T-0970〕が同じ系を解いて比べる)
        const EquilibriumBlockSystem& BlockSystem() const { return m_blockSystem; }
        const EquilibriumBlockGrid& BlockGrid() const { return m_blockGrid; }
        const std::vector<int64_t>& BlockValues() const { return m_blockState.values; }

    private:
        struct Bond {
            uint32_t low = 0;   // −軸の側のセル
            uint32_t high = 0;  // +軸の側のセル
            uint32_t axis = 0;  // 0 = x・1 = 上
            uint64_t unit = 0;  // b(面の残りの硬さを掛けた後)
            uint32_t intact = SUPPORT_INTACT_FULL;
        };

        // 段の斜めの棒: low = 下のセル・high = 斜め上のセル・across = 横の向き(+1 = +x へ・−1 = −x へ)
        struct Brace {
            uint32_t low = 0;
            uint32_t high = 0;
            int32_t across = 0;
            uint64_t unit = 0;  // 式の係数(間の面の残りの硬さの小さい方を掛けた後)
        };

        struct Clamp {
            uint32_t cell = 0;
            uint32_t axis = 0;
            int32_t side = 0;  // −1 = −軸の面・+1 = +軸の面
        };

        // 違う大きさの梁のエネルギーの項 ½ c (g_low · x_low + g_high · x_high)²(T-1120。帳簿が上の力を読む)
        struct MixedTerm {
            uint32_t low = 0;
            uint32_t high = 0;
            uint64_t coefficient = 0;
            std::array<int64_t, SUPPORT_COMPONENTS> gradientLow{};
            std::array<int64_t, SUPPORT_COMPONENTS> gradientHigh{};
            uint32_t intact = SUPPORT_INTACT_FULL;
            bool negative = false;  // エネルギーが −½ c (…)²(対のずれの項。T-1191)
        };

        // 3 つの節の項 ½ c (Σ g · x)²(対のずれの伸び。nodes = 粗いセル・自分の細かいセル・対の相手の細かいセル。T-1191)
        struct TripleTerm {
            std::array<uint32_t, 3> nodes{};
            std::array<std::array<int64_t, SUPPORT_COMPONENTS>, 3> gradients{};
            uint64_t coefficient = 0;
        };

        // 違う大きさの面 1 つ(項 3 つの組。T-1191): 対として読む割合 β と β の分の硬さ
        struct MixedGroup {
            uint32_t beta = 0;           // Q16(0 = 面ごとの ③。FacePair で対の中の面と両方の面が割れていなければ全部)
            uint64_t axialPair = 0;      // β の分の伸びの係数(対の平均のずれの歪みを引いて読む項 = TripleTerm)
            uint64_t shearPair = 0;      // β の分のずれの係数(対の負の項の重み)
            uint64_t bendPair = 0;       // β の分の曲げの係数(対の曲げの差の負の項の重み。T-1207)
            uint32_t pair = UINT32_MAX;  // m_mixedPairs の番号(無ければ UINT32_MAX)
            bool upper = false;          // 対の +横の側(U)か
        };

        // 粗いセルの面に並ぶ 2 つの細かいセルの対(L = −横・U = +横)。ずれの負の項は m_pairTerms[pairTerm]・曲げの差の負の項は
        // m_pairTerms[pairTerm + 1](FacePairShear では係数 0。T-1207)・
        // 伸びの 3 つの節の項は m_tripleTerms[firstTriple](L)と +1(U)
        struct MixedPair {
            uint32_t lower = 0;  // L の面の最初の項
            uint32_t upper = 0;  // U の面の最初の項
            uint32_t pairTerm = 0;
            uint32_t firstTriple = 0;
        };

        // 遅らせた結合: 行の成分の式に K × (列の成分の値) が入る
        struct Lag {
            uint32_t row = 0;
            uint32_t column = 0;
            uint32_t columnComponent = 0;
            bool negative = false;   // K < 0
            uint64_t magnitude = 0;  // |K|(2^-32 mN/nm)
        };

        void Build();
        void BuildCells();
        void BuildMixedCells();
        void BuildMixedBonds();
        void AddFineClamps(uint32_t cell);
        void AddFineBraces(uint32_t cell);
        void AddMixedBraces();
        void AddMixedBrace(uint32_t fx, uint32_t fup, int32_t across);
        void AddMixedBond(uint32_t coarse, uint32_t fine, uint32_t axis, bool coarseLow, int64_t offset,
                          uint32_t intact, uint32_t beta);
        uint32_t PairBeta(uint32_t fine, uint32_t axis, uint32_t faceX, uint32_t faceUp) const;
        void BuildMixedPairs();
        void AddMixedPair(uint32_t lower, uint32_t upper);
        void BuildMixedBlocks(std::vector<EquilibriumBlockFace>& faces) const;
        void AddTriple(EquilibriumBlockFace& own, EquilibriumBlockFace& other, EquilibriumBlockFace& pairFace,
                       const TripleTerm& triple) const;
        int64_t TermProjection(const MixedTerm& term) const;
        int64_t TermForce(const MixedTerm& term) const;  // c p(負の項は −c p)
        int64_t TripleProjection(const TripleTerm& term) const;
        void BuildBlocks();
        int64_t CellValue(uint32_t cell, uint32_t component) const;
        int64_t UpForceOnTerm(const MixedTerm& term) const;  // high のセルへ(low へは同じ値の逆)
        void StepCoupled();
        bool Coupled() const;
        void AddBond(const Bond& bond, uint32_t intact);
        int64_t CarriedValue(const SupportLaw& before, uint32_t cell, uint32_t component) const;
        void AddClamp(const Clamp& clamp);
        void AddBrace(const Brace& brace);
        int64_t UpForceOnBrace(const Brace& brace) const;  // high のセルへ(low へは同じ値の逆)
        void AddLag(uint32_t rowComponent, uint32_t row, uint32_t columnComponent, uint32_t column, int64_t sign,
                    uint64_t magnitude);
        int64_t UpForceOnClamp(const Clamp& clamp) const;
        int64_t UpForceOnBond(const Bond& bond) const;  // high のセルへ low のセルから(low へは同じ値の逆)
        uint32_t ComponentCount() const;
        uint32_t Intact(uint32_t x, uint32_t up, uint32_t axis) const;
        uint32_t FineIntact(uint32_t fx, uint32_t fup, uint32_t axis) const;
        SupportFaceForces BondForces(const Bond& bond) const;
        SupportFaceForces MixedForces(size_t first, uint32_t axis) const;
        uint32_t BraceIntact(uint32_t x, uint32_t up, uint32_t sideX, bool side) const;
        int64_t CellLoad(uint32_t cell) const;  // 重さ + 載せた荷(mN。下向きが正)
        void BuildLoads();

        SupportScene m_scene;
        SupportOptions m_options;
        uint64_t m_bondX = 0;   // b = ka ÷ 6(ka = E × セル。2^-32 mN/nm)。x の面
        uint64_t m_bondUp = 0;  // 上の面
        uint64_t m_brace = 0;   // 段の斜めの棒の式の係数(棒の硬さの半分。既定 ka/24。横と上の硬さの平均)
        int64_t m_weight = 0;   // セル 1 つの重さ(mN)
        bool m_mixed = false;   // 違う大きさのセルが混ざる(T-1120)

        std::vector<std::array<uint32_t, 2>> m_cells;  // セルの (x, up)(そのセルの大きさの座標。細かいセルは細かい格子)
        std::vector<uint8_t> m_cellFine;               // セルが細かいか(混ざる場面だけ。空 = 全部同じ大きさ)
        std::vector<int64_t> m_loads;                  // セルの重さ + 載せた荷(mN)
        std::vector<uint32_t> m_index;      // [x + width × up] → セル(無ければ UINT32_MAX・分けたセルは SPLIT_CELL)
        std::vector<uint32_t> m_fineIndex;  // [fx + 2 width × fup] → 細かいセル(混ざる場面だけ。無ければ UINT32_MAX)
        std::vector<MixedTerm> m_mixedTerms;
        std::vector<MixedGroup> m_mixedGroups;  // 違う大きさの面ごと(m_mixedTerms の 4 つの項の組)
        std::vector<MixedPair> m_mixedPairs;
        std::vector<MixedTerm> m_pairTerms;     // 対のずれの負の項(low = L の細かいセル・high = U の細かいセル)
        std::vector<TripleTerm> m_tripleTerms;  // 対の伸び(対の平均のずれの歪みを引く)
        std::vector<uint32_t>
            m_mixedFaceAt;  // [(fx + 2 width × fup) × 2 + 軸] → 違う大きさの梁の最初の項(T-1067。無ければ UINT32_MAX)
        std::vector<Bond> m_bonds;
        std::vector<uint32_t> m_bondAt;  // [セル × 2 + 軸] → 面(無ければ UINT32_MAX)
        std::vector<Clamp> m_clamps;
        std::vector<Brace> m_braces;

        std::array<EquilibriumSystem, SUPPORT_COMPONENTS> m_systems;
        std::array<std::vector<Lag>, SUPPORT_COMPONENTS> m_lags;
        std::array<EquilibriumState, SUPPORT_COMPONENTS> m_states;

        // --- 成分を持つ節(Coupled)---
        EquilibriumBlockSystem m_blockSystem;
        EquilibriumBlockGrid m_blockGrid;
        EquilibriumBlockState m_blockState;
        uint64_t m_lastNodeUpdates = 0;
        uint32_t m_lastUnknowns = 0;
    };

}  // namespace bicameral::sim
