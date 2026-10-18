// probe_gas_tick.hlsl — 仮の世界の GPU の気体の段(T-0592・ADR-0256)。CPU リファレンスは engine/src/sim/probe_gas.cpp の ProbeGasField::Step。
//
// 適用の単位の中(コマンド → 物理の熱 → 表の作り直し → **気体** → 起こす刻みの来たブロックを一覧へ)で、engine/src/sim/probe_sim.cpp の
// RecordGas が次の順に投げる(段の間は全部の UAV の書き込みを待つ):
//   GasBegin   制御と物質の印を空に
//   GasCollect 区画のセルの凝縮相でない物質に印(1 スレッド = 1 セル)
//   GasPrepare 印 + 基準の大気から欄の物質(ID の順。数の上限なし。T-0598)を決め、設定(gas_step の t0)と開いた境界の外(t2)を作る
//   GasGather  区画のセル → 気体の箱のセル(成分を欄へ・エネルギーの気体のぶん + 運動エネルギーのぶん・運動量)。凝縮相のぶんは残す
//              セルの気体の空き(固体と液体が占めない割合。T-0617)を t3 へ書き、一部のセルがあれば設定の openness を立てる
//   (気体の箱の 1 刻み: gas_step.hlsl の小刻み × 9 段。sim/gpu_gas の RecordStepAt)
//   GasCheck   書き戻すと 1 セルの成分がインラインの数を超えるか(超えるセルが 1 つでもあれば、この刻みは流さない)
//   GasScatter 気体の箱 → 区画のセル(凝縮相の成分 + 0 でない気体の成分を ID の順に)。変わったセルは熱のキャッシュを作り直し、
//              ブロックを「刻みの直前に変わった」にして起こす刻みを今にする(物理の熱と同じ。後ろの WakeDueBlocks が一覧へ足す)
//   GasFinish  帳簿(物質 ID ごと・格子の軸)へ足す
// ハッシュの単位(HashCells の後)では GasHash が区画の気体の欄と帳簿を状態のハッシュへ足す(T-0620)。
// 区画の中の番号 = x + 幅 × (z + 奥行き × y)(気体の箱の並び。箱の z = 格子の y = 上)。整数だけ(D-205)。
// 気体の箱のセルは頭の並び + 成分ごとの面(欄の数 header.slotCapacity)、外は「頭 + 物質量 × 欄の数」の件(gas_gpu.hlsli)。
// 回す欄はこの刻みの欄の物質の数だけ。
#include "common/probe_gas_gpu.hlsli"
#include "common/probe_region_hash.hlsli"
#include "sim/probe_bindings.hlsli"

RWStructuredBuffer<ProbeGasCellState> gasState : register(u15);  // 区画のセルごとの気体の欄
RWByteAddressBuffer gasWork : register(u16);                     // 制御・設定・見出し・帳簿など(probe_gas_gpu.hlsli)
RWByteAddressBuffer gasCells : register(u17);                    // 気体の箱のセルの件(gas_step の u0)
RWByteAddressBuffer gasBoxLedger : register(u18);                // 気体の箱の 1 刻みの帳簿(gas_step の u5)

// 気体のセルの部品(gas_cell.hlsli)が読む設定の頭と欄ごとの物質の値(この刻みの設定。読む段の初めに入れる)
static GasGpuParameters g_gasParameters;
static uint32_t g_gasSpeciesOffset;

GasGpuSpecies GasSlotSpecies(uint32_t s) {
    return gasWork.Load<GasGpuSpecies>(g_gasSpeciesOffset + (s * GAS_GPU_SPECIES_BYTES));
}

#define GAS_P g_gasParameters
#define GAS_SPECIES(s) GasSlotSpecies(s)
#include "common/gas_cell.hlsli"

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

// 欄 s の物質 ID
uint32_t SlotSpecies(ProbeGasHeader header, uint32_t s) {
    return gasWork.Load(header.slotsOffset + s * 4);
}

// 物質 ID の欄(この刻みに無ければ PROBE_GAS_NO_SLOT)
uint32_t SlotOf(ProbeGasHeader header, uint32_t id) {
    return id < header.speciesCount ? gasWork.Load(header.slotOfOffset + id * 4) : PROBE_GAS_NO_SLOT;
}

bool Skipped() {
    return gasWork.Load(PROBE_GAS_CONTROL_TOO_MANY) != 0 || gasWork.Load(PROBE_GAS_CONTROL_OVERFLOW) != 0;
}

