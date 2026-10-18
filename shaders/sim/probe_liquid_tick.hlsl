// probe_liquid_tick.hlsl — 仮の世界の GPU の液体の段(T-0613・ADR-0266・ADR-0276・ADR-0268)。
// CPU リファレンスは engine/src/sim/probe_liquid.cpp の ProbeLiquidField::Step(ProbeReference::Advance の (1c))。
// 式(柱の静水圧・面の力・摩擦・移流・速さの上限・位置/運動のエネルギー)は common/liquid_flow.hlsli の 1 つの源を CPU と同じく呼ぶ(T-0716)。
// ここに残るのは段の分け方(面やセルごとに「集める」形)・作業場の置き場・成分の書き戻し・帳簿への足し方。
//
// 適用の単位の中(コマンド → 物理の熱 → 表の作り直し → 気体 → **液体** → 起こす刻みの来たブロックを一覧へ)で、engine/src/sim/probe_sim_liquid.cpp の
// RecordLiquid が次の順に投げる(段の間は全部の UAV の書き込みを待つ。CPU の 1 刻みの段を、面やセルごとに「集める」形に直した):
//   LiquidRead        刻みの初めのセルを写し、セルの値(質量・体積・空き・熱)を作る。区画の外の段で現れた・消えた液体の位置のエネルギーを帳簿へ
//   LiquidPressure    列ごとに上から圧力(1 スレッド = 1 列)
//   LiquidForces      段の外で液体の質量が変わった面の運動量を合わせ直し(T-0656)、圧力の差・重さ・壁・摩擦を足し、望む動く質量を
//                     決める(1 スレッド = 1 セルの + 側の面 ×3)
//   LiquidOutgoing    セルから出る望む質量の合計(自分が上流の面を集める)
//   LiquidMoveFaces   出る合計が中身を超えるセルは比で縮め、面を実際に動く質量を決める
//   LiquidApply       自分に関わる 6 つの面の流れを集めて成分・液体のぶんのエネルギーを当てる(上流のセルは刻みの初めの写しから読む)
//   LiquidLeaving     面のまわりの体積から出る 2 倍の流れの合計(移流。T-0610)
//   LiquidAdvect      面の運動量と運動のエネルギーを風上に運ぶ(出る分と入る分を面ごとに集める。移流なしなら写すだけ)
//   LiquidSettleFaces まわりが空の面・速さの上限・運動のエネルギーを計算し直し、熱になる分を面ごとに
//   LiquidSettleCells 面の熱と位置のエネルギーの差をセルの熱へ。変わったセルは熱のキャッシュを作り直してブロックを起こす(気体と同じ)
// 帳簿(外から入った運動量・位置のエネルギーの出入り)は整数の和なので、波の和 + 64bit の原子的な足し算で順番によらず CPU と同じになる。
// 区画の中の番号 = x + 幅 × (z + 奥行き × y)。整数だけ(D-205)。動的な添字で読むのはバッファ(作業場)だけで、ローカルの配列は決まった数の回しで読む(T-0397)。
#include "common/probe_liquid_gpu.hlsli"
#include "common/probe_region_hash.hlsli"
#include "sim/probe_bindings.hlsli"

RWByteAddressBuffer liquidWork : register(u19);  // 制御・帳簿・見出し・状態・作業の値(probe_liquid_gpu.hlsli)

// --- 定数(probe_liquid.cpp と同じ)---
static const uint64_t CELL_EDGE_MILLIMETERS = 500;
static const uint64_t CELL_VOLUME_PICOLITERS = (uint64_t)125000000 * 1000000;  // 0.5 m 角 = 1.25e14 pL
static const uint64_t VOLUME_CAP = (uint64_t)1 << 62;                          // 占める体積の頭打ち(sim/cell_volume)
static const uint64_t NANOGRAMS_PER_MILLIGRAM = 1000000;
static const uint32_t RX_CELL_BYTES = 112;  // RxCell(エネルギー 8・数 4・空き 4・物質 ID 4 × 8・物質量 8 × 8)
static const uint32_t NO_SPECIES = 0xFFFFFFFFu;

// 約束が崩れた種類(DebugFormat::ProbeLiquidInvariant の 2 つ目の引数)
static const uint32_t INVARIANT_NEGATIVE_HEAT = 1;
static const uint32_t INVARIANT_NEGATIVE_AMOUNT = 2;

// --- 整数の道具 ---

uint32_t Component(uint3 value, uint32_t axis) {
    return axis == 0 ? value.x : (axis == 1 ? value.y : value.z);
}

// 外から入った運動量・エネルギーを帳簿へ(波の和 → 波の先頭が 1 回。整数の和なので順番によらない)
void AddLedger(uint32_t address, int64_t value) {
    const int64_t sum = WaveActiveSum(value);
    if (WaveIsFirstLane() && sum != 0) {
        uint64_t before;
        liquidWork.InterlockedAdd64(address, (uint64_t)sum, before);
    }
}

