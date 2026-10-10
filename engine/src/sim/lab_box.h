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
#include "sim/species_remap.h"

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

    // 直方体の範囲(両端を含む。T-0220)。箱全体は LabWholeBox()
    struct LabCellRange {
        LabCellPosition low;
        LabCellPosition high;
    };

    [[nodiscard]] constexpr LabCellRange LabWholeBox() {
        return {.low = {}, .high = {.x = LAB_BOX_EDGE - 1, .y = LAB_BOX_EDGE - 1, .z = LAB_BOX_EDGE - 1}};
    }

    // --- 量の換算(T-0220。パネルが量・圧力を µmol に直す。整数だけ)---

    inline constexpr uint64_t LAB_MAX_PRESSURE_PASCAL = 100'000'000;  // 100 MPa(これより上は当てない)

    // 1 セル(0.125 m³)を温度 temperature の理想気体で満たして分圧 pressure になる物質量(µmol。切り捨て)。
    // 圧力・温度が範囲の外(温度 0 か LAB_MAX_TEMPERATURE_MILLIKELVIN より上・圧力が上限より上)なら 0。箱は圧力を状態に持たない(置いた時の量を決めるだけ。07 の気体とは別)
    [[nodiscard]] uint64_t LabGasMicromoles(uint64_t pressurePascal, uint32_t temperatureMilliKelvin);

    // 質量(mg)を物質量(µmol。切り捨て)に。molarMass = mg/mol(BakedReactionTable::molarMasses)。0 なら 0
    inline constexpr uint64_t LAB_MAX_MASS_MILLIGRAMS = 1'000'000'000'000;  // 1000 t(これより上は 0)
    [[nodiscard]] uint64_t LabMassMicromoles(uint64_t milligrams, uint32_t molarMassMilligramsPerMol);

    // --- コマンド ---

    // セルの中身を物質(LAB_MAX_FILL_SPECIES まで)と温度で置き換える
    [[nodiscard]] Command MakeLabFillCommand(uint64_t targetTick, uint32_t sequence, LabCellPosition cell,
                                             std::span<const SpeciesAmount> contents, uint32_t temperatureMilliKelvin);

    // セルの温度を決める(成分はそのまま)
    [[nodiscard]] Command MakeLabTemperatureCommand(uint64_t targetTick, uint32_t sequence, LabCellPosition cell,
                                                    uint32_t temperatureMilliKelvin);

    // 範囲のセル全部を FILL・TEMPERATURE と同じに(T-0220。範囲が箱の外なら当てないコマンドになる)
    [[nodiscard]] Command MakeLabFillRegionCommand(uint64_t targetTick, uint32_t sequence, LabCellRange range,
                                                   std::span<const SpeciesAmount> contents,
                                                   uint32_t temperatureMilliKelvin);
    [[nodiscard]] Command MakeLabTemperatureRegionCommand(uint64_t targetTick, uint32_t sequence, LabCellRange range,
                                                          uint32_t temperatureMilliKelvin);

    // 反応表を替えた印(T-0218・ADR-0055)。刻み targetTick のコマンドを当てる前に、その刻みから表を tableVersion に替える。
    // 印そのものはセルを変えず、箱をつつく(待ちの予定を新しい表で求め直す)。印は版だけ。表の中身は記録の tables に残す(T-0217)
    [[nodiscard]] Command MakeLabTableCommand(uint64_t targetTick, uint32_t sequence, uint64_t tableVersion);

    // 表を替えた印なら、その表の版
    [[nodiscard]] std::optional<uint64_t> LabTableVersionOf(const Command& command);

    // 64 バイトをそのまま uint32 × 16 で読む(GPU に写す形と同じ)
    [[nodiscard]] lab::LabCommand ToLabCommand(const Command& command);

    // --- 試験の表の材料(パネルの「置く」の選択肢。tests/packages/combustion_test と同じ物質の名前)---

    struct LabMaterial {
        std::string_view name;  // 表示の名前
        std::vector<SpeciesAmount> contents;
    };

    // 空気・木(セルロース 1 割 + 孔の空気。仮の世界の木箱の壁と同じ)・木炭・二酸化炭素・窒素・魔素(試験。仮の魔素 mana_test。T-0225)。
    // 表に無い物質の材料は入らない
    [[nodiscard]] std::vector<LabMaterial> MakeLabMaterials(const BakedReactionTable& table);

    // --- 箱 ---

    [[nodiscard]] MultiresCapacity LabBoxCapacity(const BakedReactionTable& table);

    // 300 K の空気を満たした箱(根はいつも頁を持つ。GPU のコマンドは頁のセルに書くため)
    [[nodiscard]] MultiresNest MakeLabBoxNest(const BakedReactionTable& table);

    // 箱のセル index(MrCellIndex)
    [[nodiscard]] reaction::RxCell LabBoxCell(const MultiresNest& nest, uint32_t index);

    // CPU リファレンス: 1 刻みのコマンド(並びは (targetTick, sequence) の昇順)を shaders/sim/lab_box.hlsl と同じ順で当てる。
    // 当てた数を返す(当てられないコマンドは飛ばす。表を替えた印は箱をつつくので数に入る。表そのものは呼ぶ側が先に替える)
    uint32_t ApplyLabCommands(MultiresNest& nest, const BakedReactionTable& table, std::span<const Command> commands);

    // --- 物質の一覧が変わる表への差し替え(T-0242・ADR-0065)---

    // CPU リファレンス: 箱の全部のセル(MultiresNest::cells。一様の値と頁。空のセルは空のまま)を remap(今の表 → next)で付け替える。
    // shaders/sim/lab_box.hlsl の RemapLabSpecies と同じ。箱は 1 レベルなので端数の枠と帳簿は使っていない(付け替えない)
    SpeciesRemapReport RemapLabBox(MultiresNest& nest, const SpeciesRemap& remap, const BakedReactionTable& next);

    // まだ刻んでいない置く操作(FILL・FILL_REGION)の材料の物質 ID を付け替える(消えた物質の材料は落とす。ほかの種類はそのまま)
    [[nodiscard]] Command RemapLabCommand(const Command& command, const SpeciesRemap& remap);

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

    // 記録に残す反応表の中身(T-0217。再生ファイルの版 2〔ADR-0050〕と同じやり方)。bytes は script::TableBytes のバイト列のまま
    // (sim は中身を読まない。別の起動で作り直すのは呼ぶ側: script::RebuildReactionTable が版とハッシュを確かめる)
    struct LabTableContent {
        uint64_t version = 0;
        std::string bytes;
    };

    // 版 2(T-0194)から、実験に使った反応表の版(script::TableVersion)を持つ。0 = 分からない(版 1 の記録・試験の表)。
    // 版 3(T-0218)から、刻みの途中で表を替えた所はコマンドの列の印(MakeLabTableCommand)で持つ。
    // 版 4(T-0217)から、刻み 0 の表と印の表の中身(tables)を持つ。別の起動でも、途中で表を替えた記録を再生できる
    struct LabRecording {
        uint64_t tableVersion = 0;      // 刻み 0 の表の版
        uint64_t tickCount = 0;         // 流した刻みの数(刻み 0 〜 tickCount − 1)
        std::vector<Command> commands;  // (targetTick, sequence) の昇順
        std::vector<uint64_t> hashes;   // 刻み t を終えた状態の HashWholeNest(CPU)。tickCount 個
        std::vector<LabTableContent>
            tables;  // 使った表の中身(版の昇順・重ならない。中身の分からない表〔試験の表など〕は入らない)
    };

    [[nodiscard]] std::vector<std::byte> SerializeLabRecording(const LabRecording& recording);
    [[nodiscard]] std::expected<LabRecording, std::string> ParseLabRecording(std::span<const std::byte> bytes);

}  // namespace bicameral::sim