// 気体の箱のセル(頭の並び + 成分ごとの面。gas_step と同じ)の区画の中の番号 region の頭と、欄 s の物質量の場所
uint32_t GasCellAddress(uint32_t region) {
    return region * GAS_GPU_CELL_BYTES;
}

uint32_t GasAmountAddress(ProbeGasHeader header, uint32_t region, uint32_t s) {
    return GasGpuPlaneAddress(GAS_GPU_CELL_BYTES, header.regionCellCount, region, s);
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

// --- エネルギーの気体のぶん(probe_gas.cpp の GasEnergyPart。式は gas_flow.hlsli の GfGasEnergyPart)---

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

    return GfGasEnergyPart(cell.energy, chemical, capacity, gasChemical, gasCapacity, hasGas, hasCondensed);
}

// --- セルの気体の空き(sim/cell_volume の ShareCellVolume・GasOpenQ16。T-0593・T-0617・ADR-0268)---
static const uint64_t PROBE_CELL_VOLUME_PICOLITERS = (uint64_t)125000000 * 1000000;  // 0.5 m 角 = 1.25e14 pL
static const uint64_t VOLUME_CAP = (uint64_t)1 << 62;  // 占める体積の頭打ち(sim/cell_volume)

// 凝縮相が占める体積(固体と液体は別々に頭打ち)を除いた残りの割合(Q16。切り捨て。占める分がセル以上なら 0)。
// 64bit の「0 で止める引き算」は NVIDIA のドライバが誤って下ろすので(T-0185)、符号付きの差を出して 0 以下を 0 にする
uint32_t GasOpenOf(ProbeGasHeader header, RxCell cell) {
    uint64_t solid = 0;
    uint64_t liquid = 0;
    [unroll] for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i < cell.speciesCount && cell.species[i] < header.speciesCount) {
            const ProbeGasSpecies species = GasSpecies(header, cell.species[i]);
            if (species.isGas == 0) {
                const FxU128 product = FxMulU64Full(cell.amounts[i], species.molarVolume);
                const uint64_t volume = product.hi != 0 ? VOLUME_CAP : min(product.lo, VOLUME_CAP);
                if (species.isLiquid != 0)
                    liquid = min(liquid + volume, VOLUME_CAP);
                else
                    solid = min(solid + volume, VOLUME_CAP);
            }
        }
    }

    const uint64_t occupied = min(solid + liquid, VOLUME_CAP);
    const int64_t room = (int64_t)PROBE_CELL_VOLUME_PICOLITERS - (int64_t)occupied;
    if (room <= 0)
        return 0;

    const FxU128 product = FxMulU64Full((uint64_t)room, GAS_GPU_OPEN_FULL);

    return (uint32_t)FxDivU128By64(product, PROBE_CELL_VOLUME_PICOLITERS).quotient;
}

// --- 段 ---

