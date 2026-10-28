// reaction.hlsli — 1 セルの反応の評価(docs/design/02-reaction-system.md §3。T-0014)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束に従う)。CPU リファレンス(engine/src/sim/reaction.h)と
// GPU のカーネル(shaders/sim/reaction_cells.hlsl)が同じ関数を呼ぶので、同じセル・同じ表からはビット単位で同じ結果になる。
//
// データの流れ:
//   ベイクした表(engine/src/sim/reaction_table.cpp の BakeReactionTable: 物質・規則・索引・速度の表)
//   + セル(成分 = 物質 ID の昇順に並べた物質量、エネルギー)→ RxStepCellWait(待ちの丸め。ADR-0018)→ 次の刻みのセル
//
// 保存則(D-206・04 R2):
//   - 元素: 反応物と生成物は「係数 × 進行度」で整数のまま増減する(ADR-0012)。ベイクが元素の釣り合いを確かめるので、元素の数は完全に保存される。
//   - エネルギー: セルのエネルギーは「熱 + 化学(Σ 物質量 × H0)」の合計。反応は成分を変えるだけで合計を変えない(熱は導出値)。
//
// 表の読み方は呼ぶ側が Table 型で渡す(C++ は配列の span、HLSL はバッファを読む構造体)。Table に要るメソッド:
//   RxSpecies Species(uint32_t id) / RxRule Rule(uint32_t id) / uint32_t RuleIndex(uint32_t position) /
//   uint64_t Rate(uint32_t rule, uint32_t kelvin) /
//   RxRule RateRule(uint32_t id)(速さと優先度だけに使う規則。key・反応物・速さの欄だけを読む。今の表はどれも Rule と同じ。
//   核が規則を読む所を「反応物と鍵」と「反応熱」に分けておくと、HW の Work Graph が小さく保てる。T-0397)
//
// セルの形も呼ぶ側が Cell 型で渡す(T-0175。成分の二段の CPU リファレンス):
//   - RxCell: 成分はインラインの RX_MAX_CELL_SPECIES 個まで(GPU と、今の多重解像度の世界)。超える生成物は待たせる(RX_LIMIT_PRODUCTS)
//   - RxWideCell(C++ だけ。engine/src/sim/reaction_wide_cell.h): 成分の数に上限が無い。生成物は待たずに足す
//   核は成分を species[i]・amounts[i]・speciesCount で読み、形ごとに違う所は下の「セルの形ごとの道具」(同じ名前の関数を形ごとに用意)を通す。
//   成分が RX_MAX_CELL_SPECIES 以下の間は、どちらの形でもビット単位で同じ結果になる
#ifndef BICAMERAL_REACTION_HLSLI
#define BICAMERAL_REACTION_HLSLI

#include "fixed.hlsli"
#include "units.hlsli"

#ifdef __cplusplus
#define RX_NAMESPACE_BEGIN          \
    namespace bicameral::reaction { \
        using namespace ::bicameral::fx;
#define RX_NAMESPACE_END }
#else
#define RX_NAMESPACE_BEGIN
#define RX_NAMESPACE_END
#endif

RX_NAMESPACE_BEGIN

// --- 大きさ ------------------------------------------------------------------------------------
FX_CONST uint32_t
    RX_MAX_CELL_SPECIES = 8;  // セルの成分のインラインの数(溢れは RxWideCell〔CPU。T-0175〕・GPU は T-0176 / R-MULTI-4)
FX_CONST uint32_t RX_MAX_REACTANTS = 3;
FX_CONST uint32_t RX_MAX_PRODUCTS = 3;
FX_CONST uint32_t RX_MAX_CANDIDATES = 16;  // 1 セルで同時に進める規則の数(超えたら刻みごとに選ぶ。RX_LIMIT_CANDIDATES)
FX_CONST uint32_t RX_RATE_TABLE_KELVINS = 4096;  // 速度の表は 0〜4095 K を 1 K ごと。それより熱いと 4095 K の値
FX_CONST uint32_t RX_NO_SLOT = 0xFFFFFFFFu;
FX_CONST uint32_t RX_RANDOM_PURPOSE = 0x52780001u;  // 取り合いの丸めの乱数の用途(FxHash64)
FX_CONST uint32_t RX_SELECT_PURPOSE = 0x52780003u;  // 候補が上限を超えた時に選ぶ乱数の用途(FxHash64。T-0022)

// 上限に当たった印(RxWaitStep::limits のビット。T-0022 の最初の段。D-401・D-428・04 R8: 黙って捨てない)。
// どちらも保存則は守る(元素・エネルギーはビット単位で変わらない)。上限そのものを無くすのは T-0022 の続き(成分の溢れの領域・候補の引き方)。
//   RX_LIMIT_PRODUCTS: 生成物を全部足すと成分がインラインの数を超えるので、セルに無い物質を作る規則をこの刻みは進めなかった(待たせた)。
//                      上限の無いセルの形(RxWideCell。T-0175)では立たない
//   RX_LIMIT_CANDIDATES: 進む規則が RX_MAX_CANDIDATES を超えたので、刻みごとの乱数で RX_MAX_CANDIDATES 個を選んで進めた
//                        (選ばれなかった規則はこの刻みは進まない。並びや ID に偏らない)
// 仮(ユーザー未確認。QUESTIONS Q19): 上限に当たった時のふるまい
FX_CONST uint32_t RX_LIMIT_PRODUCTS = 1u;
FX_CONST uint32_t RX_LIMIT_CANDIDATES = 2u;

// 吸熱の規則が 1 刻みに使える熱は、今の熱の 2^-RX_ENDOTHERMIC_HEAT_SHIFT まで(温度が 1 刻みで 1/8 より下がらない)。
// 1 刻みに 1 回の評価(陽的)だと、速い吸熱の反応は始めの温度の速さで進み続け、1 刻みで 0 K まで冷えてしまう(T-0014 で確認)。
// 温度が下がれば速さも指数的に落ちるので、刻みごとに少しずつ冷えて自然に止まる。R-REACT-1(細分)で見直す
FX_CONST uint32_t RX_ENDOTHERMIC_HEAT_SHIFT = 3;

// 物質の印(RxSpecies::flags)。RX_SPECIES_GAS = 気体のぶんに数える物質(condensed でも liquid でもない。気体の区画が流す物質。ADR-0256)
FX_CONST uint32_t RX_SPECIES_GAS = 1u;
// 凝縮相の置かれ方(T-1176。世界の木の細かくする段が読む。sim/reaction_table の相と solid_motion から。どれも無い凝縮相は無い)
FX_CONST uint32_t RX_SPECIES_LIQUID = 2u;    // 液体(phase = "liquid")
FX_CONST uint32_t RX_SPECIES_GRAIN = 4u;     // ばらける固体(solid_motion = "loose")
FX_CONST uint32_t RX_SPECIES_ANCHORED = 8u;  // 据わる固体(書かない固体)

// 望む進行度の上限。望む量は保存量ではないので飽和させてよく、この後で反応物の量に縮める
FX_CONST uint64_t RX_EXTENT_SATURATION = FX_U64(0x40000000u, 0u);

// エネルギー(mJ)を熱の単位(µJ)にする倍率、温度(mK)を熱 ÷ 熱容量(µJ ÷ nJ/K)から出す倍率
FX_CONST uint64_t RX_MICROJOULES_PER_MILLIJOULE = 1000;
FX_CONST uint64_t RX_MILLIKELVIN_NUMERATOR = 1000000;

// --- 表の形(ベイクが作り、GPU にはこのまま載せる。sim/reaction_table.h が大きさを static_assert する)---

// 物質(ID が添字。0 は使わない)
struct RxSpecies {
    int64_t h0;             // 化学のエネルギー(J/mol = µJ/µmol)。0 K 基準: 生成エンタルピー − 比熱 × 298.15 K
    uint32_t heatCapacity;  // 比熱(mJ/(mol·K))。相ごとの値は M2
    uint32_t ruleBegin;     // 索引(この物質を「ID が最小の反応物」に持つ規則)の始まり
    uint32_t ruleCount;
    uint32_t
        conductivity;  // 熱伝導率(mW/(m·K)。室温の値)。セルの値は体積で重み付けた平行の道(heat_conduction.hlsli。D-637)
    uint32_t flags;    // RX_SPECIES_*(気体のぶんに数えるか。T-0622)
    uint32_t molarVolume;  // 凝縮相が占める体積(mm³/mol)。気体と密度を書いていない物質は 0(場所を取らない。T-1066)
};

// 反応の規則
struct RxRule {
    // --- 反応物・生成物(係数は正の整数)---
    uint32_t reactantCount;
    uint32_t productCount;
    uint32_t reactants[RX_MAX_REACTANTS];
    uint32_t reactantCoefficients[RX_MAX_REACTANTS];
    uint32_t products[RX_MAX_PRODUCTS];
    uint32_t productCoefficients[RX_MAX_PRODUCTS];

    // --- 速度と熱 ---
    uint32_t firstOrderMask;    // 速度式で次数 1 の反応物(ビット i = reactants[i])。1〜2 個
    uint32_t activationEnergy;  // J/mol(Arrhenius の Ea。速さの表はベイク済み。表を読む道具が見るだけ)
    int64_t reactionEnthalpy;   // 進行度 1 µmol あたりの µJ(= J/mol)。Σ 生成物の H0 − Σ 反応物の H0。正なら吸熱
    uint64_t key;               // 規則の名前のハッシュ。乱数の入力に使い、表の並び(規則の ID)に結果を依存させない

    // --- 釣り合い(T-0623・ADR-0290。釣り合いの規則だけ。ほかは 0)---
    // 釣り合いの規則は「自分の向きに釣り合いへ向かう時だけ」進む(逆向きはベイクが作る別の規則)。
    // log2 K(T) = balanceLog2Constant + balanceSlope × (1/T_eq − 1/T)。Q は気体の項の µmol で数える(セルの体積の換算は定数に入れてある)
    int64_t balanceLog2Constant;  // log2 K(T_eq)(µmol の単位。Q32)
    int64_t balanceSlope;         // ΔH_eq ÷ (R ln 2)(K。Q32)。正なら温めると生成物の側へ
    int64_t heatCapacityChange;   // Σ 生成物の比熱 − Σ 反応物の比熱(mJ/(mol·K) = nJ/(µmol·K))。進めた後の温度に使う
    uint32_t balanceKelvin;       // 釣り合いの温度 T_eq(mK)。0 = 釣り合いの規則ではない
    uint32_t
        balanceGasMask;  // 釣り合いの式に入る項(ビット i = reactants[i]、ビット RX_BALANCE_PRODUCT_SHIFT + i = products[i])

    // --- セルの気体の器(T-0622・ADR-0316)---
    uint32_t
        gasGrowth;  // 進行度 1 µmol で増える気体のぶん(µmol。Σ 気体の生成物の係数 − Σ 気体の反応物の係数。減るか同じなら 0)
    uint32_t padding;
};

// セル(レベル 0)。成分は物質 ID の昇順、物質量が 0 になったものは消す(同じ中身なら同じ並び = 同じハッシュ)
struct RxCell {
    int64_t energy;  // mJ(熱 + 化学)
    uint32_t speciesCount;
    uint32_t padding;
    uint32_t species[RX_MAX_CELL_SPECIES];
    uint64_t amounts[RX_MAX_CELL_SPECIES];  // µmol
};

// セルの熱(導出値)
struct RxThermal {
    int64_t heat;           // µJ = エネルギー × 1000 − Σ 物質量 × H0
    uint64_t heatCapacity;  // nJ/K = Σ 物質量 × 比熱
    int32_t temperature;    // mK
};

// 1 刻みで評価する規則の候補と、その進行度(µmol)
struct RxCandidates {
    uint32_t count;
    uint32_t rules[RX_MAX_CANDIDATES];
    uint64_t extents[RX_MAX_CANDIDATES];
};

// 候補の欄を読む・書く(T-1233)。C++ は配列をそのまま。GPU は候補の回し(候補の数まで)の中の動的な添字でも配列を動的な添字で
// 読み書きしないよう、決まった数の回しで一致する欄だけを選ぶ・書く(下の成分の道具と同じ考え。T-0397・CLAUDE.md §4)
#ifdef __cplusplus
#define RX_CANDIDATE_RULE(candidates, c) (candidates).rules[c]
#define RX_CANDIDATE_EXTENT(candidates, c) (candidates).extents[c]
#define RX_SET_CANDIDATE_EXTENT(candidates, c, value) (candidates).extents[c] = value
#define RX_SET_CANDIDATE(candidates, c, ruleId, value) \
    (candidates).rules[c] = ruleId;                    \
    (candidates).extents[c] = value
#else
FX_FN uint32_t RxPickCandidateRule(uint32_t rules[RX_MAX_CANDIDATES], uint32_t index) {
    uint32_t value = 0;
    FX_UNROLL for (uint32_t k = 0; k < RX_MAX_CANDIDATES; ++k) value = k == index ? rules[k] : value;

    return value;
}

FX_FN uint64_t RxPickCandidateExtent(uint64_t extents[RX_MAX_CANDIDATES], uint32_t index) {
    uint64_t value = 0;
    FX_UNROLL for (uint32_t k = 0; k < RX_MAX_CANDIDATES; ++k) value = k == index ? extents[k] : value;

    return value;
}

FX_FN void RxPutCandidateRule(inout uint32_t rules[RX_MAX_CANDIDATES], uint32_t index, uint32_t value) {
    FX_UNROLL for (uint32_t k = 0; k < RX_MAX_CANDIDATES; ++k) rules[k] = k == index ? value : rules[k];
}

FX_FN void RxPutCandidateExtent(inout uint64_t extents[RX_MAX_CANDIDATES], uint32_t index, uint64_t value) {
    FX_UNROLL for (uint32_t k = 0; k < RX_MAX_CANDIDATES; ++k) extents[k] = k == index ? value : extents[k];
}

#define RX_CANDIDATE_RULE(candidates, c) RxPickCandidateRule((candidates).rules, c)
#define RX_CANDIDATE_EXTENT(candidates, c) RxPickCandidateExtent((candidates).extents, c)
#define RX_SET_CANDIDATE_EXTENT(candidates, c, value) RxPutCandidateExtent((candidates).extents, c, value)
#define RX_SET_CANDIDATE(candidates, c, ruleId, value) \
    RxPutCandidateRule((candidates).rules, c, ruleId); \
    RxPutCandidateExtent((candidates).extents, c, value)
#endif

// --- 小さな道具 --------------------------------------------------------------------------------

FX_FN RxCell RxMakeEmptyCell(int64_t energy) {
    RxCell cell;
    cell.energy = energy;
    cell.speciesCount = 0;
    cell.padding = 0;
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        cell.species[i] = 0;
        cell.amounts[i] = 0;
    }

    return cell;
}

