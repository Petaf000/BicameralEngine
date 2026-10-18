// multires_table_words.hlsli — GPU の多重解像度の世界の「表の語」(t2。uint32 の配列)の並び(T-0766・T-0773・ADR-0343 Proposed)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。C++ が書き(下の AppendTableWords。engine/src/sim/gpu_multires.cpp)、
// GPU が同じ読む関数(Tw*)で読む(反応の索引は shaders/sim/multires_bindings.hlsli の MR_TABLE_WORDS_RULE_INDEX、相の表と世界の端は
// 伝導の段 shaders/sim/multires_conduct.hlsli だけが include する)。試験は multires_edge_test が C++ で写して読み戻す。
//
// なぜ 1 本か: 多重解像度のルート署名は 64 / 64 語で、表のバッファ(t0〜t3)を足せない。反応の索引(前の t2)の前に見出しを置き、
// 後ろにセルの見立ての相の表(ADR-0333 の 3。物質ごとの 16 バイト)と世界の端(ADR-0337。箱・端の種類・外の柱)を置く。
// 相の表は反応の表と一緒に作り直し(表の差し替え)、世界の端は木を写す時(RecordUpload)に MultiresNest::edges と違えば作り直す。
// 並び(語):
//   [0〜3] 見出し: 相の表の始まり・世界の端の始まり・外の柱の始まり・外の柱の行の数
//   [4〜]  反応の索引(TW_HEADER_WORDS + RxSpecies::ruleBegin からの位置)
//   [相の表] 物質ごとに 4 語: モル体積(下位・上位)・モル質量・相(CvSpeciesPhase)
//   [世界の端] 18 語: 根のレベル・箱の low x・y・z と high x・y・z(各 下位・上位)・上・下・横・外の柱の 0 行目の縦(下位・上位)
//   [外の柱] 行ごとに 27 語: エネルギー(下位・上位)・物質の数・物質 × 8・物質量 × 8(下位・上位)
// 浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_MULTIRES_TABLE_WORDS_HLSLI
#define BICAMERAL_MULTIRES_TABLE_WORDS_HLSLI

#include "cell_view.hlsli"
#include "world_edge.hlsli"

#ifdef __cplusplus
#include <span>
#include <vector>
#endif

MR_NAMESPACE_BEGIN

// --- 並び ---------------------------------------------------------------------------------------
FX_CONST uint32_t TW_HEADER_WORDS = 4;
FX_CONST uint32_t TW_HEADER_PHASES = 0;                         // 相の表の始まり(語)
FX_CONST uint32_t TW_HEADER_EDGES = 1;                          // 世界の端の始まり(語)
FX_CONST uint32_t TW_HEADER_COLUMN = 2;                         // 外の柱の始まり(語)
FX_CONST uint32_t TW_HEADER_COLUMN_ROWS = 3;                    // 外の柱の行の数(0 = 外は真空)
FX_CONST uint32_t TW_PHASE_WORDS = 4;                           // 物質 1 つの相の値
FX_CONST uint32_t TW_EDGE_WORDS = 18;                           // 世界の箱と端の種類と外の柱の 0 行目
FX_CONST uint32_t TW_CELL_WORDS = 3 + RX_MAX_CELL_SPECIES * 3;  // 外の柱のセル 1 つ

// --- 読む(Words は uint32_t Word(uint32_t at) を持つ。HLSL は t2、C++ は配列)-----------------------------

template <typename Words>
FX_FN uint64_t TwU64(Words words, uint32_t at) {
    return FX_U64(words.Word(at + 1), words.Word(at));
}

template <typename Words>
FX_FN int64_t TwS64(Words words, uint32_t at) {
    return (int64_t)TwU64(words, at);
}

// 反応の索引の position 番目(reaction.hlsli の Table の RuleIndex)
template <typename Words>
FX_FN uint32_t TwRuleIndex(Words words, uint32_t position) {
    return words.Word(TW_HEADER_WORDS + position);
}

// 物質 id の相の値(cell_view.hlsli の Table の Phase)
template <typename Words>
FX_FN CvSpeciesPhase TwPhase(Words words, uint32_t id) {
    const uint32_t at = words.Word(TW_HEADER_PHASES) + TW_PHASE_WORDS * id;

    CvSpeciesPhase phase;
    phase.molarVolume = TwU64(words, at);
    phase.molarMass = words.Word(at + 2);
    phase.phase = words.Word(at + 3);

    return phase;
}