// --- 見出し・区画 ---

ProbeLiquidHeader Header() {
    return liquidWork.Load<ProbeLiquidHeader>(PROBE_LIQUID_HEADER_OFFSET);
}

// 見立ての値(common/liquid_flow.hlsli。CPU は ProbeLiquidField::Create で同じ値を作る)
LqParams Params(ProbeLiquidHeader header) {
    return LqMakeParams(header.gravityMicrometersPerSecond2, CELL_EDGE_MILLIMETERS, header.frictionShift,
                        header.overfillStiffness, header.maxSpeedMillimetersPerSecond, header.advection);
}

uint32_t RegionSize(ProbeLiquidHeader header, uint32_t axis) {
    return axis == 0 ? header.regionSize[0] : (axis == 1 ? header.regionSize[1] : header.regionSize[2]);
}

// 区画の中の隣(格子の軸)
uint32_t Stride(ProbeLiquidHeader header, uint32_t axis) {
    return axis == 0 ? 1u : (axis == 1 ? header.regionSize[0] * header.regionSize[2] : header.regionSize[0]);
}

// 区画の中の番号 → 座標(格子の x・y・z)
uint3 RegionAt(ProbeLiquidHeader header, uint32_t region) {
    const uint32_t width = header.regionSize[0];
    const uint32_t depth = header.regionSize[2];

    return uint3(region % width, region / (width * depth), (region / width) % depth);
}

uint32_t RegionIndex(ProbeLiquidHeader header, uint32_t x, uint32_t y, uint32_t z) {
    return x + (header.regionSize[0] * (z + (header.regionSize[2] * y)));
}

// 区画の中の番号 → 今の世代のセルの番号
uint32_t CellIndex(ProbeLiquidHeader header, uint32_t region) {
    const uint3 at = RegionAt(header, region);
    const uint32_t grid = ProbeCellIndex(header.regionMin[0] + at.x, header.regionMin[1] + at.y,
                                         header.regionMin[2] + at.z);

    return GenerationBase(CurrentTick()) + grid;
}

// 面の欄(軸ごとに区画のセルの数だけ)の場所
uint32_t FaceAddress(ProbeLiquidHeader header, uint32_t offset, uint32_t axis, uint32_t region) {
    return offset + ((axis * header.regionCellCount) + region) * 8;
}

uint32_t CellAddress(uint32_t offset, uint32_t region) {
    return offset + region * 8;
}

int64_t LoadSigned(uint32_t address) {
    return liquidWork.Load<int64_t>(address);
}

uint64_t LoadUnsigned(uint32_t address) {
    return liquidWork.Load<uint64_t>(address);
}

// --- 物質 ---

ProbeLiquidSpecies LiquidSpecies(ProbeLiquidHeader header, uint32_t id) {
    return liquidWork.Load<ProbeLiquidSpecies>(header.speciesOffset + id * PROBE_LIQUID_SPECIES_BYTES);
}

bool IsLiquid(ProbeLiquidHeader header, uint32_t id) {
    return id < header.speciesCount && (LiquidSpecies(header, id).flags & PROBE_LIQUID_SPECIES_LIQUID) != 0;
}

// --- セルの値(probe_liquid.cpp の ReadState・sim/cell_volume の ShareCellVolume)---

ProbeLiquidCellState LoadState(ProbeLiquidHeader header, uint32_t region) {
    return liquidWork.Load<ProbeLiquidCellState>(header.stateOffset + region * PROBE_LIQUID_CELL_STATE_BYTES);
}

uint32_t LoadOpen(ProbeLiquidHeader header, uint32_t region) {
    return LoadState(header, region).open;
}

ProbeLiquidCellState ReadState(ProbeLiquidHeader header, uint32_t region, RxCell cell) {
    int64_t chemical = 0;
    uint64_t capacity = 0;
    uint64_t massNanograms = 0;
    uint64_t volume = 0;
    uint64_t solid = 0;
    [unroll] for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i < cell.speciesCount) {
            const uint32_t id = cell.species[i];
            const uint64_t amount = cell.amounts[i];
            const RxSpecies species = reactionSpecies[id];
            chemical += (int64_t)amount * species.h0;
            capacity += amount * (uint64_t)species.heatCapacity;
            if (id < header.speciesCount) {
                const ProbeLiquidSpecies liquid = LiquidSpecies(header, id);
                const bool isLiquid = (liquid.flags & PROBE_LIQUID_SPECIES_LIQUID) != 0;
                if (isLiquid) {
                    massNanograms += amount * (uint64_t)liquid.molarMass;
                    volume += amount * liquid.molarVolume;
                }

                // 固体が占める体積(液体の体積は空きに数えない。µmol × mm³/mol。溢れる分は頭打ち)
                if ((liquid.flags & PROBE_LIQUID_SPECIES_CONDENSED) != 0 && !isLiquid) {
                    const FxU128 product = FxMulU64Full(amount, liquid.molarVolume);
                    const uint64_t occupied = product.hi != 0 ? VOLUME_CAP : min(product.lo, VOLUME_CAP);
                    solid = min(solid + occupied, VOLUME_CAP);
                }
            }
        }
    }

    const int64_t heat = (cell.energy * 1000) - chemical;
    DEBUG_ASSERT(heat >= 0, DebugFormat::ProbeLiquidInvariant, region, INVARIANT_NEGATIVE_HEAT);

    return LqMakeCellView(massNanograms / NANOGRAMS_PER_MILLIGRAM, volume, solid, CELL_VOLUME_PICOLITERS, heat,
                          capacity);
}

