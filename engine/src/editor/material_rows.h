// material_rows.h — エディタで材料を表の全物質から組む行(実験室〔T-0220〕と筆〔T-0245〕で共通。docs/design/14 §2)。
//
// データの流れ:
//   ひな形(sim::MakeLabMaterials)を読み込む・物質を選ぶ・量を mol・g・kPa で入れる(表示の入力だけ浮動小数点)
//   → ComposeMaterial が整数の µmol の材料(sim::SpeciesAmount。3 つまで)にする(換算は sim::LabMassMicromoles・LabGasMicromoles)
//   → 実験室の「置く」・筆の置くコマンドの材料へ。行は View の状態で、世界にも記録にも入らない(入るのはコマンドの整数だけ)。
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "sim/lab_box.h"

namespace bicameral::editor {

    enum class AmountUnit : uint8_t {
        Mole,
        Gram,
        KiloPascal,  // 1 セル(0.125 m³)を理想気体で満たした分圧(置く温度で µmol に直す)
    };

    // 材料の 1 行(表示の入力。置く時に整数の µmol に直す)
    struct MaterialRow {
        int species = 0;  // 物質 ID(0 = なし)
        double amount = 0.0;
        AmountUnit unit = AmountUnit::Mole;
    };

    // 1 つの材料の行(実験室と筆の置く操作の上限 3 つ)
    using MaterialRows = std::array<MaterialRow, lab::LAB_MAX_FILL_SPECIES>;

    // 1 行の量を µmol に(表に無い物質・0 以下は 0)
    [[nodiscard]] uint64_t MaterialRowMicromoles(const sim::BakedReactionTable& table, const MaterialRow& row,
                                                 uint32_t temperatureMilliKelvin);

    // 行を材料に(量が 0 の行と、前の行と同じ物質は飛ばす)
    [[nodiscard]] std::vector<sim::SpeciesAmount> ComposeMaterial(const sim::BakedReactionTable& table,
                                                                  std::span<const MaterialRow> rows,
                                                                  uint32_t temperatureMilliKelvin);

    // ひな形を行に(mol で)
    void LoadMaterialRows(const sim::LabMaterial& material, MaterialRows& rows);

    // 物質の一覧が変わる表へ替えた時、行の物質 ID を名前で新しい表へ(消えた物質の行は空に。T-0242)
    void RemapMaterialRows(const sim::BakedReactionTable& from, const sim::BakedReactionTable& to, MaterialRows& rows);

    // ImGui の行(物質・量・単位・µmol の表示)。ImGui のフレームの中で呼ぶ
    void BuildMaterialRowsUi(const sim::BakedReactionTable& table, MaterialRows& rows, uint32_t temperatureMilliKelvin);

}  // namespace bicameral::editor
