// probe_gas_tick.hlsl — 仮の世界の GPU の気体の段(T-0592・ADR-0256)。CPU リファレンスは engine/src/sim/probe_gas.cpp の ProbeGasField::Step。
//
// 適用の単位の中(コマンド → 物理の熱 → 表の作り直し → **気体** → 起こす刻みの来たブロックを一覧へ)で、engine/src/sim/probe_sim.cpp の
// RecordGas が次の順に投げる(段の間は全部の UAV の書き込みを待つ):
//   GasBegin   制御と物質の印を空に
//   GasCollect 区画のセルの凝縮相でない物質に印(1 スレッド = 1 セル)
//   GasPrepare 印 + 基準の大気から欄の物質(ID の順。GPU は 4 つまで。T-0598)を決め、設定(gas_step の t0)と開いた境界の外(t2)を作る
//   GasGather  区画のセル → 気体の箱のセル(成分を欄へ・エネルギーの気体のぶん + 運動エネルギーのぶん・運動量)。凝縮相のぶんは残す
//   (気体の箱の 1 刻み: gas_step.hlsl の小刻み × 9 段。sim/gpu_gas の RecordStepAt)
//   GasCheck   書き戻すと 1 セルの成分がインラインの数を超えるか(超えるセルが 1 つでもあれば、この刻みは流さない)
//   GasScatter 気体の箱 → 区画のセル(凝縮相の成分 + 0 でない気体の成分を ID の順に)。変わったセルは熱のキャッシュを作り直し、
//              ブロックを「刻みの直前に変わった」にして起こす刻みを今にする(物理の熱と同じ。後ろの WakeDueBlocks が一覧へ足す)
//   GasFinish  帳簿(物質 ID ごと・格子の軸)へ足す
// 区画の中の番号 = x + 幅 × (z + 奥行き × y)(気体の箱の並び。箱の z = 格子の y = 上)。整数だけ(D-205)。
#include "common/probe_gas_gpu.hlsli"
#include "sim/probe_bindings.hlsli"

RWStructuredBuffer<ProbeGasCellState> gasState : register(u15);  // 区画のセルごとの気体の欄
RWByteAddressBuffer gasWork : register(u16);                     // 制御・設定・見出し・帳簿など(probe_gas_gpu.hlsli)
RWStructuredBuffer<GasGpuCell> gasCells : register(u17);         // 気体の箱のセル(gas_step の u0)
RWByteAddressBuffer gasBoxLedger : register(u18);                // 気体の箱の 1 刻みの帳簿(gas_step の u5)

static GasGpuParameters g_gasParameters;
#define GAS_P g_gasParameters
#include "common/gas_cell.hlsli"

static const uint32_t NO_SLOT = GAS_GPU_MAX_SPECIES;
static const uint32_t NO_SPECIES = 0xFFFFFFFFu;

// --- 読み書き ---

ProbeGasHeader GasHeader() {
    return gasWork.Load<ProbeGasHeader>(PROBE_GAS_HEADER_OFFSET);
}

ProbeGasSpecies GasSpecies(ProbeGasHeader header, uint32_t id) {
    return gasWork.Load<ProbeGasSpecies>(header.speciesOffset + id * PROBE_GAS_SPECIES_BYTES);
}

// 凝縮相でない(表に無い ID は気体として扱う。CPU の IsGas と同じ)
bool IsGasSpecies(ProbeGasHeader header, uint32_t id) {
    return id < header.speciesCount ? GasSpecies(header, id).isGas != 0 : true;
}

uint32_t SlotCount() {
    return gasWork.Load(PROBE_GAS_CONTROL_SLOT_COUNT);
}

uint4 Slots() {
    return gasWork.Load4(PROBE_GAS_CONTROL_SLOTS);
}

bool Skipped() {
    return gasWork.Load(PROBE_GAS_CONTROL_TOO_MANY) != 0 || gasWork.Load(PROBE_GAS_CONTROL_OVERFLOW) != 0;
}

// 物質 ID の欄(無ければ NO_SLOT)
uint32_t SlotOf(uint4 slots, uint32_t slotCount, uint32_t id) {
    uint32_t result = NO_SLOT;
    [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
        if (s < slotCount && slots[s] == id)
            result = s;
    }

    return result;
}