// 区画のセル region の + 側の面(格子の軸 axis)の開き(Q16。両側の空きの小さい方。区画の外は 0)
uint32_t FaceOpenAt(ProbeLiquidHeader header, uint32_t region, uint32_t axis) {
    if (Component(RegionAt(header, region), axis) + 1 >= RegionSize(header, axis))
        return 0;

    return LqFaceOpen(LoadOpen(header, region), LoadOpen(header, region + Stride(header, axis)));
}

// --- 位置のエネルギー(mJ。セルのエネルギーの外)---

int64_t PotentialOf(ProbeLiquidHeader header, uint32_t region, uint64_t massMilligrams) {
    const LqParams params = Params(header);

    return LqPotentialEnergy(massMilligrams, LqCellHeightMillimeters(RegionAt(header, region).y, params), params);
}

// --- 刻みの初めのセルの写し(上流のセルの成分を、動的な添字でバッファから読むため)---

uint32_t SnapshotAddress(ProbeLiquidHeader header, uint32_t region) {
    return header.snapshotOffset + region * RX_CELL_BYTES;
}

void StoreSnapshot(uint32_t address, RxCell cell) {
    liquidWork.Store<int64_t>(address, cell.energy);
    liquidWork.Store(address + 8, cell.speciesCount);
    liquidWork.Store(address + 12, 0u);
    [unroll] for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        liquidWork.Store(address + 16 + i * 4, cell.species[i]);
        liquidWork.Store<uint64_t>(address + 48 + i * 8, cell.amounts[i]);
    }
}

