// world_atmosphere.h — 世界の基準の大気(地面の大気の成分・温度・圧力。ADR-0176 の 3・ADR-0182・T-0373)。
//
// データの流れ: パッケージの分類 atmosphere(物質 → ppm)と environment の atmosphere_temperature_mk・atmosphere_surface_pressure_pa
//   (script/environment_package の ReadWorldAtmosphere が名前のまま読む)→ WorldAtmosphereDefinition
//   (ReactionTableDefinition::atmosphere)→ BakeWorldAtmosphere(BakeReactionTable の最後。物質の名前を ID に直して確かめる)
//   → WorldAtmosphere(BakedReactionTable::atmosphere)→ 気体の層の基準(MakeHydrostaticReference の成分・温度・地面の圧力。
//   気体を世界に載せる段 T-0210 が読む)。
// エンジンは基準の大気の「書き方」と検査だけを持ち、成分と値(地球の大気など)はパッケージが決める(D-454)。
// 分類が無ければ空(ブランクの世界。D-455)。表の中身に入るので、再生ファイルは表の中身だけから同じ大気を作り直す。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace bicameral::sim {

    struct BakedReactionTable;

    // 割合の単位: 100 万分の 1(ppm。物質量の割合)。成分を足すとちょうど 1 になる
    inline constexpr uint32_t ATMOSPHERE_PARTS_PER_WHOLE = 1000000;

    // --- 定義(人が書く値。名前のまま)---

    struct AtmosphereFractionDefinition {
        std::string species;
        uint32_t partsPerMillion = 0;
    };

    struct WorldAtmosphereDefinition {
        std::vector<AtmosphereFractionDefinition> fractions;  // 書いた順(ベイクが名前の順に並べる)
        uint32_t temperatureMilliKelvin = 0;                  // 0 = 書いていない
        uint32_t surfacePressurePascals = 0;                  // 0 = 書いていない
    };

    // --- ベイクしたもの(物質は ID。物質 ID の順)---

    struct AtmosphereFraction {
        uint32_t species = 0;
        uint32_t partsPerMillion = 0;
    };

    struct WorldAtmosphere {
        std::vector<AtmosphereFraction> fractions;  // 空 = 基準の大気が無い(ブランク)
        uint32_t temperatureMilliKelvin = 0;
        uint32_t surfacePressurePascals = 0;

        [[nodiscard]] bool Empty() const { return fractions.empty(); }
    };

    // 名前を ID に直して確かめる: 知らない物質・同じ物質が 2 回・割合 0・足して ATMOSPHERE_PARTS_PER_WHOLE でない・
    // 成分があるのに温度か圧力が無い(か、その逆)は誤り。全部書いていなければ空
    [[nodiscard]] std::expected<WorldAtmosphere, std::string> BakeWorldAtmosphere(
        const WorldAtmosphereDefinition& definition, const BakedReactionTable& table);

}  // namespace bicameral::sim