// 1 スレッド = 物質の印の 1 語(その語の 32 個の物質の「物質 → 欄」も空に。スレッド 0 は制御も)
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void GasBegin(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeGasHeader header = GasHeader();
    const uint32_t word = dispatchThreadId.x;
    if (word < header.maskWords) {
        gasWork.Store(header.maskOffset + word * 4, 0);
        for (uint32_t bit = 0; bit < 32; ++bit) {
            const uint32_t id = word * 32 + bit;
            if (id < header.speciesCount)
                gasWork.Store(header.slotOfOffset + id * 4, PROBE_GAS_NO_SLOT);
        }
    }

    if (word != 0)
        return;

    gasWork.Store4(PROBE_GAS_CONTROL_SLOT_COUNT, uint4(0, 0, 0, 0));  // 数・多すぎ・溢れ・変わったセル
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

    // --- 欄の物質(印 + 基準の大気。ID の順。欄の数を超えたら流さない〔欄の数 = 表の気体の物質の数なので起きない〕)---
    uint32_t count = 0;
    for (uint32_t word = 0; word < header.maskWords; ++word) {
        uint32_t bits = gasWork.Load(header.maskOffset + word * 4);
        for (uint32_t a = 0; a < header.atmosphereCount; ++a) {
            const uint32_t id = gasWork.Load(header.atmosphereOffset + a * 4);
            if (id / 32 == word)
                bits |= 1u << (id % 32);
        }

        while (bits != 0) {
            const uint32_t id = word * 32 + firstbitlow(bits);
            bits &= bits - 1;
            if (count < header.slotCapacity) {
                gasWork.Store(header.slotsOffset + count * 4, id);
                gasWork.Store(header.slotOfOffset + id * 4, count);
            }

            ++count;
        }
    }

    const uint32_t slotCount = min(count, header.slotCapacity);
    gasWork.Store(PROBE_GAS_CONTROL_SLOT_COUNT, slotCount);
    gasWork.Store(PROBE_GAS_CONTROL_TOO_MANY, count > header.slotCapacity ? 1u : 0u);
    gasWork.Store(PROBE_GAS_CONTROL_SPECIES_SEEN, count);

    // --- 設定(型 + 欄ごとの物質の値。丸めの鍵は物質 ID)---
    GasGpuParameters parameters = gasWork.Load<GasGpuParameters>(PROBE_GAS_TEMPLATE_OFFSET);
    parameters.speciesCount = slotCount;
    gasWork.Store<GasGpuParameters>(header.parametersOffset, parameters);
    for (uint32_t s = 0; s < slotCount; ++s) {
        const uint32_t id = SlotSpecies(header, s);
        const ProbeGasSpecies species = GasSpecies(header, id);
        GasGpuSpecies slot;
        slot.massPerAmount = species.massPerAmount;
        slot.formationEnergy = species.formationEnergy;
        slot.heatCapacity = species.heatCapacity;
        slot.roundingKey = id;
        gasWork.Store<GasGpuSpecies>(header.parametersOffset + GAS_GPU_PARAMETERS_BYTES + s * GAS_GPU_SPECIES_BYTES,
                                     slot);
    }

    // --- 開いた境界の外(基準の大気の並び → 欄の並び)---
    const uint32_t ghostStride = GasGpuCellStride(header.slotCapacity);
    for (uint32_t layer = 0; layer < header.layerCount; ++layer) {
        const uint32_t source = header.ghostAtmosphereOffset + layer * ghostStride;
        const uint32_t target = header.ghostOffset + layer * ghostStride;
        gasWork.Store<GasGpuCell>(target, gasWork.Load<GasGpuCell>(source));
        for (uint32_t clear = 0; clear < slotCount; ++clear)
            gasWork.Store<uint64_t>(target + GAS_GPU_CELL_BYTES + clear * 8, 0);

        for (uint32_t b = 0; b < header.atmosphereCount; ++b) {
            const uint32_t slot = SlotOf(header, gasWork.Load(header.atmosphereOffset + b * 4));
            if (slot < slotCount) {
                gasWork.Store<uint64_t>(target + GAS_GPU_CELL_BYTES + slot * 8,
                                        gasWork.Load<uint64_t>(source + GAS_GPU_CELL_BYTES + b * 8));
            }
        }
    }

    // --- 箱の帳簿(この刻みのぶん)---
    const uint32_t ledgerWords = GasGpuLedgerBytes(header.slotCapacity) / 8;
    for (uint32_t ledgerWord = 0; ledgerWord < ledgerWords; ++ledgerWord)
        gasBoxLedger.Store2(ledgerWord * 8, uint2(0, 0));
}

    // 1 スレッド = 区画の 1 セル
    [numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void GasGather(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeGasHeader header = GasHeader();
    const uint32_t region = dispatchThreadId.x;
    if (region >= header.regionCellCount)
        return;

    g_gasParameters = gasWork.Load<GasGpuParameters>(
        header.parametersOffset);  // 質量を数える(GasPrepare が書いた刻みの欄の物質)
    g_gasSpeciesOffset = header.parametersOffset + GAS_GPU_PARAMETERS_BYTES;
    const uint32_t slotCount = SlotCount();
    const RxCell cell = cells[GenerationBase(CurrentTick()) + RegionGridCell(header, region)];

    // --- セルの気体の空き(gas_step の t3)。一部のセルが 1 つでもあれば、この刻みの箱は空きを読む(probe_gas.cpp の CollectGasOpen)---
    const uint32_t open = GasOpenOf(header, cell);
    gasWork.Store(header.openOffset + region * 4, open);
    if (open != GAS_GPU_OPEN_FULL) {
        uint32_t before;
        gasWork.InterlockedOr(header.parametersOffset + GAS_GPU_PARAMETERS_OPENNESS, 1u, before);
    }

    // --- 成分を欄へ(欄の物質の成分だけ。ほかの欄は 0)---
    for (uint32_t clear = 0; clear < slotCount; ++clear)
        gasCells.Store<uint64_t>(GasAmountAddress(header, region, clear), 0);

    bool empty = true;
    [unroll] for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i < cell.speciesCount) {
            const uint32_t slot = SlotOf(header, cell.species[i]);
            if (slot < slotCount) {
                gasCells.Store<uint64_t>(GasAmountAddress(header, region, slot), cell.amounts[i]);
                empty = empty && cell.amounts[i] == 0;
            }
        }
    }

    // --- エネルギーと運動量(気体が無いセルの運動量は外へ出たものとして帳簿へ、運動エネルギーのぶんは凝縮相の熱へ)---
    const int64_t gasPart = GasEnergyPart(header, cell);
    int64_t condensed = cell.energy - gasPart;
    GasGpuCell gas;
    gas.energy = gasPart;
    gas.momentum[0] = 0;
    gas.momentum[1] = 0;
    gas.momentum[2] = 0;

    const ProbeGasCellState state = gasState[region];
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
        // 段の外(反応・相変化・置く操作)で気体の質量が減ったセルは速さを保つ(減った気体の運動量は帳簿へ。運動のエネルギーの減った分は熱へ。
        // follow_matter.hlsli の FmMomentumAfterMassChange = 液体の面と同じ式。T-0646・ADR-0286・T-0718)
        GasGpuTotals totals = GasZeroTotals();
        for (uint32_t s = 0; s < slotCount; ++s)
            GasAddSpecies(totals, s, gasCells.Load<uint64_t>(GasAmountAddress(header, region, s)));

        const uint64_t mass = totals.mass;
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
            const int64_t kept = FmMomentumAfterMassChange(state.momentum[axis], state.mass, mass);

            const int64_t lost = state.momentum[axis] - kept;
            if (lost != 0) {
                int64_t original;
                gasWork.InterlockedAdd64(PROBE_GAS_CONTROL_LOST + ProbeGasGridAxis(axis) * 8, lost, original);
            }

            gas.momentum[axis] = kept;
        }

        gas.energy += state.kinetic;
    }

    gasWork.Store<int64_t>(header.condensedOffset + region * 8, condensed);
    gasCells.Store<GasGpuCell>(GasCellAddress(region), gas);
}