// === 1. 刻みの初めの値 ===
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void LiquidRead(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeLiquidHeader header = Header();
    const uint32_t region = dispatchThreadId.x;
    int64_t entered = 0;
    if (region < header.regionCellCount) {
        const RxCell cell = cells[CellIndex(header, region)];
        StoreSnapshot(SnapshotAddress(header, region), cell);
        const ProbeLiquidCellState state = ReadState(header, region, cell);
        liquidWork.Store<ProbeLiquidCellState>(header.stateOffset + region * PROBE_LIQUID_CELL_STATE_BYTES, state);

        // 区画の外の段(反応・置いた水)で液体が現れた・消えた分の位置のエネルギーを帳簿へ
        const uint32_t address = CellAddress(header.potentialOffset, region);
        const int64_t potential = PotentialOf(header, region, state.mass);
        entered = potential - LoadSigned(address);
        liquidWork.Store<int64_t>(address, potential);
    }

    if (region == 0)
        liquidWork.Store(PROBE_LIQUID_CONTROL_CHANGED, 0u);

    AddLedger(PROBE_LIQUID_LEDGER_ENERGY, entered);
}

    // === 2. 圧力(質量の単位。P = 2 × 支えている上の液体 + 自分 + 溢れ)。1 スレッド = 1 列、上から ===
    [numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void LiquidPressure(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeLiquidHeader header = Header();
    const uint32_t column = dispatchThreadId.x;
    const uint32_t width = header.regionSize[0];
    if (column >= width * header.regionSize[2])
        return;

    const uint32_t x = column % width;
    const uint32_t z = column / width;
    const uint32_t height = header.regionSize[1];
    const LqParams params = Params(header);
    uint64_t above = 0;
    for (uint32_t k = 0; k < height; ++k) {
        const uint32_t y = height - 1 - k;
        const uint32_t region = RegionIndex(header, x, y, z);
        const ProbeLiquidCellState state = LoadState(header, region);
        ProbeLiquidCellState below = state;  // 床(y = 0)では読まない
        if (y != 0)
            below = LoadState(header, RegionIndex(header, x, y - 1, z));

        const LqColumnStep cellPressure = LqColumnPressure(state, above, y != 0 ? 1u : 0u, below, params);
        above = cellPressure.above;
        liquidWork.Store<int64_t>(CellAddress(header.pressureOffset, region), cellPressure.pressure);
    }
}

// === 3. 面の力(圧力の差・重さ・壁・摩擦)と望む動く質量。1 スレッド = 1 セルの + 側の面 ×3 ===
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void LiquidForces(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeLiquidHeader header = Header();
    const uint32_t region = dispatchThreadId.x;
    int64_t3 external = int64_t3(0, 0, 0);  // 壁・重さ・摩擦・行き場なし(格子の軸)
    if (region < header.regionCellCount) {
        const uint3 at = RegionAt(header, region);
        const ProbeLiquidCellState a = LoadState(header, region);
        const int64_t pressure = LoadSigned(CellAddress(header.pressureOffset, region));
        const LqParams params = Params(header);
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
            const uint32_t step = Stride(header, axis);
            int64_t face = LoadSigned(FaceAddress(header, header.facesOffset, axis, region));
            int64_t vanished = 0;

            // 区画の外の段で液体の質量が変わった面の運動量を合わせ直す(CPU の FollowMassChange。T-0656・ADR-0286)。
            // massOffset は前の刻みの終わりの質量(この段では誰も書かない。LiquidApply が書き直す)
            if (face != 0 && Component(at, axis) + 1 < RegionSize(header, axis)) {
                const uint32_t other = region + step;
                const uint64_t before = LoadUnsigned(CellAddress(header.massOffset, region)) +
                                        LoadUnsigned(CellAddress(header.massOffset, other));
                const uint64_t now = a.mass + LoadState(header, other).mass;
                const int64_t kept = FmMomentumAfterMassChange(face, before, now);
                vanished -= face - kept;
                face = kept;
            }

            // − 側が区画の外か閉じた面なら、その面の押し返しは壁から(+ 向き)
            const bool minusClosed = Component(at, axis) == 0 ||
                                     LqFaceOpen(LoadOpen(header, region - step), a.open) == 0;
            const int64_t minusWall = LqMinusWallPush(a, pressure, minusClosed ? 1u : 0u);

            // + 側の面(閉じた面・開いた面。式は common/liquid_flow.hlsli)
            const uint32_t faceOpen = FaceOpenAt(header, region, axis);
            LqFaceForce force = LqClosedFace(face, a, pressure);
            if (faceOpen != 0) {
                const uint32_t neighbor = region + step;
                force = LqOpenFaceForce(face, faceOpen, axis == 1 ? 1u : 0u, a, pressure, LoadState(header, neighbor),
                                        LoadSigned(CellAddress(header.pressureOffset, neighbor)), params);
            }

            liquidWork.Store<int64_t>(FaceAddress(header, header.forcedOffset, axis, region), force.face);
            liquidWork.Store<uint64_t>(FaceAddress(header, header.desiredOffset, axis, region), force.desired);
            external[axis] = minusWall + force.wall + force.gravity + force.friction + force.dropped + vanished;
        }
    }

    [unroll] for (uint32_t axis = 0; axis < 3; ++axis)
        AddLedger(PROBE_LIQUID_LEDGER_EXTERNAL + axis * 8, external[axis]);
}

    // === 4. セルから出る望む質量の合計(自分が上流の面を集める)===
    [numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void LiquidOutgoing(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeLiquidHeader header = Header();
    const uint32_t region = dispatchThreadId.x;
    if (region >= header.regionCellCount)
        return;

    const uint3 at = RegionAt(header, region);
    uint64_t outgoing = 0;
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        if (LoadSigned(FaceAddress(header, header.forcedOffset, axis, region)) > 0)
            outgoing += LoadUnsigned(FaceAddress(header, header.desiredOffset, axis, region));

        if (Component(at, axis) != 0) {
            const uint32_t minus = region - Stride(header, axis);
            if (LoadSigned(FaceAddress(header, header.forcedOffset, axis, minus)) < 0)
                outgoing += LoadUnsigned(FaceAddress(header, header.desiredOffset, axis, minus));
        }
    }

    liquidWork.Store<uint64_t>(CellAddress(header.outgoingOffset, region), outgoing);
}

// === 5. 面を実際に動く質量(出る合計が中身を超えるセルは比で縮める)===
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void LiquidMoveFaces(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeLiquidHeader header = Header();
    const uint32_t region = dispatchThreadId.x;
    if (region >= header.regionCellCount)
        return;

    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        const uint64_t flux = LoadUnsigned(FaceAddress(header, header.desiredOffset, axis, region));
        int64_t moved = 0;
        if (flux != 0) {
            const bool forward = LoadSigned(FaceAddress(header, header.forcedOffset, axis, region)) > 0;
            const uint32_t from = forward ? region : region + Stride(header, axis);
            const uint64_t fromMass = LoadState(header, from).mass;
            const uint64_t outgoing = LoadUnsigned(CellAddress(header.outgoingOffset, from));
            const uint64_t mass = LqLimitOutflow(flux, fromMass, outgoing);
            moved = forward ? (int64_t)mass : -(int64_t)mass;
        }

        liquidWork.Store<int64_t>(FaceAddress(header, header.movedOffset, axis, region), moved);
    }
}

// === 6. 成分と液体のぶんのエネルギーを当てる ===

