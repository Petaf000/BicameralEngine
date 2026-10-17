// world_layout_package.h — パッケージの分類 world_cells・world_bodies・gas_regions・liquid_regions(世界の組み立て)を
// sim::WorldLayoutDefinition に読む(T-0398・ADR-0172・T-0671・ADR-0310)。
//
// データの流れ: LoadPackages の合わせた表 → ReadWorldLayout(ReadReactionTableDefinition の最後に呼ばれる)
//   → ReactionTableDefinition::worldLayout → sim::BakeWorldLayout(物質の名前を ID に)→ MakeProbeInitialWorld・MakeLayoutPhysicsScene。
// 書き方(1 つの鍵 = 1 つの領域・物。Mod は鍵を足して増やす。値は整数で、単位は欄の名前に書く):
//   world_cells = {
//       air = { amounts_umol = { oxygen = 1063800, nitrogen = 4014200 }, temperature_mk = 300000 },  -- 場所を省けば格子の全部
//       crate_wall = { layer = 1, min_cell = { 28, 28, 28 }, size_cells = { 8, 8, 8 }, shell_cells = 1,   -- 箱の外側の厚さ 1 の殻
//                      amounts_umol = { cellulose = 38600000 }, temperature_mk = 300000 },
//   }
//   world_bodies = {
//       ground = { size_mm = { 100000, 1000, 100000 }, position_mm = { 16000, -500, 16000 }, mass_g = 0 },  -- 0 = 動かない
//       block = { order = 1, size_mm = { 1000, 1000, 1000 }, position_mm = { 10000, 500, 16250 }, mass_g = 500000 },
//   }
//   gas_regions = {      -- 気体を流す箱(書かなければ流れない。D-455)。今は 1 つまで・基準の大気が要る
//       air_over_fire = { min_cell = { 0, 4, 16 }, size_cells = { 48, 12, 32 }, boundary = { "wall", "open", "wall" } },
//   }
//   liquid_regions = {   -- 液体を流す箱。今は 1 つまで・重力が要る
//       river = { min_cell = { 40, 1, 0 }, size_cells = { 4, 6, 64 }, advection = true },
//   }
// 領域は layer の小さい順(同じなら鍵の順)に重ね書きする。物は order の小さい順(同じなら鍵の順)に番号を振る(物理の結果は番号の順に依る)。
// 知らない欄は誤り。物質の名前の検査(知らない物質)はベイクが行う。
// 欄を変えたら、型の定義 data/types/bicameral.d.luau も変える(T-0140)。ここは CPU だけ。
#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "script/luau_package.h"
#include "sim/world_layout.h"

namespace bicameral::script {

    constexpr std::string_view WORLD_CELLS_CATEGORY = "world_cells";
    constexpr std::string_view WORLD_BODIES_CATEGORY = "world_bodies";
    constexpr std::string_view GAS_REGIONS_CATEGORY = "gas_regions";
    constexpr std::string_view LIQUID_REGIONS_CATEGORY = "liquid_regions";

    // 合わせた表から世界の組み立てを読む(分類が無ければ空)。誤りには「どのパッケージの・どの欄か」を付ける
    [[nodiscard]] std::expected<sim::WorldLayoutDefinition, std::string> ReadWorldLayout(
        const PackageSetResult& packages);

}  // namespace bicameral::script
