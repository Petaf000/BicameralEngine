// gas_base_layers.h — 陰の圧力と気体の流れの「基準の層」(重さで決まる静水圧の部分)を、多重解像度の粗さをまたいで整数の和が合う形で作る
// (T-0953・ADR-0431・20 §11.2・§13.1。根拠は T-0900 M-0「10 km の空気の柱」)。
//
// 気体の力は基準との差で入れる(p̃ = α(p − p_基準(z))・重さの力 =(m − m_基準)g)。基準そのものは止まっているので、層ごとの基準が
// 静水圧の釣り合いから少しずれていても静かな大気は動かない。動くのは「状態と基準が違う」時だけ。
// 粗い所と細かい所が並ぶ世界では、粗いセルの状態は細かい 8 セルの和 ÷ 8(粗くすること = 混ぜること。20 §11.2)から来る。
// レベルごとに基準を別々に積み上げる(MakeHydrostaticReference を各レベルで呼ぶ)と、粗くした静かな空気が粗い段の基準と食い違い、
// その差の分だけ見かけの力が出る(眠った空気が起きた時・粗さの境)。ここではその食い違いを形で 0 にする:
//   - 段 0 = いちばん細かいレベルの層を、今と同じ静水圧の積み上げ(MakeHydrostaticReference)で理想の値として作る。
//   - 段 d(レベル = いちばん細かい − d)の層 j は、段 d − 1 の層 2j・2j + 1 の和のちょうど半分(量は成分ごと・エネルギー。整数で一致)。
//     横の 4 つ × 縦の 2 層 = 8 つの子の和 ÷ 8 なので、静かな細かい 8 セルを粗くすると端数 0 で粗い段の基準そのものになる。
//   - 作り方: 段 d の理想(細かい層の和 ÷ 2^d の切り捨て)を上から決め、子の 2 層へは理想の値を置いてから「2 × 親 − 子の理想の和」の小さな差を
//     半分ずつ足す(差は子の段ごとに 1 単位ほどしか増えない。いちばん細かい段の理想からのずれは段の数 + 1 単位〔µmol・mJ〕以内)。
//   - 粗い段を持たない世界(段の数 0)では段 0 = MakeHydrostaticReference とビット単位で同じ(今の箱・ハッシュは変わらない)。
// 起こす(粗い 1 セル → 細かい 8 セル)は「基準からのずれを同じ数で配る」: 子 = 子の基準 + (親 − 親の基準)。基準の和が合うので
// 8 つの子の和 = 8 × 親(整数で一致・保存はビット単位)、静かな親からは子の基準そのもの(見かけの力 0)が出る。
//
// データの流れ: 世界のデータ(重力・音速・成分・層ごとの温度と成分の割合・地面の圧力)→ MakeGasBaseLayers → 段ごとの層
//   → GasBaseLevelReference(箱の基準 = MakeGasBox の reference。GPU の気体の箱は箱の referenceDerived を写すので同じ値を読む)
//   → CoarsenGasCells / RefineGasCell(粗くする・起こす)。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "sim/gas_reference.h"

namespace bicameral::sim {

    // 段ごとの基準の層(z = 0 が地面。段 d の層の数 = 段 0 の層の数 >> d)
    struct GasBaseLayers {
        int32_t finestLevel = 0;  // 段 0 のセルのレベル(GasConfig::cellLevel)
        std::vector<std::vector<GasCell>>
            levels;  // levels[d] = レベル finestLevel − d の層(量・エネルギーはそのレベルの単位)

        [[nodiscard]] uint32_t CoarserLevels() const { return levels.empty() ? 0 : (uint32_t)levels.size() - 1; }
    };

    // 基準の層を作る。finestConfig はいちばん細かいレベルの箱の設定(cellLevel・sizeZ = 層の数・物質・重力)、profile はその層ごとの
    // 温度と成分(sizeZ 層)、coarserLevels は粗い段の数。呼ぶ側の約束: sizeZ は 2^coarserLevels の倍数・
    // finestConfig.cellLevel − coarserLevels ≥ GAS_MIN_CELL_LEVEL
    [[nodiscard]] GasBaseLayers MakeGasBaseLayers(const GasConfig& finestConfig,
                                                  std::span<const GasReferenceLayer> profile,
                                                  uint64_t surfacePressureMicroPascal, uint32_t coarserLevels);

    // レベル level の層 firstLayer から count 層(箱の基準。MakeGasBox(config, これ)。config.cellLevel = level・sizeZ = count)
    [[nodiscard]] std::vector<GasCell> GasBaseLevelReference(const GasBaseLayers& layers, int32_t level,
                                                             uint32_t firstLayer, uint32_t count);

    // 粗くした時の端数(子の単位。Σ 子 − 8 × 親。0〜7。世界の帳簿へ)
    struct GasCoarsenRemainder {
        GasPerSpecies<uint64_t> amounts;
        int64_t energy = 0;
        std::array<int64_t, GAS_AXES> momentum = {};
    };

    // 子 8 つ(x が最も速く回る順。0〜3 が下の層・4〜7 が上の層)→ 親(和 ÷ 8 の切り捨て。端数は remainder へ。20 §11.2 の VOLUME の規則)
    [[nodiscard]] GasCell CoarsenGasCells(const GasConfig& config, std::span<const GasCell, 8> children,
                                          GasCoarsenRemainder& remainder);

    // 親 → 子 8 つ(同じ順)。子 = 子の基準(下の層 lowerReference・上の層 upperReference)+(親 − 親の基準)。
    // どれかの子の成分が負になる時(基準より大きく減った親)だけ、全部の子を親と同じ数にする(どちらも Σ 子 = 8 × 親)
    [[nodiscard]] std::array<GasCell, 8> RefineGasCell(const GasConfig& config, const GasCell& parent,
                                                       const GasCell& parentReference, const GasCell& lowerReference,
                                                       const GasCell& upperReference);

}  // namespace bicameral::sim
