// lab_box.hlsl — 実験室の箱(T-0142。common/lab_box.hlsli)のコマンドを、刻みの前に箱のセルへ当てる Compute。
// ルート署名は多重解像度のもの(multires_bindings.hlsli。sim/gpu_multires.cpp)に、外のバッファ u4(この刻みのコマンドの列)を足して使う。
// 呼ぶのは sim/gpu_lab_box.cpp(GpuMultires::RecordExternalDispatch)。CPU リファレンスは sim/lab_box.cpp の ApplyLabCommands。
//
// 1 スレッドだけで列の順((targetTick, sequence) の昇順。CPU が並べて写す)に当てる。同じセルに当たるコマンドの順が結果を決めるので、
// 並べて走らせない(数は 1 刻みに LAB_MAX_COMMANDS_PER_TICK まで。費用は小さい)。
#include "common/lab_box.hlsli"
#include "sim/multires_bindings.hlsli"

RWStructuredBuffer<uint32_t> g_labCommands : register(u4);  // LabCommand × 数(LAB_COMMAND_WORDS 語ずつ)

// g_external0 = コマンドの数、g_external1 = 箱の枠、g_external2 = 表の物質の数
[numthreads(1, 1, 1)] void ApplyLabCommands() {
    const uint32_t slot = g_external1;
    const MrBlock block = g_blocks[slot];
    if (MrIsUniform(block))
        return;  // 箱はいつも頁を持つ(sim/lab_box.cpp の MakeLabBoxNest)。持たなければ当てない(CPU も同じ)

    bool applied = false;
    for (uint32_t i = 0; i < g_external0; ++i) {
        LabCommand command;
        for (uint32_t word = 0; word < LAB_COMMAND_WORDS; ++word)
            command.words[word] = g_labCommands[(i * LAB_COMMAND_WORDS) + word];

        const uint32_t cell = LabCommandCell(command, g_external2);
        if (cell == LAB_NO_CELL)
            continue;

        const uint32_t address = PageCellAddress(block.page, cell);
        g_cells[address] = LabApplyCommand(MakeTable(), g_cells[address], command);
        applied = true;
    }

    // --- 変えたブロックは「つつかれた」(次の刻みで変わったとみなして全部のセルを評価し直す。multires_tree.cpp の PokeBlock)---
    if (applied)
        g_blocks[slot].busyTick = MR_BUSY_POKED;
}
