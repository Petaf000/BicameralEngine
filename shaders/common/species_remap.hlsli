// species_remap.hlsli — セル 1 つの物質 ID を名前で付け替える(物質を足す・消す反応表の差し替え。T-0223・ADR-0065・D-438)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(engine/src/sim/species_remap.cpp の RemapReactionCell)と
// GPU の付け替えの段(shaders/sim/probe_tick.hlsl の RemapSpecies)が同じ関数を呼ぶので、同じセル・同じ付け替えの表からはビット単位で同じ結果になる。
//
// データの流れ: 付け替えの表(engine/src/sim/species_remap.cpp の BuildSpeciesRemap が今の表と新しい表から作る)+ 今の表の ID のセル
//   → RxRemapCell → 新しい表の ID のセル(成分は ID の昇順のまま・物質量 0 の成分は作らない)
// 規則(詳しくは engine/src/sim/species_remap.h):
//   - 残る物質は ID だけ替える(名前のバイト順の ID なので、残る物質どうしの順は変わらない = 昇順のまま)。
//   - 消えた物質は元素ごとの単体へ原子の数を保って分ける。単体は新しい表の ID の昇順に 1 つずつ、そのセルの消えた物質の原子を全部集めて
//     単体の原子の数で割る(端数は失う)。入りきらない単体(成分の上限)は足さない(失う)。どちらも報告する。
//   - 熱を保つ: 分けた分の化学のエネルギーの差(µJ)を mJ に切り上げてエネルギーに足す(熱は減らず、増えても 1 mJ 未満)。
//
// 付け替えの表の読み方は呼ぶ側が Remap 型で渡す(C++ は配列の span、HLSL はバッファを読む構造体)。Remap に要るメソッド:
//   uint32_t NewId(uint32_t oldId)(0 = 消えた)/ uint32_t PartBegin(uint32_t oldId)(PartBegin(oldId + 1) までが分けた先)/
//   RxRemapPart Part(uint32_t index) / int64_t RemovedH0(uint32_t oldId)(今の表の h0)/
//   uint32_t UnitCount() / RxRemapUnit Unit(uint32_t index)(使う単体。新しい表の ID の昇順)
// 新しい表は Table 型(reaction.hlsli と同じ。h0 を読む)。
//
// GPU のバッファの形(ByteAddressBuffer。sim::PackSpeciesRemap が作る。h0 は 8 バイト境界):
//   見出し 4 語 [今の物質の数 n・分けた先の数 p・単体の数 u・0] → RemovedH0 × n(int64)→ NewId × n → PartBegin × (n + 1)
//   → Part × p(species・atomsPerMol)→ Unit × u(species・atoms)
#ifndef BICAMERAL_SPECIES_REMAP_HLSLI
#define BICAMERAL_SPECIES_REMAP_HLSLI

#include "reaction.hlsli"

RX_NAMESPACE_BEGIN

// 消えた物質 1 mol を分けた先の 1 つ(元素 1 つぶん)
struct RxRemapPart {
    uint32_t species;      // 新しい表の単体の ID
    uint32_t atomsPerMol;  // 消えた物質 1 mol に入っているこの元素の原子の数(mol)
};

// 分けた先に使う単体
struct RxRemapUnit {
    uint32_t species;  // 新しい表の ID
    uint32_t atoms;    // 単体 1 mol の原子の数(mol)
};

struct RxRemapResult {
    RxCell cell;

    // --- 報告(セル 1 つぶん)---
    uint64_t decomposed;      // 元素に分けた物質の量(µmol)
    uint64_t remainderAtoms;  // 単体の原子の数で割り切れずに失った原子(µmol)
    uint64_t overflowAtoms;   // 成分の上限に入らずに失った原子(µmol)
    int64_t energyDelta;      // エネルギーに足した量(mJ)
};

FX_CONST uint32_t RX_REMAP_HEADER_WORDS = 4;

// 切り上げの割り算(divisor > 0。0 以下は 0 方向の切り捨てが切り上げ)
FX_FN int64_t RxCeilDivS64(int64_t value, int64_t divisor) {
    if (value <= 0)
        return FxDivS64(value, divisor);

    return FxDivS64(value - 1, divisor) + 1;
}

// セルの消えた物質のうち、単体 unitSpecies へ分ける原子の数(µmol)
template <typename Remap>
FX_FN uint64_t RxRemapUnitAtoms(Remap remap, RxCell cell, uint32_t unitSpecies) {
    uint64_t atoms = 0;
    for (uint32_t slot = 0; slot < cell.speciesCount; ++slot) {
        const uint32_t species = cell.species[slot];
        if (remap.NewId(species) != 0)
            continue;

        for (uint32_t index = remap.PartBegin(species); index < remap.PartBegin(species + 1); ++index) {
            const RxRemapPart part = remap.Part(index);
            if (part.species == unitSpecies)
                atoms += cell.amounts[slot] * (uint64_t)part.atomsPerMol;
        }
    }

    return atoms;
}

template <typename Remap, typename Table>
FX_FN RxRemapResult RxRemapCell(Remap remap, Table table, RxCell cell) {
    RxRemapResult result;
    result.cell = RxMakeEmptyCell(cell.energy);
    result.decomposed = 0;
    result.remainderAtoms = 0;
    result.overflowAtoms = 0;
    result.energyDelta = 0;

    // --- 残る物質は ID だけ替える(昇順のまま)。消えた物質の化学のエネルギー(今の表)を数える ---
    int64_t removedChemical = 0;
    for (uint32_t slot = 0; slot < cell.speciesCount; ++slot) {
        const uint32_t species = cell.species[slot];
        const uint32_t newId = remap.NewId(species);
        if (newId == 0) {
            result.decomposed += cell.amounts[slot];
            removedChemical += RxMulS64Checked((int64_t)cell.amounts[slot], remap.RemovedH0(species));
            continue;
        }

        result.cell.species[result.cell.speciesCount] = newId;
        result.cell.amounts[result.cell.speciesCount] = cell.amounts[slot];
        result.cell.speciesCount += 1;
    }

    // --- 単体を ID の昇順に足す(足した分の化学のエネルギーは新しい表)---
    int64_t addedChemical = 0;
    for (uint32_t index = 0; index < remap.UnitCount(); ++index) {
        const RxRemapUnit unit = remap.Unit(index);
        const uint64_t atoms = RxRemapUnitAtoms(remap, cell, unit.species);
        const uint64_t amount = atoms / (uint64_t)unit.atoms;
        result.remainderAtoms += atoms % (uint64_t)unit.atoms;
        if (amount == 0)
            continue;

        const RxAdded added = RxTryAddSpecies(result.cell, unit.species, amount);
        if (added.overflowed != 0) {
            result.overflowAtoms += amount * (uint64_t)unit.atoms;
            continue;
        }

        result.cell = added.cell;
        addedChemical += RxMulS64Checked((int64_t)amount, table.Species(unit.species).h0);
    }

    // --- 熱を保つ: 化学のエネルギーの差を mJ に切り上げて足す ---
    result.energyDelta = RxCeilDivS64(addedChemical - removedChemical, (int64_t)RX_MICROJOULES_PER_MILLIJOULE);
    result.cell.energy += result.energyDelta;

    return result;
}

RX_NAMESPACE_END

#endif  // BICAMERAL_SPECIES_REMAP_HLSLI
