// gas_fastest_layout.h — 最速 × 小さい空きの検査を、仮の世界の格子の区画(区画のセルの並び・格子の座標)で呼ぶ道具と、
// 置いた世界(世界の組み立て)の読み込みの検査(T-0804・D-526 ①。T-0788 の検査 gas_fastest_check と同じ関数)。
// データの流れ: 表の環境(gas_pressure_speed = "instant")・世界の組み立て(gas_regions・world_cells)→ 区画のセルの空き(sim/cell_volume)
//   → FindGasSmallOpenJoints → 文(仮の世界の刻みの中の警告 sim/probe_gas と同じ文)→ 読み込み(script/reaction_table_loader)が表の warnings へ。
// GPU を知らない(読み込みの層から呼べる)。浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "common/reaction.hlsli"
#include "sim/gas_fastest_check.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 気体の箱の軸(x・y・z = 上)→ 仮の世界の格子の軸(x・z・y。格子は y が上)
    inline constexpr std::array<uint32_t, GAS_AXES> PROBE_GRID_AXIS_OF_GAS_AXIS = {0, 2, 1};

    // 区画のセルの空きの数え(T-0593・ADR-0268)
    struct GasOpenCounts {
        uint32_t blockedCells = 0;  // 空きが 0(気体が入れない・面は全部閉じる)
        uint32_t partialCells = 0;  // 空きが一部
    };

    // 区画のセル cells の気体の空き(Q16。固体と液体が占めない体積の割合)。全部のセルが全部なら空(今まで通り)。counts に足す
    [[nodiscard]] std::vector<uint32_t> CollectProbeGasOpen(const BakedReactionTable& table,
                                                            std::span<const reaction::RxCell> cells,
                                                            GasOpenCounts& counts);

    // 区画(格子の minCell から。気体の箱の座標の組 joint)の作り手へ見せる文(範囲は格子の座標。頭は GAS_FASTEST_SMALL_OPEN_WARNING_PREFIX)
    [[nodiscard]] std::string ProbeRegionJointWarning(const GasSmallOpenJoint& joint,
                                                      const std::array<uint32_t, 3>& minCell);

    // 読み込みの検査(T-0804・D-526 ①): 世界が最速を選び(環境の gas_pressure_speed = "instant")、世界の組み立てに気体の区画
    // (gas_regions)があれば、区画の初めのセル(world_cells。後に書いた箱が上書きする = MakeProbeInitialWorld と同じ)の空きで、
    // 刻みの中と同じ検査をした警告の文。音速の世界・区画が無いなら空
    [[nodiscard]] std::vector<std::string> CheckLayoutFastestOpenings(const BakedReactionTable& table);

}  // namespace bicameral::sim