// 区画の中の番号 → 格子のセルの番号
uint32_t RegionGridCell(ProbeGasHeader header, uint32_t region) {
    const uint32_t width = header.regionSize[0];
    const uint32_t depth = header.regionSize[2];
    const uint32_t x = region % width;
    const uint32_t z = (region / width) % depth;
    const uint32_t y = region / (width * depth);

    return ProbeCellIndex(header.regionMin[0] + x, header.regionMin[1] + y, header.regionMin[2] + z);
}

void AddLedger(uint32_t address, int64_t value) {
    gasWork.Store<int64_t>(address, gasWork.Load<int64_t>(address) + value);
}

// --- エネルギーの気体のぶん(probe_gas.cpp の GasEnergyPart)---

// 床で割る(負の数も下へ)
int64_t DivideDown(int64_t value, uint64_t divisor) {
    FxU128 magnitude;
    magnitude.hi = 0;
    magnitude.lo = FxAbsU64(value);
    const FxDivResult result = FxDivU128By64(magnitude, divisor);
    if (value >= 0)
        return (int64_t)result.quotient;

    return -(int64_t)(result.quotient + (result.remainder != 0 ? 1 : 0));
}

// value × numerator ÷ denominator(0 の向きへ切り捨て)
int64_t ScaleToward0(int64_t value, uint64_t numerator, uint64_t denominator) {
    const uint64_t scaled = FxDivU128By64(FxMulU64Full(FxAbsU64(value), numerator), denominator).quotient;

    return FxApplySign(scaled, value < 0);
}

int64_t GasEnergyPart(ProbeGasHeader header, RxCell cell) {
    int64_t chemical = 0;
    int64_t gasChemical = 0;
    uint64_t capacity = 0;
    uint64_t gasCapacity = 0;
    bool hasCondensed = false;
    bool hasGas = false;
    [unroll] for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i < cell.speciesCount) {
            const ProbeGasSpecies species = GasSpecies(header, cell.species[i]);
            const int64_t amountChemical = (int64_t)cell.amounts[i] * species.formationEnergy;
            const uint64_t amountCapacity = cell.amounts[i] * (uint64_t)species.heatCapacity;
            chemical += amountChemical;
            capacity += amountCapacity;
            if (IsGasSpecies(header, cell.species[i])) {
                hasGas = true;
                gasChemical += amountChemical;
                gasCapacity += amountCapacity;
            } else {
                hasCondensed = true;
            }
        }
    }

    if (!hasGas)
        return 0;

    if (!hasCondensed || capacity == 0)
        return cell.energy;

    const int64_t heat = (cell.energy * 1000) - chemical;  // µJ
    const int64_t gasHeat = ScaleToward0(heat, gasCapacity, capacity);

    return DivideDown(gasHeat + gasChemical, 1000);
}

// --- 段 ---

// 1 スレッド = 物質の印の 1 語(スレッド 0 は制御も)
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void GasBegin(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeGasHeader header = GasHeader();
    const uint32_t word = dispatchThreadId.x;
    if (word < header.maskWords)
        gasWork.Store(header.maskOffset + word * 4, 0);

    if (word != 0)
        return;

    gasWork.Store4(PROBE_GAS_CONTROL_SLOT_COUNT, uint4(0, 0, 0, 0));  // 数・多すぎ・溢れ・変わったセル
    gasWork.Store4(PROBE_GAS_CONTROL_SLOTS, uint4(0, 0, 0, 0));
    gasWork.Store<int64_t>(PROBE_GAS_CONTROL_LOST, 0);
    gasWork.Store<int64_t>(PROBE_GAS_CONTROL_LOST + 8, 0);
    gasWork.Store<int64_t>(PROBE_GAS_CONTROL_LOST + 16, 0);
    gasWork.Store(PROBE_GAS_CONTROL_SPECIES_SEEN, 0);
}

    // 1 スレッド = 区画の 1 セル
    [numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void GasCollect(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeGasHeader header = GasHeader();
    const uint32_t region = dispatchThreadId.x;
    if (region >= header.regionCellCount)
        return;

    const uint32_t index = GenerationBase(CurrentTick()) + RegionGridCell(header, region);
    const uint32_t count = cells[index].speciesCount;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t id = cells[index].species[i];
        if (cells[index].amounts[i] == 0 || !IsGasSpecies(header, id) || id >= header.speciesCount)
            continue;

        gasWork.InterlockedOr(header.maskOffset + (id / 32) * 4, 1u << (id % 32));
    }
}

