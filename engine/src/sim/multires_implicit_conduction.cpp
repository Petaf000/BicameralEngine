// multires_implicit_conduction.cpp — 細かいレベルの熱の陰解法(方式②。ADR-0019)を木の伝導につなぐ CPU リファレンス(T-0119。D-434 の案 a)。
// 面は「流れを計算する側(違うレベルなら細かい側。同じレベルなら両側)」のレベルで分ける: 基準(subcycleBaseLevel)以下は今までの
// 陽解法(multires_conduction.cpp の ComputeConduction)、基準より細かい(Δk 1〜implicitMaxGap)セルの面はここで陰解法の
// 1 刻み(StepImplicit)に入れる。それより細かいセルは陽解法のまま(温度の端数の精度で 1 mK の判定に届かない。下の「約束」)。
// データの流れ: 頁のある・凍っていない本物のブロックのセル(MrCellThermal: 刻みの初めの温度と熱容量)→ 陰解法のセルと面
//   (面の隣は陽解法と同じ MrFindFaceNeighbor)→ StepImplicit(V サイクル → 面の流れ → 安全網 → 両側に足す)→ 変化の表 deltas
//   (呼ぶ側の StepPagedBlock が反応の前に足す)
// 約束:
//   - 系の未知数は基準より細かいセル。その面の粗い側の基準以下のセルも「境のセル」として入れる(境のセルの基準以下の面は陽解法のまま、
//     境の面だけこの系で受ける = 面で分けた分割)。どちらも頁のある・凍っていない・熱容量のあるセルだけ。一様なブロック・凍らせた
//     ブロック・熱容量 0 のセルとの面は流れない(陽解法と同じ。一様なブロックは刻みの初めに隣と温度差が無いので頁を持たない)
//   - 熱容量は刻みの初めの値で線形にする(相変化・反応で変わる分は次の刻みの温度に出る)。温度は整数部だけから(陽解法と同じ)
//   - 活性で刻まない(眠っている)頁のブロックも入れる: 陰解法では熱が 1 刻みに 1 セルより遠くへ届くので、全部刻んだ時と同じ系にする
//     (活性 = 全部のビット一致)。変わったブロックは StepPagedBlock が changed にし、次の刻みに起こされる
//   - Δk > implicitMaxGap(既定 8)のセルは陽解法のまま(頭打ちで 1 段ごとに約 1/4 遅い)。T* は mK × 2^16 の整数なので、面の流れの
//     誤差は約 4^Δk × 2^-16 mK になり、Δk 8 を超えると「新しい温度の誤差 1 mK」で止まらず上限まで回って安全網が陽解法に戻した
//     (たくさんの要求の場面の深さ 26 段で、毎刻み 64 回・14 万セル。T-0119 の計測)。そこの陽解法の面は、陰解法の系のセルにも
//     刻みの初めの温度から流れを足す(境の面の逆向き)
//   - 影のブロック(覗き窓)は陽解法のまま(中間のセルも刻むので、多重格子の親と重なる。T-0128)
//   - 系の大きさの上限(T-0178。options.implicitLimits。GPU では VRAM に先に取る大きさ): 流れの段の前に MarkImplicitBlocks が、
//     入れられるブロックを枠の順に「未知数・セルの上限の見込み(ImBlockCellBound)の和」が予算(MakeMultiresImplicitBudget)に入る所まで選ぶ。
//     選ばれなかったブロックはその刻みだけ陽解法(頭打ち)のまま(QUESTIONS Q22 の案 A。仮〔ユーザー未確認〕)。選ばれたブロックとの
//     同じレベルの面は、選ばれなかった側が境のセルとしてこの系で受ける(陽解法の側は計算しない。multires_conduction.cpp の CollectCellFaces)。
//     粗い側・細かい側の面は、今までの境のセル・Δk > implicitMaxGap と同じ形。多重格子の 2 段目からの節・隣が上限に入らなければ、
//     その段を作らずに縮約を止める(BuildImplicitGrid の limits。反復は遅くなりうるが、保存と安全網は同じ)
//   - 保存は面ごとに 1 つの値(ADR-0017 の形)なのでビット単位。端数の枠が無い粗い側へは整数の単位の倍数だけ送る
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#include <algorithm>
#include <limits>