// 1 スレッド = 区画の 1 セル: 書き戻した成分の数がインラインの数を超えるか
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void GasCheck(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeGasHeader header = GasHeader();
    const uint32_t region = dispatchThreadId.x;
    if (region >= header.regionCellCount || gasWork.Load(PROBE_GAS_CONTROL_TOO_MANY) != 0)
        return;

    const RxCell cell = cells[GenerationBase(CurrentTick()) + RegionGridCell(header, region)];
    uint32_t count = 0;
    [unroll] for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i < cell.speciesCount && !IsGasSpecies(header, cell.species[i]))
            ++count;
    }

    const uint32_t slotCount = SlotCount();
    for (uint32_t s = 0; s < slotCount; ++s) {
        if (gasCells.Load<uint64_t>(GasAmountAddress(header, region, s)) != 0)
            ++count;
    }

    if (count > RX_MAX_CELL_SPECIES)
        gasWork.InterlockedOr(PROBE_GAS_CONTROL_OVERFLOW, 1u);
}

// 欄 from から先で、物質量が 0 でない最初の欄(無ければ slotCount)
uint32_t NextGasSlot(ProbeGasHeader header, uint32_t region, uint32_t from, uint32_t slotCount) {
    uint32_t slot = from;
    while (slot < slotCount && gasCells.Load<uint64_t>(GasAmountAddress(header, region, slot)) == 0)
        ++slot;

    return slot;
}

