// phase_layer_explicit_lag_test.cpp — 混ざったセル(凝縮相の層 + 気体)を陰の伝導の系から外して陽で流した時の遅れの測り
// (T-1230 段 2・ADR-0512 の確かめ 2・D-642 の 0.1 K)。CPU だけ・測りの模型(浮動小数点。tests/ は検査の外)。
//
// 何を比べるか(1 列・下 = 固体・上 = 空気・端は断熱・刻み 1/60 秒・20 秒):
//   参照 R: 全部を陰(後退オイラー)で解く。混ざったセルは 2 未知数(層・気体)+ 層の上の面(ADR-0512 の (b) と同じ答え)
//   A(n): 混ざったセルだけ系から外し、3 つの面(下 = 層 ⇄ 固体・層の上の面・上 = 気体 ⇄ 空気)を陽で流す(刻みの初めの温度。n 回の小刻み)。
//         ほかのセルは陰で、陽の面の流れを源として受ける(ADR-0512 の 2「安全網と同じ道」)。n = 1 が今の案・n = 4^σ が (a) 小刻み
// 面の式はエンジンと同じ形: 片側の値 = 伝導率 × 面積 ÷ (2 × 中心から面まで)(heat_conduction.hlsli・phase_layer.hlsli の PlBottomSide・PlTopSide)、
//   面の係数 = min(両側)(陰は頭打ちなし = multires_implicit_conduction.cpp の FaceCoefficient)、陽は さらに min(両側の熱容量 ÷ 8)(HcFaceFlow)。
//   層の上の面は 2 つの半分の道の直列(PlInnerFlow)。
// 模型の約束(エンジンの整数とは違う所): 物質の値は表に近い丸めた値・熱容量は一定・相変化なし(顕熱だけ)・横の面なし(層は横に一様)。
//   相が変わる層(溶ける氷・露が付く)は温度が止まるので、遅れは温度ではなく「受ける熱の速さ」に出る(下の表の Q の比)。
// 結果は表で出す(チケット T-1230 の「段 2 の測り」に写す)。試験として確かめるのは 参照の保存・陽の行き過ぎなし・小刻みで誤差が減ること。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr double DT_S = 1.0 / 60.0;
    constexpr int TICKS = 1200;  // 20 秒
    constexpr int SIDE_CELLS = 96;
    constexpr double BUDGET_K = 0.1;

    struct Material {
        const char* name;
        double conductivity;  // W/(m·K)
        double rhoC;          // J/(m³·K)
    };

    constexpr Material STONE = {"石", 2.8, 1.42e6};
    constexpr Material IRON = {"鉄", 50.0, 3.5e6};
    constexpr Material WATER = {"水", 0.6, 4.18e6};
    constexpr Material ICE = {"氷", 2.2, 1.9e6};
    constexpr Material AIR = {"空気", 0.026, 1.2e3};

    struct Scenario {
        const char* name;
        Material solid;
        double solidK;
        Material layer;
        double thicknessM;
        double layerK;
        double airK;
    };

    // --- 列の節(下から: 固体 → 満ちた層のセル → 混ざったセルの層・気体 → 空気)---
    struct Column {
        std::vector<double> capacity;     // J/K
        std::vector<double> temperature;  // K
        std::vector<double> link;         // 節 i ⇄ i + 1 の陰の係数(J/K/刻み)。混ざったセルの 3 つの面も含む
        std::vector<double> sideBelow;    // 陽の上限の計算用: 節 i の下の面・上の面の片側(W/K)
        std::vector<double> sideAbove;
        int layerNode = 0;  // 混ざったセルの層の節(気体はその次)
    };

    double SideValue(double conductivity, double area, double distance) {
        return conductivity * area / (2.0 * distance);
    }

    Column MakeColumn(const Scenario& scenario, double size) {
        const double area = size * size;
        const int fullLayers = static_cast<int>(std::ceil(scenario.thicknessM / size)) - 1;
        const double rest = scenario.thicknessM - fullLayers * size;
        Column column;
        auto add = [&](double capacity, double temperature, double below, double above) {
            column.capacity.push_back(capacity);
            column.temperature.push_back(temperature);
            column.sideBelow.push_back(below);
            column.sideAbove.push_back(above);
        };

        const double half = size / 2.0;
        for (int i = 0; i < SIDE_CELLS; ++i) {
            const double side = SideValue(scenario.solid.conductivity, area, half);
            add(scenario.solid.rhoC * area * size, scenario.solidK, side, side);
        }

        for (int i = 0; i < fullLayers; ++i) {
            const double side = SideValue(scenario.layer.conductivity, area, half);
            add(scenario.layer.rhoC * area * size, scenario.layerK, side, side);
        }

        // 混ざったセル: 層の下の面(道 = 層の厚さの半分)・気体の上の面(道 = 気体の厚さの半分)
        column.layerNode = static_cast<int>(column.capacity.size());
        const double gasThickness = size - rest;
        add(scenario.layer.rhoC * area * rest, scenario.layerK,
            SideValue(scenario.layer.conductivity, area, rest / 2.0), 0.0);
        add(AIR.rhoC * area * gasThickness, scenario.airK, 0.0, SideValue(AIR.conductivity, area, gasThickness / 2.0));
        for (int i = 0; i < SIDE_CELLS; ++i) {
            const double side = SideValue(AIR.conductivity, area, half);
            add(AIR.rhoC * area * size, scenario.airK, side, side);
        }

        // 面の係数: min(両側)× Δt。層の上の面は直列(PlInnerFlow: 半分の道 = 伝導率 × 面積 ÷ (厚さ ÷ 2))
        const size_t count = column.capacity.size();
        column.link.assign(count - 1, 0.0);
        for (size_t i = 0; i + 1 < count; ++i)
            column.link[i] = std::min(column.sideAbove[i], column.sideBelow[i + 1]) * DT_S;

        const double layerHalf = scenario.layer.conductivity * area / (rest / 2.0);
        const double gasHalf = AIR.conductivity * area / (gasThickness / 2.0);
        column.link[column.layerNode] = layerHalf * gasHalf / (layerHalf + gasHalf) * DT_S;

        return column;
    }

    // 後退オイラーの 1 刻み(3 重対角。links の 0 は切れた面)。source = 刻みの間に受ける熱(J)
    void SolveImplicit(const std::vector<double>& capacity, const std::vector<double>& links,
                       std::vector<double>& temperature, const std::vector<double>& source) {
        const size_t count = capacity.size();
        std::vector<double> lower(count, 0.0);
        std::vector<double> diagonal(count, 0.0);
        std::vector<double> upper(count, 0.0);
        std::vector<double> rhs(count, 0.0);
        for (size_t i = 0; i < count; ++i) {
            diagonal[i] = capacity[i];
            rhs[i] = capacity[i] * temperature[i] + source[i];
        }

        for (size_t i = 0; i + 1 < count; ++i) {
            diagonal[i] += links[i];
            diagonal[i + 1] += links[i];
            upper[i] = -links[i];
            lower[i + 1] = -links[i];
        }

        for (size_t i = 1; i < count; ++i) {
            const double factor = lower[i] / diagonal[i - 1];
            diagonal[i] -= factor * upper[i - 1];
            rhs[i] -= factor * rhs[i - 1];
        }

        temperature[count - 1] = rhs[count - 1] / diagonal[count - 1];
        for (size_t i = count - 1; i-- > 0;)
            temperature[i] = (rhs[i] - upper[i] * temperature[i + 1]) / diagonal[i];
    }

    double Energy(const Column& column) {
        double sum = 0.0;
        for (size_t i = 0; i < column.capacity.size(); ++i)
            sum += column.capacity[i] * column.temperature[i];

        return sum;
    }

    // 参照: 全部を陰で(混ざったセルは 2 未知数)。split 回に分けると刻み Δt ÷ split の参照(参照そのものの時間の誤差の目安)
    void StepReference(Column& column, int split = 1) {
        const std::vector<double> none(column.capacity.size(), 0.0);
        if (split == 1) {
            SolveImplicit(column.capacity, column.link, column.temperature, none);
            return;
        }

        std::vector<double> links = column.link;
        for (double& link : links)
            link /= split;

        for (int i = 0; i < split; ++i)
            SolveImplicit(column.capacity, links, column.temperature, none);
    }

    // 陽の面の係数(刻み dt の分): min(陰の係数 × dt ÷ Δt, 両側の熱容量 ÷ 8)
    double ExplicitCoefficient(const Column& column, int below, double fraction) {
        const double capacityLimit = std::min(column.capacity[below], column.capacity[below + 1]) / 8.0;

        return std::min(column.link[below] * fraction, capacityLimit);
    }

    // C: 混ざったセルの 2 つだけを陰で(隣は刻みの初めの温度で固定した境・2 × 2 の系)。流れは隣へ源として
    void StepLocalImplicit(Column& column, double& toLayer, double& toAbove) {
        const int layer = column.layerNode;
        const int gas = layer + 1;
        const double belowT = column.temperature[layer - 1];
        const double aboveT = column.temperature[gas + 1];
        const double bottom = column.link[layer - 1];
        const double inner = column.link[layer];
        const double top = column.link[gas];
        const double a = column.capacity[layer] + bottom + inner;
        const double d = column.capacity[gas] + inner + top;
        const double r0 = column.capacity[layer] * column.temperature[layer] + bottom * belowT;
        const double r1 = column.capacity[gas] * column.temperature[gas] + top * aboveT;
        const double determinant = a * d - inner * inner;
        const double layerT = (r0 * d + inner * r1) / determinant;
        const double gasT = (a * r1 + inner * r0) / determinant;
        toLayer = bottom * (belowT - layerT);
        toAbove = top * (gasT - aboveT);
        column.temperature[layer] = layerT;
        column.temperature[gas] = gasT;
    }

    // A(n): 混ざったセル(層・気体)を系から外して陽で n 回(n = 0 は C)。ほかは陰で、陽の面の流れを源として受ける
    void StepExplicitMixed(Column& column, int substeps) {
        const int layer = column.layerNode;
        const int gas = layer + 1;
        double toLayer = 0.0;  // 刻みの間に固体 → 層
        double toAbove = 0.0;  // 刻みの間に気体 → 空気
        if (substeps == 0)
            StepLocalImplicit(column, toLayer, toAbove);

        const double fraction = substeps == 0 ? 0.0 : 1.0 / substeps;
        const double belowT = column.temperature[layer - 1];
        const double aboveT = column.temperature[gas + 1];
        const double bottom = ExplicitCoefficient(column, layer - 1, fraction);
        const double inner = ExplicitCoefficient(column, layer, fraction);
        const double top = ExplicitCoefficient(column, gas, fraction);
        for (int s = 0; s < substeps; ++s) {
            const double fromBelow = bottom * (belowT - column.temperature[layer]);
            const double upward = inner * (column.temperature[layer] - column.temperature[gas]);
            const double outward = top * (column.temperature[gas] - aboveT);
            column.temperature[layer] += (fromBelow - upward) / column.capacity[layer];
            column.temperature[gas] += (upward - outward) / column.capacity[gas];
            toLayer += fromBelow;
            toAbove += outward;
        }

        std::vector<double> links = column.link;
        links[layer - 1] = 0.0;
        links[layer] = 0.0;
        links[gas] = 0.0;
        std::vector<double> source(column.capacity.size(), 0.0);
        source[layer - 1] = -toLayer;
        source[gas + 1] = toAbove;
        const double layerT = column.temperature[layer];
        const double gasT = column.temperature[gas];
        SolveImplicit(column.capacity, links, column.temperature, source);
        column.temperature[layer] = layerT;
        column.temperature[gas] = gasT;
    }

    struct Result {
        double layerMaxK = 0.0;
        double gasMaxK = 0.0;
        double neighborMaxK = 0.0;
        double layerEndK = 0.0;
        double settledMaxK = 0.0;     // 初めの 1 秒(60 刻み)の後の最大(層・気体・隣)
        double referenceTimeK = 0.0;  // 参照そのものの時間の誤差(刻み 1/60 秒 と 1/3840 秒の差の最大。過渡の目安)
        int ticksOverBudget = 0;
        double heatRatio = 1.0;  // 20 秒に固体から層が受けた熱(A ÷ R)
        bool energyKept = true;
        bool bounded = true;
    };

    Result Measure(const Scenario& scenario, double size, int substeps) {
        Column reference = MakeColumn(scenario, size);
        Column measured = reference;
        Column fine = reference;
        const int layer = reference.layerNode;
        const double startEnergy = Energy(reference);
        const double low = std::min({scenario.solidK, scenario.layerK, scenario.airK});
        const double high = std::max({scenario.solidK, scenario.layerK, scenario.airK});
        Result result;
        for (int tick = 0; tick < TICKS; ++tick) {
            StepReference(reference);
            StepExplicitMixed(measured, substeps);
            if (substeps == 1) {
                StepReference(fine, 64);
                const double layerTime = std::fabs(fine.temperature[layer] - reference.temperature[layer]);
                const double gasTime = std::fabs(fine.temperature[layer + 1] - reference.temperature[layer + 1]);
                result.referenceTimeK = std::max({result.referenceTimeK, layerTime, gasTime});
            }

            double neighbor = 0.0;
            for (size_t i = 0; i < reference.capacity.size(); ++i) {
                if (static_cast<int>(i) == layer || static_cast<int>(i) == layer + 1)
                    continue;

                neighbor = std::max(neighbor, std::fabs(measured.temperature[i] - reference.temperature[i]));
            }

            const double layerError = std::fabs(measured.temperature[layer] - reference.temperature[layer]);
            const double gasError = std::fabs(measured.temperature[layer + 1] - reference.temperature[layer + 1]);
            result.layerMaxK = std::max(result.layerMaxK, layerError);
            result.gasMaxK = std::max(result.gasMaxK, gasError);
            result.neighborMaxK = std::max(result.neighborMaxK, neighbor);
            const double worst = std::max({layerError, gasError, neighbor});
            result.ticksOverBudget += worst > BUDGET_K ? 1 : 0;
            if (tick >= 60)
                result.settledMaxK = std::max(result.settledMaxK, worst);

            result.layerEndK = layerError;
            for (double t : measured.temperature)
                result.bounded = result.bounded && t >= low - 1e-9 && t <= high + 1e-9;
        }

        const double tolerance = 1e-9 * std::fabs(startEnergy);
        result.energyKept = std::fabs(Energy(reference) - startEnergy) < tolerance &&
                            std::fabs(Energy(measured) - startEnergy) < tolerance;
        // 固体が失った熱で比べる(層の中の熱の行き先によらない)
        double solidLossReference = 0.0;
        double solidLossMeasured = 0.0;
        const Column start = MakeColumn(scenario, size);
        for (int i = 0; i < layer; ++i) {
            solidLossReference += start.capacity[i] * (start.temperature[i] - reference.temperature[i]);
            solidLossMeasured += start.capacity[i] * (start.temperature[i] - measured.temperature[i]);
        }

        result.heatRatio = solidLossReference == 0.0 ? 1.0 : solidLossMeasured / solidLossReference;

        return result;
    }

}  // namespace

