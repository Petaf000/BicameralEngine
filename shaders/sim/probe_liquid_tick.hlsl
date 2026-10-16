// probe_liquid_tick.hlsl — 仮の世界の GPU の液体の段(T-0613・ADR-0266・ADR-0276・ADR-0268)。
// CPU リファレンスは engine/src/sim/probe_liquid.cpp の ProbeLiquidField::Step(ProbeReference::Advance の (1c))。式はそこと 1 行ずつ同じ。
//
// 適用の単位の中(コマンド → 物理の熱 → 表の作り直し → 気体 → **液体** → 起こす刻みの来たブロックを一覧へ)で、engine/src/sim/probe_sim_liquid.cpp の
// RecordLiquid が次の順に投げる(段の間は全部の UAV の書き込みを待つ。CPU の 1 刻みの段を、面やセルごとに「集める」形に直した):
//   LiquidRead        刻みの初めのセルを写し、セルの値(質量・体積・空き・熱)を作る。区画の外の段で現れた・消えた液体の位置のエネルギーを帳簿へ
//   LiquidPressure    列ごとに上から圧力(1 スレッド = 1 列)
//   LiquidForces      面の運動量に圧力の差・重さ・壁・摩擦を足し、望む動く質量を決める(1 スレッド = 1 セルの + 側の面 ×3)
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
#include "sim/probe_bindings.hlsli"

RWByteAddressBuffer liquidWork : register(u19);  // 制御・帳簿・見出し・状態・作業の値(probe_liquid_gpu.hlsli)

// --- 定数(probe_liquid.cpp と同じ)---
static const uint64_t CELL_EDGE_MILLIMETERS = 500;
static const uint64_t CELL_VOLUME_PICOLITERS = (uint64_t)125000000 * 1000000;  // 0.5 m 角 = 1.25e14 pL
static const uint32_t OPEN_FULL = 65536;               // 割合の 1(Q16。sim/cell_volume の CELL_OPEN_FULL)
static const uint64_t VOLUME_CAP = (uint64_t)1 << 62;  // 占める体積の頭打ち(sim/cell_volume)
static const uint64_t FLUX_DENOMINATOR = (uint64_t)2 * 60 * 60 * 500 * 1000;         // 2 × 60² × 1 辺(µm)
static const uint64_t KINETIC_DENOMINATOR = (uint64_t)144 * 100000000 * 1000000000;  // 14400 × 1e15
static const uint64_t POTENTIAL_DENOMINATOR = (uint64_t)1000000 * 1000000;           // 1e12
static const uint64_t NANOGRAMS_PER_MILLIGRAM = 1000000;
static const uint32_t RX_CELL_BYTES = 112;  // RxCell(エネルギー 8・数 4・空き 4・物質 ID 4 × 8・物質量 8 × 8)
static const uint32_t NO_SPECIES = 0xFFFFFFFFu;

// 約束が崩れた種類(DebugFormat::ProbeLiquidInvariant の 2 つ目の引数)
static const uint32_t INVARIANT_NEGATIVE_HEAT = 1;
static const uint32_t INVARIANT_NEGATIVE_AMOUNT = 2;
static const uint32_t INVARIANT_KINETIC_RANGE = 3;

// --- 整数の道具 ---

// a × b ÷ c(切り捨て。結果が 64bit に収まること)
uint64_t MulDiv(uint64_t a, uint64_t b, uint64_t c) {
    return FxDivU128By64(FxMulU64Full(a, b), c).quotient;
}

// value × numerator ÷ denominator(0 の向きへ切り捨て)
int64_t ScaleSigned(int64_t value, uint64_t numerator, uint64_t denominator) {
    return FxApplySign(MulDiv(FxAbsU64(value), numerator, denominator), value < 0);
}

