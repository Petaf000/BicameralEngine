// world_layout.h — 世界の組み立て(初めのセルの中身と、置く物)。パッケージの分類 world_cells・world_bodies に書く(T-0398・ADR-0172)。
//
// データの流れ: パッケージの world_cells・world_bodies(script/world_layout_package が名前のまま読む)→ WorldLayoutDefinition
//   (ReactionTableDefinition::worldLayout)→ BakeWorldLayout(BakeReactionTable の最後。物質の名前を ID に直し、領域ごとのセルを作る)
//   → WorldLayout(BakedReactionTable::worldLayout)→ MakeProbeInitialWorld(セル)・MakeLayoutPhysicsScene(物)。
// エンジンは組み立ての「書き方」だけを持ち、何をどこに置くか(空気・木箱・積み木)はパッケージが決める(D-454・ADR-0151 の 2)。
// 分類が無ければ空(ブランクの世界。D-455): セルは全部空・物なし。
// 表の中身(TableBytes)に入るので、再生ファイルは表の中身だけから同じ初めの世界を作り直す(T-0193)。
// 層(T-1236・ADR-0489・20 §10.5): 領域は中身のうち積もった層の量(settled_umol)を書ける。ベイクは成分の並びの位置ごとの層にし、
//   多重解像度の世界の根を置く時(sim::PlaceLayoutRootBlock)に層つきで置く。仮の世界(MakeProbeInitialWorld)は層を持たない。
//   層を書かなかった粒の物質の読み方は UnwrittenGrainLayers(ADR-0489 の 3。仮 = 舞う粒。値で切り替える)。
// 流れる所(D-510・T-0724・ADR-0342 仮): 区画の箱はやめた。気体は基準の大気がある世界、液体は重力がある世界で、世界のどこでも流れる。
//   仮の世界では世界いっぱいの区画 1 つ(WorldLayout::gasRegion・liquidRegion)→ SetLayoutRegions(probe_sim.h)が ProbeSimOptions::gas・liquid に。
//   前の分類 gas_regions・liquid_regions は読まず、書いてあれば注意を出す(retiredCategories → 表の警告)。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "common/reaction.hlsli"
#include "sim/gas_reference.h"

namespace bicameral::sim {

    struct BakedReactionTable;

    // --- 定義(人が書く値。名前のまま)---

    struct WorldCellAmount {
        std::string species;
        uint64_t micromoles = 0;
    };

    // セルの領域 1 つ: 箱(min から size セル)の中を同じ中身のセルで埋める。shellCells > 0 なら外側の厚さ shellCells の殻だけ。
    // 層 layer の小さい順(同じ層は名前の順)に重ねて書く(後から書いた領域が上書きする)
    struct WorldCellRegionDefinition {
        std::string name;
        int64_t layer = 0;

        // --- 場所(セル。wholeWorld なら格子の全部)---
        bool wholeWorld = true;
        std::array<uint32_t, 3> minCell{};
        std::array<uint32_t, 3> sizeCells{};
        uint32_t shellCells = 0;  // 0 = 中まで埋める

        // --- 中身 ---
        std::vector<WorldCellAmount> contents;
        int32_t temperatureMilliKelvin = 0;

        // --- 層(T-1236)---
        std::vector<WorldCellAmount> settled;  // 中身のうち積もった層の量(物質は contents にあること。層 ≤ 量)
    };

    // 層を書かなかった粒の物質(RX_SPECIES_GRAIN)を、層を使う世界でどう読むか(ADR-0489 の 3。ユーザー未確認)
    enum class UnwrittenGrainLayers : uint8_t {
        Floating,  // (a) 舞う粒(層 0)。ベイクが領域ごとに知らせを作る(WorldLayout::unsettledGrainNotes)
        Settled,   // (b) 全部が層(層 = 量)。知らせは作らない
    };

    // 今の読み方(仮 = (a)。ADR-0489 の 3 の答えでここを変える)
    constexpr UnwrittenGrainLayers UNWRITTEN_GRAIN_LAYERS = UnwrittenGrainLayers::Floating;

    // 物 1 つ(直方体。物理の座標 = y が上・格子の (0, 0, 0) の角が原点)。並び order の小さい順(同じなら名前の順)に番号を振る
    struct WorldBodyDefinition {
        std::string name;
        int64_t order = 0;

