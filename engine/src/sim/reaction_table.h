// reaction_table.h — 反応表の定義(人が書く値)と、GPU と CPU リファレンスが読む整数の表へのベイク(02 §1〜2。T-0014)。
//
// データの流れ:
//   ReactionTableDefinition(元素・物質・規則。SI の値。Luau のパッケージから読む〔script/reaction_package。T-0021〕か、
//   試験の中で C++ に書く〔上限・溢れの試験の表〕。試験の表は data/packages/combustion_test だけ〔T-0169〕)
//   → BakeReactionTable(整数の数学ライブラリだけで計算する。R7)→ BakedReactionTable(RxSpecies・RxRule・索引・速度の表)
//   → ReactionTableView(CPU の RxStepCellWait 用)/ そのままバッファに載せて GPU へ(shaders/sim/reaction_cells.hlsl)
//
// ベイクの検査(02 §2): 元素の釣り合いが崩れた規則・知らない元素と物質・係数や次数の誤り・名前の重なりはエラーにする。
// 反応熱は生成エンタルピーの差から自動で決まる(手で書かない)ので、エネルギーの釣り合いは構造的に守られる。
// 文献の反応熱を書いた規則は、生成エンタルピーの差と比べて食い違えば警告を出す(BakedReactionTable::warnings。02 §2 の 2)。
// 坂を上る規則(物質の標準エントロピーから ΔG° > 0)も警告する。表はそのまま当てる(sim/reaction_slope。T-0492・D-487)。
// 並びに依らない(T-0021・ADR-0032): 元素・物質・規則・規則の項は名前のバイト順に並べ直してから ID を振る。
// 定義の並び(C++ の配列の順・Luau の表の順・Mod を入れた順)が違っても、中身が同じなら同じバイト列の表になる。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/reaction.hlsli"
#include "sim/coupling_table.h"
#include "sim/material_templates.h"
#include "sim/play_metric.h"
#include "sim/reaction_slope.h"
#include "sim/reaction_wide_cell.h"
#include "sim/world_atmosphere.h"
#include "sim/world_layout.h"
#include "sim/world_physics.h"

namespace bicameral::sim {

    // --- 定義(人が書く値)---

    struct ElementDefinition {
        std::string name;
        uint32_t atomicMass = 0;  // mg/mol(C = 12,011)
    };

    struct ElementCount {
        std::string element;
        uint32_t count = 0;
    };

    // 物質の相(パッケージの phase。T-0710・土台の決定 3)。物質ごとに 1 本で、どの流れがその物質を運ぶかを決める。
    // 前の condensed(凝縮相か)は Liquid か Solid、前の liquid は Liquid に当たる
    enum class SpeciesPhase : uint8_t {
        Gas = 0,  // 既定(書かない物質)
        Liquid = 1,
        Solid = 2,
    };

    // 物質の動き方の任意の値(T-0710・土台の決定 3。名前と単位は設計の仮)。書かない値は「今と同じ動き」で、0 と書いたのとは違う。
    // どれも相の決まった物質だけに書ける(ベイクが確かめる)。まだ刻みは読まない(土台の段 1 以降)。GPU の表には入らない
    struct SpeciesTransport {
        std::optional<uint64_t> viscosityMicroPascalSeconds;  // 液体の粘り(µPa·s。水 890)
        std::optional<uint32_t> diffusivityMm2PerS;           // 気体の混ざる速さ(mm²/s。湯気 25)
        std::optional<uint64_t>
            permeabilityNm2;                     // 固体の孔の通しやすさ(nm²。パッケージは µm² で書く。1e-6 µm² = 1 nm²)
        std::optional<uint32_t> suctionPascals;  // 固体の吸い上げ(Pa。乾いた砂 5000)

        [[nodiscard]] bool operator==(const SpeciesTransport&) const = default;
    };