// 1 スレッド: 欄の物質・設定・開いた境界の外・箱の帳簿を空に
[numthreads(1, 1, 1)] void GasPrepare() {
    const ProbeGasHeader header = GasHeader();

    // --- 欄の物質(印 + 基準の大気。ID の順。GPU の欄の数を超えたら流さない)---
    uint32_t count = 0;
    for (uint32_t word = 0; word < header.maskWords; ++word) {
        uint32_t bits = gasWork.Load(header.maskOffset + word * 4);
        [unroll] for (uint32_t a = 0; a < PROBE_GAS_MAX_ATMOSPHERE; ++a) {
            const uint32_t id = header.atmosphereSpecies[a];
            if (a < header.atmosphereCount && id / 32 == word)
                bits |= 1u << (id % 32);
        }

        while (bits != 0) {
            const uint32_t id = word * 32 + firstbitlow(bits);
            bits &= bits - 1;
            if (count < GAS_GPU_MAX_SPECIES)
                gasWork.Store(PROBE_GAS_CONTROL_SLOTS + count * 4, id);

            ++count;
        }
    }

    const uint32_t slotCount = min(count, GAS_GPU_MAX_SPECIES);
    gasWork.Store(PROBE_GAS_CONTROL_SLOT_COUNT, slotCount);
    gasWork.Store(PROBE_GAS_CONTROL_TOO_MANY, count > GAS_GPU_MAX_SPECIES ? 1u : 0u);
    gasWork.Store(PROBE_GAS_CONTROL_SPECIES_SEEN, count);
    const uint4 slots = Slots();

    // --- 設定(型 + 欄ごとの物質の値。丸めの鍵は物質 ID)---
    GasGpuParameters parameters = gasWork.Load<GasGpuParameters>(PROBE_GAS_TEMPLATE_OFFSET);
    [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
        parameters.massPerAmount[s] = 0;
        parameters.formationEnergy[s] = 0;
        parameters.heatCapacity[s] = 0;
        parameters.roundingKey[s] = s;
        if (s < slotCount) {
            const ProbeGasSpecies species = GasSpecies(header, slots[s]);
            parameters.massPerAmount[s] = species.massPerAmount;
            parameters.formationEnergy[s] = species.formationEnergy;
            parameters.heatCapacity[s] = species.heatCapacity;
            parameters.roundingKey[s] = slots[s];
        }
    }

    parameters.speciesCount = slotCount;
    gasWork.Store<GasGpuParameters>(PROBE_GAS_PARAMETERS_OFFSET, parameters);

    // --- 開いた境界の外(基準の大気の並び → 欄の並び)---
    uint32_t atmosphereSlots[PROBE_GAS_MAX_ATMOSPHERE];
    [unroll] for (uint32_t a = 0; a < PROBE_GAS_MAX_ATMOSPHERE; ++a)
        atmosphereSlots[a] = a < header.atmosphereCount ? SlotOf(slots, slotCount, header.atmosphereSpecies[a])
                                                        : NO_SLOT;

    for (uint32_t layer = 0; layer < header.layerCount; ++layer) {
        const GasGpuCell source = gasWork.Load<GasGpuCell>(header.ghostAtmosphereOffset + layer * 64);
        GasGpuCell ghost = source;
        [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
            ghost.amounts[s] = 0;
            [unroll] for (uint32_t b = 0; b < PROBE_GAS_MAX_ATMOSPHERE; ++b) {
                if (atmosphereSlots[b] == s)
                    ghost.amounts[s] = source.amounts[b];
            }
        }

        gasWork.Store<GasGpuCell>(header.ghostOffset + layer * 64, ghost);
    }

    // --- 箱の帳簿(この刻みのぶん)---
    gasBoxLedger.Store4(0, uint4(0, 0, 0, 0));
    gasBoxLedger.Store4(16, uint4(0, 0, 0, 0));
    gasBoxLedger.Store4(32, uint4(0, 0, 0, 0));
    gasBoxLedger.Store4(48, uint4(0, 0, 0, 0));
}

    // 1 スレッド = 区画の 1 セル
    [numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void GasGather(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeGasHeader header = GasHeader();
    const uint32_t region = dispatchThreadId.x;
    if (region >= header.regionCellCount)
        return;

    const uint32_t slotCount = SlotCount();
    const uint4 slots = Slots();
    const RxCell cell = cells[GenerationBase(CurrentTick()) + RegionGridCell(header, region)];

    // --- 成分を欄へ ---
    GasGpuCell gas;
    [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
        gas.amounts[s] = 0;
        [unroll] for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
            if (s < slotCount && i < cell.speciesCount && cell.species[i] == slots[s])
                gas.amounts[s] = cell.amounts[i];
        }
    }

    // --- エネルギーと運動量(気体が無いセルの運動量は外へ出たものとして帳簿へ、運動エネルギーのぶんは凝縮相の熱へ)---
    const int64_t gasPart = GasEnergyPart(header, cell);
    int64_t condensed = cell.energy - gasPart;
    gas.energy = gasPart;
    gas.momentum[0] = 0;
    gas.momentum[1] = 0;
    gas.momentum[2] = 0;

    const ProbeGasCellState state = gasState[region];
    bool empty = true;
    [unroll] for (uint32_t t = 0; t < GAS_GPU_MAX_SPECIES; ++t) empty = empty && gas.amounts[t] == 0;

    if (empty) {
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
            if (state.momentum[axis] != 0) {
                int64_t original;
                gasWork.InterlockedAdd64(PROBE_GAS_CONTROL_LOST + ProbeGasGridAxis(axis) * 8, state.momentum[axis],
                                         original);
            }
        }

        condensed += state.kinetic;
    } else {
        gas.momentum = state.momentum;
        gas.energy += state.kinetic;
    }

    gasWork.Store<int64_t>(header.condensedOffset + region * 8, condensed);
    gasCells[region] = gas;
}