// 床で割る(負の数も下へ。divisor は正)
int64_t DivideDown(int64_t value, uint64_t divisor) {
    const uint64_t magnitude = FxAbsU64(value);
    const uint64_t quotient = magnitude / divisor;
    if (value >= 0)
        return (int64_t)quotient;

    return -(int64_t)(quotient + (magnitude % divisor != 0 ? 1 : 0));
}

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

// セルの体積から占める分を引いた残り(pL。0 で止める)。64bit の「0 で止める引き算」(a > b ? a − b : 0 の形)は NVIDIA のドライバが
// 誤って下ろすので(T-0185)、符号付きの差を出して 0 以下を 0 にする。occupied は VOLUME_CAP(2^62)まで
uint64_t RoomLeft(uint64_t occupied) {
    const int64_t room = (int64_t)CELL_VOLUME_PICOLITERS - (int64_t)occupied;
    if (room <= 0)
        return 0;

    return (uint64_t)room;
}

// 占める体積の割合を除いた残り(Q16。切り捨て。占める分がセル以上なら 0)
uint32_t RemainingQ16(uint64_t occupied) {
    return (uint32_t)MulDiv(RoomLeft(occupied), OPEN_FULL, CELL_VOLUME_PICOLITERS);
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

    ProbeLiquidCellState state;
    state.mass = massNanograms / NANOGRAMS_PER_MILLIGRAM;
    state.volume = volume;
    state.open = RemainingQ16(solid);
    state.wall = state.open == 0 ? 1u : 0u;
    state.room = RoomLeft(solid);
    state.equivalent = state.open == OPEN_FULL || state.open == 0 ? state.mass
                                                                  : MulDiv(state.mass, OPEN_FULL, state.open);
    state.heat = (cell.energy * 1000) - chemical;
    state.capacity = capacity;
    DEBUG_ASSERT(state.heat >= 0, DebugFormat::ProbeLiquidInvariant, region, INVARIANT_NEGATIVE_HEAT);

    return state;
}

// 区画のセル region の + 側の面(格子の軸 axis)の開き(Q16。両側の空きの小さい方。区画の外は 0)
uint32_t FaceOpenAt(ProbeLiquidHeader header, uint32_t region, uint32_t axis) {
    if (Component(RegionAt(header, region), axis) + 1 >= RegionSize(header, axis))
        return 0;

    return min(LoadOpen(header, region), LoadOpen(header, region + Stride(header, axis)));
}

// --- 位置と運動のエネルギー(mJ。セルのエネルギーの外)---

int64_t PotentialOf(ProbeLiquidHeader header, uint32_t region, uint64_t massMilligrams) {
    const uint64_t heightMillimeters = (RegionAt(header, region).y * CELL_EDGE_MILLIMETERS) +
                                       (CELL_EDGE_MILLIMETERS / 2);

    return (int64_t)MulDiv(massMilligrams * heightMillimeters, header.gravityMicrometersPerSecond2,
                           POTENTIAL_DENOMINATOR);
}

int64_t KineticOf(ProbeLiquidHeader header, uint32_t region, int64_t momentum, uint64_t doubledMass) {
    if (momentum == 0 || doubledMass == 0)
        return 0;

    const FxU128 scaled = FxMulU64Full(FxAbsU64(momentum), header.gravityMicrometersPerSecond2);
    DEBUG_ASSERT(scaled.hi == 0, DebugFormat::ProbeLiquidInvariant, region, INVARIANT_KINETIC_RANGE);
    const FxU128 squared = FxMulU64Full(scaled.lo, scaled.lo);

    return (int64_t)(FxDivU128By64(squared, KINETIC_DENOMINATOR).quotient / doubledMass);
}