// 1 スレッド = 区画の 1 セル: 気体の箱 → セル
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void GasScatter(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeGasHeader header = GasHeader();
    const uint32_t region = dispatchThreadId.x;
    if (region >= header.regionCellCount || Skipped())
        return;

    g_gasParameters = gasWork.Load<GasGpuParameters>(header.parametersOffset);
    g_gasSpeciesOffset = header.parametersOffset + GAS_GPU_PARAMETERS_BYTES;
    const uint32_t slotCount = SlotCount();
    const uint64_t tick = CurrentTick();
    const uint32_t grid = RegionGridCell(header, region);
    const uint32_t index = GenerationBase(tick) + grid;
    const RxCell before = cells[index];
    const GasGpuCell gas = gasCells.Load<GasGpuCell>(GasCellAddress(region));

    // --- 運動エネルギーのぶん(µJ → mJ。最も近い整数。負にならない)---
    GasGpuTotals totals = GasZeroTotals();
    for (uint32_t s = 0; s < slotCount; ++s)
        GasAddSpecies(totals, s, gasCells.Load<uint64_t>(GasAmountAddress(header, region, s)));

    const uint64_t mass = totals.mass;
    const int64_t kinetic = GfKineticMilliJoule(KineticEnergy(gas, mass));
    const int64_t condensed = gasWork.Load<int64_t>(header.condensedOffset + region * 8);

    // --- 凝縮相の成分 + 0 でない気体の成分を ID の順に(どちらも ID の順で、重ならない。気体は欄の順 = ID の順に進む)---
    uint32_t gasInBefore = 0;  // before の成分のうち気体の印
    [unroll] for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i < before.speciesCount && IsGasSpecies(header, before.species[i]))
            gasInBefore |= 1u << i;
    }

    RxCell after = RxMakeEmptyCell(condensed + gas.energy - kinetic);
    uint32_t last = 0;
    bool first = true;
    uint32_t nextGas = NextGasSlot(header, region, 0, slotCount);
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

        bool takesGas = false;
        if (nextGas < slotCount) {
            const uint32_t id = SlotSpecies(header, nextGas);
            if (id < bestId) {
                bestId = id;
                bestAmount = gasCells.Load<uint64_t>(GasAmountAddress(header, region, nextGas));
                takesGas = true;
            }
        }

        if (bestId != NO_SPECIES) {
            after.species[k] = bestId;
            after.amounts[k] = bestAmount;
            after.speciesCount = k + 1;
            last = bestId;
            first = false;
        }

        if (takesGas)
            nextGas = NextGasSlot(header, region, nextGas + 1, slotCount);
    }

    // --- 気体の欄 ---
    ProbeGasCellState state;
    state.momentum = gas.momentum;
    state.kinetic = kinetic;
    state.mass = mass;

    // 気体が残らなかったセルの運動量は帳簿へ(質量の無い運動量は持たない。CPU と同じ。T-0646)
    if (mass == 0) {
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
            if (gas.momentum[axis] != 0) {
                int64_t original;
                gasWork.InterlockedAdd64(PROBE_GAS_CONTROL_LOST + ProbeGasGridAxis(axis) * 8, gas.momentum[axis],
                                         original);
            }

            state.momentum[axis] = 0;
        }
    }

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
    for (uint32_t s = 0; s < slotCount; ++s) {
        AddLedger(header.ledgerOffset + SlotSpecies(header, s) * 8,
                  gasBoxLedger.Load<int64_t>(GAS_GPU_LEDGER_AMOUNTS + s * 8));
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

// --- 状態のハッシュ(T-0620。ハッシュの単位の HashCells の後。区画があるときだけ)---
// 区画のセルの気体の欄の寄与 + 帳簿の寄与(区画の番号 0 のスレッド)を、S(t + 1) の欄の hash へ足す(common/probe_region_hash.hlsli。
// CPU は ProbeGasField::HashTerm)。気体の欄は 1 組だけ持つので、刻み t の気体の段の後の値 = S(t + 1) の値
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void GasHash(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeGasHeader header = GasHeader();
    const uint32_t region = dispatchThreadId.x;
    uint64_t hash = 0;
    if (region < header.regionCellCount) {
        const ProbeGasCellState state = gasState[region];
        uint64_t term = ProbeRegionHashBegin(PROBE_GAS_CELL_HASH_SEED, region);
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis) term = ProbeRegionHashAdd(term, state.momentum[axis]);

        term = ProbeRegionHashAdd(term, state.kinetic);
        hash = ProbeRegionHashEnd(ProbeRegionHashAdd(term, (int64_t)state.mass));
    }

    // 帳簿: 物質 ID ごとの量 → エネルギー → 運動量 × 3(格子の軸)の順に並んだ int64
    if (region == 0) {
        uint64_t term = ProbeRegionHashBegin(PROBE_GAS_LEDGER_HASH_SEED, header.speciesCount);
        for (uint32_t word = 0; word < header.speciesCount + 4; ++word)
            term = ProbeRegionHashAdd(term, gasWork.Load<int64_t>(header.ledgerOffset + word * 8));

        hash += ProbeRegionHashEnd(term);
    }

    const uint64_t sum = WaveActiveSum(hash);
    if (WaveIsFirstLane()) {
        uint64_t original;
        hashes.InterlockedAdd64(HashEntryAddress(CurrentTick() + 1) + PROBE_HASH_OFFSET_HASH, sum, original);
    }
}
