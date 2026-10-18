// gas_cells.h — 世界のセル(RxCell)と気体の箱(GasBox)の間の写し。仮の世界(sim/probe_gas)と多重解像度の世界(sim/multires_gas)が
// 同じ関数を呼ぶ(運ぶ計算の共通の土台の段 3 ②。T-0720)。
//
// 1 刻みの流れ: LoadGasCells(セルの気体の成分 → 箱。エネルギーは気体のぶん。段の外で気体の質量が変わったセルの運動量を合わせ直す)
//   → StepGas(箱の面・または面の一覧。小刻みの当てる段 → 粘り)→ RestGasCells(静止の丸め。当てる段の後・書き戻しの前。T-0765)
//   → StoreGasCells(箱 → セル。凝縮相の成分とエネルギーはそのまま・欄と帳簿)。
// セルの並びは呼ぶ側の並び(仮の世界 = 区画の並び・多重解像度 = ブロックの順 × ブロックの中の番号)。小刻みは Jacobi 型(刻みの初めの
// 値だけから決め、足し引きは整数)なので、並びが違っても値はビット単位で同じ。
// 約束(ADR-0256・ADR-0268・ADR-0286): 気体の成分 = 表で凝縮相でない物質。セルの温度は気体と凝縮相で同じ(気体のぶんのエネルギーは
//   gas_flow.hlsli の GfGasEnergyPart)。運動量と運動のエネルギーは気体の物質に付く(減れば速さを保つ・増えれば運動量を保つ。
//   気体が無くなったセルの運動量は帳簿へ)。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "common/reaction.hlsli"
#include "sim/gas_reference.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 層ごとの基準の大気(層 = 縦のセルの段。層 k の高さ = 一番下の層の高さ + k × 層の厚さ。層の厚さはセルのレベル level の 1 辺
    // 〔レベル 0 = 0.5 m・1 段細かいと半分。GasConfig::cellLevel と同じ数。T-0765〕。world_atmosphere.h の SampleAtmosphere)
    class GasAtmosphereLayers {
    public:
        [[nodiscard]] static GasAtmosphereLayers Create(const BakedReactionTable& table,
                                                        int64_t bottomHeightMillimeters, uint32_t layerCount,
                                                        int32_t level = 0);

        [[nodiscard]] std::span<const uint32_t> Species() const { return m_species; }  // 基準の大気の物質(ID の順)
        [[nodiscard]] uint32_t LayerCount() const { return (uint32_t)m_temperatures.size(); }

        // 気体の箱の設定(物質 = slots〔ID の順〕。大きさと境界は気体の箱の軸 x・y・z〔z が上〕。sizes[2] = 層の数)
        [[nodiscard]] GasConfig MakeConfig(const BakedReactionTable& table, std::span<const uint32_t> slots,
                                           std::array<uint32_t, GAS_AXES> sizes,
                                           std::array<GasBoundary, GAS_AXES> boundary, uint32_t substeps,
                                           uint64_t randomSeed) const;

        // 層ごとの基準のセル(静水圧の釣り合い。呼ぶ側の約束: slots は Species() を全部含む)
        [[nodiscard]] std::vector<GasCell> MakeReference(const GasConfig& config,
                                                         std::span<const uint32_t> slots) const;

    private:
        std::vector<std::vector<std::array<uint32_t, 2>>> m_layerFractions;  // 層ごとの (物質 ID, ppm)
        std::vector<uint32_t> m_temperatures;                                // 層ごとの基準の温度(mK)
        uint64_t m_surfacePressureMicroPascal = 0;                           // 一番下の層の圧力
        std::vector<uint32_t> m_species;
    };

    // セルの外に持つ気体の欄(セルごと。運動量は気体の箱の軸)
    struct GasCellFields {
        std::vector<std::array<int64_t, GAS_AXES>> momentum;  // mg·2^-20 m/s
        std::vector<int64_t> kinetic;                         // 運動エネルギーのぶん(mJ。セルのエネルギーの外)
        std::vector<uint64_t> mass;                           // 前の気体の段の終わりの気体の質量(mg。T-0646)

        void Assign(uint32_t count) {
            momentum.assign(count, {0, 0, 0});
            kinetic.assign(count, 0);
            mass.assign(count, 0);
        }
    };

    // 1 刻みの帳簿の足し(外から入った量を正。物質は ID ごと・運動量は気体の箱の軸)。合計と理由ごとの内訳(T-0763。多重解像度の世界は
    // 内訳を世界の帳簿の理由の列へ。和 = 合計)
    struct GasCellsLedger {
        std::vector<int64_t> amounts;  // µmol。添字 = 物質 ID(呼ぶ側が表の物質の数で作る)。全部が世界の端
        int64_t energy = 0;            // mJ(開いた境界の出入り + 重さの仕事)
        std::array<int64_t, GAS_AXES> momentum = {};

        // --- 理由ごと(edge + gravity = energy・edge + wall + gravity + follow + rest = momentum)---
        int64_t edgeEnergy = 0;                              // 世界の端(開いた境界を出入りした塊)
        int64_t gravityEnergy = 0;                           // 重さの仕事
        std::array<int64_t, GAS_AXES> edgeMomentum = {};     // 世界の端(出入りした塊・外の大気の圧力の力積)
        std::array<int64_t, GAS_AXES> wallMomentum = {};     // 壁の力積(壁・塞いだ物)
        std::array<int64_t, GAS_AXES> gravityMomentum = {};  // 重さの力積
        // 欄が物質に付く(段の外で気体の質量が減った・無くなったセルと、段の中で気体が残らなかったセルの運動量。ADR-0286)
        std::array<int64_t, GAS_AXES> followMomentum = {};

        // 静止の丸め(T-0723)で消した運動量(momentum に含む)と熱へ移した運動エネルギー(mJ。セルの中で移すので energy には入れない)
        std::array<int64_t, GAS_AXES> restMomentum = {};
        int64_t restEnergy = 0;
        uint32_t restedCells = 0;  // 静止の丸めで止めたセルの数(報告)
    };

    // 1 刻みの写しの作業場(呼ぶ側が box を物質 slots の設定と基準で作り、openQ16・cellLayers・tick を書いてから LoadGasCells)
    struct GasCellsStep {
        GasBox box;
        std::vector<uint32_t> slots;           // 箱の欄の物質(ID の順)
        std::vector<int64_t> condensedEnergy;  // セルのエネルギーのうち凝縮相のぶん(mJ)
        std::vector<std::array<int64_t, GAS_AXES>>
            momentumBefore;                       // 刻みの初めの運動量(合わせ直しの後。静止の丸めが比べる)
        std::array<int64_t, GAS_AXES> lost = {};  // 段の外で減った・無くなった気体の運動量(書き戻す時に帳簿から引く)
    };

    [[nodiscard]] bool IsGasSpecies(const BakedReactionTable& table, uint32_t species);

    // 箱の欄の物質: 基準の大気の物質と、セルにある凝縮相でない物質(ID の順)
    [[nodiscard]] std::vector<uint32_t> CollectGasSpecies(const BakedReactionTable& table,
                                                          std::span<const reaction::RxCell> cells,
                                                          std::span<const uint32_t> atmosphere);

    // セル → 箱(step.box.cells を cells と同じ並びで書く)
    void LoadGasCells(const BakedReactionTable& table, std::span<const reaction::RxCell> cells,
                      const GasCellFields& fields, GasCellsStep& step);

    // 静止の丸め(T-0723・T-0758。restSpeedUnits = 0 で切る。置き場は当てる段と粘りの後・書き戻しの前。T-0765): 速さがどの軸も
    // restSpeedUnits 単位(2^-20 m/s)未満で、この刻みで速くなっていない(運動量の最大の軸の大きさが刻みの初め以下)気体のあるセルの
    // 運動量を 0 に(帳簿の「静止の丸め」へ)。箱のエネルギーは全エネルギーなので、運動エネルギーのぶんはそのままセルの熱になる
    // (common/rest_rounding.hlsli。液体の面と同じ形)
    void RestGasCells(GasCellsStep& step, uint32_t restSpeedUnits, GasCellsLedger& ledger);

    // 箱 → セル・欄・帳簿。流すと 1 セルの成分がインラインの数(RX_MAX_CELL_SPECIES)を超えるなら false(何も変えない)。
    // 値が変わったセルの changed を 1 に(呼ぶ側が起こす・熱のキャッシュを作り直す)
    [[nodiscard]] bool StoreGasCells(const BakedReactionTable& table, const GasCellsStep& step,
                                     std::span<reaction::RxCell> cells, GasCellFields& fields, GasCellsLedger& ledger,
                                     std::vector<uint8_t>& changed);

}  // namespace bicameral::sim
