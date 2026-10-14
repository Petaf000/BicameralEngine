// lab_box.hlsl — 実験室の箱(T-0142。common/lab_box.hlsli)のコマンドを、刻みの前に箱のセルへ当てる Compute。
// ルート署名は多重解像度のもの(multires_bindings.hlsli。sim/gpu_multires.cpp)に、外のバッファ u4(この刻みのコマンドの列)を足して使う。
// 呼ぶのは sim/gpu_lab_box.cpp(GpuMultires::RecordExternalDispatch)。CPU リファレンスは sim/lab_box.cpp の ApplyLabCommands。
//
// 1 スレッドだけで列の順((targetTick, sequence) の昇順。CPU が並べて写す)に当てる。同じセルに当たるコマンドの順が結果を決めるので、
// 並べて走らせない(数は 1 刻みに LAB_MAX_COMMANDS_PER_TICK まで。費用は小さい)。
// 箱は根を並べたもの(T-0173。根の枠は x → y → z の順に 0 から)。当てたセルを含む根だけをつつく。
// 物質の一覧が変わる表に替えた刻み(T-0242・ADR-0065)は、コマンドの前に RemapLabSpecies で箱の全部のセルを名前で付け替える
// (u4 には付け替えの表〔sim::PackSpeciesRemap の語の列〕を結ぶ。CPU リファレンスは sim/lab_box.cpp の RemapLabBox)。
#include "common/lab_box.hlsli"
#include "common/species_remap.hlsli"
#include "sim/multires_bindings.hlsli"

RWStructuredBuffer<uint32_t> g_labCommands : register(u4);  // LabCommand × 数(LAB_COMMAND_WORDS 語ずつ)
// common/lab_box.hlsli の Blocks の約束(箱の根の枠)
struct LabBlocks {
    uint32_t unused;

    MrBlock Block(uint32_t slot) { return g_blocks[slot]; }
};

LabBlocks MakeLabBlocks() {
    LabBlocks blocks;
    blocks.unused = 0;

    return blocks;
}

// common/lab_box.hlsli の Cells の約束: 枠 slot のセル local にコマンドを当てる。ブロックが一様なら当てない(箱の根はいつも頁を持ち、
// 頁を持つ親を細かくした子も頁を持つ: sim/lab_box.cpp の MakeLabBoxNest・17 §5。CPU の ApplyLabCommands も同じ)。当てたら true
struct LabCells {
    uint32_t unused;

    bool Apply(uint32_t slot, uint32_t local, LabCommand command) {
        const MrBlock block = g_blocks[slot];
        if (MrIsUniform(block))
            return false;

        const uint32_t address = PageCellAddress(block.page, local);
        g_cells[address] = LabApplyCommand(MakeTable(), g_cells[address], command);

        return true;
    }
};

LabCells MakeLabCells() {
    LabCells cells;
    cells.unused = 0;

    return cells;
}

// つついた根の木の全部の枠(根・細かくした子と孫。T-0314)を「つつかれた」にする
void PokeRoots(LabBoxShape shape, uint64_t roots) {
    for (uint32_t root = 0; root < LabRootCount(shape); ++root) {
        if (((roots >> root) & 1) == 0)
            continue;

        for (uint32_t ordinal = 0; ordinal < LAB_SUBTREE_SLOTS; ++ordinal) {
            const uint32_t slot = LabSubtreeSlot(MakeLabBlocks(), root, ordinal);
            if (slot != MR_NO_BLOCK)
                g_blocks[slot].busyTick = MR_BUSY_POKED;
        }
    }
}