// --- セルの形ごとの道具(T-0175。RxWideCell の版は engine/src/sim/reaction_wide_cell.h に同じ名前で)---
// 核は Cell 型の引数からこれらを呼ぶ。RxCell の版はインラインの数で止まり、何も伸ばさない(GPU のコードは前と同じ)

#ifdef __cplusplus
#define RX_UNUSED(x) (void)(x)
#else
#define RX_UNUSED(x)
#endif

// 成分をもう 1 つ増やせるか(RxCell はインラインの数まで)
FX_FN bool RxHasRoomForSpecies(RxCell cell) {
    return cell.speciesCount < RX_MAX_CELL_SPECIES;
}

// 書く前に、成分の数 + 1 個ぶんの場所を用意する(RxCell は初めからインラインの数だけあるので、そのまま)
FX_FN RxCell RxCellWithRoom(RxCell cell) {
    return cell;
}

// 同じ形の空のセル
FX_FN RxCell RxEmptyCellLike(RxCell cell, int64_t energy) {
    RX_UNUSED(cell);

    return RxMakeEmptyCell(energy);
}

// --- 成分を読む・書く・写す(T-0362。核はセルの成分をこれらのマクロだけで触る)---
// 既定は配列をそのまま読み書きする(RxCell・GPU の速い形・C++ の RxWideCell)。マクロにしたのは、値を返す関数にすると結果は同じでも
// 既定の世界のシェーダーの DXIL が増えたから(reaction_cells 4765 → 5091 行。inout でも 4990 行。runner の dxc で比べた。ADR-0052 追記 14)。
// RX_CELL_ACCESSORS を定義したシェーダー(作業場の段 multires_step_wide_big だけ)は下の道具の関数を通し、GPU の作業場の形
// (RxGpuSpaceCell。shaders/sim/multires_wide_step.hlsli)が明示的な特殊化で u6 の作業場を読み書きする(1 スレッドが上限の大きさの
// 配列を持たない)。書く道具は書いたセルを返す(作業場の形は同じ場所を指したまま = 書いたセルと元のセルは同じ中身になる。元を残す
// 所は RX_MAKE_WRITABLE で先に別の場所へ写す)。RX_SET_ENTRY は既定で 2 文になるので、{} の中だけで使う
#ifdef RX_CELL_ACCESSORS
template <typename Cell>
FX_FN uint32_t RxSpeciesAt(Cell cell, uint32_t i) {
    return cell.species[i];
}

template <typename Cell>
FX_FN uint64_t RxAmountAt(Cell cell, uint32_t i) {
    return cell.amounts[i];
}

template <typename Cell>
FX_FN Cell RxWithAmount(Cell cell, uint32_t i, uint64_t amount) {
    cell.amounts[i] = amount;

    return cell;
}

template <typename Cell>
FX_FN Cell RxWithEntry(Cell cell, uint32_t i, uint32_t speciesId, uint64_t amount) {
    cell.species[i] = speciesId;
    cell.amounts[i] = amount;

    return cell;
}

// 書き換える前の写し(元のセルを残して書く所の始め。配列の形は値の写しなのでそのまま、作業場の形は別の場所へ写す)
template <typename Cell>
FX_FN Cell RxWritableCell(Cell cell) {
    return cell;
}

template <typename Usage>
FX_FN uint64_t RxUsageAt(Usage usage, uint32_t i) {
    return usage.amounts[i];
}

template <typename Usage>
FX_FN Usage RxWithUsage(Usage usage, uint32_t i, uint64_t amount) {
    usage.amounts[i] = amount;

    return usage;
}

#define RX_SPECIES_AT(cell, i) RxSpeciesAt(cell, i)
#define RX_AMOUNT_AT(cell, i) RxAmountAt(cell, i)
#define RX_USAGE_AT(usage, i) RxUsageAt(usage, i)
#define RX_ADD_AMOUNT(cell, i, amount) cell = RxWithAmount(cell, i, RxAmountAt(cell, i) + (amount))
#define RX_SUB_AMOUNT(cell, i, amount) cell = RxWithAmount(cell, i, RxAmountAt(cell, i) - (amount))
#define RX_SET_ENTRY(cell, i, speciesId, amount) cell = RxWithEntry(cell, i, speciesId, amount)
#define RX_ADD_USAGE(usage, i, amount) usage = RxWithUsage(usage, i, RxUsageAt(usage, i) + (amount))
#define RX_MAKE_WRITABLE(cell) cell = RxWritableCell(cell)
#elif defined(__cplusplus)
#define RX_SPECIES_AT(cell, i) cell.species[i]
#define RX_AMOUNT_AT(cell, i) cell.amounts[i]
#define RX_USAGE_AT(usage, i) usage.amounts[i]
#define RX_ADD_AMOUNT(cell, i, amount) cell.amounts[i] += amount
#define RX_SUB_AMOUNT(cell, i, amount) cell.amounts[i] -= amount
#define RX_SET_ENTRY(cell, i, speciesId, amount) \
    cell.species[i] = speciesId;                 \
    cell.amounts[i] = amount
#define RX_ADD_USAGE(usage, i, amount) usage.amounts[i] += amount
#define RX_MAKE_WRITABLE(cell)
#else
// GPU の既定(RxCell・RxUsage の RX_MAX_CELL_SPECIES 個の配列。T-1233): 添字が動的でも配列を動的な添字で読み書きしない。
// 決まった数の回しで「添字が一致する欄だけ」を選ぶ・書く(T-0397・CLAUDE.md §4。動的な添字で読むと 1 スレッドの局所の配列になり、
// 局所メモリへ出る)。回しの中で添字が決まる所(下の決まった数の回し)では、開いた後に 1 つの欄の読み書きに畳まれる。
// 範囲の外の添字(RX_NO_SLOT など)は 0 を読み、何も書かない(前は未定義。核は範囲の外を読み書きしない)
FX_FN uint32_t RxPickU32(uint32_t values[RX_MAX_CELL_SPECIES], uint32_t index) {
    uint32_t value = 0;
    FX_UNROLL for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) value = k == index ? values[k] : value;

    return value;
}

FX_FN uint64_t RxPickU64(uint64_t values[RX_MAX_CELL_SPECIES], uint32_t index) {
    uint64_t value = 0;
    FX_UNROLL for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) value = k == index ? values[k] : value;

    return value;
}

FX_FN void RxPutU32(inout uint32_t values[RX_MAX_CELL_SPECIES], uint32_t index, uint32_t value) {
    FX_UNROLL for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) values[k] = k == index ? value : values[k];
}

FX_FN void RxPutU64(inout uint64_t values[RX_MAX_CELL_SPECIES], uint32_t index, uint64_t value) {
    FX_UNROLL for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) values[k] = k == index ? value : values[k];
}

// 足す・引く(符号なしの剰余。添字が一致しない欄には 0 を足す)
FX_FN void RxAddAtU64(inout uint64_t values[RX_MAX_CELL_SPECIES], uint32_t index, uint64_t amount) {
    FX_UNROLL for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) values[k] += k == index ? amount : (uint64_t)0;
}