    struct SpeciesDefinition {
        std::string name;
        std::vector<ElementCount> composition;  // 物質 1 mol あたりの元素の数
        int64_t formationEnthalpy = 0;          // J/mol(298.15 K)
        uint32_t heatCapacity = 0;              // mJ/(mol·K)(一定と置く)
        uint32_t thermalConductivity = 0;       // mW/(m·K)(一定と置く。T-0089)
        // 標準モルエントロピー(mJ/(mol·K)。298.15 K・1 bar)。任意。GPU の表に入らない(表とハッシュは変わらない)。
        // 坂の検査だけが使う(ベイクの警告 sim/reaction_slope・T-0492。試験 slope_check・T-0491。D-487)
        std::optional<int64_t> standardEntropy;
        // 相(パッケージの phase。既定 Gas)。液体は液体の場が低い方へ流し、密度(density)が要る。
        // 固体と液体は凝縮相(坂の計器 sim/slope_gauge が活量を 1 と置く。T-0518)。GPU の表には気体の印
        // (RxSpecies::flags の RX_SPECIES_GAS = Gas)としてだけ入り、セルの気体の器を数える(T-0622・ADR-0316)
        SpeciesPhase phase = SpeciesPhase::Gas;
        // パッケージが廃止した欄 condensed で相を書いた(読み手が phase へ読み替えた印。ベイクが警告する。T-0710)
        bool legacyCondensed = false;
        // 1 mol が放射を遮る面積(mm²/mol)。任意(既定 0 = 放射しない・吸わない)。セルの吸収率 = 1 − e^−Σ a·n ÷ 面の面積で、
        // 放射率も同じ値(灰色体)。GPU の表にはまだ入らない(放射の CPU リファレンス sim/radiation_reference だけが読む。T-0583・ADR-0252)
        uint32_t radiationAbsorption = 0;
        // 密度(kg/m³。パッケージの density_kg_per_m3)。任意(既定 0 = 書かない)。液体には要る(セルに入る量 = セルの体積 ÷ モル体積)。
        // 固体の密度はセルの体積を分け合う所(sim/cell_volume。ADR-0268)が読む。固体に書かないとベイクが警告する(T-0616)
        uint32_t density = 0;
        // 動き方の任意の値(粘り・混ざる速さ・孔・吸い上げ。T-0710)
        SpeciesTransport transport;
    };

    struct RuleTerm {
        std::string species;
        uint32_t coefficient = 1;
        bool firstOrder = false;  // 反応物だけ: 速度式で次数 1(false なら次数 0)
    };

    // Arrhenius: k = A · exp(−Ea / RT)。A は SI(次数 1 の反応物が n 個なら (m³/mol)^(n−1) / s)で、仮数 × 10^指数 で書く
    struct ArrheniusRate {
        uint64_t preExponentialMantissa = 0;
        int32_t preExponentialExponent10 = 0;
        int64_t activationEnergy = 0;  // J/mol
    };

    // 釣り合い(T-0623・ADR-0290)。その温度で気体の項の濃さの比 Q が concentration^Δn で釣り合い(Δn = 気体の生成物 − 気体の反応物の係数)、
    // 温度で ΔH_eq の急さ(van 't Hoff)で動く。凝縮相の項は活量 1(Q に入らない)。ベイクが逆向きの規則(名前 + REVERSE_RULE_SUFFIX)を作る
    struct BalanceDefinition {
        uint32_t temperatureMilliKelvin = 0;  // 釣り合いの温度 T_eq(mK。1〜4095 K)
        uint64_t
            concentrationMantissa = 0;  // 釣り合いの濃さ(mol/m³)= 仮数 × 10^指数。0 = 書いていない(Δn = 0 の時だけよい)
        int32_t concentrationExponent10 = 0;
        int64_t enthalpy = 0;  // ΔH_eq(J/mol)。正なら温めると生成物の側へ
    };

    inline constexpr std::string_view REVERSE_RULE_SUFFIX = "~逆";

    struct RuleDefinition {
        std::string name;
        std::vector<RuleTerm> reactants;
        std::vector<RuleTerm> products;
        ArrheniusRate rate;

        // --- 検査だけに使う(表には入らない)---
        // 文献の反応熱 ΔH(298.15 K、J/mol)。書いたら、生成エンタルピーの差との食い違いを警告する(02 §2 の 2)
        std::optional<int64_t> declaredReactionEnthalpy;
        std::optional<BalanceDefinition> balance;  // 釣り合いの規則(T-0623)。rate は釣り合いへ近づく速さ(1/s)
    };

    struct ReactionTableDefinition {
        std::vector<ElementDefinition> elements;
        std::vector<SpeciesDefinition> species;
        std::vector<RuleDefinition> rules;

        // --- 同じ読み込みに足した分類(世界のデータ。名前のまま。T-0398〜T-0400。分類ごとに 1 行)---
        WorldLayoutDefinition worldLayout;  // 世界の組み立て(world_cells・world_bodies。T-0398)

        MaterialTemplateDefinition templates;  // 材料のひな形・抽出の物質・既定(T-0399)
        WorldPhysics environment;              // 環境の値(分類 environment。無ければブランク = 0。T-0400・ADR-0176)
        WorldAtmosphereDefinition
            atmosphere;  // 基準の大気(分類 atmosphere と environment の大気の値。T-0373・ADR-0182)
        std::vector<PlayMetricDefinition> playMetrics;  // 遊びの指標(分類 play_metrics。T-0432・ADR-0206)
        std::vector<CouplingDefinition> couplings;      // 流れどうしの結合(分類 drags・machines。T-0536・ADR-0238)