// 1 スレッド = 区画の 1 セル: 書き戻した成分の数がインラインの数を超えるか
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void GasCheck(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeGasHeader header = GasHeader();
    const uint32_t region = dispatchThreadId.x;
    if (region >= header.regionCellCount || gasWork.Load(PROBE_GAS_CONTROL_TOO_MANY) != 0)
        return;

    const RxCell cell = cells[GenerationBase(CurrentTick()) + RegionGridCell(header, region)];
    const GasGpuCell gas = gasCells[region];
    uint32_t count = 0;
    [unroll] for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i < cell.speciesCount && !IsGasSpecies(header, cell.species[i]))
            ++count;
    }

    [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
        if (gas.amounts[s] != 0)
            ++count;
    }

    if (count > RX_MAX_CELL_SPECIES)
        gasWork.InterlockedOr(PROBE_GAS_CONTROL_OVERFLOW, 1u);
}

    // 1 スレッド = 区画の 1 セル: 気体の箱 → セル
    [numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void GasScatter(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeGasHeader header = GasHeader();
    const uint32_t region = dispatchThreadId.x;
    if (region >= header.regionCellCount || Skipped())
        return;

    g_gasParameters = gasWork.Load<GasGpuParameters>(PROBE_GAS_PARAMETERS_OFFSET);
    const uint32_t slotCount = SlotCount();
    const uint4 slots = Slots();
    const uint64_t tick = CurrentTick();
    const uint32_t grid = RegionGridCell(header, region);
    const uint32_t index = GenerationBase(tick) + grid;
    const RxCell before = cells[index];
    const GasGpuCell gas = gasCells[region];

    // --- 運動エネルギーのぶん(µJ → mJ。最も近い整数。負にならない)---
    const int64_t kineticMicroJoule = KineticEnergy(gas, CellMass(gas));
    const int64_t kinetic = (int64_t)(((uint64_t)kineticMicroJoule + 500) / 1000);
    const int64_t condensed = gasWork.Load<int64_t>(header.condensedOffset + region * 8);

    // --- 凝縮相の成分 + 0 でない気体の成分を ID の順に(どちらも ID の順で、重ならない)---
    uint32_t gasInBefore = 0;  // before の成分のうち気体の印
    [unroll] for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i < before.speciesCount && IsGasSpecies(header, before.species[i]))
            gasInBefore |= 1u << i;
    }

    RxCell after = RxMakeEmptyCell(condensed + gas.energy - kinetic);
    uint32_t last = 0;
    bool first = true;
    [unroll] for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
        uint32_t bestId = NO_SPECIES;
        uint64_t bestAmount = 0;
        [unroll] for (uint32_t c = 0; c < RX_MAX_CELL_SPECIES; ++c) {
            const uint32_t id = before.species[c];
            if (c < before.speciesCount && (gasInBefore & (1u << c)) == 0 && (first || id > last) && id < bestId) {
                bestId = id;
                bestAmount = before.amounts[c];
            }
        }

        [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
            const uint32_t id = slots[s];
            if (s < slotCount && gas.amounts[s] != 0 && (first || id > last) && id < bestId) {
                bestId = id;
                bestAmount = gas.amounts[s];
            }
        }

        if (bestId != NO_SPECIES) {
            after.species[k] = bestId;
            after.amounts[k] = bestAmount;
            after.speciesCount = k + 1;
            last = bestId;
            first = false;
        }
    }

    // --- 気体の欄 ---
    ProbeGasCellState state;
    state.momentum = gas.momentum;
    state.kinetic = kinetic;
    gasState[region] = state;

    // --- 変わったセルだけ書く ---
    bool same = after.energy == before.energy && after.speciesCount == before.speciesCount;
    [unroll] for (uint32_t j = 0; j < RX_MAX_CELL_SPECIES; ++j) {
        if (j < after.speciesCount)
            same = same && after.species[j] == before.species[j] && after.amounts[j] == before.amounts[j];
    }

    if (same)
        return;

    cells[index] = after;
    thermal[index] = ProbeMakeCache(ReactionTable(), after);

    const uint32_t block = ProbeBlockOfCell(grid % PROBE_GRID_SIZE, (grid / PROBE_GRID_SIZE) % PROBE_GRID_SIZE,
                                            grid / PROBE_SLICE_CELL_COUNT);
    StoreBlockMark(PROBE_SCHEDULE_CHANGED_WORD, block, ProbeChangeMark(tick) - 1);
    StoreBlockMark(PROBE_SCHEDULE_WAKE_WORD, block, ProbeChangeMark(tick));

    uint32_t changed;
    gasWork.InterlockedAdd(PROBE_GAS_CONTROL_CHANGED, 1, changed);
}

