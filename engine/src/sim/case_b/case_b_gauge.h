// case_b_gauge.h — 案 B の試作の計器(B1。T-0867。21 §5.2)。R1〜R6・M1 の作業役が合格を測る値を、箱から 1 刻みに 1 つ取る。世界に何も返さない。
// 取る値(刻みごと): ハッシュ・保存の誤差が 0 か・動く面の数・最大の速さ・費用(ns)・見る範囲ごとの相の質量・液体の水位・
// 粒の高さの横の形から崩れる角度・水面のセルの圧力の幅・範囲の平均の圧力。
// 刻みの列から: 揺り戻しの回数(CountReversals)・戻る時間(SettleTick)・届いた時(FirstTickAtLeast)・最大とその刻み(PeakOf)・
// 2 つの細かさの比べ(DifferencePermille)。
// データの流れ: RunScene(箱・段・圧力の口・刻みの数・見る範囲)→ Recording(刻みごとの GaugeSample)→ 列の関数 → 試験が合格を見る。
// 表示は整数(小数は呼ぶ側が FormatFixed で)。費用の時間は std::chrono の整数の ns(結果には入らない)。浮動小数点は使わない。
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "sim/case_b/case_b_box.h"
#include "sim/case_b/case_b_scene.h"

namespace bicameral::sim::case_b {

    // 動く面・ほぼ止まるの目安(21 §2.11 の仮: 最大の速さ 1 mm/s 未満)
    inline constexpr int64_t MOVING_SPEED = (int64_t)1 << 10;  // 2^-20 m/s で約 1 mm/s(0.98 mm/s)

    // 見る範囲の値(1 刻み)
    struct RegionSample {
        std::array<int64_t, MOVING_PHASE_COUNT> phaseMassMg{};  // 気体・液体・粒
        int64_t liquidLevelUm = 0;  // 液体の体積 ÷ 範囲の幅(列あたりの水の高さ。範囲の下端から)
        int64_t grainLevelUm = 0;   // 詰まった粒の体積 ÷ 幅
        int64_t meanPressureMicroPascal = 0;
        int64_t surfaceMinPressureMicroPascal =
            0;  // 水面のセル(液体 ≥ 1/2 で上が < 1/2)の圧力の最小・最大。水面が無ければ 0
        int64_t surfaceMaxPressureMicroPascal = 0;
        uint32_t surfaceCells = 0;
        uint32_t reposeAngleDegrees = 0;  // 粒の高さの形の両側の斜面の角度の大きい方(度)。粒が無ければ 0
    };

    struct GaugeSample {
        uint64_t tick = 0;
        uint64_t hash = 0;
        bool conserved = false;    // CheckConservation が 0
        uint32_t movingFaces = 0;  // |面の速さ| ≥ MOVING_SPEED の面(全部の置き場)
        uint32_t totalFaces = 0;   // 隣のある面(全部の置き場)
        int64_t maxSpeed = 0;      // 面の速さの最大(2^-20 m/s)
        int64_t stepNanoseconds = 0;
        std::vector<RegionSample> regions;
    };

    struct Recording {
        std::vector<GaugeSample> samples;  // 0 番は刻みを進める前
        uint32_t cellCount = 0;

        // 費用(ns/セル/刻み。1 刻み目からの平均)
        [[nodiscard]] int64_t NanosecondsPerCellTick() const;
        // 全部の刻みで保存が 0 か
        [[nodiscard]] bool AllConserved() const;
        // 刻みごとのハッシュを順に合成した 1 つの値(2 回の実行の比べ)
        [[nodiscard]] uint64_t HashChain() const;
    };

    [[nodiscard]] RegionSample MeasureRegion(const Box& box, const View& view, const BoxRegion& region);
    [[nodiscard]] GaugeSample MeasureSample(const Box& box, std::span<const BoxRegion> regions);

    // 刻みを ticks 回進めながら計器を取る。afterTick が null でなければ毎刻みの後に呼ぶ(場面の操作:堤を消すなど)
    [[nodiscard]] Recording RunScene(Box& box, const Stages& stages, const PressureSolver& pressureSolver,
                                     uint64_t ticks, std::span<const BoxRegion> regions,
                                     const std::function<void(Box&)>& afterTick = nullptr);

    // --- 刻みの列 ---
    // 見る範囲 region の値を刻みの順に取り出す
    [[nodiscard]] std::vector<int64_t> RegionSeries(const Recording& recording, uint32_t region,
                                                    int64_t RegionSample::* field);
    // 揺り戻しの回数: 向きが変わった回数(threshold 以下の揺れは数えない。ヒステリシス)
    [[nodiscard]] uint32_t CountReversals(std::span<const int64_t> series, int64_t threshold);
    // 戻る時間: この刻みから後はずっと |値 − target| ≤ tolerance(最後まで入らなければ std::nullopt)
    [[nodiscard]] std::optional<uint64_t> SettleTick(std::span<const int64_t> series, int64_t target,
                                                     int64_t tolerance);
    // 届いた時: 初めて値 ≥ threshold になった刻み
    [[nodiscard]] std::optional<uint64_t> FirstTickAtLeast(std::span<const int64_t> series, int64_t threshold);

    struct Peak {
        int64_t value = 0;
        uint64_t tick = 0;
    };

    [[nodiscard]] Peak PeakOf(std::span<const int64_t> series);
    // 2 つの値の差の割合(‰)= |a − b| × 1000 ÷ max(|a|, |b|)(両方 0 なら 0)
    [[nodiscard]] int64_t DifferencePermille(int64_t a, int64_t b);

    // 粒の高さの形(列ごとの高さ µm)から斜面の角度(度)。最も高い列から、左右それぞれ高さが 0 になる手前の列までの平均の傾き
    [[nodiscard]] uint32_t SlopeAngleDegrees(std::span<const int64_t> heightsUm, uint32_t columnWidthUm);
    // 傾き(Q16 = 高さ ÷ 横)から角度(度。四捨五入)
    [[nodiscard]] uint32_t AngleFromSlopeQ16(uint64_t slopeQ16);

    // 整数を小数の表示に(value ÷ 10^decimals。例: FormatFixed(2130, 3) = "2.130")
    [[nodiscard]] std::string FormatFixed(int64_t value, uint32_t decimals);

}  // namespace bicameral::sim::case_b