FX_FN void RxSubAtU64(inout uint64_t values[RX_MAX_CELL_SPECIES], uint32_t index, uint64_t amount) {
    FX_UNROLL for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) values[k] -= k == index ? amount : (uint64_t)0;
}

// セルの形ごとの道具(RxCell の版。速い形 RxGpuMidCell の版は shaders/sim/multires_wide_step.hlsli に同じ名前で〔配列をそのまま〕)
FX_FN uint32_t RxSpeciesOf(RxCell cell, uint32_t i) {
    return RxPickU32(cell.species, i);
}

FX_FN uint64_t RxAmountOf(RxCell cell, uint32_t i) {
    return RxPickU64(cell.amounts, i);
}

FX_FN void RxAddAmountAt(inout RxCell cell, uint32_t i, uint64_t amount) {
    RxAddAtU64(cell.amounts, i, amount);
}

FX_FN void RxSubAmountAt(inout RxCell cell, uint32_t i, uint64_t amount) {
    RxSubAtU64(cell.amounts, i, amount);
}

FX_FN void RxSetEntryAt(inout RxCell cell, uint32_t i, uint32_t speciesId, uint64_t amount) {
    RxPutU32(cell.species, i, speciesId);
    RxPutU64(cell.amounts, i, amount);
}

#define RX_SPECIES_AT(cell, i) RxSpeciesOf(cell, i)
#define RX_AMOUNT_AT(cell, i) RxAmountOf(cell, i)
#define RX_USAGE_AT(usage, i) RxUsageOf(usage, i)
#define RX_ADD_AMOUNT(cell, i, amount) RxAddAmountAt(cell, i, amount)
#define RX_SUB_AMOUNT(cell, i, amount) RxSubAmountAt(cell, i, amount)
#define RX_SET_ENTRY(cell, i, speciesId, amount) RxSetEntryAt(cell, i, speciesId, amount)
#define RX_ADD_USAGE(usage, i, amount) RxAddUsageAt(usage, i, amount)
#define RX_MAKE_WRITABLE(cell)
#endif

// 成分を順に見る回し(T-1233): GPU の RxCell は決まった数(RX_MAX_CELL_SPECIES)まで回して成分の数で抜ける(回しの数が定数なので
// コンパイラが開き、添字が定数になって上の選ぶ道具が 1 つの欄に畳まれる)。回しの終わりはセルの形ごとの道具 RxSpeciesLoopEnd
// (速い形・作業場の形は成分の数。[unroll(N)] は N 回で打ち切るので使わない)。C++ は成分の数まで(RxWideCell は上限が無い)。
// 使い方: for (uint32_t i = 0; i < RX_SPECIES_END(cell); ++i) { if (i >= cell.speciesCount) break; … }
#ifdef __cplusplus
#define RX_SPECIES_END(cell) (cell).speciesCount
#else
FX_FN uint32_t RxSpeciesLoopEnd(RxCell cell) {
    RX_UNUSED(cell);

    return RX_MAX_CELL_SPECIES;
}

#define RX_SPECIES_END(cell) RxSpeciesLoopEnd(cell)
#endif

// 規則の反応物の欄を読む(T-1233)。反応物の数までの回し(動的な添字)の中で使う。C++ は配列をそのまま、GPU は決まった数から選ぶ
// (規則は表から写した局所の値なので、欄を動的な添字で読むと局所の配列になる。T-0397。回しを開くより命令が少ない)
#ifdef __cplusplus
#define RX_REACTANT_AT(rule, i) (rule).reactants[i]
#define RX_REACTANT_COEFFICIENT_AT(rule, i) (rule).reactantCoefficients[i]
#else
FX_FN uint32_t RxPickReactantField(uint32_t values[RX_MAX_REACTANTS], uint32_t index) {
    uint32_t value = 0;
    FX_UNROLL for (uint32_t k = 0; k < RX_MAX_REACTANTS; ++k) value = k == index ? values[k] : value;

    return value;
}

#define RX_REACTANT_AT(rule, i) RxPickReactantField((rule).reactants, i)
#define RX_REACTANT_COEFFICIENT_AT(rule, i) RxPickReactantField((rule).reactantCoefficients, i)
#endif

// 物質 ID の位置(無ければ RX_NO_SLOT)
template <typename Cell>
FX_FN uint32_t RxFindSlot(Cell cell, uint32_t speciesId) {
    // 成分の ID は重ならないので、一致は高々 1 つ。一致したら抜ける(T-1233 で抜けずに全部回す形にしたら、成分の多いセルで
    // 回しの数が倍ほどになり遅くなった。T-1234。抜けても開いた回しの添字は定数のまま)
    uint32_t slot = RX_NO_SLOT;
    for (uint32_t i = 0; i < RX_SPECIES_END(cell); ++i) {
        if (i >= cell.speciesCount)
            break;

        if (RX_SPECIES_AT(cell, i) == speciesId) {
            slot = i;
            break;
        }
    }

    return slot;
}

// 符号つきの積が int64 に収まることを確かめて返す(R8)
FX_FN int64_t RxMulS64Checked(int64_t a, int64_t b) {
    const FxU128 product = FxMulS64Full(a, b);
    FX_ASSERT(product.hi == ((int64_t)product.lo < 0 ? FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu) : (uint64_t)0));

    return (int64_t)product.lo;
}

// 128bit を右へずらした下位 64bit(はみ出す上位は捨てる。端数を取り出すため)
FX_FN uint64_t RxShiftRightLow64(FxU128 value, uint32_t shift) {
    if (shift == 0)
        return value.lo;

    if (shift < 64)
        return (value.lo >> shift) | (value.hi << (64 - shift));

    return shift < 128 ? value.hi >> (shift - 64) : (uint64_t)0;
}

// --- 熱と温度 ----------------------------------------------------------------------------------

// 化学のエネルギー(µJ)= Σ 物質量 × H0
template <typename Table, typename Cell>
FX_FN int64_t RxChemicalEnergy(Table table, Cell cell) {
    int64_t chemical = 0;
    for (uint32_t i = 0; i < RX_SPECIES_END(cell); ++i) {
        if (i >= cell.speciesCount)
            break;

        const int64_t term = RxMulS64Checked((int64_t)RX_AMOUNT_AT(cell, i), table.Species(RX_SPECIES_AT(cell, i)).h0);
        FX_ASSERT(!FxAddOverflowsS64(chemical, term));
        chemical += term;
    }

    return chemical;
}

// 熱容量(nJ/K)= Σ 物質量 × 比熱
template <typename Table, typename Cell>
FX_FN uint64_t RxHeatCapacity(Table table, Cell cell) {
    uint64_t heatCapacity = 0;
    for (uint32_t i = 0; i < RX_SPECIES_END(cell); ++i) {
        if (i >= cell.speciesCount)
            break;

        heatCapacity += RX_AMOUNT_AT(cell, i) * (uint64_t)table.Species(RX_SPECIES_AT(cell, i)).heatCapacity;
    }

    return heatCapacity;
}

// エネルギー(mJ)・化学のエネルギーの和・熱容量の和から熱と温度(RxComputeThermal の後半。溢れを 1 成分ずつ読んで足す GPU の変種も使う。T-0272)
FX_FN RxThermal RxThermalFromSums(int64_t energy, int64_t chemical, uint64_t heatCapacity) {
    RxThermal thermal;
    thermal.heat = RxMulS64Checked(energy, (int64_t)RX_MICROJOULES_PER_MILLIJOULE) - chemical;
    thermal.heatCapacity = heatCapacity;
    thermal.temperature = 0;
    FX_ASSERT(thermal.heat >= 0);  // 吸熱の規則は熱を資源として縮めるので、熱は負にならない
    if (heatCapacity == 0 || thermal.heat <= 0)
        return thermal;

    // 温度(mK)= 熱(µJ)× 1e6 ÷ 熱容量(nJ/K)。上位が熱容量以上なら 64bit に収まらない = 温度の上限を超える
    const FxU128 numerator = FxMulU64Full((uint64_t)thermal.heat, RX_MILLIKELVIN_NUMERATOR);
    FX_ASSERT(numerator.hi < heatCapacity);
    if (numerator.hi >= heatCapacity) {
        thermal.temperature = TEMPERATURE_MAX_MILLIKELVIN;
        return thermal;
    }

    const uint64_t temperature = FxDivU128By64(numerator, heatCapacity).quotient;
    FX_ASSERT(temperature <= (uint64_t)TEMPERATURE_MAX_MILLIKELVIN);
    thermal.temperature = (int32_t)(temperature < (uint64_t)TEMPERATURE_MAX_MILLIKELVIN
                                        ? temperature
                                        : (uint64_t)TEMPERATURE_MAX_MILLIKELVIN);

    return thermal;
}

template <typename Table, typename Cell>
FX_FN RxThermal RxComputeThermal(Table table, Cell cell) {
    return RxThermalFromSums(cell.energy, RxChemicalEnergy(table, cell), RxHeatCapacity(table, cell));
}

// --- 速度: 望む進行度 --------------------------------------------------------------------------

// 速度の表の値(上位 32bit = 仮数、下位 32bit = 2 の指数(int32))。値 = 仮数 × 2^指数
FX_FN uint64_t RxRateMantissa(uint64_t packed) {
    return packed >> 32;
}

FX_FN int32_t RxRateExponent(uint64_t packed) {
    return (int32_t)(uint32_t)(packed & FX_LOW32_MASK);
}

// --- 待ちの丸め: 遅い反応は「次に 1 単位進む刻み」を決める(T-0105・D-429。研究)-----------------------
// 今までの丸め(毎刻みの乱数と端数を比べる。T-0130 で消した)は、次に当たる刻みを閉じた式で求められないので、眠らせたまま起こす刻みを決められない。
// 待ちの丸めは、ブロックが最後に変わった刻み changedTick からの待ち n を幾何分布で引く: 進行度 = 整数部 + [刻み ≥ changedTick + n]。
//   - n は P(n = k) = (1 − f)^(k−1) f(f = 1 刻みの進みの端数)。乱数は (世界のシード, changedTick, セル, 規則の鍵) だけで決まり、刻みに依らない。
//   - 幾何分布は記憶が無いので、ブロックが変わるたびに引き直しても平均の速さは偏らない(Gillespie 1977 の 1 単位ずつの確率過程と同じ)。
//     毎刻み変わるセルでは n ≤ 1 ⇔ 確率 f で、今までの丸めと同じ分布になる。
//   - 速さ f はセルと規則の表だけで決まるので、ブロックが変わらない間は変わらない(表を替えた時は「変わった」とみなして引き直す。ADR-0018)。
//   - 眠ったブロックを changedTick + n(セル・規則の最小)に起こせば、全部を計算した場合とビット一致する。D-424 の下限は要らない。
// n = max(1, ceil(log2 U ÷ log2(1 − f)))、U = (乱数 | 1) ÷ 2^64。log2 は整数(FxLog2U64。R7)。f が 1/16 未満の時は log2(1 − f) を
// 級数(−ln(1 − f) = f Σ f^(k−1) ÷ k)で求める(1 − f を作ると f の下の桁が消えるので)。
// 待ちが約 2^61 刻み(60 刻み/秒で約 12 億年。割る数の正規化で 2^60.5〜2^61.6)を超える引きは「起きない」
FX_CONST uint32_t RX_WAIT_PURPOSE = 0x52780002u;  // 待ちの乱数の用途(FxHash64)
FX_CONST uint64_t RX_WAIT_NEVER = FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu);
FX_CONST uint64_t RX_WAIT_SERIES_LIMIT = FX_U64(0x10000000u, 0u);  // 端数(2^-64 単位)がこれ未満(f < 1/16)なら級数
FX_CONST uint32_t RX_WAIT_SERIES_TERMS = 16;                       // f < 1/16 なら 16 項で 2^-64 まで
FX_CONST uint32_t RX_WAIT_MAX_NUMERATOR_BITS = 122;                // 割られる数の桁の上限(商 = 待ちは 2^62 未満)
FX_CONST uint64_t RX_WAIT_TICK_LIMIT = FX_U64(0x40000000u, 0u);    // 待ちと刻みの印の上限 2^62(RxWakeTickOf)