// 1 スレッド: 帳簿へ(流さなかった刻みは数えるだけ)
[numthreads(1, 1, 1)] void GasFinish() {
    if (gasWork.Load(PROBE_GAS_CONTROL_TOO_MANY) != 0) {
        gasWork.Store(PROBE_GAS_CONTROL_SKIPPED_TOO_MANY, gasWork.Load(PROBE_GAS_CONTROL_SKIPPED_TOO_MANY) + 1);
        return;
    }

    if (gasWork.Load(PROBE_GAS_CONTROL_OVERFLOW) != 0) {
        gasWork.Store(PROBE_GAS_CONTROL_SKIPPED_OVERFLOW, gasWork.Load(PROBE_GAS_CONTROL_SKIPPED_OVERFLOW) + 1);
        return;
    }

    const ProbeGasHeader header = GasHeader();
    const uint32_t slotCount = SlotCount();
    const uint4 slots = Slots();
    [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
        if (s < slotCount)
            AddLedger(header.ledgerOffset + slots[s] * 8, gasBoxLedger.Load<int64_t>(GAS_GPU_LEDGER_AMOUNTS + s * 8));
    }

    const uint32_t energyAddress = header.ledgerOffset + header.speciesCount * 8;
    AddLedger(energyAddress, gasBoxLedger.Load<int64_t>(GAS_GPU_LEDGER_ENERGY));
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        AddLedger(energyAddress + 8 + ProbeGasGridAxis(axis) * 8,
                  gasBoxLedger.Load<int64_t>(GAS_GPU_LEDGER_MOMENTUM + axis * 8));
        AddLedger(energyAddress + 8 + axis * 8, -gasWork.Load<int64_t>(PROBE_GAS_CONTROL_LOST + axis * 8));
    }

    gasWork.Store(PROBE_GAS_CONTROL_STEPPED, gasWork.Load(PROBE_GAS_CONTROL_STEPPED) + 1);
}