        // 気体(condensed でない物質)の比熱を定圧の値 c_p として読み、ベイクが定積の比熱 c_v = c_p − R を作る(T-0591・ADR-0259。
        // ユーザーの決定 F 2026-10-17 で既定 true。T-0597)。セルは体積が一定なので、反応・伝導・気体の段の温度は c_v で読むのが正しい
        // (H0 は書いた c_p から。298.15 K の内部エネルギー = 生成エンタルピー − RT)。作り手は資料の c_p をそのまま書く。
        // パッケージには切り替えを置かない(heat_capacity_mj_per_mol_k の意味は 1 つ。ADR-0259 の追記)。
        // false は C++ の試験だけ(書いた値をそのまま定積の比熱として読む = T-0597 より前)
        bool gasHeatCapacityAtConstantPressure = true;
    };

    // --- ベイクした表 ---

    static_assert(sizeof(reaction::RxSpecies) == 32);
    static_assert(sizeof(reaction::RxRule) == 120);
    static_assert(sizeof(reaction::RxCell) == 112);

    // CPU の RxStepCellWait に渡す表の読み方(reaction.hlsli の Table の約束)
    struct ReactionTableView {
        std::span<const reaction::RxSpecies> species;
        std::span<const reaction::RxRule> rules;
        std::span<const uint32_t> ruleIndex;
        std::span<const uint64_t> rates;

        [[nodiscard]] reaction::RxSpecies Species(uint32_t id) const { return species[id]; }
        [[nodiscard]] reaction::RxRule Rule(uint32_t id) const { return rules[id]; }
        [[nodiscard]] reaction::RxRule RateRule(uint32_t id) const {
            return rules[id];
        }  // 速さと優先度だけに使う規則(T-0397)
        [[nodiscard]] uint32_t RuleIndex(uint32_t position) const { return ruleIndex[position]; }
        [[nodiscard]] uint64_t Rate(uint32_t rule, uint32_t kelvin) const {
            return rates[(size_t{rule} * reaction::RX_RATE_TABLE_KELVINS) + kelvin];
        }
    };

    struct BakedReactionTable {
        // --- GPU に載せる(添字 = ID。物質の 0 は使わない)---
        std::vector<reaction::RxSpecies> species;
        std::vector<reaction::RxRule> rules;
        std::vector<uint32_t> ruleIndex;  // 物質ごとの規則の一覧をつないだもの(RxSpecies::ruleBegin から)
        std::vector<uint64_t> rates;      // 規則 × RX_RATE_TABLE_KELVINS(仮数 << 32 | 指数)

        // --- CPU だけ(検査と表示)---
        std::vector<std::string> elementNames;
        std::vector<std::string> speciesNames;  // 添字 = 物質 ID
        std::vector<std::string> ruleNames;     // 添字 = 規則 ID
        std::vector<uint32_t> molarMasses;      // mg/mol。添字 = 物質 ID
        std::vector<uint32_t> speciesElements;  // 物質 ID × 元素の数 + 元素
        std::vector<std::string>
            warnings;  // ベイクは通したが、書いた人に見せる食い違い(文献の反応熱・坂を上る規則など)

        // --- CPU だけ: 坂の検査(sim/reaction_slope。T-0492・D-487)---
        std::vector<std::optional<int64_t>> standardEntropies;  // mJ/(mol·K)。添字 = 物質 ID(書いていなければ無し)
        std::vector<RuleSlope> ruleSlopes;                      // 規則ごとの坂(添字 = 規則 ID。パネルの「坂」の欄)
        std::vector<uint8_t> speciesCondensed;  // 1 = 凝縮相(活量 1。坂の計器 sim/slope_gauge・T-0518)。添字 = 物質 ID

        // --- CPU だけ: 相と動き方(T-0710。相の表の源。speciesCondensed・speciesLiquid はここから作る写し〔段 1 で読み手を移して消す〕)---
        std::vector<SpeciesPhase> speciesPhase;          // 添字 = 物質 ID(ID 0 は Gas)
        std::vector<SpeciesTransport> speciesTransport;  // 添字 = 物質 ID(書かない値は無し)

        // --- CPU だけ(GPU の表に入るのは T-0586): 放射(sim/radiation_reference。T-0583・ADR-0252)---
        std::vector<uint32_t> speciesRadiationAbsorption;  // mm²/mol(1 mol が放射を遮る面積)。添字 = 物質 ID

        // --- CPU だけ(GPU は T-0612 の後): 液体(sim/probe_liquid。T-0609・ADR-0266)---
        std::vector<uint8_t> speciesLiquid;  // 1 = 液体(流れる。凝縮相でもある)。添字 = 物質 ID
        std::vector<uint64_t>
            speciesMolarVolume;  // mm³/mol(= モル質量 ÷ 密度。密度を書いていなければ 0)。添字 = 物質 ID