// 世界の箱と端の種類(world_edge.hlsli の WeWorld)
template <typename Words>
FX_FN WeWorld TwWorldEdges(Words words) {
    const uint32_t at = words.Word(TW_HEADER_EDGES);

    WeWorld world;
    world.rootLevel = (int32_t)words.Word(at);
    world.lowX = TwS64(words, at + 1);
    world.lowY = TwS64(words, at + 3);
    world.lowZ = TwS64(words, at + 5);
    world.highX = TwS64(words, at + 7);
    world.highY = TwS64(words, at + 9);
    world.highZ = TwS64(words, at + 11);
    world.top = words.Word(at + 13);
    world.bottom = words.Word(at + 14);
    world.sides = words.Word(at + 15);

    return world;
}

// 外の柱の 0 行目のレベル 0 の縦の座標・行の数
template <typename Words>
FX_FN int64_t TwColumnBaseUp(Words words) {
    return TwS64(words, words.Word(TW_HEADER_EDGES) + 16);
}

template <typename Words>
FX_FN uint32_t TwColumnRows(Words words) {
    return words.Word(TW_HEADER_COLUMN_ROWS);
}

// 外の柱の行 row のセル
template <typename Words>
FX_FN RxCell TwColumnCell(Words words, uint32_t row) {
    const uint32_t at = words.Word(TW_HEADER_COLUMN) + TW_CELL_WORDS * row;

    RxCell cell;
    cell.energy = TwS64(words, at);
    cell.speciesCount = words.Word(at + 2);
    cell.padding = 0;
    FX_UNROLL
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        cell.species[i] = words.Word(at + 3 + i);
        cell.amounts[i] = TwU64(words, at + 3 + RX_MAX_CELL_SPECIES + (2 * i));
    }

    return cell;
}

#ifdef __cplusplus
// --- 書く(C++ だけ)-----------------------------------------------------------------------------

struct TwVectorWords {
    std::span<const uint32_t> words;

    [[nodiscard]] uint32_t Word(uint32_t at) const { return words[at]; }
};

inline void TwAppendU64(std::vector<uint32_t>& words, uint64_t value) {
    words.push_back(static_cast<uint32_t>(value));
    words.push_back(static_cast<uint32_t>(value >> 32));
}

// 表の語を作る: 反応の索引・相の表(物質ごと)・世界の端(箱と端の種類・外の柱。柱が空なら外は真空)
inline std::vector<uint32_t> MakeTableWords(std::span<const uint32_t> ruleIndex, std::span<const CvSpeciesPhase> phases,
                                            const WeWorld& world, int64_t columnBaseUp,
                                            std::span<const RxCell> column) {
    std::vector<uint32_t> words(TW_HEADER_WORDS, 0);
    words.insert(words.end(), ruleIndex.begin(), ruleIndex.end());

    // --- 相の表 ---
    words[TW_HEADER_PHASES] = static_cast<uint32_t>(words.size());
    for (const CvSpeciesPhase& phase : phases) {
        TwAppendU64(words, phase.molarVolume);
        words.push_back(phase.molarMass);
        words.push_back(phase.phase);
    }

    // --- 世界の端 ---
    words[TW_HEADER_EDGES] = static_cast<uint32_t>(words.size());
    words.push_back(static_cast<uint32_t>(world.rootLevel));
    for (const int64_t value : {world.lowX, world.lowY, world.lowZ, world.highX, world.highY, world.highZ})
        TwAppendU64(words, static_cast<uint64_t>(value));

    words.push_back(world.top);
    words.push_back(world.bottom);
    words.push_back(world.sides);
    TwAppendU64(words, static_cast<uint64_t>(columnBaseUp));

    // --- 外の柱 ---
    words[TW_HEADER_COLUMN] = static_cast<uint32_t>(words.size());
    words[TW_HEADER_COLUMN_ROWS] = static_cast<uint32_t>(column.size());
    for (const RxCell& cell : column) {
        TwAppendU64(words, static_cast<uint64_t>(cell.energy));
        words.push_back(cell.speciesCount);
        words.insert(words.end(), std::begin(cell.species), std::end(cell.species));
        for (const uint64_t amount : cell.amounts)
            TwAppendU64(words, amount);
    }

    return words;
}
#endif

MR_NAMESPACE_END

#endif  // BICAMERAL_MULTIRES_TABLE_WORDS_HLSLI