int main() {
    const Scenario scenarios[] = {
        {"露 0.07 mm(層は石の温度・空気が暖かい)", STONE, 280.0, WATER, 7.0e-5, 280.0, 300.0},
        {"露 0.07 mm(層は空気の温度で付いた)", STONE, 280.0, WATER, 7.0e-5, 300.0, 300.0},
        {"水たまり 5 mm(日なたの石)", STONE, 330.0, WATER, 5.0e-3, 290.0, 300.0},
        {"氷 2 cm(冷えた石・暖かい空気)", STONE, 265.0, ICE, 2.0e-2, 263.0, 290.0},
        {"鉄の板の上の水の膜 0.5 mm", IRON, 370.0, WATER, 5.0e-4, 300.0, 300.0},
    };
    const double sizes[] = {0.5, 0.125, 0.03125, 0.0078125, 0.001953125, 0.00048828125};
    const int substepChoices[] = {1, 4, 64, 0};

    std::printf(
        "混ざったセルを陽で流した時の参照(全部を陰・2 未知数)との差の最大(K)。20 秒・刻み 1/60 秒・予算 %.1f K\n",
        BUDGET_K);
    std::printf(
        "列: n(0 = セルの中だけ陰)/ 層 / 気体 / 隣のセル(最大)/ 1 秒の後の最大 / 予算を超えた刻み / "
        "固体が失った熱の比(A ÷ R)\n");
    std::printf(
        "    参照の時間の誤差 = 参照(刻み 1/60 秒)と刻み 1/3840 秒の差の最大(過渡では参照そのものもこれだけ違う)\n");
    for (const Scenario& scenario : scenarios) {
        std::printf("\n== %s ==\n", scenario.name);
        for (double size : sizes) {
            for (int substeps : substepChoices) {
                const Result result = Measure(scenario, size, substeps);
                EXPECT(result.energyKept);
                EXPECT(result.bounded);
                std::printf(
                    "  セル %8.3f mm  n=%2d  層 %8.4f  気体 %8.4f  隣 %8.4f  1 秒後 %8.4f  超え %4d  熱の比 %.4f",
                    size * 1000.0, substeps, result.layerMaxK, result.gasMaxK, result.neighborMaxK, result.settledMaxK,
                    result.ticksOverBudget, result.heatRatio);
                if (substeps == 1)
                    std::printf("  参照の時間の誤差 %.4f", result.referenceTimeK);

                std::printf("\n");
            }
        }
    }

    // 小刻みは誤差を減らす(n = 64 は n = 1 以下)
    for (const Scenario& scenario : scenarios) {
        for (double size : sizes) {
            const Result one = Measure(scenario, size, 1);
            const Result many = Measure(scenario, size, 64);
            EXPECT(many.layerMaxK <= one.layerMaxK + 1e-9);
        }
    }

    if (failureCount != 0) {
        std::printf("%d 件の失敗\n", failureCount);
        return 1;
    }

    std::printf("ok\n");
    return 0;
}