// 成分の増減の作業場(区画のセルごとに PROBE_LIQUID_LIST_ENTRIES 件)
uint32_t ListAddress(ProbeLiquidHeader header, uint32_t region, uint32_t entry) {
    return header.listOffset + ((region * PROBE_LIQUID_LIST_ENTRIES) + entry) * PROBE_LIQUID_LIST_ENTRY_BYTES;
}

// 物質 species の件に change を足す(無ければ末尾に足す。件数を超えたら false)
bool AddToList(ProbeLiquidHeader header, uint32_t region, inout uint32_t count, uint32_t species, int64_t change) {
    for (uint32_t entry = 0; entry < count; ++entry) {
        const uint32_t address = ListAddress(header, region, entry);
        if (liquidWork.Load(address) == species) {
            liquidWork.Store<int64_t>(address + 8, LoadSigned(address + 8) + change);
            return true;
        }
    }

    if (count >= PROBE_LIQUID_LIST_ENTRIES)
        return false;

    const uint32_t address = ListAddress(header, region, count);
    liquidWork.Store(address, species);
    liquidWork.Store(address + 4, 0u);
    liquidWork.Store<int64_t>(address + 8, change);
    ++count;

    return true;
}

// 上流 from のセル(刻みの初めの写し)から質量 mass(mg)の液体を動かした時の、自分の成分の増減(sign = −1 なら自分が上流)と
// 液体のぶんのエネルギー(probe_liquid.cpp の MoveLiquid)。成分を 1 つでも動かしたら touched
void MoveLiquid(ProbeLiquidHeader header, uint32_t self, uint32_t from, uint64_t mass, bool selfIsFrom,
                inout uint32_t count, inout int64_t energy, inout bool touched, inout bool overflow) {
    const ProbeLiquidCellState state = LoadState(header, from);
    const uint32_t snapshot = SnapshotAddress(header, from);
    const uint32_t speciesCount = liquidWork.Load(snapshot + 8);
    uint64_t movedCapacity = 0;
    int64_t movedChemical = 0;
    for (uint32_t i = 0; i < speciesCount; ++i) {
        const uint32_t species = liquidWork.Load(snapshot + 16 + i * 4);
        if (!IsLiquid(header, species))
            continue;

        const uint64_t moved = LqMovedShare(LoadUnsigned(snapshot + 48 + i * 8), mass, state.mass);
        if (moved == 0)
            continue;

        touched = true;
        if (!AddToList(header, self, count, species, selfIsFrom ? -(int64_t)moved : (int64_t)moved))
            overflow = true;

        const RxSpecies table = reactionSpecies[species];
        movedCapacity += moved * (uint64_t)table.heatCapacity;
        movedChemical += (int64_t)moved * table.h0;
    }

    if (movedCapacity == 0)
        return;

    const int64_t movedEnergy = LqMovedEnergy(state, movedCapacity, movedChemical);
    energy += selfIsFrom ? -movedEnergy : movedEnergy;
}

