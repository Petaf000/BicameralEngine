// world_atmosphere.h — 世界の基準の大気(海面からの高さの点ごとの温度と成分・一番低い点の圧力。ADR-0176 の 3・ADR-0182・T-0373・T-0499)。
//
// データの流れ: パッケージの environment.atmosphere_profile(高さの点の列)と atmosphere_surface_pressure_pa
//   (または一様な大気の書き方: 分類 atmosphere〔物質 → ppm〕と atmosphere_temperature_mk = 高さ 0 の点 1 つ)
//   → script/environment_package の ReadWorldAtmosphere が名前のまま読む → WorldAtmosphereDefinition
//   (ReactionTableDefinition::atmosphere)→ BakeWorldAtmosphere(BakeReactionTable の最後。物質の名前を ID に直して確かめる)
//   → WorldAtmosphere(BakedReactionTable::atmosphere)→ SampleAtmosphere(ある高さの温度と成分)・AtmospherePressureMicroPascal
//   (ある高さの静水圧)→ 気体の層の基準(MakeHydrostaticReference の層ごとの版。気体を世界に載せる段 T-0210 が読む)。
// 点の間は直線でつなぐ(温度も成分の割合も)。一番低い点より下・一番高い点より上は端の点の値のまま。
// 圧力は一番低い点の高さの値だけを作り手が書き、ほかの高さはエンジンが重力・温度・成分(平均のモル質量)から静水圧の釣り合いで出す
// (手で書いた圧力が重力と合わずに風が吹き出すことが無い。D-494)。
// エンジンは基準の大気の「書き方」と検査と計算だけを持ち、成分と値(地球の大気など)はパッケージが決める(D-454)。
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

    // 高さの範囲(mm。海面から ±2^40 mm ≒ ±110 万 km。直線でつなぐ計算を 64bit に収める器の決まり)
    inline constexpr int64_t ATMOSPHERE_HEIGHT_LIMIT_MILLIMETERS = (int64_t)1 << 40;

    // 静水圧を積む段(mm)。気体のレベル 0 のセルの高さ(0.5 m)と同じにして、気体の層の基準と同じ形の積み方にする
    inline constexpr int64_t ATMOSPHERE_PRESSURE_STEP_MILLIMETERS = 500;

    // --- 定義(人が書く値。名前のまま)---

    struct AtmosphereFractionDefinition {
        std::string species;
        uint32_t partsPerMillion = 0;
    };

    struct AtmospherePointDefinition {
        std::string label;              // 誤りに出す場所(例 "environment.atmosphere_profile[2]"。一様な書き方は空)
        int64_t heightMillimeters = 0;  // 海面からの高さ
        uint32_t temperatureMilliKelvin = 0;                  // 0 = 書いていない
        std::vector<AtmosphereFractionDefinition> fractions;  // 書いた順(ベイクが物質 ID の順に並べる)
    };

    struct WorldAtmosphereDefinition {
        std::vector<AtmospherePointDefinition>
            points;                           // 低い順に書く(並んでいなければベイクの誤り)。空 = 基準の大気が無い
        uint32_t surfacePressurePascals = 0;  // 一番低い点の高さの圧力。0 = 書いていない
    };

    // --- ベイクしたもの(物質は ID。物質 ID の順)---

    struct AtmosphereFraction {
        uint32_t species = 0;
        uint32_t partsPerMillion = 0;

        [[nodiscard]] bool operator==(const AtmosphereFraction&) const = default;
    };

    struct AtmospherePoint {
        int64_t heightMillimeters = 0;
        uint32_t temperatureMilliKelvin = 0;
        std::vector<AtmosphereFraction> fractions;  // 0 ppm の物質は載せない

        [[nodiscard]] bool operator==(const AtmospherePoint&) const = default;
    };

    struct WorldAtmosphere {
        std::vector<AtmospherePoint> points;  // 高さの順(厳密に増える)。空 = 基準の大気が無い(ブランク)
        uint32_t surfacePressurePascals = 0;  // 一番低い点の高さの圧力

        [[nodiscard]] bool Empty() const { return points.empty(); }
        [[nodiscard]] bool operator==(const WorldAtmosphere&) const = default;
    };

    // ある高さの基準の大気(点の間を直線でつないだ値)
    struct AtmosphereSample {
        uint32_t temperatureMilliKelvin = 0;
        std::vector<AtmosphereFraction>
            fractions;  // 物質 ID の順・足してちょうど ATMOSPHERE_PARTS_PER_WHOLE・0 ppm は載せない

        [[nodiscard]] bool operator==(const AtmosphereSample&) const = default;
    };

    // 名前を ID に直して確かめる: 点ごとに、知らない物質・同じ物質が 2 回・割合 0・足して ATMOSPHERE_PARTS_PER_WHOLE でない・
    // 温度が無い。点が高さの順に厳密に増えていない・高さの範囲の外。点があるのに地面の圧力が無い(か、その逆)。
    // 全部書いていなければ空
    [[nodiscard]] std::expected<WorldAtmosphere, std::string> BakeWorldAtmosphere(
        const WorldAtmosphereDefinition& definition, const BakedReactionTable& table);

    // 高さ heightMillimeters の温度と成分。点の間は直線(下の点の側へ切り捨て、割合の端数は余りの大きい順に 1 ppm ずつ配って
    // 合計をちょうど 1 にする。同じ余りは物質 ID の小さい順)。端の点の外は端の値。呼ぶ側の約束: 空でない
    [[nodiscard]] AtmosphereSample SampleAtmosphere(const WorldAtmosphere& atmosphere, int64_t heightMillimeters);

    // 高さ heightMillimeters の静水圧(µPa): 一番低い点の圧力から、ATMOSPHERE_PRESSURE_STEP_MILLIMETERS ずつ
    // Δp = p × M̄ × g × Δz ÷ (R × T)(段の下の端の温度と成分。M̄ は表のモル質量の割合の平均)を引いて(下へは足して)積む。
    // 整数だけ・同じ入力なら同じ値。重力 0 なら一定。0 より下がらず、上限(ATMOSPHERE_PRESSURE_LIMIT_MICROPASCAL)で止まる。
    // 呼ぶ側の約束: 空でない・table はベイクした表(モル質量を読む)
    [[nodiscard]] uint64_t AtmospherePressureMicroPascal(const WorldAtmosphere& atmosphere,
                                                         const BakedReactionTable& table,
                                                         uint32_t gravityMicrometersPerSecond2,
                                                         int64_t heightMillimeters);

    // 静水圧の上限(µPa。10^9 Pa = 1 万気圧の 1000 倍。深く下へ積んだ時に 64bit の計算を溢れさせない)
    inline constexpr uint64_t ATMOSPHERE_PRESSURE_LIMIT_MICROPASCAL = 1000000000000000000ull;

}  // namespace bicameral::sim