#include "common/implicit_conduction.hlsli"
#include "common/multires_conduction.hlsli"
#include "sim/implicit_conduction.h"
#include "sim/multires_nest.h"
#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        using nest_detail::CellThermals;
        using nest_detail::CpuTree;

        constexpr uint32_t NOT_IN_SYSTEM = std::numeric_limits<uint32_t>::max();

        // 陰解法の系(セルの番号 = grid.cells の添字)
        struct ImplicitSystem {
            std::vector<ImplicitCell> cells;
            std::vector<ImplicitFace> faces;

            // --- セルごと: 木のどこか・足す前の仮のエネルギー ---
            std::vector<uint32_t> slots;
            std::vector<uint32_t> indices;
            std::vector<int64_t> startEnergies;

            std::vector<uint32_t> ids;  // [枠 × 512] → セルの番号(無ければ NOT_IN_SYSTEM)
        };

        // 系に入れられるブロックか: 世界の本物のブロックで、頁があり、凍っていない
        bool CanJoin(const MultiresNest& nest, std::span<const uint8_t> frozen, uint32_t slot) {
            const MrBlock& block = nest.blocks[slot];
            const bool frozenNow = !frozen.empty() && frozen[slot] != 0;

            return slot < nest.capacity.worldBlocks && block.kind == MR_BLOCK_REAL && !MrIsUniform(block) && !frozenNow;
        }

        // セルを系に足す(熱容量 0 なら足さない)。番号を返す
        uint32_t AddCell(ImplicitSystem& system, const MultiresNest& nest, CellThermals& thermals, uint32_t slot,
                         uint32_t index) {
            const size_t address = (size_t{slot} * MR_BLOCK_CELLS) + index;
            if (system.ids[address] != NOT_IN_SYSTEM)
                return system.ids[address];

            const MrThermal thermal = thermals.At(slot, index);
            if (thermal.capacityLimit == 0)
                return NOT_IN_SYSTEM;

            // --- 熱容量 C = 8 × 上限の係数(レベルによらない数)。仮のエネルギーは刻みの初めの温度に当たる値 ---
            const MrBlock& block = nest.blocks[slot];
            ImplicitCell cell;
            cell.level = block.level;
            cell.x = block.originX + static_cast<int64_t>(MrCellX(index));
            cell.y = block.originY + static_cast<int64_t>(MrCellY(index));
            cell.z = block.originZ + static_cast<int64_t>(MrCellZ(index));
            cell.heatCapacity = thermal.capacityLimit << IM_ENERGY_BITS_PER_LEVEL;
            cell.conductance = thermal.conductance;
            cell.startTemperature = int64_t{thermal.temperature} << IM_TEMPERATURE_SHIFT;
            cell.energy = ImplicitEnergyFor(cell.heatCapacity, thermal.temperature);
            cell.coarseFraction = block.fraction != MR_NO_FRACTION;

            const auto id = static_cast<uint32_t>(system.cells.size());
            system.ids[address] = id;
            system.cells.push_back(cell);
            system.slots.push_back(slot);
            system.indices.push_back(index);
            system.startEnergies.push_back(cell.energy);

            return id;
        }

        // 面の係数(細かい側の単位。頭打ちなし)= min(G) × G の係数 × 4^kf(ADR-0019。陽解法の MrConductanceLimit と同じ値)
        fx::FxU128 FaceCoefficient(const ImplicitCell& fine, const ImplicitCell& coarse) {
            const uint64_t conductance = fine.conductance < coarse.conductance ? fine.conductance : coarse.conductance;

            return ImWideShiftLeft(ImWide(conductance * HC_LIMIT_PER_CONDUCTANCE),
                                   2 * static_cast<uint32_t>(fine.level));
        }

        // 未知数のセル(基準より細かい)の 6 面。同じレベルの面は番号の小さい側が 1 回だけ足す。粗い側は境のセルとして足す
        void AddCellFaces(ImplicitSystem& system, const MultiresNest& nest, CellThermals& thermals,
                          std::span<const uint8_t> frozen, uint32_t id) {
            const CpuTree tree{.nest = &nest};
            const uint32_t slot = system.slots[id];
            const uint32_t index = system.indices[id];
            const MrBlock block = nest.blocks[slot];
            for (uint32_t face = 0; face < MR_FACES; ++face) {
                const MrFaceNeighbor neighbor = MrFindFaceNeighbor(tree, slot, block, index, face,
                                                                   nest.capacity.rootLevel);
                if (neighbor.kind != MR_NEIGHBOR_SAME && neighbor.kind != MR_NEIGHBOR_COARSER)
                    continue;

                if (!CanJoin(nest, frozen, neighbor.slot))
                    continue;

                const uint32_t other = AddCell(system, nest, thermals, neighbor.slot, neighbor.index);
                if (other == NOT_IN_SYSTEM || (neighbor.kind == MR_NEIGHBOR_SAME && other < id))
                    continue;

                const uint32_t gap = neighbor.kind == MR_NEIGHBOR_SAME ? 0 : neighbor.gap;
                system.faces.push_back({.fine = id,
                                        .coarse = other,
                                        .gap = gap,
                                        .coefficient = FaceCoefficient(system.cells[id], system.cells[other])});
            }
        }

        // 系を作る: まず未知数のセル(MarkImplicitBlocks が選んだブロック)を枠の順に全部足し(番号を先に決める)、次にその面と境のセル
        ImplicitSystem MakeSystem(const MultiresNest& nest, CellThermals& thermals, std::span<const uint8_t> frozen,
                                  std::span<const uint8_t> implicitBlocks) {
            ImplicitSystem system;
            system.ids.assign(nest.blocks.size() * MR_BLOCK_CELLS, NOT_IN_SYSTEM);
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                if (!CanJoin(nest, frozen, slot) || !nest_detail::IsImplicitBlock(implicitBlocks, slot))
                    continue;

                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    if (MrIsSteppedCell(nest.blocks[slot], index))
                        AddCell(system, nest, thermals, slot, index);
                }
            }

            const auto unknowns = static_cast<uint32_t>(system.cells.size());
            for (uint32_t id = 0; id < unknowns; ++id)
                AddCellFaces(system, nest, thermals, frozen, id);

            return system;
        }

        // 多重格子の段ごとの節の数と隣の最大(計測用。GPU の ImTail の境〔節 1024・隣 32〕を見る。T-0120)
        void RecordLevels(MultiresNest& nest, const ImplicitGrid& grid) {
            for (const ImplicitGridLevel& level : grid.levels) {
                uint32_t widest = 0;
                for (size_t node = 0; node + 1 < level.rowStarts.size(); ++node)
                    widest = std::max(widest, level.rowStarts[node + 1] - level.rowStarts[node]);

                nest.implicitLevels.push_back({static_cast<uint32_t>(level.levels.size()), widest});
            }
        }

        // 系を作る時の木を写す(試験用。T-0129)
        void CaptureNest(MultiresNest& nest, std::span<const uint8_t> frozen) {
            nest.implicitNest.reset();
            auto copy = std::make_shared<MultiresNest>(nest);
            copy->captureImplicitNest = false;
            copy->implicitFrozen.clear();
            copy->implicitGrid = {};
            nest.implicitNest = std::move(copy);
            nest.implicitFrozen.assign(frozen.begin(), frozen.end());
        }

        // ブロックの未知数のセルの数(刻むセルで熱容量のあるもの。AddCell が足すものと同じ)
        uint32_t CountUnknowns(const MultiresNest& nest, CellThermals& thermals, uint32_t slot) {
            uint32_t count = 0;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (MrIsSteppedCell(nest.blocks[slot], index) && thermals.At(slot, index).capacityLimit != 0)
                    ++count;
            }

            return count;
        }

        // 選んだ形(ADR-0019): V(2,2)・新しい温度の誤差の見込み 1 mK で止める
        ImplicitOptions MakeImplicitOptions(const MultiresStepOptions& options) {
            ImplicitOptions implicit;
            implicit.method = ImplicitMethod::Multigrid;
            implicit.cycles = options.implicitMaxCycles;
            implicit.toleranceMillikelvin = 1;

            return implicit;
        }

    }  // namespace

    namespace nest_detail {

        bool InImplicitConduction(const MultiresNest& nest, uint32_t slot, const MultiresStepOptions& options) {
            const MrBlock& block = nest.blocks[slot];

            const int32_t finest = options.subcycleBaseLevel + static_cast<int32_t>(options.implicitMaxGap);

            return options.implicitConduction && slot < nest.capacity.worldBlocks && block.kind == MR_BLOCK_REAL &&
                   block.level > options.subcycleBaseLevel && block.level <= finest;
        }

        bool IsImplicitBlock(std::span<const uint8_t> implicitBlocks, uint32_t slot) {
            return slot < implicitBlocks.size() && implicitBlocks[slot] != 0;
        }

        std::vector<uint8_t> MarkImplicitBlocks(MultiresNest& nest, CellThermals& thermals,
                                                std::span<const uint8_t> frozen, const MultiresStepOptions& options) {
            std::vector<uint8_t> marks(nest.blocks.size(), 0);
            nest.implicitDeferredBlocks = 0;
            if (!options.implicitConduction)
                return marks;

            // --- 枠の順に、和が予算に入る所まで(超えたブロックから後は全部陽解法。GPU は同じ和を接頭和で)---
            FX_ASSERT(options.implicitOverflow == ImplicitOverflow::Explicit);  // B・C は未実装(Q22)
            const MultiresImplicitBudget budget = MakeMultiresImplicitBudget(options.implicitLimits);
            uint64_t unknowns = 0;
            uint64_t cells = 0;
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                if (!CanJoin(nest, frozen, slot) || !InImplicitConduction(nest, slot, options))
                    continue;

                const uint32_t count = CountUnknowns(nest, thermals, slot);
                unknowns += count;
                cells += ImBlockCellBound(count);
                if (unknowns > budget.unknowns || cells > budget.cells) {
                    nest.implicitDeferredBlocks += 1;
                    continue;
                }

                marks[slot] = 1;
            }

            return marks;
        }

        void AddImplicitConduction(MultiresNest& nest, CellThermals& thermals, std::span<const uint8_t> frozen,
                                   std::span<const uint8_t> implicitBlocks, const MultiresStepOptions& options,
                                   std::span<MrEnergyDelta> deltas) {
            nest.implicitCost = {};
            nest.implicitCells = 0;
            nest.implicitLevels.clear();
            nest.implicitGrid = {};
            nest.implicitNest.reset();
            ImplicitSystem system = MakeSystem(nest, thermals, frozen, implicitBlocks);
            if (system.faces.empty())
                return;

            if (nest.captureImplicitNest)
                CaptureNest(nest, frozen);

            // --- 1 刻み解いて、足した後と仮のエネルギーの差を変化の表へ ---
            const ImplicitGridLimits gridLimits{.nodes = options.implicitLimits.nodes,
                                                .links = options.implicitLimits.links};
            ImplicitGrid grid = BuildImplicitGrid(std::move(system.cells), std::move(system.faces), false, gridLimits);
            if (nest.captureImplicitGrid)
                nest.implicitGrid = grid;

            nest.implicitCost = StepImplicit(grid, MakeImplicitOptions(options));
            nest.implicitCells = static_cast<uint32_t>(grid.cells.size());
            RecordLevels(nest, grid);
            for (size_t id = 0; id < grid.cells.size(); ++id) {
                const ImplicitCell& cell = grid.cells[id];
                const MrEnergyDelta change = {.whole = cell.energy - system.startEnergies[id],
                                              .fraction = cell.fraction};
                if (MrEnergyDeltaIsZero(change))
                    continue;

                const MrBlock& block = nest.blocks[system.slots[id]];
                MrEnergyDelta& entry = deltas[PageCellAddress(nest, block.page, system.indices[id])];
                entry = MrAddEnergyDelta(entry, change);
            }
        }

    }  // namespace nest_detail

}  // namespace bicameral::sim