// floor(2^64 ÷ k)(k = 2〜16。級数の 1 ÷ k)
FX_CONST uint64_t RX_RECIPROCAL_Q64[17] = {0u,
                                           0u,
                                           FX_U64(0x80000000u, 0x00000000u),
                                           FX_U64(0x55555555u, 0x55555555u),
                                           FX_U64(0x40000000u, 0x00000000u),
                                           FX_U64(0x33333333u, 0x33333333u),
                                           FX_U64(0x2AAAAAAAu, 0xAAAAAAAAu),
                                           FX_U64(0x24924924u, 0x92492492u),
                                           FX_U64(0x20000000u, 0x00000000u),
                                           FX_U64(0x1C71C71Cu, 0x71C71C71u),
                                           FX_U64(0x19999999u, 0x99999999u),
                                           FX_U64(0x1745D174u, 0x5D1745D1u),
                                           FX_U64(0x15555555u, 0x55555555u),
                                           FX_U64(0x13B13B13u, 0xB13B13B1u),
                                           FX_U64(0x12492492u, 0x49249249u),
                                           FX_U64(0x11111111u, 0x11111111u),
                                           FX_U64(0x10000000u, 0x00000000u)};

// 1 刻みの進みの期待値 = 整数部 + 端数 ÷ 2^64(µmol)
struct RxRate {
    uint64_t integerPart;
    uint64_t fraction;
};

FX_FN RxRate RxMakeRate(uint64_t integerPart, uint64_t fraction) {
    RxRate rate;
    rate.integerPart = integerPart;
    rate.fraction = fraction;

    return rate;
}

// 反応物の量の積 × 仮数 × 2^指数 を、整数部と 64bit の端数に分ける(下限は無い。端数は待ちで丸める)
FX_FN RxRate RxSplitRate(FxU128 product, uint64_t mantissa, int32_t exponent) {
    if (mantissa == 0 || (product.hi == 0 && product.lo == 0))
        return RxMakeRate(0, 0);

    // --- 積を 64bit に収める(下位を切り捨て、そのぶん指数に足す)---
    int32_t shift = exponent;
    uint64_t value = product.lo;
    if (product.hi != 0) {
        const uint32_t dropped = FxMsbU64(product.hi) + 1;
        value = RxShiftRightLow64(product, dropped);
        shift += (int32_t)dropped;
    }

    // scaled < 2^96
    const FxU128 scaled = FxMulU64Full(value, mantissa);

    // --- 2^shift 倍が整数になる(端数なし)---
    if (shift >= 0) {
        if (shift >= 62 || scaled.hi != 0 || (scaled.lo >> (uint32_t)(62 - shift)) != 0)
            return RxMakeRate(RX_EXTENT_SATURATION, 0);

        return RxMakeRate(scaled.lo << (uint32_t)shift, 0);
    }

    // --- 右へずらす: 整数の部分と、その下の 64bit の端数 ---
    const uint32_t right = (uint32_t)(-shift);
    if (right < 64 && (scaled.hi >> right) != 0)
        return RxMakeRate(RX_EXTENT_SATURATION, 0);

    const uint64_t integerPart = RxShiftRightLow64(scaled, right);
    if (integerPart >= RX_EXTENT_SATURATION)
        return RxMakeRate(RX_EXTENT_SATURATION, 0);

    const uint64_t fraction = right >= 64 ? RxShiftRightLow64(scaled, right - 64) : scaled.lo << (64 - right);

    return RxMakeRate(integerPart, fraction);
}

// −log2(1 − f) を 仮数 × 2^-scale で(f = fraction ÷ 2^64、0 < f < 1)
struct RxWaitDivisor {
    uint64_t mantissa;
    uint32_t scale;
};

FX_FN RxWaitDivisor RxWaitDivisorOf(uint64_t fraction) {
    RxWaitDivisor divisor;

    // --- f ≥ 1/16: 1 − f をそのまま log2(Q32)---
    if (fraction >= RX_WAIT_SERIES_LIMIT) {
        divisor.mantissa = ((uint64_t)64 << 32) - (uint64_t)FxLog2U64((uint64_t)0 - fraction);
        divisor.scale = 32;

        return divisor;
    }

    // --- f < 1/16: Σ f^(k−1) ÷ k(Q63)× log2 e(Q62)→ Q61、それに f を掛ける ---
    uint64_t power = FX_U64(0x80000000u, 0u);  // f^(k−1)(Q63)
    uint64_t series = power;
    for (uint32_t k = 2; k <= RX_WAIT_SERIES_TERMS; ++k) {
        power = FxMulHiU64(power, fraction);
        series += FxMulHiU64(power, RX_RECIPROCAL_Q64[k]);
    }

    const uint64_t perFraction = FxMulHiU64(series, FX_LOG2E_Q62);  // Q61。1.44〜1.49
    const uint32_t top = FxMsbU64(fraction);
    divisor.mantissa = FxMulHiU64(fraction << (63 - top), perFraction);  // = f × 2^-64 × … を 2^(124 − top) 倍した値
    divisor.scale = 124 - top;

    return divisor;
}

// 待ちの刻みの数 n ≥ 1(f = fraction ÷ 2^64 の幾何分布。hash は 64bit の乱数)。f = 0 なら RX_WAIT_NEVER
FX_FN uint64_t RxWaitTicks(uint64_t fraction, uint64_t hash) {
    if (fraction == 0)
        return RX_WAIT_NEVER;

    // −log2 U(Q32)。U = (hash | 1) ÷ 2^64 なので 0 < U < 1 で、値は 1 以上 2^38 以下
    const uint64_t numerator = ((uint64_t)64 << 32) - (uint64_t)FxLog2U64(hash | 1);
    const RxWaitDivisor divisor = RxWaitDivisorOf(fraction);

    // n = ceil(numerator × 2^-32 ÷ (仮数 × 2^-scale)) = ceil(numerator × 2^(scale − 32) ÷ 仮数)
    const uint32_t shift = divisor.scale - 32;
    if (FxMsbU64(numerator) + shift >= RX_WAIT_MAX_NUMERATOR_BITS)
        return RX_WAIT_NEVER;

    FxU128 shifted;
    shifted.hi = shift == 0 ? 0 : (shift >= 64 ? numerator << (shift - 64) : numerator >> (64 - shift));
    shifted.lo = shift >= 64 ? 0 : numerator << shift;
    const FxDivResult divided = FxDivU128By64(shifted, divisor.mantissa);
    const uint64_t ticks = divided.quotient + (divided.remainder != 0 ? (uint64_t)1 : (uint64_t)0);

    return ticks > 1 ? ticks : (uint64_t)1;
}

// --- 釣り合いの規則(T-0623・ADR-0290)---------------------------------------------------------------
// 釣り合いの規則は、今の成分と温度から「釣り合いまであとどれだけ進めるか」(距離)を整数で求め、1 刻みに距離 × min(1, k(T)/60) だけ進む。
// 距離は g(ξ) = log2 Q(ξ) − log2 K(T(ξ)) が負のままの最大の ξ(ξ だけ進めた後の成分と温度。温度は反応熱と比熱の変化を入れる)。
// g は ξ について増える(Q は生成物が増えて上がり、K は ΔH_eq と反応熱が同じ符号なら温度の変化で下がる)ので、整数の二分法で決まる。
// 凝縮相だけの規則(Q = 1)は「温度がちょうど T_eq になる所」まで進む = 融点が mK でちょうど。距離 0 なら進まない(眠れる)

FX_CONST uint32_t RX_BALANCE_PRODUCT_SHIFT = 4;       // balanceGasMask の生成物のビットの位置
FX_CONST uint32_t RX_BALANCE_MIN_MILLIKELVIN = 1000;  // K(T) を数える温度の下限(1/T が大きくなりすぎないように)
FX_CONST uint32_t RX_BALANCE_SEARCH_STEPS = 66;       // 二分法の回数の上限(0 と上限を見る 2 回 + 63 回)
FX_CONST uint64_t RX_BALANCE_GAP_LIMIT = FX_U64(0x40000000u, 0u);  // g の「無限」(物質が 0 になる)と、熱の変化の上限

// 温度(mK)= 熱(µJ)× 1e6 ÷ 熱容量(nJ/K)。熱が 0 以下なら 0(RxThermalFromSums と同じ割り算)
FX_FN uint32_t RxTemperatureOfHeat(int64_t heat, int64_t heatCapacity) {
    if (heat <= 0 || heatCapacity <= 0)
        return 0;

    const FxU128 numerator = FxMulU64Full((uint64_t)heat, RX_MILLIKELVIN_NUMERATOR);
    if (numerator.hi >= (uint64_t)heatCapacity)
        return (uint32_t)TEMPERATURE_MAX_MILLIKELVIN;

    const uint64_t temperature = FxDivU128By64(numerator, (uint64_t)heatCapacity).quotient;

    return (uint32_t)(temperature < (uint64_t)TEMPERATURE_MAX_MILLIKELVIN ? temperature
                                                                          : (uint64_t)TEMPERATURE_MAX_MILLIKELVIN);
}

