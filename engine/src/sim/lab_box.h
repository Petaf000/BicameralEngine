// lab_box.h — 実験室の箱(14 §2「実験室」・T-0142・ADR-0037)の CPU 側: コマンドを作る・箱を作る・CPU リファレンスの 1 刻み・
// CPU と GPU の箱を比べて最初に違ったセルを探す・実験の記録(コマンドの列と刻みごとのハッシュ)の読み書き。GPU を知らない。
//
// データの流れ:
//   パネル(editor/lab_panel)が置く操作を Command(LAB_COMMAND_TYPE_*。common/lab_box.hlsli)にする
//   → CPU: StepLabBox(コマンドを当てる → StepNest。反応 + 熱の伝導)/ GPU: sim/gpu_lab_box(同じ関数を GPU で)
//   → FindLabMismatch で刻みごとに比べる。食い違えば最初の刻みとセル。
//   記録(LabRecording)はコマンドの列とハッシュの列だけ(初めの箱は決まっている: 300 K の空気)。読み直して流せば同じ実験になる。
// 箱は多重解像度の木の本物の根 1 つ(8³ セル、0.5 m 角 = 4 m 角。外とは熱も物も通さない)。仮の世界(ProbeSim)とは別の世界。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/lab_box.hlsli"
#include "sim/command.h"
#include "sim/multires_nest.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    inline constexpr uint32_t LAB_BOX_SLOT = 0;  // 箱の根の枠
    inline constexpr uint32_t LAB_BOX_EDGE = multires::MR_BLOCK_EDGE;
    inline constexpr uint32_t LAB_MAX_COMMANDS_PER_TICK = 64;  // 1 刻みに当てるコマンドの上限(GPU の写しの大きさ)
    inline constexpr uint64_t LAB_WORLD_SEED = 20261009;
    inline constexpr int32_t LAB_INITIAL_TEMPERATURE_MILLIKELVIN = 300000;

    // 箱の刻み: 反応(待ちの丸め)+ 熱の伝導(17 §4。陰解法・小刻みは使わない: 箱は 1 レベルだけ)
    inline constexpr MultiresStepOptions LAB_STEP_OPTIONS = {.conduction = true};

    struct LabCellPosition {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t z = 0;

        [[nodiscard]] uint32_t Index() const { return multires::MrCellIndex(x, y, z); }
    };

    // --- コマンド ---

    // セルの中身を物質(LAB_MAX_FILL_SPECIES まで)と温度で置き換える
    [[nodiscard]] Command MakeLabFillCommand(uint64_t targetTick, uint32_t sequence, LabCellPosition cell,
                                             std::span<const SpeciesAmount> contents, uint32_t temperatureMilliKelvin);

    // セルの温度を決める(成分はそのまま)
    [[nodiscard]] Command MakeLabTemperatureCommand(uint64_t targetTick, uint32_t sequence, LabCellPosition cell,
                                                    uint32_t temperatureMilliKelvin);

    // 64 バイトをそのまま uint32 × 16 で読む(GPU に写す形と同じ)
    [[nodiscard]] lab::LabCommand ToLabCommand(const Command& command);

    // --- 試験の表の材料(パネルの「置く」の選択肢。tests/packages/combustion_test と同じ物質の名前)---

    struct LabMaterial {
        std::string_view name;  // 表示の名前
        std::vector<SpeciesAmount> contents;
    };

    // 空気・木(セルロース 1 割 + 孔の空気。仮の世界の木箱の壁と同じ)・木炭・二酸化炭素・窒素。表に無い物質の材料は入らない
    [[nodiscard]] std::vector<LabMaterial> MakeLabMaterials(const BakedReactionTable& table);

    // --- 箱 ---

    [[nodiscard]] MultiresCapacity LabBoxCapacity(const BakedReactionTable& table);

    // 300 K の空気を満たした箱(根はいつも頁を持つ。GPU のコマンドは頁のセルに書くため)
    [[nodiscard]] MultiresNest MakeLabBoxNest(const BakedReactionTable& table);

    // 箱のセル index(MrCellIndex)
    [[nodiscard]] reaction::RxCell LabBoxCell(const MultiresNest& nest, uint32_t index);

    // CPU リファレンス: 1 刻みのコマンド(並びは (targetTick, sequence) の昇順)を shaders/sim/lab_box.hlsl と同じ順で当てる。
    // 当てた数を返す(当てられないコマンドは飛ばす)
    uint32_t ApplyLabCommands(MultiresNest& nest, const BakedReactionTable& table, std::span<const Command> commands);

    // CPU リファレンスの 1 刻み: コマンドを当てる → StepNest(LAB_STEP_OPTIONS)
    void StepLabBox(MultiresNest& nest, const BakedReactionTable& table, uint64_t tick,
                    std::span<const Command> commands);

    // --- 比べる ---

    struct LabMismatch {
        uint64_t tick = 0;
        uint32_t
            cell = lab::LAB_NO_CELL;  // 最初に違ったセル(LAB_NO_CELL ならセルは同じで、見出し・端数・帳簿などが違う)
        reaction::RxCell cpu{};
        reaction::RxCell gpu{};
        std::string what;  // 表示用の説明
    };

    // 状態の全部(HashWholeNest)が違えば、最初に違ったセル(番号の順)か、セル以外の欄の名前
    [[nodiscard]] std::optional<LabMismatch> FindLabMismatch(const MultiresNest& cpu, const MultiresNest& gpu,
                                                             uint64_t tick);

    // --- 実験の記録(コマンドの列 + 刻みごとの状態のハッシュ)---

    struct LabRecording {
        uint64_t tickCount = 0;         // 流した刻みの数(刻み 0 〜 tickCount − 1)
        std::vector<Command> commands;  // (targetTick, sequence) の昇順
        std::vector<uint64_t> hashes;   // 刻み t を終えた状態の HashWholeNest(CPU)。tickCount 個
    };

    [[nodiscard]] std::vector<std::byte> SerializeLabRecording(const LabRecording& recording);
    [[nodiscard]] std::expected<LabRecording, std::string> ParseLabRecording(std::span<const std::byte> bytes);

}  // namespace bicameral::sim