// g_external0 = コマンドの数、g_external1 = 箱の形(LabPackShape。T-0173)、g_external2 = 表の物質の数
[numthreads(1, 1, 1)] void ApplyLabCommands() {
    const LabBoxShape shape = LabUnpackShape(g_external1);
    uint64_t touched = 0;  // つつく根(LabRootBit)
    for (uint32_t i = 0; i < g_external0; ++i) {
        LabCommand command;
        for (uint32_t word = 0; word < LAB_COMMAND_WORDS; ++word)
            command.words[word] = g_labCommands[(i * LAB_COMMAND_WORDS) + word];

        // --- 表を替えた印: セルは変えず、箱の根を全部つつくだけ(sim/lab_box.cpp の ApplyLabCommands と同じ。T-0218)---
        if (LabCommandMarksTable(command)) {
            touched |= LabAllRoots(shape);
            continue;
        }

        // --- 範囲(T-0220): 範囲の葉のセル全部(細かくした所は細かいセル全部。T-0314。sim/lab_box.cpp と同じ順)---
        if (LabRegionCommandValid(command, g_external2, shape)) {
            touched |= LabApplyToTarget(MakeLabBlocks(), MakeLabCells(), shape, LabRegionTarget(command), command);
            continue;
        }

        const uint32_t cell = LabCommandCell(command, g_external2, shape);
        if (cell != LAB_NO_CELL)
            touched |= LabApplyToTarget(MakeLabBlocks(), MakeLabCells(), shape, LabCellTarget(cell), command);
    }

    // --- 変えた根の木は「つつかれた」(次の刻みで変わったとみなして全部のセルを評価し直す。multires_tree.cpp の PokeBlock)---
    PokeRoots(shape, touched);
}

// --- 物質の付け替え(T-0242・ADR-0065): u4 = 付け替えの表(common/species_remap.hlsli の「GPU のバッファの形」を 32bit の語で)---
// common/species_remap.hlsli の Remap の約束(sim/probe_bindings.hlsli の ProbeSpeciesRemap と同じ並びを、語の配列で読む)
struct LabSpeciesRemap {
    uint32_t speciesCount;
    uint32_t partCount;
    uint32_t unitCount;

    uint32_t NewIdWord() { return RX_REMAP_HEADER_WORDS + (2 * speciesCount); }

    uint32_t PartBeginWord() { return NewIdWord() + speciesCount; }

    uint32_t PartWord() { return PartBeginWord() + speciesCount + 1; }

    uint32_t UnitWord() { return PartWord() + (2 * partCount); }

    uint32_t NewId(uint32_t oldId) { return g_labCommands[NewIdWord() + oldId]; }

    uint32_t PartBegin(uint32_t oldId) { return g_labCommands[PartBeginWord() + oldId]; }

    RxRemapPart Part(uint32_t index) {
        RxRemapPart part;
        part.species = g_labCommands[PartWord() + (2 * index)];
        part.atomsPerMol = g_labCommands[PartWord() + (2 * index) + 1];

        return part;
    }

    int64_t RemovedH0(uint32_t oldId) {
        const uint32_t word = RX_REMAP_HEADER_WORDS + (2 * oldId);
        const uint64_t bits = (uint64_t)g_labCommands[word] | ((uint64_t)g_labCommands[word + 1] << 32);

        return (int64_t)bits;
    }

    uint32_t UnitCount() { return unitCount; }

    RxRemapUnit Unit(uint32_t index) {
        RxRemapUnit unit;
        unit.species = g_labCommands[UnitWord() + (2 * index)];
        unit.atoms = g_labCommands[UnitWord() + (2 * index) + 1];

        return unit;
    }
};

// g_external0 = 箱のセルの数(u1 の全部 = 一様の値 × 枠 + 頁 × 512。使っていないセルは空なので付け替えても空のまま)。
// 1 スレッド = 1 セル。新しい表はもう結んである(単体の h0 を新しい表から読む。CPU リファレンスは sim/lab_box.cpp の RemapLabBox)
[numthreads(64, 1, 1)] void RemapLabSpecies(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t index = dispatchThreadId.x;
    if (index >= g_external0)
        return;

    LabSpeciesRemap remap;
    remap.speciesCount = g_labCommands[0];
    remap.partCount = g_labCommands[1];
    remap.unitCount = g_labCommands[2];

    g_cells[index] = RxRemapCell(remap, MakeTable(), g_cells[index]).cell;
}