        std::array<int64_t, 3> sizeMillimeters{};      // 辺の長さ
        std::array<int64_t, 3> positionMillimeters{};  // 重心
        uint64_t massGrams = 0;                        // 0 = 動かない物(地面・壁)
    };

    // 仮の世界で気体が流れる所(D-510 からは世界いっぱい。ベイクが作る。作り手は書かない)
    struct WorldGasRegion {
        std::string name;
        std::array<uint32_t, 3> minCell{};
        std::array<uint32_t, 3> sizeCells{};
        // 箱の外の面(格子の軸 x・y・z の順)。既定は閉じた壁
        std::array<GasBoundary, 3> boundary = {GasBoundary::Wall, GasBoundary::Wall, GasBoundary::Wall};
    };

    // 仮の世界で液体が流れる所(D-510 からは世界いっぱい。ベイクが作る。作り手は書かない)
    struct WorldLiquidRegion {
        std::string name;
        std::array<uint32_t, 3> minCell{};
        std::array<uint32_t, 3> sizeCells{};
        bool advection = true;  // 運動量を液体と一緒に運ぶ(T-0610)
    };

    struct WorldLayoutDefinition {
        std::vector<WorldCellRegionDefinition> cellRegions;
        std::vector<WorldBodyDefinition> bodies;
        std::vector<std::string>
            retiredCategories;  // 書いてあったが読まない分類(gas_regions・liquid_regions。D-510。注意を出す)
    };

    // --- ベイクしたもの(物質は ID。並びは書く順)---

    struct WorldCellRegion {
        bool wholeWorld = true;
        std::array<uint32_t, 3> minCell{};
        std::array<uint32_t, 3> sizeCells{};
        uint32_t shellCells = 0;
        reaction::RxCell cell{};  // 埋める中身(MakeReactionCell で作ったもの)

        // 成分の並びの位置ごとの層(位置は cell.species と同じ。0 ≤ 層 ≤ 量。成分の数の外は 0。T-1236)
        std::array<uint64_t, reaction::RX_MAX_CELL_SPECIES> layers{};
    };

    struct WorldLayout {
        std::vector<WorldCellRegion> cellRegions;       // 層の順
        std::vector<WorldBodyDefinition> bodies;        // 番号の順(物質を指さないので定義のまま)
        std::optional<WorldGasRegion> gasRegion;        // 世界いっぱい。基準の大気が無ければ無し(気体は流れない)
        std::optional<WorldLiquidRegion> liquidRegion;  // 世界いっぱい。重力が 0 なら無し(液体は流れない)

        // 層を書かなかった粒の物質の知らせ(読み方が Floating の時だけ。領域・物質ごとに 1 つ。T-1236)。
        // 層を使う世界(environment.cell_layers)なら、表のベイクが表の警告に足す(使わない世界では出さない。T-1241)
        std::vector<std::string> unsettledGrainNotes;

        // セル (x, y, z) が region に入るか(箱の中で、殻なら外側の厚さ shellCells の中)
        [[nodiscard]] static bool Contains(const WorldCellRegion& region, uint32_t x, uint32_t y, uint32_t z);
    };

    // 名前を ID に直し、領域ごとのセルを作る(table は物質・環境の値・基準の大気までベイクした表)。知らない物質・1 セルの成分の数の上限
    // (RX_MAX_CELL_SPECIES)を超える中身・同じ物質が 2 回は誤り。流れる所は表の基準の大気と重力から作る(D-510)。
    // 層(settled)は中身に無い物質・量を超える層・同じ物質が 2 回が誤り。書かなかった粒の物質は grainReading で読む(T-1236)
    [[nodiscard]] std::expected<WorldLayout, std::string> BakeWorldLayout(
        const WorldLayoutDefinition& definition, const BakedReactionTable& table,
        UnwrittenGrainLayers grainReading = UNWRITTEN_GRAIN_LAYERS);

    // 格子のセル (x, y, z) の初めの中身と層(後に書いた領域が勝つ。どの領域にも入らなければ空のセル・層 0。T-1236)
    struct WorldLayoutCell {
        reaction::RxCell cell{};
        std::array<uint64_t, reaction::RX_MAX_CELL_SPECIES> layers{};
    };
    [[nodiscard]] WorldLayoutCell LayoutCellAt(const WorldLayout& layout, uint32_t x, uint32_t y, uint32_t z);

}  // namespace bicameral::sim