// log2 K(T)(Q32)= 定数 + 急さ × (1/T_eq − 1/T)。1/T_eq − 1/T = (T − T_eq) × 1000 ÷ (T × T_eq)(mK で)
FX_FN int64_t RxBalanceLog2K(RxRule rule, uint32_t milliKelvin) {
    const uint64_t temperature = milliKelvin > RX_BALANCE_MIN_MILLIKELVIN ? (uint64_t)milliKelvin
                                                                          : (uint64_t)RX_BALANCE_MIN_MILLIKELVIN;
    const uint64_t balance = (uint64_t)rule.balanceKelvin;
    const bool warmer = temperature >= balance;
    const uint64_t difference = warmer ? temperature - balance : balance - temperature;
    const bool rising = rule.balanceSlope >= 0;
    const uint64_t slope = rising ? (uint64_t)rule.balanceSlope : (uint64_t)(-rule.balanceSlope);

    // 商 ≤ 急さ × 1000 ÷ min(T, T_eq) ≤ 急さ(T・T_eq ≥ 1 K)なので 64bit に収まる
    const FxU128 numerator = FxMulU64Full(slope, difference * 1000);
    const int64_t term = (int64_t)FxDivU128By64(numerator, temperature * balance).quotient;

    return rule.balanceLog2Constant + (warmer == rising ? term : -term);
}

// g(ξ) = log2 Q(ξ) − log2 K(T(ξ))(Q32)。extent ≤ 反応物の「ある量 ÷ 係数」の最小。
// 気体の反応物が 0 になれば +∞(それ以上進めない)、気体の生成物が 0 なら −∞(まだ進める)
template <typename Cell>
FX_FN int64_t RxBalanceGap(Cell cell, RxThermal thermal, RxRule rule, int64_t reactionEnthalpy, uint64_t extent) {
    int64_t log2Quotient = 0;
    bool reactantEmpty = false;
    bool productEmpty = false;
    FX_UNROLL for (uint32_t i = 0; i < RX_MAX_REACTANTS; ++i) {
        if (i >= rule.reactantCount || ((rule.balanceGasMask >> i) & 1) == 0)
            continue;

        const uint32_t slot = RxFindSlot(cell, rule.reactants[i]);
        const uint64_t amount = slot == RX_NO_SLOT ? (uint64_t)0 : RX_AMOUNT_AT(cell, slot);
        const uint64_t left = amount - (extent * (uint64_t)rule.reactantCoefficients[i]);
        reactantEmpty = reactantEmpty || left == 0;
        log2Quotient -= left == 0 ? (int64_t)0 : (int64_t)rule.reactantCoefficients[i] * FxLog2U64(left);
    }

    FX_UNROLL for (uint32_t i = 0; i < RX_MAX_PRODUCTS; ++i) {
        if (i >= rule.productCount || ((rule.balanceGasMask >> (RX_BALANCE_PRODUCT_SHIFT + i)) & 1) == 0)
            continue;

        const uint32_t slot = RxFindSlot(cell, rule.products[i]);
        const uint64_t amount = slot == RX_NO_SLOT ? (uint64_t)0 : RX_AMOUNT_AT(cell, slot);
        const uint64_t made = amount + (extent * (uint64_t)rule.productCoefficients[i]);
        productEmpty = productEmpty || made == 0;
        log2Quotient += made == 0 ? (int64_t)0 : (int64_t)rule.productCoefficients[i] * FxLog2U64(made);
    }

    if (reactantEmpty)
        return (int64_t)RX_BALANCE_GAP_LIMIT;

    if (productEmpty)
        return -(int64_t)RX_BALANCE_GAP_LIMIT;

    // --- ξ だけ進めた後の温度(熱 = 今の熱 − ξ × 反応熱・熱容量 = 今 + ξ × 比熱の変化。どちらも成分の和なので負にならない)---
    const bool endothermic = reactionEnthalpy >= 0;
    const FxU128 heatChange = FxMulU64Full(extent,
                                           endothermic ? (uint64_t)reactionEnthalpy : (uint64_t)(-reactionEnthalpy));
    const bool rising = rule.heatCapacityChange >= 0;
    const FxU128 capacityChange = FxMulU64Full(
        extent, rising ? (uint64_t)rule.heatCapacityChange : (uint64_t)(-rule.heatCapacityChange));
    const bool huge = heatChange.hi != 0 || heatChange.lo >= RX_BALANCE_GAP_LIMIT || capacityChange.hi != 0 ||
                      capacityChange.lo >= RX_BALANCE_GAP_LIMIT;
    const int64_t heat = endothermic ? thermal.heat - (int64_t)heatChange.lo : thermal.heat + (int64_t)heatChange.lo;
    const int64_t capacity = rising ? (int64_t)thermal.heatCapacity + (int64_t)capacityChange.lo
                                    : (int64_t)thermal.heatCapacity - (int64_t)capacityChange.lo;
    const uint32_t temperature = huge ? (endothermic ? 0u : (uint32_t)TEMPERATURE_MAX_MILLIKELVIN)
                                      : RxTemperatureOfHeat(heat, capacity);

    return log2Quotient - RxBalanceLog2K(rule, temperature);
}

// 釣り合いまでの距離(µmol の進行度。0〜ownLimit)。g(0) ≥ 0 なら 0(この向きには進まない)。
// 呼ぶ所を 1 か所にするため、0 と上限を見る 2 回と二分法を 1 つの回しで行う(HW の Work Graph の核を小さく。T-0397)
template <typename Table, typename Cell>
FX_FN uint64_t RxBalanceDistance(Table table, Cell cell, RxRule rule, int64_t reactionEnthalpy, uint64_t ownLimit) {
    const RxThermal thermal = RxComputeThermal(table, cell);
    uint64_t low = 0;
    uint64_t high = ownLimit;
    bool searching = true;
    for (uint32_t step = 0; step < RX_BALANCE_SEARCH_STEPS && searching; ++step) {
        const uint64_t probe = step == 0 ? (uint64_t)0 : (step == 1 ? ownLimit : low + ((high - low) >> 1));
        const bool below = RxBalanceGap(cell, thermal, rule, reactionEnthalpy, probe) < 0;
        if (step == 0) {
            searching = below;
        } else if (step == 1) {
            low = below ? ownLimit : low;
            searching = !below && high - low > 1;
        } else {
            low = below ? probe : low;
            high = below ? high : probe;
            searching = high - low > 1;
        }
    }

    return low;
}

// 規則 rule のこのセルでの 1 刻みの進みの期待値(反応物が 1 つでも無ければ 0)と、反応物それぞれの「ある量 ÷ 係数」の最小
struct RxRuleRate {
    RxRate rate;
    uint64_t ownLimit;
};

template <typename Table, typename Cell>
FX_FN RxRuleRate RxComputeRuleRate(Table table, Cell cell, uint32_t ruleId, RxRule rule, uint32_t kelvin) {
    RxRuleRate result;
    result.rate = RxMakeRate(0, 0);
    result.ownLimit = 0;

    // 次数 1 の反応物の量(2 つまで)。配列にして factorCount で書くと局所の配列になるので 2 つの値で持つ(T-1233)
    uint64_t firstFactor = 1;
    uint64_t secondFactor = 1;
    uint32_t factorCount = 0;
    uint64_t ownLimit = RX_EXTENT_SATURATION;
    bool missing = false;
    for (uint32_t i = 0; i < rule.reactantCount; ++i) {
        const uint32_t slot = RxFindSlot(cell, RX_REACTANT_AT(rule, i));
        if (slot == RX_NO_SLOT) {
            missing = true;
            break;
        }

        const uint64_t amount = RX_AMOUNT_AT(cell, slot);
        const uint64_t limit = amount / (uint64_t)RX_REACTANT_COEFFICIENT_AT(rule, i);
        ownLimit = limit < ownLimit ? limit : ownLimit;
        if (((rule.firstOrderMask >> i) & 1) != 0 && factorCount < 2) {
            firstFactor = factorCount == 0 ? amount : firstFactor;
            secondFactor = factorCount == 1 ? amount : secondFactor;
            factorCount += 1;
        }
    }

    if (missing)
        return result;

    const uint64_t packed = table.Rate(ruleId, kelvin);
    if (rule.balanceKelvin == 0 || ownLimit == 0) {
        result.rate = RxSplitRate(FxMulU64Full(firstFactor, secondFactor), RxRateMantissa(packed),
                                  RxRateExponent(packed));
        result.ownLimit = ownLimit;

        return result;
    }

    // --- 釣り合いの規則: 距離 × k(T)/60(k は 1/s。表は次数に依らず 1 刻みの割合)。距離を越えない ---
    const uint64_t distance = RxBalanceDistance(table, cell, rule, table.Rule(ruleId).reactionEnthalpy, ownLimit);
    const RxRate approach = RxSplitRate(FxMulU64Full(distance, 1), RxRateMantissa(packed), RxRateExponent(packed));
    const bool reaches = approach.integerPart >= distance;
    result.rate = RxMakeRate(reaches ? distance : approach.integerPart, reaches ? (uint64_t)0 : approach.fraction);
    result.ownLimit = distance;

    return result;
}

// 待ちの丸めの望む進行度と、changedTick から数えて何刻み目に初めて 0 でなくなるか(wait。整数部があれば 1、進めないなら RX_WAIT_NEVER)
struct RxWaitSample {
    uint64_t value;
    uint64_t wait;
};

template <typename Table, typename Cell>
FX_FN RxWaitSample RxDesiredExtentWait(Table table, Cell cell, uint32_t ruleId, RxRule rule, uint32_t kelvin,
                                       uint64_t waitSeed, uint64_t elapsed) {
    const RxRuleRate ruleRate = RxComputeRuleRate(table, cell, ruleId, rule, kelvin);
    RxWaitSample sample;
    sample.value = 0;
    sample.wait = RX_WAIT_NEVER;
    if (ruleRate.ownLimit == 0)
        return sample;

    const uint64_t ticks = RxWaitTicks(ruleRate.rate.fraction, FxHashCombine(waitSeed, rule.key));
    const uint64_t desired = ruleRate.rate.integerPart + (elapsed >= ticks ? (uint64_t)1 : (uint64_t)0);
    sample.value = desired < ruleRate.ownLimit ? desired : ruleRate.ownLimit;
    sample.wait = ruleRate.rate.integerPart != 0 ? (uint64_t)1 : ticks;

    return sample;
}

// 待ちの丸めで候補を集める。規則は「ID が最小の反応物」の索引にだけ入っているので、成分を順に見れば重複なく引ける。wait は進めない規則も含めた待ちの最小。
// offered は進む(望む進行度が 0 でない)規則の数。RX_MAX_CANDIDATES を超えたら、優先度(RxSelectPriority)の小さい RX_MAX_CANDIDATES 個を残す
// (T-0022。前は 17 個目以降を捨てていたので、ID の大きい物質の規則だけが系統的に起きなかった)
struct RxWaitCandidates {
    RxCandidates candidates;
    uint64_t wait;
    uint32_t offered;
};

// 候補が上限を超えた時に残す順(小さいほど残る)。刻みとセルごとの種と規則の鍵から作るので、表の並び・規則の ID・集める順に依らない
FX_FN uint64_t RxSelectPriority(uint64_t selectSeed, RxRule rule) {
    return FxHashCombine(selectSeed, rule.key);
}

// 規則 a が b より先に残るか(優先度、同じなら規則の ID)
FX_FN bool RxSelectBefore(uint64_t priorityA, uint32_t ruleA, uint64_t priorityB, uint32_t ruleB) {
    return priorityA < priorityB || (priorityA == priorityB && ruleA < ruleB);
}