[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void LiquidApply(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeLiquidHeader header = Header();
    const uint32_t region = dispatchThreadId.x;
    if (region >= header.regionCellCount)
        return;

    const uint3 at = RegionAt(header, region);
    const uint32_t index = CellIndex(header, region);
    const RxCell before = cells[index];

    // --- 作業場に自分の成分を写す ---
    uint32_t count = 0;
    [unroll] for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i < before.speciesCount) {
            const uint32_t address = ListAddress(header, region, i);
            liquidWork.Store(address, before.species[i]);
            liquidWork.Store(address + 4, 0u);
            liquidWork.Store<uint64_t>(address + 8, before.amounts[i]);
            count = i + 1;
        }
    }

    // --- 自分に関わる 6 つの面の流れ(+ 側の面と − 側の面)---
    int64_t energy = 0;
    int64_t potentialCarried = LoadSigned(CellAddress(header.potentialOffset, region));
    bool touched = false;
    bool overflow = false;
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        const uint32_t step = Stride(header, axis);
        [unroll] for (uint32_t side = 0; side < 2; ++side) {
            const bool plus = side == 0;
            const bool exists = plus ? Component(at, axis) + 1 < RegionSize(header, axis) : Component(at, axis) != 0;
            if (!exists)
                continue;

            const uint32_t faceRegion = plus ? region : region - step;
            const int64_t moved = LoadSigned(FaceAddress(header, header.movedOffset, axis, faceRegion));
            if (moved == 0)
                continue;

            // + 側の面で正 / − 側の面で負なら自分が上流
            const bool selfIsFrom = plus ? moved > 0 : moved < 0;
            const uint32_t other = plus ? region + step : region - step;
            const uint32_t from = selfIsFrom ? region : other;
            const uint64_t mass = FxAbsU64(moved);
            MoveLiquid(header, region, from, mass, selfIsFrom, count, energy, touched, overflow);

            const uint64_t fromPotential = (uint64_t)LoadSigned(CellAddress(header.potentialOffset, from));
            const int64_t potential = (int64_t)LqMovedShare(fromPotential, mass, LoadState(header, from).mass);
            potentialCarried += selfIsFrom ? -potential : potential;
        }
    }

    // --- 0 でない成分を ID の順に(作業場は動的な添字のバッファ。ローカルの配列へは決まった数の回しで書く)---
    RxCell after = before;
    uint32_t nonzero = 0;
    for (uint32_t entry = 0; entry < count; ++entry) {
        const int64_t amount = LoadSigned(ListAddress(header, region, entry) + 8);
        DEBUG_ASSERT(amount >= 0, DebugFormat::ProbeLiquidInvariant, region, INVARIANT_NEGATIVE_AMOUNT);
        if (amount != 0)
            ++nonzero;
    }

    overflow = overflow || nonzero > RX_MAX_CELL_SPECIES;
    DEBUG_ASSERT(!overflow, DebugFormat::ProbeLiquidCellOverflow, region, nonzero);
    if (touched && !overflow) {
        after = RxMakeEmptyCell(before.energy + energy);
        uint32_t last = 0;
        bool first = true;
        [unroll] for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
            uint32_t bestId = NO_SPECIES;
            int64_t bestAmount = 0;
            for (uint32_t entry = 0; entry < count; ++entry) {
                const uint32_t address = ListAddress(header, region, entry);
                const uint32_t species = liquidWork.Load(address);
                const int64_t amount = LoadSigned(address + 8);
                if (amount != 0 && (first || species > last) && species < bestId) {
                    bestId = species;
                    bestAmount = amount;
                }
            }

            if (bestId != NO_SPECIES) {
                after.species[k] = bestId;
                after.amounts[k] = (uint64_t)bestAmount;
                after.speciesCount = k + 1;
                last = bestId;
                first = false;
            }
        }
    }

    if (overflow) {
        uint32_t previous;
        liquidWork.InterlockedAdd(PROBE_LIQUID_CONTROL_OVERFLOW, 1, previous);
    }

    // --- 動かしたセルは書き直す(CPU の ApplyDelta と同じ)。値が変わったかと、動かした後の液体の質量(mg)---
    bool same = after.energy == before.energy && after.speciesCount == before.speciesCount;
    uint64_t massNanograms = 0;
    [unroll] for (uint32_t j = 0; j < RX_MAX_CELL_SPECIES; ++j) {
        if (j < after.speciesCount) {
            same = same && after.species[j] == before.species[j] && after.amounts[j] == before.amounts[j];
            if (IsLiquid(header, after.species[j]))
                massNanograms += after.amounts[j] * (uint64_t)LiquidSpecies(header, after.species[j]).molarMass;
        }
    }

    if (touched && !overflow)
        cells[index] = after;

    liquidWork.Store<uint64_t>(CellAddress(header.massOffset, region), massNanograms / NANOGRAMS_PER_MILLIGRAM);
    liquidWork.Store<int64_t>(CellAddress(header.potentialCarriedOffset, region), potentialCarried);
    liquidWork.Store(header.changedOffset + region * 4, same ? 0u : 1u);
}

// === 7・8. 運動量の移流(T-0610・ADR-0276。面のまわりの体積の境を通る 2 倍の流れで風上に運ぶ)===

// 面 (axis, region) を下側に持つ across 向きの境の 2 倍の流れ(境が無ければ 0)
int64_t LowerFlow(ProbeLiquidHeader header, uint3 at, uint32_t region, uint32_t axis, uint32_t across) {
    if (Component(at, axis) + 1 >= RegionSize(header, axis) || Component(at, across) + 1 >= RegionSize(header, across))
        return 0;

    return LoadSigned(FaceAddress(header, header.movedOffset, across, region)) +
           LoadSigned(FaceAddress(header, header.movedOffset, across, region + Stride(header, axis)));
}

// 面 (axis, region) を上側に持つ across 向きの境の 2 倍の流れ(下側の面は region − across の隣。境が無ければ 0)
int64_t UpperFlow(ProbeLiquidHeader header, uint3 at, uint32_t region, uint32_t axis, uint32_t across) {
    if (Component(at, across) == 0)
        return 0;

    if (across != axis && Component(at, axis) + 1 >= RegionSize(header, axis))
        return 0;

    const uint32_t lower = region - Stride(header, across);

    return LoadSigned(FaceAddress(header, header.movedOffset, across, lower)) +
           LoadSigned(FaceAddress(header, header.movedOffset, across, lower + Stride(header, axis)));
}

[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void LiquidLeaving(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeLiquidHeader header = Header();
    const uint32_t region = dispatchThreadId.x;
    if (region >= header.regionCellCount)
        return;

    const uint3 at = RegionAt(header, region);
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        uint64_t leaving = 0;
        [unroll] for (uint32_t across = 0; across < 3; ++across) {
            const int64_t lower = LowerFlow(header, at, region, axis, across);
            const int64_t upper = UpperFlow(header, at, region, axis, across);
            if (lower > 0)
                leaving += (uint64_t)lower;

            if (upper < 0)
                leaving += FxAbsU64(upper);
        }

        liquidWork.Store<uint64_t>(FaceAddress(header, header.leavingOffset, axis, region), leaving);
    }
}