uint64_t MaxMomentum(ProbeLiquidHeader header, uint64_t doubledMass) {
    const uint64_t speedMicrometers = (uint64_t)header.maxSpeedMillimetersPerSecond * 1000;

    return MulDiv(speedMicrometers * (uint64_t)TICKS_PER_SECOND, doubledMass, header.gravityMicrometersPerSecond2);
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
    uint64_t above = 0;
    for (uint32_t k = 0; k < height; ++k) {
        const uint32_t y = height - 1 - k;
        const uint32_t region = RegionIndex(header, x, y, z);
        const ProbeLiquidCellState state = LoadState(header, region);
        int64_t pressure = 0;
        if (state.wall != 0 || state.mass == 0) {
            above = 0;
        } else {
            // 圧力は空きの体積と「セル全部の液体に直した質量」で書く(孔の中の水の圧力 = 開けた水と同じ高さの圧力)
            // 溢れ = 体積 − 空き(0 で止める引き算を符号付きの差で。T-0185。体積は 2^63 未満)
            uint64_t overfill = 0;
            const int64_t excess = (int64_t)state.volume - (int64_t)state.room;
            if (excess > 0)
                overfill = MulDiv(state.equivalent * header.overfillStiffness, (uint64_t)excess, state.volume);

            // 満ちていないセルは上の液体を満ち具合の分だけ支える
            const uint64_t filled = min(state.volume, state.room);
            const uint64_t supported = MulDiv(above, filled, state.room);

            // 下に支えられている分だけ圧力になる(固体で満ちたセル・区画の床は全部・液体は空きの満ち具合・空は 0)
            uint64_t supportNumerator = 1;
            uint64_t supportDenominator = 1;
            if (y != 0) {
                const ProbeLiquidCellState below = LoadState(header, RegionIndex(header, x, y - 1, z));
                if (below.wall == 0) {
                    supportNumerator = min(below.volume, below.room);
                    supportDenominator = below.room;
                }
            }

            pressure = (int64_t)MulDiv((2 * supported) + state.equivalent + overfill, supportNumerator,
                                       supportDenominator);
            above = supported + state.equivalent;
        }

        liquidWork.Store<int64_t>(CellAddress(header.pressureOffset, region), pressure);
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
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
            const uint32_t step = Stride(header, axis);
            int64_t face = LoadSigned(FaceAddress(header, header.facesOffset, axis, region));
            int64_t wall = 0;
            int64_t gravity = 0;
            int64_t friction = 0;
            int64_t dropped = 0;
            uint64_t desired = 0;

            // − 側が区画の外か閉じた面なら、その面の押し返しは壁から(+ 向き)
            const bool minusClosed = Component(at, axis) == 0 || min(LoadOpen(header, region - step), a.open) == 0;
            if (a.wall == 0 && minusClosed)
                wall += pressure;

            // + 側の面(一部だけ開いた面 = 孔は、力も重さも開いた割合だけ)
            const uint32_t faceOpen = FaceOpenAt(header, region, axis);
            if (faceOpen == 0) {
                if (a.wall == 0)
                    wall -= pressure;

                wall -= face;  // 閉じた面に残った運動量は壁が受ける
                face = 0;
            } else {
                const uint32_t neighbor = region + step;
                const ProbeLiquidCellState b = LoadState(header, neighbor);
                const int64_t push = pressure - LoadSigned(CellAddress(header.pressureOffset, neighbor));
                const int64_t weight = axis == 1 ? (int64_t)(a.equivalent + b.equivalent) : 0;
                if (faceOpen == OPEN_FULL) {
                    face += push - weight;
                    gravity -= weight;
                } else {
                    const int64_t openWeight = ScaleSigned(weight, faceOpen, OPEN_FULL);
                    const int64_t change = ScaleSigned(push - weight, faceOpen, OPEN_FULL);
                    face += change;
                    gravity -= openWeight;
                    wall += change - push + openWeight;
                }

                // まわりに液体が無い面の運動量は行き場が無い。移流なしでは上流が空の面も
                const uint64_t upwindMass = face > 0 ? a.mass : b.mass;
                if (a.mass + b.mass == 0 || (header.advection == 0 && upwindMass == 0)) {
                    dropped -= face;
                    face = 0;
                } else {
                    const int64_t slowed = FxApplySign(FxAbsU64(face) >> header.frictionShift, face < 0);
                    face -= slowed;
                    friction -= slowed;

                    // 上流が空: 運動量は残し(移流が運ぶ)、この面は流れない
                    if (upwindMass != 0)
                        desired = MulDiv(FxAbsU64(face), header.gravityMicrometersPerSecond2, FLUX_DENOMINATOR);
                }
            }

            liquidWork.Store<int64_t>(FaceAddress(header, header.forcedOffset, axis, region), face);
            liquidWork.Store<uint64_t>(FaceAddress(header, header.desiredOffset, axis, region), desired);
            external[axis] = wall + gravity + friction + dropped;
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
            const uint64_t mass = outgoing > fromMass ? MulDiv(flux, fromMass, outgoing) : flux;
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

        const uint64_t moved = MulDiv(LoadUnsigned(snapshot + 48 + i * 8), mass, state.mass);
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

    // 熱は動く熱容量の比で(液体とセルの温度を同じと置く)。mJ へは床で(渡す側の熱が負にならない)
    const int64_t movedHeat = (int64_t)MulDiv((uint64_t)state.heat, movedCapacity, state.capacity);
    const int64_t movedEnergy = DivideDown(movedHeat + movedChemical, 1000);
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
            const int64_t potential = (int64_t)MulDiv(fromPotential, mass, LoadState(header, from).mass);
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
    momentum = 0;
    energy = 0;
    const int64_t before = LoadSigned(FaceAddress(header, header.forcedOffset, axis, from));
    if (before == 0)
        return;

    const uint64_t doubledMass = LoadState(header, from).mass + LoadState(header, from + Stride(header, axis)).mass;
    const uint64_t denominator = max(doubledMass, LoadUnsigned(FaceAddress(header, header.leavingOffset, axis, from)));
    const int64_t magnitude = (int64_t)MulDiv(FxAbsU64(before), share, denominator);
    momentum = before > 0 ? magnitude : -magnitude;
    energy = (int64_t)MulDiv((uint64_t)LoadSigned(FaceAddress(header, header.kineticOffset, axis, from)), share,
                             denominator);
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
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
            const uint32_t faceAddress = FaceAddress(header, header.facesOffset, axis, region);
            int64_t face = LoadSigned(faceAddress);
            const bool inside = Component(at, axis) + 1 < RegionSize(header, axis);
            const uint64_t doubled = inside ? LoadUnsigned(CellAddress(header.massOffset, region)) +
                                                  LoadUnsigned(
                                                      CellAddress(header.massOffset, region + Stride(header, axis)))
                                            : 0;
            int64_t lost = 0;
            if (face != 0 && doubled == 0) {
                lost -= face;
                face = 0;
            }

            const uint64_t limit = MaxMomentum(header, doubled);
            if (FxAbsU64(face) > limit) {
                const int64_t kept = face > 0 ? (int64_t)limit : -(int64_t)limit;
                lost -= face - kept;
                face = kept;
            }

            const int64_t kinetic = KineticOf(header, region, face, doubled);
            const int64_t released = LoadSigned(FaceAddress(header, header.carriedOffset, axis, region)) - kinetic;
            liquidWork.Store<int64_t>(faceAddress, face);
            liquidWork.Store<int64_t>(FaceAddress(header, header.kineticOffset, axis, region), kinetic);
            liquidWork.Store<int64_t>(FaceAddress(header, header.releasedOffset, axis, region), released);
            external[axis] = lost;
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
            heat += released - DivideDown(released, 2);
        else
            heat += released;

        if (Component(at, axis) != 0) {
            const uint32_t minus = region - Stride(header, axis);
            heat += DivideDown(LoadSigned(FaceAddress(header, header.releasedOffset, axis, minus)), 2);
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