// 候補が一杯の時に規則 ruleId(望む進行度 value)を入れる: 残っている中で最も後に回る候補より先なら置き換える。
// 集め終わった時に、全部の進む規則のうち優先度の小さい RX_MAX_CANDIDATES 個が残る(どの順に来ても同じ集合)
template <typename Table>
FX_FN RxCandidates RxReplaceLastCandidate(Table table, RxCandidates candidates, uint32_t ruleId, uint64_t value,
                                          uint64_t selectSeed) {
    // 回しは決まった数なので開く(添字 c が定数になり、候補の欄を選ぶ道具が 1 つの欄の読みに畳まれる)。最も後に回る候補の規則は
    // 値で持つ(動的な添字 last で選び直さない。T-1233 の形では 1 回の置き換えで 16 × 16 の選びになり、成分の多いセルで遅くなった。T-1234)
    uint32_t last = 0;
    uint32_t lastRule = 0;
    uint64_t lastPriority = 0;
    FX_UNROLL for (uint32_t c = 0; c < RX_MAX_CANDIDATES; ++c) {
        const uint32_t candidateRule = RX_CANDIDATE_RULE(candidates, c);
        const uint64_t priority = RxSelectPriority(selectSeed, table.RateRule(candidateRule));
        if (c == 0 || RxSelectBefore(lastPriority, lastRule, priority, candidateRule)) {
            last = c;
            lastRule = candidateRule;
            lastPriority = priority;
        }
    }

    if (!RxSelectBefore(RxSelectPriority(selectSeed, table.RateRule(ruleId)), ruleId, lastPriority, lastRule))
        return candidates;

    RX_SET_CANDIDATE(candidates, last, ruleId, value);

    return candidates;
}

template <typename Table, typename Cell>
FX_FN RxWaitCandidates RxCollectCandidatesWait(Table table, Cell cell, uint32_t kelvin, uint64_t waitSeed,
                                               uint64_t elapsed, uint64_t selectSeed) {
    RxWaitCandidates result;
    result.wait = RX_WAIT_NEVER;
    result.offered = 0;
    result.candidates.count = 0;
    FX_UNROLL for (uint32_t i = 0; i < RX_MAX_CANDIDATES; ++i) {
        result.candidates.rules[i] = 0;
        result.candidates.extents[i] = 0;
    }

    for (uint32_t slot = 0; slot < cell.speciesCount; ++slot) {
        const RxSpecies species = table.Species(RX_SPECIES_AT(cell, slot));
        for (uint32_t j = 0; j < species.ruleCount; ++j) {
            const uint32_t ruleId = table.RuleIndex(species.ruleBegin + j);
            const RxWaitSample sample = RxDesiredExtentWait(table, cell, ruleId, table.RateRule(ruleId), kelvin,
                                                            waitSeed, elapsed);
            result.wait = sample.wait < result.wait ? sample.wait : result.wait;
            if (sample.value == 0)
                continue;

            result.offered += 1;
            if (result.candidates.count >= RX_MAX_CANDIDATES) {
                result.candidates = RxReplaceLastCandidate(table, result.candidates, ruleId, sample.value, selectSeed);
                continue;
            }

            RX_SET_CANDIDATE(result.candidates, result.candidates.count, ruleId, sample.value);
            result.candidates.count += 1;
        }
    }

    return result;
}

// --- 取り合いの解決(02 §3 の 3。並び順に依存しない)------------------------------------------------

// 規則の候補が使う量の合計(反応物は必ずセルにあるので、成分の位置で持つ)と、吸熱の規則が使う熱。
// セルの形ごとに型が違う(RxWideCell は RxWideUsage)ので、核は RxEmptyUsage(cell) で作ったものを引数で受け取って型を決める
struct RxUsage {
    uint64_t amounts[RX_MAX_CELL_SPECIES];
    uint64_t heat;
    uint64_t gas;  // 気体を増やす規則が増やす気体のぶん(µmol。減らす規則の分は差し引かない)
};

#if !defined(__cplusplus) && !defined(RX_CELL_ACCESSORS)
// 使う量を読む・足す(RxUsage の版。上の成分の道具と同じく、添字が一致する欄だけを選ぶ・書く。T-1233)
FX_FN uint64_t RxUsageOf(RxUsage usage, uint32_t i) {
    return RxPickU64(usage.amounts, i);
}

FX_FN void RxAddUsageAt(inout RxUsage usage, uint32_t i, uint64_t amount) {
    RxAddAtU64(usage.amounts, i, amount);
}
#endif

// 0 の使う量(セルの形の道具。RxCell はインラインの数だけ)
FX_FN RxUsage RxEmptyUsage(RxCell cell) {
    RX_UNUSED(cell);
    RxUsage usage;
    // HLSL には範囲 for が無い
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i)
        usage.amounts[i] = 0;

    usage.heat = 0;
    usage.gas = 0;

    return usage;
}

// 候補が使う量を usage(RxEmptyUsage(cell) の 0 から)に足す
template <typename Table, typename Cell, typename Usage>
FX_FN Usage RxSumUsage(Table table, Cell cell, RxCandidates candidates, Usage usage) {
    for (uint32_t c = 0; c < candidates.count; ++c) {
        // 候補の欄は 1 回ずつ選んで値で持つ(T-1234)。反応物は RateRule から、反応熱と気体の増え方だけを Rule から読む
        // (規則の構造体を丸ごと動的な添字で読まない。T-0397)
        const uint32_t ruleId = RX_CANDIDATE_RULE(candidates, c);
        const uint64_t extent = RX_CANDIDATE_EXTENT(candidates, c);
        const RxRule rule = table.RateRule(ruleId);
        const int64_t reactionEnthalpy = table.Rule(ruleId).reactionEnthalpy;
        const uint32_t gasGrowth = table.Rule(ruleId).gasGrowth;
        for (uint32_t i = 0; i < rule.reactantCount; ++i) {
            const uint32_t slot = RxFindSlot(cell, RX_REACTANT_AT(rule, i));
            const uint64_t amount = extent * (uint64_t)RX_REACTANT_COEFFICIENT_AT(rule, i);
            FX_ASSERT(RX_USAGE_AT(usage, slot) + amount >= amount);
            RX_ADD_USAGE(usage, slot, amount);
        }

        if (reactionEnthalpy > 0) {
            const FxU128 heat = FxMulU64Full(extent, (uint64_t)reactionEnthalpy);
            FX_ASSERT(heat.hi == 0 && usage.heat + heat.lo >= usage.heat);
            usage.heat += heat.lo;
        }

        if (gasGrowth > 0) {
            const FxU128 gas = FxMulU64Full(extent, (uint64_t)gasGrowth);
            FX_ASSERT(gas.hi == 0 && usage.gas + gas.lo >= usage.gas);
            usage.gas += gas.lo;
        }
    }

    return usage;
}

// 縮めるときの乱数(規則ごと)。取り合いの種と規則の鍵のハッシュの上位 32bit(下位 32bit は今までの丸め〔T-0130 で消した〕が使っていた)
FX_FN uint32_t RxShrinkRandom(uint64_t randomSeed, RxRule rule) {
    return (uint32_t)(FxHashCombine(randomSeed, rule.key) >> 32);
}

// extent × available ÷ demand を確率的に丸める(available < demand のときだけ呼ぶ。T-0106・D-431)。
// 端数 = 余り ÷ demand を乱数 random(2^-32 単位)と比べる: random ÷ 2^32 < 余り ÷ demand ⇔ random × demand < 余り × 2^32。
// 切り捨てだと、取り合いで 1 刻み 1 未満に縮んだ脇の反応(酸素不足の火の炭の燃焼など)が毎刻み 0 になって消える
// 縮めた進行度: 切り捨てと、確率的に丸めた値(切り捨てか、その + 1)
struct RxShrunk {
    uint64_t truncated;
    uint64_t rounded;
};

FX_FN RxShrunk RxShrink(uint64_t extent, uint64_t available, uint64_t demand, uint32_t random) {
    const FxDivResult divided = FxDivU128By64(FxMulU64Full(extent, available), demand);
    const FxU128 threshold = FxMulU64Full((uint64_t)random, demand);
    const uint64_t remainderHigh = divided.remainder >> 32;
    const uint64_t remainderLow = divided.remainder << 32;
    const bool roundUp = threshold.hi < remainderHigh || (threshold.hi == remainderHigh && threshold.lo < remainderLow);

    RxShrunk shrunk;
    shrunk.truncated = divided.quotient;
    shrunk.rounded = divided.quotient + (roundUp ? (uint64_t)1 : (uint64_t)0);

    return shrunk;
}

// 物質でない資源の、この刻みに使える量: 吸熱の規則が使える熱(µJ)と、気体を増やす規則が使えるセルの気体の器の残り(µmol)
struct RxAvailable {
    uint64_t heat;
    uint64_t gas;
};

// セルの気体の器の残り(µmol)= 器(CELL_GAS_AMOUNT_LIMIT。気体の式の Q32 の器。units.hlsli・ADR-0260)− 1 − 今の気体のぶん。
// 気体のぶんが器に届いていれば 0(T-0622・ADR-0316)
template <typename Table, typename Cell>
FX_FN uint64_t RxGasRoom(Table table, Cell cell) {
    uint64_t gas = 0;
    for (uint32_t i = 0; i < RX_SPECIES_END(cell); ++i) {
        if (i >= cell.speciesCount)
            break;

        const bool counted = (table.Species(RX_SPECIES_AT(cell, i)).flags & RX_SPECIES_GAS) != 0;
        const uint64_t amount = counted ? RX_AMOUNT_AT(cell, i) : (uint64_t)0;
        gas = amount < CELL_GAS_AMOUNT_LIMIT_MICROMOLES - gas ? gas + amount : CELL_GAS_AMOUNT_LIMIT_MICROMOLES;
    }

    return gas < CELL_GAS_AMOUNT_LIMIT_MICROMOLES ? CELL_GAS_AMOUNT_LIMIT_MICROMOLES - 1 - gas : (uint64_t)0;
}

// 規則が、ある量を超えて使われている資源(物質か、吸熱なら熱か、気体を増やすなら気体の器)を使うか。
// 反応物は rule(RateRule)、反応熱は reactionEnthalpy・気体の増え方は gasGrowth(Rule)
template <typename Cell, typename Usage>
FX_FN bool RxOverdraws(RxRule rule, int64_t reactionEnthalpy, uint32_t gasGrowth, Cell cell, Usage used,
                       RxAvailable available) {
    bool overdrawn = false;
    for (uint32_t i = 0; i < rule.reactantCount; ++i) {
        const uint32_t slot = RxFindSlot(cell, RX_REACTANT_AT(rule, i));
        overdrawn = overdrawn || RX_USAGE_AT(used, slot) > RX_AMOUNT_AT(cell, slot);
    }

    return overdrawn || (reactionEnthalpy > 0 && used.heat > available.heat) ||
           (gasGrowth > 0 && used.gas > available.gas);
}