        // --- CPU だけ: 同じ読み込みに足した分類(世界のデータ。物質は ID に直したもの。T-0398〜T-0400。分類ごとに 1 行)---
        WorldLayout worldLayout;     // 世界の組み立て(MakeProbeInitialWorld・MakeLayoutPhysicsScene が読む。T-0398)
        WorldPhysics environment;    // 環境の値(重力・気体の音速の基準 → 物理と気体の設定。T-0400・ADR-0176)
        WorldAtmosphere atmosphere;  // 基準の大気(成分・温度・地面の圧力 → 気体の層の基準。T-0373・ADR-0182。空 = 無い)

        MaterialTemplates templates;          // 材料のひな形・抽出の物質・既定(T-0399)
        std::vector<PlayMetric> playMetrics;  // 遊びの指標(鍵の順。sim/play_metric_measure が測る。T-0432・ADR-0206)
        std::vector<Coupling> couplings;      // 流れどうしの結合(drags → machines・鍵の順。まだ刻みは読まない。T-0536)

        [[nodiscard]] ReactionTableView View() const {
            return {.species = species, .rules = rules, .ruleIndex = ruleIndex, .rates = rates};
        }

        // 名前から物質 ID(無ければ 0)
        [[nodiscard]] uint32_t SpeciesId(std::string_view name) const;
    };

    [[nodiscard]] std::expected<BakedReactionTable, std::string> BakeReactionTable(
        const ReactionTableDefinition& definition);

    // --- セルを作る・調べる(テストと、T-0089 の初期状態)---

    struct SpeciesAmount {
        uint32_t species = 0;
        uint64_t amount = 0;  // µmol
    };

    // 1 セルの中身がセルの器に入るか(合計 ≤ CELL_AMOUNT_LIMIT・凝縮相でない成分の合計 < CELL_GAS_AMOUNT_LIMIT。units.hlsli・ADR-0260)。
    // 入らなければ理由(場所は呼ぶ側が前に付ける)。世界の組み立てと材料のひな形のベイクが使う
    [[nodiscard]] std::expected<void, std::string> CheckCellAmounts(const BakedReactionTable& table,
                                                                    std::span<const SpeciesAmount> amounts);

    // 成分と温度(mK)からセルを作る。エネルギー = 化学 + 熱容量 × 温度 を mJ に切り上げる(熱が負にならないように)
    [[nodiscard]] reaction::RxCell MakeReactionCell(const BakedReactionTable& table,
                                                    std::span<const SpeciesAmount> amounts,
                                                    int32_t temperatureMilliKelvin);

    // 同じ値の、成分の数に上限が無いセル(T-0175。成分が RX_MAX_CELL_SPECIES を超えてよい)
    [[nodiscard]] reaction::RxWideCell MakeWideReactionCell(const BakedReactionTable& table,
                                                            std::span<const SpeciesAmount> amounts,
                                                            int32_t temperatureMilliKelvin);

    // 元素ごとの数(µmol ぶんの原子の数。添字 = 元素)
    [[nodiscard]] std::vector<uint64_t> CountElements(const BakedReactionTable& table, const reaction::RxCell& cell);
    [[nodiscard]] std::vector<uint64_t> CountElements(const BakedReactionTable& table,
                                                      const reaction::RxWideCell& cell);

    // セルの中身のハッシュ(エネルギーと成分。比べるため。成分が同じなら RxCell と RxWideCell で同じ値)
    [[nodiscard]] uint64_t HashReactionCell(const reaction::RxCell& cell);
    [[nodiscard]] uint64_t HashReactionCell(const reaction::RxWideCell& cell);

    // CPU リファレンス: 1 セルの 1 刻み(待ちの丸め。T-0105・D-429)。changedTick = セルのブロックが最後に変わった刻み。
    // 変わらなければ次に評価が要る刻みも返す
    [[nodiscard]] reaction::RxWaitStep StepReactionCellWait(const BakedReactionTable& table,
                                                            const reaction::RxCell& cell, uint64_t worldSeed,
                                                            uint64_t tick, uint64_t changedTick, uint64_t cellId);

    // 同じ、成分の数に上限が無いセルの版(T-0175。生成物を待たせない = RX_LIMIT_PRODUCTS は立たない)
    [[nodiscard]] reaction::RxWideWaitStep StepReactionCellWait(const BakedReactionTable& table,
                                                                const reaction::RxWideCell& cell, uint64_t worldSeed,
                                                                uint64_t tick, uint64_t changedTick, uint64_t cellId);

    // CPU リファレンス: 1 セルだけのブロックを tickBegin から tickCount 刻み進める(shaders/sim/reaction_cells.hlsl と同じ関数。T-0130)
    [[nodiscard]] reaction::RxLoneCell AdvanceLoneReactionCell(const BakedReactionTable& table,
                                                               const reaction::RxLoneCell& lone, uint64_t worldSeed,
                                                               uint64_t tickBegin, uint32_t tickCount, uint64_t cellId);

}  // namespace bicameral::sim