// 面 from から境を通って運ぶ運動量と運動のエネルギー(share = 境の 2 倍の流れの大きさ)
void CarriedFrom(ProbeLiquidHeader header, uint32_t axis, uint32_t from, uint64_t share, out int64_t momentum,
                 out int64_t energy) {
    const int64_t before = LoadSigned(FaceAddress(header, header.forcedOffset, axis, from));
    const uint64_t doubledMass = LoadState(header, from).mass + LoadState(header, from + Stride(header, axis)).mass;
    const LqCarried carried = LqAdvectShare(before, LoadSigned(FaceAddress(header, header.kineticOffset, axis, from)),
                                            share, doubledMass,
                                            LoadUnsigned(FaceAddress(header, header.leavingOffset, axis, from)));
    momentum = carried.momentum;
    energy = carried.energy;
}

[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void LiquidAdvect(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeLiquidHeader header = Header();
    const uint32_t region = dispatchThreadId.x;
    int64_t3 walls = int64_t3(0, 0, 0);
    if (region < header.regionCellCount) {
        const uint3 at = RegionAt(header, region);
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
            int64_t face = LoadSigned(FaceAddress(header, header.forcedOffset, axis, region));
            int64_t carried = LoadSigned(FaceAddress(header, header.kineticOffset, axis, region));
            int64_t wall = 0;
            if (header.advection != 0) {
                const bool open = FaceOpenAt(header, region, axis) != 0;
                [unroll] for (uint32_t across = 0; across < 3; ++across) {
                    const uint32_t step = Stride(header, across);
                    [unroll] for (uint32_t side = 0; side < 2; ++side) {
                        // side 0: 自分が下側の境(相手 = region + across の隣)・side 1: 自分が上側の境(相手 = region − across の隣)
                        const bool lowerSide = side == 0;
                        const int64_t flow = lowerSide ? LowerFlow(header, at, region, axis, across)
                                                       : UpperFlow(header, at, region, axis, across);
                        if (flow == 0)
                            continue;

                        const uint32_t other = lowerSide ? region + step : region - step;
                        const bool selfIsFrom = lowerSide ? flow > 0 : flow < 0;
                        const uint32_t from = selfIsFrom ? region : other;
                        int64_t momentum;
                        int64_t energy;
                        CarriedFrom(header, axis, from, FxAbsU64(flow), momentum, energy);
                        if (selfIsFrom) {
                            face -= momentum;
                            carried -= energy;
                            if (FaceOpenAt(header, other, axis) == 0)
                                wall -= momentum;  // 閉じた面(壁・区画の外)へ入る運動量は壁が受ける
                        } else {
                            carried += energy;  // 閉じた面へ入った分は熱に(LiquidSettleFaces)
                            if (open)
                                face += momentum;
                        }
                    }
                }
            }

            liquidWork.Store<int64_t>(FaceAddress(header, header.facesOffset, axis, region), face);
            liquidWork.Store<int64_t>(FaceAddress(header, header.carriedOffset, axis, region), carried);
            walls[axis] = wall;
        }
    }

    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) AddLedger(PROBE_LIQUID_LEDGER_EXTERNAL + axis * 8, walls[axis]);
}

    // === 9. 面: まわりが空になった面・速さの上限・運動のエネルギー(差は熱へ)===
    [numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void LiquidSettleFaces(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeLiquidHeader header = Header();
    const uint32_t region = dispatchThreadId.x;
    int64_t3 external = int64_t3(0, 0, 0);  // 行き場なし・速さの上限
    if (region < header.regionCellCount) {
        const uint3 at = RegionAt(header, region);
        const LqParams params = Params(header);
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
            const uint32_t faceAddress = FaceAddress(header, header.facesOffset, axis, region);
            int64_t face = LoadSigned(faceAddress);
            const bool inside = Component(at, axis) + 1 < RegionSize(header, axis);
            const uint64_t doubled = inside ? LoadUnsigned(CellAddress(header.massOffset, region)) +
                                                  LoadUnsigned(
                                                      CellAddress(header.massOffset, region + Stride(header, axis)))
                                            : 0;
            const LqSettledFace settled = LqSettleFace(
                face, doubled, LoadSigned(FaceAddress(header, header.carriedOffset, axis, region)), params);
            liquidWork.Store<int64_t>(faceAddress, settled.face);
            liquidWork.Store<int64_t>(FaceAddress(header, header.kineticOffset, axis, region), settled.kinetic);
            liquidWork.Store<int64_t>(FaceAddress(header, header.releasedOffset, axis, region), settled.released);
            external[axis] = settled.dropped + settled.capped;
        }
    }

    [unroll] for (uint32_t axis = 0; axis < 3; ++axis)
        AddLedger(PROBE_LIQUID_LEDGER_EXTERNAL + axis * 8, external[axis]);
}