// 丸め上げた規則(roundedUp のビット)のせいで足りなくなった資源があれば、その資源を使う丸め上げた規則のうち
// 進行度が最も大きいもの(同じなら鍵が小さいもの)から 1 つずつ丸め上げを戻す。大きい規則ほど 1 の差の割合が小さいので、
// 偏りは主な反応に寄せ、脇の反応の期待値は崩さない。切り捨ての値は必ずある量以下なので、全部戻せば必ず足りる。
// used = resolved の使う量(RxSumUsage。型をセルの形に合わせるため引数で受け取る)
template <typename Table, typename Cell, typename Usage>
FX_FN RxCandidates RxRevokeRoundUps(Table table, Cell cell, RxCandidates resolved, uint32_t roundedUp,
                                    RxAvailable available, Usage used) {
    for (uint32_t step = 0; step < RX_MAX_CANDIDATES && roundedUp != 0; ++step) {
        // 選んだ候補の進行度は値で持つ(動的な添字 pick で選び直さない。T-1234)
        uint32_t pick = RX_NO_SLOT;
        uint64_t pickKey = 0;
        uint64_t pickExtent = 0;
        for (uint32_t c = 0; c < resolved.count; ++c) {
            const uint32_t ruleId = RX_CANDIDATE_RULE(resolved, c);
            const RxRule rule = table.RateRule(ruleId);
            const int64_t reactionEnthalpy = table.Rule(ruleId).reactionEnthalpy;
            const uint32_t gasGrowth = table.Rule(ruleId).gasGrowth;
            if (((roundedUp >> c) & 1) == 0 || !RxOverdraws(rule, reactionEnthalpy, gasGrowth, cell, used, available))
                continue;

            const uint64_t extent = RX_CANDIDATE_EXTENT(resolved, c);
            const bool larger = pick == RX_NO_SLOT || extent > pickExtent ||
                                (extent == pickExtent && rule.key < pickKey);
            if (!larger)
                continue;

            pick = c;
            pickKey = rule.key;
            pickExtent = extent;
        }

        if (pick == RX_NO_SLOT)
            break;

        RX_SET_CANDIDATE_EXTENT(resolved, pick, pickExtent - 1);
        roundedUp &= ~(1u << pick);
        used = RxSumUsage(table, cell, resolved, RxEmptyUsage(cell));
    }

    return resolved;
}

// 物質ごとに「要求の合計」と「ある量」を比べ、足りない資源を使う規則を min(ある量 ÷ 要求) の比で縮める。
// 吸熱の規則は熱(の 1/8。RX_ENDOTHERMIC_HEAT_SHIFT)も資源として同じく縮める(熱が負にならない)。
// 気体を増やす規則はセルの気体の器の残り(RxGasRoom)も資源として同じく縮める(気体のぶんが気体の式の器を超えない。T-0622・ADR-0316)。
// どの規則も元の進行度に同じ比を掛けるので、並び順は結果に影響しない。
// 縮めた値は確率的に丸める(規則ごとに 1 つの乱数を、足りない資源のどれにも使う。丸めは単調なので、資源ごとに丸めた最小 = 比の最小を丸めた値)。
// 丸め上げで足りなくなった資源があれば RxRevokeRoundUps が戻すので、縮めた後の消費の合計は必ずある量以下
// demand = 候補の使う量の合計(RxSumUsage。型をセルの形に合わせるため引数で受け取る)
template <typename Table, typename Cell, typename Usage>
FX_FN RxCandidates RxShrinkToDemand(Table table, Cell cell, RxThermal thermal, RxCandidates candidates,
                                    uint64_t randomSeed, Usage demand) {
    RxAvailable available;
    available.heat = thermal.heat > 0 ? (uint64_t)thermal.heat >> RX_ENDOTHERMIC_HEAT_SHIFT : (uint64_t)0;
    available.gas = demand.gas > 0 ? RxGasRoom(table, cell) : (uint64_t)0;  // 気体を増やす候補が無ければ数えない

    // --- 縮める(元の進行度から、足りない資源ごとの比の最小)---
    RxCandidates resolved = candidates;
    uint32_t roundedUp = 0;
    for (uint32_t c = 0; c < candidates.count; ++c) {
        const uint32_t ruleId = RX_CANDIDATE_RULE(candidates, c);  // 候補の欄は 1 回ずつ選ぶ(T-1234)
        const RxRule rule = table.RateRule(ruleId);                // 反応物と鍵
        const int64_t reactionEnthalpy = table.Rule(ruleId).reactionEnthalpy;
        const uint32_t gasGrowth = table.Rule(ruleId).gasGrowth;
        const uint64_t extent = RX_CANDIDATE_EXTENT(candidates, c);
        const uint32_t random = RxShrinkRandom(randomSeed, rule);
        uint64_t result = extent;
        uint64_t truncated = extent;
        for (uint32_t i = 0; i < rule.reactantCount; ++i) {
            const uint32_t slot = RxFindSlot(cell, RX_REACTANT_AT(rule, i));
            if (RX_USAGE_AT(demand, slot) <= RX_AMOUNT_AT(cell, slot))
                continue;

            const RxShrunk shrunk = RxShrink(extent, RX_AMOUNT_AT(cell, slot), RX_USAGE_AT(demand, slot), random);
            result = shrunk.rounded < result ? shrunk.rounded : result;
            truncated = shrunk.truncated < truncated ? shrunk.truncated : truncated;
        }

        if (reactionEnthalpy > 0 && demand.heat > available.heat) {
            const RxShrunk shrunk = RxShrink(extent, available.heat, demand.heat, random);
            result = shrunk.rounded < result ? shrunk.rounded : result;
            truncated = shrunk.truncated < truncated ? shrunk.truncated : truncated;
        }

        if (gasGrowth > 0 && demand.gas > available.gas) {
            const RxShrunk shrunk = RxShrink(extent, available.gas, demand.gas, random);
            result = shrunk.rounded < result ? shrunk.rounded : result;
            truncated = shrunk.truncated < truncated ? shrunk.truncated : truncated;
        }

        RX_SET_CANDIDATE_EXTENT(resolved, c, result);
        roundedUp |= result > truncated ? (1u << c) : 0u;
    }

    if (roundedUp == 0)
        return resolved;

    return RxRevokeRoundUps(table, cell, resolved, roundedUp, available,
                            RxSumUsage(table, cell, resolved, RxEmptyUsage(cell)));
}

template <typename Table, typename Cell>
FX_FN RxCandidates RxResolveContention(Table table, Cell cell, RxThermal thermal, RxCandidates candidates,
                                       uint64_t randomSeed) {
    return RxShrinkToDemand(table, cell, thermal, candidates, randomSeed,
                            RxSumUsage(table, cell, candidates, RxEmptyUsage(cell)));
}

// --- 成分を更新する ----------------------------------------------------------------------------

// 物質量が 0 になった成分を消して詰める(並びは保つ)
template <typename Cell>
FX_FN Cell RxCompactCell(Cell cell) {
    Cell result = RxEmptyCellLike(cell, cell.energy);
    for (uint32_t i = 0; i < RX_SPECIES_END(cell); ++i) {
        if (i >= cell.speciesCount)
            break;

        if (RX_AMOUNT_AT(cell, i) == 0)
            continue;

        result = RxCellWithRoom(result);
        RX_SET_ENTRY(result, result.speciesCount, RX_SPECIES_AT(cell, i), RX_AMOUNT_AT(cell, i));
        result.speciesCount += 1;
    }

    return result;
}

// 物質を足した結果。overflowed = 成分がインラインの数を超えるので足せなかった(cell は足す前のまま。RxWideCell では立たない)
template <typename Cell>
struct RxAddedOf {
    Cell cell;
    uint32_t overflowed;
};

typedef RxAddedOf<RxCell> RxAdded;

// 物質を足す(無ければ ID の順の位置に差し込む)。入りきらなければ足さずに overflowed を立てる(呼ぶ側が保存を守る)
template <typename Cell>
FX_FN RxAddedOf<Cell> RxTryAddSpecies(Cell cell, uint32_t speciesId, uint64_t amount) {
    RxAddedOf<Cell> added;
    added.cell = cell;
    added.overflowed = 0;
    if (amount == 0)
        return added;

    uint32_t position = 0;
    while (position < cell.speciesCount && RX_SPECIES_AT(cell, position) < speciesId)
        position += 1;

    if (position < cell.speciesCount && RX_SPECIES_AT(cell, position) == speciesId) {
        FX_ASSERT(RX_AMOUNT_AT(cell, position) + amount >= amount);
        RX_ADD_AMOUNT(added.cell, position, amount);
        return added;
    }

    if (!RxHasRoomForSpecies(cell)) {
        added.overflowed = 1;
        return added;
    }

    added.cell = RxCellWithRoom(cell);
    for (uint32_t i = cell.speciesCount; i > position; --i) {
        RX_SET_ENTRY(added.cell, i, RX_SPECIES_AT(cell, i - 1), RX_AMOUNT_AT(cell, i - 1));
    }

    RX_SET_ENTRY(added.cell, position, speciesId, amount);
    added.cell.speciesCount += 1;

    return added;
}

// 物質を足す。呼ぶ側が入りきることを保証する所だけで使う(初めのセル・畳んだ値〔成分は 8 つまで〕・影の引き戻し〔親の成分だけ〕)。
// 反応の生成物は RxApplyExtents が入りきるかを確かめてから足す(T-0022)
template <typename Cell>
FX_FN Cell RxAddSpecies(Cell cell, uint32_t speciesId, uint64_t amount) {
    const RxAddedOf<Cell> added = RxTryAddSpecies(cell, speciesId, amount);
    FX_ASSERT(added.overflowed == 0);

    return added.cell;
}

// 決まった進行度で反応物を引き、生成物を足す。エネルギーは変えない(化学のエネルギーが熱に変わるだけ)。
// 生成物が入りきらなければ overflowed を立てる(その時の cell は使わない。RxApplyExtentsHeld が規則を待たせてやり直す)
template <typename Table, typename Cell>
FX_FN RxAddedOf<Cell> RxApplyExtentsChecked(Table table, Cell cell, RxCandidates resolved) {
    // --- 反応物を引く(差し込みで位置がずれる前に。呼ぶ側の cell は残す)---
    RX_MAKE_WRITABLE(cell);
    for (uint32_t c = 0; c < resolved.count; ++c) {
        const RxRule rule = table.RateRule(RX_CANDIDATE_RULE(resolved, c));  // 反応物
        const uint64_t extent = RX_CANDIDATE_EXTENT(resolved, c);            // 候補の欄は 1 回ずつ選ぶ(T-1234)
        for (uint32_t i = 0; i < rule.reactantCount; ++i) {
            const uint32_t slot = RxFindSlot(cell, RX_REACTANT_AT(rule, i));
            const uint64_t amount = extent * (uint64_t)RX_REACTANT_COEFFICIENT_AT(rule, i);
            FX_ASSERT(amount <= RX_AMOUNT_AT(cell, slot));
            RX_SUB_AMOUNT(cell, slot, amount);
        }
    }

    // --- 生成物を足す ---
    RxAddedOf<Cell> result;
    result.cell = RxCompactCell(cell);
    result.overflowed = 0;
    for (uint32_t c = 0; c < resolved.count; ++c) {
        // 生成物の欄は決まった数だけ回す(規則を動的な添字で読むと、GPU でローカルの配列になる。T-0397)
        const RxRule rule = table.Rule(RX_CANDIDATE_RULE(resolved, c));
        const uint64_t extent = RX_CANDIDATE_EXTENT(resolved, c);
        FX_UNROLL for (uint32_t i = 0; i < RX_MAX_PRODUCTS; ++i) {
            if (i >= rule.productCount)
                break;

            const uint64_t amount = extent * (uint64_t)rule.productCoefficients[i];
            const RxAddedOf<Cell> added = RxTryAddSpecies(result.cell, rule.products[i], amount);
            result.cell = added.cell;
            result.overflowed |= added.overflowed;
        }
    }

    return result;
}

// セル cell に無い物質を作る規則の進行度を 0 にする(この刻みは進めない)。ほかの規則の生成物はどれも cell にある物質なので、
// 反応物が使い切られて消えても足し直す位置があり、成分の数は cell の数を超えない(= 必ず入りきる)
template <typename Table, typename Cell>
FX_FN RxCandidates RxHoldNewSpeciesRules(Table table, Cell cell, RxCandidates resolved) {
    for (uint32_t c = 0; c < resolved.count; ++c) {
        const RxRule rule = table.Rule(RX_CANDIDATE_RULE(resolved, c));
        bool makesNew = false;
        FX_UNROLL for (uint32_t i = 0; i < RX_MAX_PRODUCTS; ++i)  // 決まった数だけ回す(上と同じ。T-0397)
            makesNew = makesNew || (i < rule.productCount && RxFindSlot(cell, rule.products[i]) == RX_NO_SLOT);

        if (makesNew)
            RX_SET_CANDIDATE_EXTENT(resolved, c, 0);
    }

    return resolved;
}

// 反応物を引き、生成物を足す。生成物が入りきらなければ、セルに無い物質を作る規則をこの刻みは待たせて(進行度 0)やり直す
// (T-0022。前は 9 種目の生成物を黙って捨て、反応物は引いてあったので元素とエネルギーの保存が破れた)。
// 待たせた規則の取り合いの分け前はほかの規則に回さない(使う量が減るだけなので、ある量を超えない)。overflowed = 待たせた
template <typename Table, typename Cell>
FX_FN RxAddedOf<Cell> RxApplyExtentsHeld(Table table, Cell cell, RxCandidates resolved) {
    RxCandidates applying = resolved;
    RxAddedOf<Cell> applied;
    applied.cell = cell;
    applied.overflowed = 0;
    uint32_t held = 0;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        applied = RxApplyExtentsChecked(table, cell, applying);
        if (applied.overflowed == 0)
            break;

        applying = RxHoldNewSpeciesRules(table, cell, applying);
        held = 1;
    }

    FX_ASSERT(applied.overflowed == 0);
    applied.overflowed = held;

    return applied;
}

// --- 1 セルの 1 刻み(02 §3。待ちの丸め。T-0105・D-429)-------------------------------------------------------------------

// 取り合いの丸めの乱数の種は (世界のシード, 刻み, セルの ID) から作る(R6)。規則ごとの乱数は、これに規則の鍵を混ぜる
FX_FN uint64_t RxRandomSeed(uint64_t worldSeed, uint64_t tick, uint64_t cellId) {
    return FxHash64(worldSeed, tick, cellId, RX_RANDOM_PURPOSE);
}

// 候補が上限を超えた時に選ぶ乱数の種(取り合いの種と同じ入力で用途だけ違う。刻みごとに選び直すので、どの規則も平均して同じ割合で進む)
FX_FN uint64_t RxSelectSeed(uint64_t worldSeed, uint64_t tick, uint64_t cellId) {
    return FxHash64(worldSeed, tick, cellId, RX_SELECT_PURPOSE);
}

// 1 刻みの結果: 新しいセル、その熱、変わらなければ次に評価が要る刻み(wakeTick。進めないなら RX_WAIT_NEVER)。
// 刻み tick ≤ t < wakeTick の間にこのセルを評価しても(ブロックが変わらなければ)何も変わらないので、眠らせてよい。
// limits = 上限に当たった印(RX_LIMIT_*。保存は守った上で、進め方を変えた)
template <typename Cell>
struct RxWaitStepOf {
    Cell cell;
    RxThermal thermal;
    uint64_t wakeTick;
    uint32_t limits;
};

typedef RxWaitStepOf<RxCell> RxWaitStep;

// 待ちの乱数の種は (世界のシード, ブロックが最後に変わった刻み, セルの ID) から作る。規則ごとの乱数は、これに規則の鍵を混ぜる
FX_FN uint64_t RxWaitSeed(uint64_t worldSeed, uint64_t changedTick, uint64_t cellId) {
    return FxHash64(worldSeed, changedTick, cellId, RX_WAIT_PURPOSE);
}

// changedTick から wait 刻み目の印(wait = RX_WAIT_NEVER なら RX_WAIT_NEVER)。待ちは 2^62 未満(RxWaitTicks の商)、印も 2^62 未満
// (60 刻み/秒で約 24 億年)なので足しても溢れない(R8: 飽和させない)。
// 「溢れたら RX_WAIT_NEVER」の飽和する足し算の形(wait >= ~changedTick ? ~0 : 和)は、NVIDIA のドライバ(RTX 3070 Ti)で
// 結果の下位 32bit が 0xFFFFFFFF になった(T-0124)。比べるのは RX_WAIT_NEVER との一致だけにする
FX_FN uint64_t RxWakeTickOf(uint64_t changedTick, uint64_t wait) {
    FX_ASSERT(changedTick < RX_WAIT_TICK_LIMIT);
    FX_ASSERT(wait == RX_WAIT_NEVER || wait < RX_WAIT_TICK_LIMIT);

    return wait == RX_WAIT_NEVER ? RX_WAIT_NEVER : changedTick + wait;
}

// 刻み tick のセルを評価する。changedTick = セルのブロックが最後に変わった刻み(tick より前。刻みの初めに読んだ値)。
// 取り合いの丸めは今までどおり刻みの乱数(取り合うのは進む規則があるセル = 起きているセルだけ)
template <typename Table, typename Cell>
FX_FN RxWaitStepOf<Cell> RxStepCellWait(Table table, Cell cell, uint64_t worldSeed, uint64_t tick, uint64_t changedTick,
                                        uint64_t cellId) {
    FX_ASSERT(changedTick < tick);
    RxWaitStepOf<Cell> step;
    step.cell = cell;
    step.thermal = RxComputeThermal(table, cell);
    step.limits = 0;

    const uint32_t kelvin = (uint32_t)step.thermal.temperature / (uint32_t)MILLIKELVIN_PER_KELVIN;
    const uint32_t tableKelvin = kelvin < RX_RATE_TABLE_KELVINS ? kelvin : RX_RATE_TABLE_KELVINS - 1;
    const RxWaitCandidates collected = RxCollectCandidatesWait(
        table, cell, tableKelvin, RxWaitSeed(worldSeed, changedTick, cellId), tick - changedTick,
        RxSelectSeed(worldSeed, tick, cellId));
    step.limits |= collected.offered > RX_MAX_CANDIDATES ? RX_LIMIT_CANDIDATES : 0u;

    // --- 進む規則が無い: 待ちの最小の刻みまで変わらない ---
    if (collected.candidates.count == 0) {
        step.wakeTick = RxWakeTickOf(changedTick, collected.wait);

        return step;
    }

    // --- 進む規則がある: 変わればブロックの changedTick が tick になる。変わらなくても次の刻みにまた評価する ---
    const RxCandidates resolved = RxResolveContention(table, cell, step.thermal, collected.candidates,
                                                      RxRandomSeed(worldSeed, tick, cellId));
    const RxAddedOf<Cell> applied = RxApplyExtentsHeld(table, cell, resolved);
    step.cell = applied.cell;
    step.limits |= applied.overflowed != 0 ? RX_LIMIT_PRODUCTS : 0u;
    step.thermal = RxComputeThermal(table, step.cell);
    step.wakeTick = tick + 1;

    return step;
}

// --- 1 セルだけのブロック(反応の試験の CPU リファレンスと shaders/sim/reaction_cells.hlsl。T-0130)-----------------

// 1 セルだけのブロック: セルと、それが最後に変わった刻み(待ちの丸めの changedTick)と、上限に当たった刻みの数(RX_LIMIT_* ごと)
struct RxLoneCell {
    RxCell cell;
    uint64_t changedTick;
    uint32_t productsHeldTicks;     // RX_LIMIT_PRODUCTS の刻みの数
    uint32_t candidatesLimitTicks;  // RX_LIMIT_CANDIDATES の刻みの数
};

FX_FN RxLoneCell RxMakeLoneCell(RxCell cell, uint64_t changedTick) {
    RxLoneCell lone;
    lone.cell = cell;
    lone.changedTick = changedTick;
    lone.productsHeldTicks = 0;
    lone.candidatesLimitTicks = 0;

    return lone;
}

// 2 つのセルがビット単位で同じか(padding は見ない)
FX_FN bool RxSameCell(RxCell a, RxCell b) {
    bool same = a.energy == b.energy && a.speciesCount == b.speciesCount;
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i)
        same = same && a.species[i] == b.species[i] && a.amounts[i] == b.amounts[i];

    return same;
}

// 刻み tickBegin から tickCount 刻み、待ちの丸めで進める(tickBegin > changedTick)。セルが変わった刻みを changedTick にする
template <typename Table>
FX_FN RxLoneCell RxAdvanceLoneCell(Table table, RxLoneCell lone, uint64_t worldSeed, uint64_t tickBegin,
                                   uint32_t tickCount, uint64_t cellId) {
    for (uint32_t i = 0; i < tickCount; ++i) {
        const uint64_t tick = tickBegin + i;
        const RxWaitStep step = RxStepCellWait(table, lone.cell, worldSeed, tick, lone.changedTick, cellId);
        if (!RxSameCell(step.cell, lone.cell))
            lone.changedTick = tick;

        lone.cell = step.cell;
        lone.productsHeldTicks += (step.limits & RX_LIMIT_PRODUCTS) != 0 ? 1u : 0u;
        lone.candidatesLimitTicks += (step.limits & RX_LIMIT_CANDIDATES) != 0 ? 1u : 0u;
    }

    return lone;
}

RX_NAMESPACE_END

#endif  // BICAMERAL_REACTION_HLSLI