// === 10. セル: 面の熱(内側の面は両側へ半分ずつ)と位置のエネルギーの差を熱へ。変わったセルを起こす ===
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void LiquidSettleCells(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeLiquidHeader header = Header();
    const uint32_t region = dispatchThreadId.x;
    if (region >= header.regionCellCount)
        return;

    if (region == 0)
        liquidWork.Store(PROBE_LIQUID_CONTROL_STEPPED, liquidWork.Load(PROBE_LIQUID_CONTROL_STEPPED) + 1);

    const uint3 at = RegionAt(header, region);
    int64_t heat = 0;
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        const int64_t released = LoadSigned(FaceAddress(header, header.releasedOffset, axis, region));
        if (Component(at, axis) + 1 < RegionSize(header, axis))
            heat += released - LqReleasedShare(released);
        else
            heat += released;

        if (Component(at, axis) != 0) {
            const uint32_t minus = region - Stride(header, axis);
            heat += LqReleasedShare(LoadSigned(FaceAddress(header, header.releasedOffset, axis, minus)));
        }
    }

    const uint32_t potentialAddress = CellAddress(header.potentialOffset, region);
    const int64_t potential = PotentialOf(header, region, LoadUnsigned(CellAddress(header.massOffset, region)));
    heat += LoadSigned(CellAddress(header.potentialCarriedOffset, region)) - potential;
    liquidWork.Store<int64_t>(potentialAddress, potential);

    const bool changed = liquidWork.Load(header.changedOffset + region * 4) != 0 || heat != 0;
    if (!changed)
        return;

    const uint32_t index = CellIndex(header, region);
    RxCell cell = cells[index];
    if (heat != 0) {
        cell.energy += heat;
        cells[index] = cell;
    }

    thermal[index] = ProbeMakeCache(ReactionTable(), cell);

    const uint64_t tick = CurrentTick();
    const uint32_t grid = index - GenerationBase(tick);
    const uint32_t block = ProbeBlockOfCell(grid % PROBE_GRID_SIZE, (grid / PROBE_GRID_SIZE) % PROBE_GRID_SIZE,
                                            grid / PROBE_SLICE_CELL_COUNT);
    StoreBlockMark(PROBE_SCHEDULE_CHANGED_WORD, block, ProbeChangeMark(tick) - 1);
    StoreBlockMark(PROBE_SCHEDULE_WAKE_WORD, block, ProbeChangeMark(tick));

    uint32_t previous;
    liquidWork.InterlockedAdd(PROBE_LIQUID_CONTROL_CHANGED, 1, previous);
}

    // --- 状態のハッシュ(T-0620。ハッシュの単位の HashCells の後。区画があるときだけ)---
    // 区画のセルの持ち越す欄(面の運動量 × 3・面の運動のエネルギー × 3・位置のエネルギー・液体の質量)の寄与 + 帳簿の寄与(区画の番号 0 のスレッド)を、
    // S(t + 1) の欄の hash へ足す(common/probe_region_hash.hlsli。CPU は ProbeLiquidField::HashTerm)
    [numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void LiquidHash(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const ProbeLiquidHeader header = Header();
    const uint32_t region = dispatchThreadId.x;
    uint64_t hash = 0;
    if (region < header.regionCellCount) {
        uint64_t term = ProbeRegionHashBegin(PROBE_LIQUID_CELL_HASH_SEED, region);
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis)
            term = ProbeRegionHashAdd(term, LoadSigned(FaceAddress(header, header.facesOffset, axis, region)));

        [unroll] for (uint32_t kineticAxis = 0; kineticAxis < 3; ++kineticAxis)
            term = ProbeRegionHashAdd(term, LoadSigned(FaceAddress(header, header.kineticOffset, kineticAxis, region)));

        term = ProbeRegionHashAdd(term, LoadSigned(CellAddress(header.potentialOffset, region)));
        hash = ProbeRegionHashEnd(ProbeRegionHashAdd(term, LoadSigned(CellAddress(header.massOffset, region))));
    }

    // 帳簿: 外から入った運動量 × 3 → 位置のエネルギーの出入り
    if (region == 0) {
        uint64_t term = ProbeRegionHashBegin(PROBE_LIQUID_LEDGER_HASH_SEED, 0);
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis)
            term = ProbeRegionHashAdd(term, LoadSigned(PROBE_LIQUID_LEDGER_EXTERNAL + axis * 8));

        hash += ProbeRegionHashEnd(ProbeRegionHashAdd(term, LoadSigned(PROBE_LIQUID_LEDGER_ENERGY)));
    }

    const uint64_t sum = WaveActiveSum(hash);
    if (WaveIsFirstLane()) {
        uint64_t original;
        hashes.InterlockedAdd64(HashEntryAddress(CurrentTick() + 1) + PROBE_HASH_OFFSET_HASH, sum, original);
    }
}
