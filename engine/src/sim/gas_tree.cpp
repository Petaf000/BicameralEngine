// gas_tree.cpp — 気体の多重解像度の木の CPU リファレンス(gas_tree.h・T-0208・07 §2.5・ADR-0117)。
// 小刻みの段は gas_reference.cpp の Substep と同じ(導く値 → 面の力を当てる → 流れと出ていく割合 → 移す成分の物質量 → 移す)。
// 違うのは面の両側がレベルの違うセルでありうることだけ: 面は細かい側のレベルの単位で計算し、粗い側の中身は × 8^d で細かい単位に直して読み、
// 粗い側への増減は整数部 + 2^-64 の端数で足す(AddScaled)。どの段も整数の足し引きだけで次の配列へ書く(順番に依存しない。04 R2・R3)。
// レベルごとの小刻み(T-0320): 1 刻みをいちばん細かい小刻み(通し番号 micro)に分け、各 micro では小刻みを始めるレベルの面だけを計算する(MicroStep)。
// セルは 3 つの状態を持つ: 小刻みの初めの値(tree.cells。導く値はここから)・力を当てた値(forced。同じ小刻みの面の力積だけ)・次の値
// (next。forced + 移した塊 + 細かい小刻みの面の力積)。小刻みの終わりに next を tree.cells へ。粗いセルの forced と next は粗い小刻みの間ずっと持つ。
#include "sim/gas_tree.h"

#include <algorithm>
#include <cassert>
#include <limits>

#include "common/fixed.hlsli"
#include "sim/gas_internal.h"
#include "sim/world_physics.h"

namespace bicameral::sim {

    namespace {

        namespace fx = bicameral::fx;
        namespace gd = gas_detail;

        constexpr uint32_t MAX_TREE_LEVEL = 20;  // 3k ≤ 61: 端数を合計(レベル 0 の 2^-64)へ正確に移せる深さ

        // 面の片側。Finer と Mixed は面を作る時だけ使う(Finer: 細かいセルが覆う = 細かい側が面を計算する。
        // Mixed: MUSCL の外側の隣がレベルの違うセル = 勾配を使わない)
        enum class SideKind : uint8_t {
            Cell,
            Ghost,
            Wall,
            Finer,
            Mixed,
        };

        struct TreeSide {
            SideKind kind = SideKind::Cell;
            uint32_t index = 0;  // Cell: セルの番号 / Ghost: 層 z
            uint32_t level = 0;
        };

        struct TreeFace {
            TreeSide left;
            TreeSide right;
            TreeSide leftOuter;
            TreeSide rightOuter;
            uint32_t axis = 0;
            uint32_t level = 0;  // 面を計算する単位(細かい側のレベル)
            uint32_t id = 0;     // 面の番号(そのレベルの座標から。乱数の丸め。R6)
            int64_t flow = 0;
            int64_t impulse = 0;
            uint64_t fraction = 0;
            int64_t weightLeft = 0;  // 面のレベルの単位
            int64_t weightRight = 0;
            GasPerSpecies<uint64_t> moved;  // 面のレベルの単位
        };

        // 小刻みの中の状態(力を当てた後・移した後)
        struct TreeState {
            std::vector<GasCell> cells;
            std::vector<GasTreeFraction> fractions;
        };

        // 1 刻みの間の作業の状態(セルごと。値はそのセルの小刻みの初めに作り、小刻みの間は持ち越す)
        struct TreeWork {
            std::vector<GasDerived> derived;  // 小刻みの初めの整数部から
            TreeState forced;                 // 小刻みの初め + 同じ小刻みの面の力積
            TreeState next;                   // forced + 移した塊 + 細かい小刻みの面の力積

            // --- 粗い小刻み 1 回の間に出した量(細かい面が何度も引くため。T-0320)---
            std::vector<uint64_t> committedOutflow;      // 同じ小刻みの面が出す割合の和(Q32。抑えた後)
            std::vector<GasPerSpecies<uint64_t>> taken;  // 出した物質量(風上のレベル + 1 の単位)

            GasTreeStepStats* stats = nullptr;  // 抑えを通った回数(試験だけ。T-0356)
        };

        // --- レベルごとの小刻み(T-0320)---

        uint32_t Exponent(const GasTree& tree, uint32_t level) {
            return tree.levels[level].subcycleExponent;
        }

        // レベル level の小刻みが、いちばん細かい小刻みの通し番号 micro で始まるか・終わるか(長さ 2^(σmax − σ))
        uint64_t PeriodMask(const GasTree& tree, uint32_t level) {
            return ((uint64_t)1 << (tree.maxSubcycleExponent - Exponent(tree, level))) - 1;
        }

        bool Starts(const GasTree& tree, uint32_t level, uint64_t micro) {
            return (micro & PeriodMask(tree, level)) == 0;
        }

        bool Ends(const GasTree& tree, uint32_t level, uint64_t micro) {
            return ((micro + 1) & PeriodMask(tree, level)) == 0;
        }

        // 乱数の小刻みの通し番号はレベルごと(tick × その小刻みの数 + その中の番号。σ = 0 なら T-0208 と同じ)
        uint64_t RoundingTick(const GasTree& tree, uint32_t level, uint64_t micro) {
            const uint32_t exponent = Exponent(tree, level);
            const uint64_t substeps = (uint64_t)tree.config.substeps << exponent;

            return (tree.tick * substeps) + (micro >> (tree.maxSubcycleExponent - exponent));
        }

        // --- 端数つきの足し算 ---

        // whole + fraction × 2^-64 に value × 2^-shift を足す(shift < 64。value は符号つき。128bit の足し算なので順に依存しない)
        void AddScaled(int64_t& whole, uint64_t& fraction, int64_t value, uint32_t shift) {
            if (shift == 0) {
                whole += value;
                return;
            }

            assert(shift < 64);
            const int64_t high = value >> shift;  // −∞ 方向(C++20 の算術シフト)
            const uint64_t low = ((uint64_t)value & (((uint64_t)1 << shift) - 1)) << (64 - shift);
            fraction += low;
            whole += high + (fraction < low ? 1 : 0);
        }

        void AddScaledUnsigned(uint64_t& whole, uint64_t& fraction, int64_t value, uint32_t shift) {
            auto signedWhole = (int64_t)whole;
            AddScaled(signedWhole, fraction, value, shift);
            assert(signedWhole >= 0);
            whole = (uint64_t)signedWhole;
        }

        // 端数(2^-64 × レベル k の単位)を合計(2^-64 × レベル 0 の単位)へ。下位の 3k ビットは 0(細かい側の単位でしか増えない)
        void AddFraction(GasQ64& total, uint64_t fraction, uint32_t shift) {
            assert(shift == 0 || (fraction & (((uint64_t)1 << shift) - 1)) == 0);
            const uint64_t low = shift == 0 ? fraction : fraction >> shift;
            total.fraction += low;
            total.whole += total.fraction < low ? 1 : 0;
        }

        void AddLedger(GasQ64& ledger, int64_t value, uint32_t level) {
            AddScaled(ledger.whole, ledger.fraction, value, 3 * level);
        }

        // --- 面の両側 ---

        uint32_t CellLayer(const GasTree& tree, uint32_t index) {
            return tree.places[index].coordinate[2];
        }

        const GasCell& SideCell(const GasTree& tree, const std::vector<GasCell>& cells, const TreeSide& side) {
            return side.kind == SideKind::Ghost ? tree.levels[side.level].ghost[side.index] : cells[side.index];
        }

        const GasDerived& SideDerived(const GasTree& tree, const std::vector<GasDerived>& derived,
                                      const TreeSide& side) {
            return side.kind == SideKind::Ghost ? tree.levels[side.level].referenceDerived[side.index]
                                                : derived[side.index];
        }

        // 側の中身を面の単位へ直すずらし(粗い側は 3d。同じレベル・境界は 0)
        uint32_t ContentShift(const TreeFace& face, const TreeSide& side) {
            return side.kind == SideKind::Cell ? 3 * (face.level - side.level) : 0;
        }

        // 面の小刻みと側の小刻みの差 g(面は側の小刻み 1 回の間に 2^g 回計算される。同じレベル・境界は 0)
        uint32_t SubcycleGap(const GasTree& tree, const TreeFace& face, const TreeSide& side) {
            if (side.kind != SideKind::Cell)
                return 0;

            return Exponent(tree, face.level) - Exponent(tree, side.level);
        }

        // 基準との質量の差(数。密度のような値なのでレベルの単位のまま)。開いた境界の外・壁は 0
        int64_t Excess(const GasTree& tree, const std::vector<GasDerived>& derived, const TreeSide& side) {
            if (side.kind != SideKind::Cell)
                return 0;

            const GasTreeLevel& level = tree.levels[side.level];
            return (int64_t)derived[side.index].mass -
                   (int64_t)level.referenceDerived[CellLayer(tree, side.index)].mass;
        }

        // 静水圧の差の半分(µPa)= 質量の差 × 層の重さの係数(標準の重力で 39.2266)÷ 2 ÷ 2^k(レベル k の数の質量の差。セルの高さが 2^-k)
        int64_t HalfHydrostatic(const GasTree& tree, int64_t excess, uint32_t level) {
            return excess * (int64_t)tree.levels[0].coefficients.hydrostatic / ((int64_t)20000 << level);
        }

        // 面の両側の質量の差を面のレベルの数へ(粗い側は高さが 2^d 倍なので静水圧の差も 2^d 倍)
        int64_t FaceExcess(const GasTree& tree, const std::vector<GasDerived>& derived, const TreeFace& face) {
            const int64_t left = Excess(tree, derived, face.left) * ((int64_t)1 << (ContentShift(face, face.left) / 3));
            const int64_t right = Excess(tree, derived, face.right) *
                                  ((int64_t)1 << (ContentShift(face, face.right) / 3));

            return left + right;
        }

        // --- 面の一覧 ---

        // レベル level の座標(軸 axis だけ箱の外になりうる)の側。無ければ 1 つ粗いレベル(隣のレベルの差は 1 まで。T-0208)
        TreeSide Resolve(const GasTree& tree, uint32_t level, std::array<int64_t, GAS_AXES> coordinate, uint32_t axis,
                         uint32_t ownLayer) {
            const GasTreeLevel& grid = tree.levels[level];
            const auto size = (int64_t)grid.size[axis];
            const GasBoundary boundary = tree.config.boundary[axis];
            if (coordinate[axis] < 0 || coordinate[axis] >= size) {
                if (boundary == GasBoundary::Open)
                    return {.kind = SideKind::Ghost, .index = ownLayer, .level = level};

                if (boundary == GasBoundary::Wall)
                    return {.kind = SideKind::Wall, .index = 0, .level = level};

                coordinate[axis] = ((coordinate[axis] % size) + size) % size;
            }

            const auto x = (uint32_t)coordinate[0];
            const auto y = (uint32_t)coordinate[1];
            const auto z = (uint32_t)coordinate[2];
            const int32_t found = tree.Find(level, x, y, z);
            if (found >= 0)
                return {.kind = SideKind::Cell, .index = (uint32_t)found, .level = level};

            if (found == GAS_TREE_COVERED)
                return {.kind = SideKind::Finer, .index = 0, .level = level + 1};

            assert(level > 0);
            const int32_t parent = tree.Find(level - 1, x / 2, y / 2, z / 2);
            assert(parent >= 0 && "隣のレベルの差は 1 まで(2:1。T-0208)");

            return {.kind = SideKind::Cell, .index = (uint32_t)parent, .level = level - 1};
        }

        // MUSCL の外側の隣: 面と同じレベルのセルか境界だけ使う
        TreeSide OuterSide(const GasTree& tree, uint32_t level, std::array<int64_t, GAS_AXES> coordinate, uint32_t axis,
                           uint32_t ownLayer) {
            const TreeSide side = Resolve(tree, level, coordinate, axis, ownLayer);
            if (side.kind == SideKind::Finer || (side.kind == SideKind::Cell && side.level != level))
                return {.kind = SideKind::Mixed, .index = 0, .level = level};

            return side;
        }

        // セル index の軸 axis の面(high = プラス側)。other は面の向こうの側
        TreeFace MakeFace(const GasTree& tree, uint32_t index, uint32_t axis, bool high, const TreeSide& other) {
            const GasTreePlace& place = tree.places[index];
            const TreeSide self = {.kind = SideKind::Cell, .index = index, .level = place.level};
            TreeFace face;
            face.axis = axis;
            face.level = place.level;
            face.left = high ? self : other;
            face.right = high ? other : self;
            face.id = ((tree.GridIndex(place.level, place.coordinate[0], place.coordinate[1], place.coordinate[2]) *
                        GAS_AXES) +
                       axis) *
                          2 +
                      (high ? 1 : 0);

            std::array<int64_t, GAS_AXES> coordinate = {place.coordinate[0], place.coordinate[1], place.coordinate[2]};
            const int64_t left = high ? coordinate[axis] : coordinate[axis] - 1;
            const bool sameLevel = other.kind != SideKind::Cell || other.level == place.level;
            face.leftOuter = {.kind = SideKind::Mixed, .index = 0, .level = place.level};
            face.rightOuter = face.leftOuter;
            if (!sameLevel)
                return face;

            coordinate[axis] = left - 1;
            face.leftOuter = OuterSide(tree, place.level, coordinate, axis, place.coordinate[2]);
            coordinate[axis] = left + 2;
            face.rightOuter = OuterSide(tree, place.level, coordinate, axis, place.coordinate[2]);

            return face;
        }

        // 面の一覧: 同じレベルの面はマイナス側のセルが 1 回(gas_reference と同じ)、レベルをまたぐ面は細かい側のセルが両向きに作る
        std::vector<TreeFace> EnumerateFaces(const GasTree& tree) {
            std::vector<TreeFace> faces;
            faces.reserve((tree.cells.size() * GAS_AXES) + 64);

            for (uint32_t index = 0; index < tree.cells.size(); ++index) {
                const GasTreePlace& place = tree.places[index];
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                    std::array<int64_t, GAS_AXES> coordinate = {place.coordinate[0], place.coordinate[1],
                                                                place.coordinate[2]};
                    coordinate[axis] -= 1;
                    const TreeSide minus = Resolve(tree, place.level, coordinate, axis, place.coordinate[2]);
                    if (minus.kind != SideKind::Finer)
                        faces.push_back(MakeFace(tree, index, axis, false, minus));

                    coordinate[axis] += 2;
                    const TreeSide plus = Resolve(tree, place.level, coordinate, axis, place.coordinate[2]);
                    const bool boundary = plus.kind == SideKind::Wall || plus.kind == SideKind::Ghost;
                    const bool coarser = plus.kind == SideKind::Cell && plus.level < place.level;
                    if (boundary || coarser)
                        faces.push_back(MakeFace(tree, index, axis, true, plus));
                }
            }

            return faces;
        }

        // --- 面の力と流れ(gas_reference.cpp の同じ名前の関数を、面のレベルの係数で)---

        int64_t DrivingPressure(const GasTree& tree, const std::vector<GasDerived>& derived, const TreeFace& face) {
            const int64_t difference = SideDerived(tree, derived, face.left).scaledPressure -
                                       SideDerived(tree, derived, face.right).scaledPressure;
            if (face.axis != 2 || !tree.config.gravity)
                return difference;

            return difference - HalfHydrostatic(tree, FaceExcess(tree, derived, face), face.level);
        }

        // 縦の面の重さ: 両側の質量の差(数)の平均の 1/4 を両側が受ける。粗い側は細かい単位で × 2^d
        // (粗いセルの 1 面は細かい面 4^d 枚に分かれ、細かい単位は 8^-d なので、全部の細かい面で同じレベルの面の 1/4 になる)
        void ComputeFaceWeight(const GasTree& tree, const std::vector<GasDerived>& derived, TreeFace& face) {
            if (face.axis != 2 || !tree.config.gravity)
                return;

            const auto coefficient = (int64_t)tree.levels[face.level].coefficients.gravityImpulse;
            if (face.left.kind == SideKind::Wall) {
                face.weightRight = fx::FxMulShiftS64(Excess(tree, derived, face.right), coefficient, 17);
                return;
            }

            if (face.right.kind == SideKind::Wall) {
                face.weightLeft = fx::FxMulShiftS64(Excess(tree, derived, face.left), coefficient, 17);
                return;
            }

            const int64_t quarter = fx::FxMulShiftS64(
                Excess(tree, derived, face.left) + Excess(tree, derived, face.right), coefficient, 18);
            face.weightLeft = quarter * ((int64_t)1 << (ContentShift(face, face.left) / 3));
            face.weightRight = quarter * ((int64_t)1 << (ContentShift(face, face.right) / 3));
        }

        void ComputeFaceForce(const GasTree& tree, const std::vector<GasDerived>& derived, TreeFace& face) {
            const auto coefficient = (int64_t)tree.levels[face.level].coefficients.pressureImpulse;
            ComputeFaceWeight(tree, derived, face);

            // --- 壁: 鏡に映した圧力(縦の壁は半セル分の静水圧で壁まで延ばす)---
            if (face.left.kind == SideKind::Wall || face.right.kind == SideKind::Wall) {
                const bool wallBelow = face.left.kind == SideKind::Wall;
                const TreeSide& inside = wallBelow ? face.right : face.left;
                int64_t pressure = derived[inside.index].scaledPressure;
                if (face.axis == 2 && tree.config.gravity) {
                    const int64_t hydrostatic = HalfHydrostatic(tree, Excess(tree, derived, inside), face.level);
                    pressure += wallBelow ? hydrostatic : -hydrostatic;
                }

                face.impulse = fx::FxMulShiftS64(pressure * 2, coefficient, 16);
                return;
            }

            const int64_t pressureSum = SideDerived(tree, derived, face.left).scaledPressure +
                                        SideDerived(tree, derived, face.right).scaledPressure;
            face.impulse = fx::FxMulShiftS64(pressureSum, coefficient, 16);
        }

        // 質量の流れ(面のレベルの単位)。運動量は数のまま足す(同じ速さなら同じ数 = 運動量の密度)
        void ComputeFaceFlow(const GasTree& tree, const std::vector<GasDerived>& derived,
                             const std::vector<GasCell>& forced, TreeFace& face) {
            if (face.left.kind == SideKind::Wall || face.right.kind == SideKind::Wall)
                return;

            const GasCoefficients& coefficients = tree.levels[face.level].coefficients;
            const int64_t momentumSum = SideCell(tree, forced, face.left).momentum[face.axis] +
                                        SideCell(tree, forced, face.right).momentum[face.axis];
            const int64_t central = fx::FxMulShiftS64(momentumSum, (int64_t)coefficients.centralFlow, 52);
            const int64_t diffusion = fx::FxMulShiftS64(DrivingPressure(tree, derived, face),
                                                        (int64_t)coefficients.pressureFlow, 40);
            face.flow = central + diffusion;
        }

        const TreeSide& Donor(const TreeFace& face) {
            return face.flow > 0 ? face.left : face.right;
        }

        // 風上の中身(面の単位)のうち移す割合(Q32。抑える前)
        uint64_t RawFraction(const GasTree& tree, const std::vector<GasDerived>& derived, const TreeFace& face) {
            if (face.flow == 0)
                return 0;

            const TreeSide& donor = Donor(face);
            const GasDerived& donorDerived = SideDerived(tree, derived, donor);
            const uint32_t shift = ContentShift(face, donor);
            uint64_t inverseMass = donorDerived.inverseMass;
            if (shift != 0)
                inverseMass = donorDerived.mass == 0
                                  ? 0
                                  : std::numeric_limits<uint64_t>::max() / (donorDerived.mass << shift);

            return gd::MulShiftU64(fx::FxAbsU64(face.flow), inverseMass, 32);
        }

        // 抑えを通った回数(試験だけ。T-0356)
        void NoteOutflowLimit(TreeWork& work, uint32_t gap, uint64_t available) {
            if (work.stats == nullptr)
                return;

            ++work.stats->limitedOutflow;
            work.stats->limitedSubcycledOutflow += gap != 0 ? 1 : 0;
            work.stats->limitedAfterCommitted += available != gd::ONE_Q32 ? 1 : 0;
        }

        void NoteAmountLimit(TreeWork& work, uint64_t taken) {
            if (work.stats == nullptr)
                return;

            ++work.stats->limitedAmounts;
            work.stats->limitedAfterTaken += taken != 0 ? 1 : 0;
        }

        // 出ていく割合の合計が 1 を超えるセルは比例して縮める(割合はどれも同じ中身に対する値なので、レベルをまたぐ面もそのまま足せる)。
        // 小刻みの細かい面(風上の小刻み 1 回の間に 2^g 回引く)は × 2^g で数え、使える分は 1 − 風上の小刻みの初めに同じ小刻みの面が出した分
        // → 風上の小刻み 1 回の合計が 1 以下(T-0320。熱の C/8 ÷ 引く回数と同じ考え。全部同じ小刻みなら T-0208 と同じ値)。
        // 合計はどちらも箱と同じ換算(gas_reference.cpp の EffectiveOutflow。押し出す仕事 n R T のぶん × (C + n R) ÷ C。T-0647・ADR-0286)で
        // 数える(引く側の熱を負にしない。換算の倍率は風上の物質だけで決まり、粗い小刻みの間は同じ)
        void LimitOutflow(const GasTree& tree, TreeWork& work, std::vector<TreeFace>& faces) {
            std::vector<uint64_t> outflow(tree.cells.size(), 0);
            for (TreeFace& face : faces) {
                face.fraction = RawFraction(tree, work.derived, face);
                const TreeSide& donor = Donor(face);
                if (face.fraction != 0 && donor.kind == SideKind::Cell)
                    outflow[donor.index] += face.fraction << SubcycleGap(tree, face, donor);
            }

            std::vector<uint64_t> available(tree.cells.size(), gd::ONE_Q32);
            for (uint32_t index = 0; index < tree.cells.size(); ++index) {
                if (outflow[index] == 0)
                    continue;

                const GasCell& source = work.forced.cells[index];
                const uint64_t committed = gd::EffectiveOutflow(tree.config, source, work.committedOutflow[index]);
                available[index] = gd::ONE_Q32 - std::min(committed, gd::ONE_Q32);
                outflow[index] = gd::EffectiveOutflow(tree.config, source, outflow[index]);
            }

            for (TreeFace& face : faces) {
                const TreeSide& donor = Donor(face);
                if (face.fraction == 0 || donor.kind != SideKind::Cell ||
                    outflow[donor.index] <= available[donor.index])
                    continue;

                NoteOutflowLimit(work, SubcycleGap(tree, face, donor), available[donor.index]);

                face.fraction = fx::FxDivU128By64(fx::FxMulU64Full(face.fraction, available[donor.index]),
                                                  outflow[donor.index])
                                    .quotient;
            }

            for (const TreeFace& face : faces) {
                const TreeSide& donor = Donor(face);
                if (face.fraction != 0 && donor.kind == SideKind::Cell && SubcycleGap(tree, face, donor) == 0)
                    work.committedOutflow[donor.index] += face.fraction;
            }
        }

        // --- 移す成分の物質量(G2。MUSCL は同じレベルの 3 つ並びだけ)---

        int64_t SpeciesRatio(const GasTree& tree, const std::vector<GasCell>& cells,
                             const std::vector<GasDerived>& derived, const TreeSide& side, uint32_t s) {
            const uint64_t inverseAmount = SideDerived(tree, derived, side).inverseAmount;

            return (int64_t)gd::MulShiftU64(SideCell(tree, cells, side).amounts[s], inverseAmount, 32);
        }

        int64_t HalfSlope(const GasTree& tree, const std::vector<GasCell>& cells,
                          const std::vector<GasDerived>& derived, const TreeFace& face, uint32_t s) {
            const bool forward = face.flow > 0;
            const TreeSide& outer = forward ? face.leftOuter : face.rightOuter;
            const TreeSide& donor = forward ? face.left : face.right;
            const TreeSide& receiver = forward ? face.right : face.left;
            const bool outerUsable = outer.kind != SideKind::Wall && outer.kind != SideKind::Mixed;
            if (donor.kind != SideKind::Cell || !outerUsable || receiver.kind == SideKind::Wall)
                return 0;

            const int64_t donorRatio = SpeciesRatio(tree, cells, derived, donor, s);
            const int64_t slope = gd::LimitedSlope(tree.config.reconstruction,
                                                   donorRatio - SpeciesRatio(tree, cells, derived, outer, s),
                                                   SpeciesRatio(tree, cells, derived, receiver, s) - donorRatio);

            return fx::FxMulShiftS64(slope, (int64_t)(gd::ONE_Q32 - face.fraction), 33);
        }

        // 移す成分 s の物質量(Q32 の面の単位。128bit)。同じレベルの面は gas_reference と同じ式、粗い風上は中身 × 8^d(1 次の風上)
        fx::FxU128 MovedAmountQ32(const GasTree& tree, const std::vector<GasCell>& cells,
                                  const std::vector<GasDerived>& derived, const TreeFace& face, uint32_t s) {
            const TreeSide& donor = Donor(face);
            const GasCell& source = SideCell(tree, cells, donor);
            const uint32_t shift = ContentShift(face, donor);
            if (shift != 0) {
                const fx::FxU128 product = fx::FxMulU64Full(source.amounts[s], face.fraction);
                return {.hi = (product.hi << shift) | (product.lo >> (64 - shift)), .lo = product.lo << shift};
            }

            const uint64_t moved = source.amounts[s] * face.fraction;
            if (tree.config.reconstruction == GasReconstruction::Upwind)
                return {.hi = 0, .lo = moved};

            const int64_t halfSlope = HalfSlope(tree, cells, derived, face, s);
            const uint64_t total = gd::TotalAmount(tree.config, source) * face.fraction;
            const uint64_t correction = gd::MulShiftU64(total, fx::FxAbsU64(halfSlope), 32);
            if (halfSlope >= 0)
                return {.hi = 0, .lo = moved + correction};

            return {.hi = 0, .lo = moved > correction ? moved - correction : 0};
        }

        uint64_t RoundAmount(const GasTree& tree, fx::FxU128 amountQ32, uint64_t hash) {
            const uint64_t whole = fx::FxShiftRightU128(amountQ32, 32);
            if (!tree.config.stochasticRounding)
                return whole;

            return whole + ((hash >> 32) < (amountQ32.lo & gd::LOW_32_BITS) ? 1 : 0);
        }

        // 乱数は(世界のシード, そのレベルの小刻みの通し番号, 面の番号, 用途 + レベル)+ 成分の鍵(既定は欄の番号。T-0598)。
        // レベル 0 は gas_reference と同じ
        void ComputeMovedAmounts(const GasTree& tree, const std::vector<GasCell>& cells,
                                 const std::vector<GasDerived>& derived, uint64_t micro, std::vector<TreeFace>& faces) {
            for (TreeFace& face : faces) {
                if (face.fraction == 0)
                    continue;

                const uint64_t roundingTick = RoundingTick(tree, face.level, micro);
                const uint64_t faceHash = fx::FxHash64(tree.config.randomSeed, roundingTick, face.id,
                                                       gd::GAS_ROUNDING_PURPOSE + face.level);
                for (uint32_t s = 0; s < tree.config.speciesCount; ++s) {
                    const fx::FxU128 amountQ32 = MovedAmountQ32(tree, cells, derived, face, s);
                    const uint32_t key = tree.config.species[s].roundingKey;
                    face.moved[s] = RoundAmount(tree, amountQ32,
                                                fx::FxHashCombine(faceHash, key == GAS_ROUNDING_KEY_SLOT ? s : key));
                }
            }
        }

        // 風上のセルごとに、移す物質量の合計が中身を超えたら比例して縮める。合計は風上のレベル + 1 の単位で数える
        // (同じレベルの面は × 8。比も切り捨ても gas_reference と同じ値になる)。中身からは風上の小刻みの間にもう出した分を引く
        // (細かい面が何度も引く時。T-0320。割合は LimitOutflow で抑えてあるので、ここに来るのは乱数の丸め上げ・MUSCL の分だけ)
        void LimitMovedAmounts(const GasTree& tree, TreeWork& work, std::vector<TreeFace>& faces) {
            const std::vector<GasCell>& cells = work.forced.cells;
            const uint32_t speciesCount = tree.config.speciesCount;
            std::vector<GasPerSpecies<uint64_t>> outgoing(tree.cells.size());
            for (const TreeFace& face : faces) {
                const TreeSide& donor = Donor(face);
                if (face.fraction == 0 || donor.kind != SideKind::Cell)
                    continue;

                const uint32_t unitShift = 3 - ContentShift(face, donor);
                for (uint32_t s = 0; s < speciesCount; ++s)
                    outgoing[donor.index][s] += face.moved[s] << unitShift;
            }

            for (TreeFace& face : faces) {
                const TreeSide& donor = Donor(face);
                if (face.fraction == 0 || donor.kind != SideKind::Cell)
                    continue;

                for (uint32_t s = 0; s < speciesCount; ++s) {
                    const uint64_t total = outgoing[donor.index][s];
                    const uint64_t content = (cells[donor.index].amounts[s] << 3) - work.taken[donor.index][s];
                    if (total <= content)
                        continue;

                    NoteAmountLimit(work, work.taken[donor.index][s]);

                    face.moved[s] = fx::FxDivU128By64(fx::FxMulU64Full(face.moved[s], content), total).quotient;
                }
            }

            for (const TreeFace& face : faces) {
                const TreeSide& donor = Donor(face);
                if (face.fraction == 0 || donor.kind != SideKind::Cell)
                    continue;

                const uint32_t unitShift = 3 - ContentShift(face, donor);
                for (uint32_t s = 0; s < speciesCount; ++s)
                    work.taken[donor.index][s] += face.moved[s] << unitShift;
            }
        }

        // --- 当てる ---

        // セルの運動量に面の単位の増減を足す(粗い側は端数つき)
        void AddMomentum(TreeState& state, const TreeSide& side, uint32_t axis, int64_t value, uint32_t faceLevel) {
            const uint32_t shift = 3 * (faceLevel - side.level);
            AddScaled(state.cells[side.index].momentum[axis], state.fractions[side.index].momentum[axis], value, shift);
        }

        // 側が受ける力(面の力積と重さ。面の単位)の multiplier 倍を state へ
        void AddSideForce(TreeState& state, const TreeFace& face, const TreeSide& side, bool right,
                          int64_t multiplier) {
            const int64_t weight = right ? face.weightRight : face.weightLeft;
            if (weight != 0)
                AddMomentum(state, side, 2, -weight * multiplier, face.level);

            if (face.impulse != 0)
                AddMomentum(state, side, face.axis, (right ? face.impulse : -face.impulse) * multiplier, face.level);
        }

        // 側が受ける力。側の小刻みが面と同じ(g = 0)なら力を当てた値(forced。その小刻みの流れに効く)へ。
        // 側が粗い小刻み(g > 0。粗い側)なら、本当の力は次の値(next。粗い小刻みの終わりに受ける。T-0320)へ足すが、粗い小刻みの初めに
        // この面の初めの力 × 2^g を見込みとして forced にも足し、同じ分を next から引く(T-0479。Berger–Colella の粗い側の流束 + 差の受け直し)。
        // 見込みが無いと、粗いセルの forced は細かい面の側の圧力を受けないまま流れを出し、一様な圧力の高まり(閉じた部屋の火・基準と違う大気)
        // で偽の流れが出た(閉じた箱の 11% の一様な加圧で約 11 m/s)。合計(next)は見込みの有無で変わらないので保存はそのまま。
        // immediatePass で forced の回と next の回に分けて 2 回呼ぶ(next は forced を写した後に足すため)。帳簿(箱の外の側)は前者の回で
        void ApplySideForce(GasTree& tree, TreeWork& work, const TreeFace& face, const TreeSide& side, bool right,
                            bool immediatePass, uint64_t micro) {
            const uint32_t gap = SubcycleGap(tree, face, side);
            const bool starts = Starts(tree, side.level, micro);
            const int64_t predicted = (int64_t)1 << gap;
            if (immediatePass) {
                if (gap == 0 || starts)
                    AddSideForce(work.forced, face, side, right, predicted);

                if (gap == 0) {
                    const int64_t weight = right ? face.weightRight : face.weightLeft;
                    AddLedger(tree.ledger.momentum[2], -weight, face.level);
                }

                return;
            }

            if (gap == 0)
                return;

            AddSideForce(work.next, face, side, right, starts ? 1 - predicted : 1);
            const int64_t weight = right ? face.weightRight : face.weightLeft;
            AddLedger(tree.ledger.momentum[2], -weight, face.level);
        }

        void ApplyImpulse(GasTree& tree, TreeWork& work, const TreeFace& face, bool immediatePass, uint64_t micro) {
            // --- セルの側(重さは箱の外から働く力なので帳簿へ)---
            if (face.left.kind == SideKind::Cell)
                ApplySideForce(tree, work, face, face.left, false, immediatePass, micro);

            if (face.right.kind == SideKind::Cell)
                ApplySideForce(tree, work, face, face.right, true, immediatePass, micro);

            // --- 箱の外の側(壁・開いた境界の外)の力積は帳簿へ ---
            if (face.impulse == 0 || !immediatePass)
                return;

            if (face.left.kind != SideKind::Cell)
                AddLedger(tree.ledger.momentum[face.axis], face.impulse, face.level);

            if (face.right.kind != SideKind::Cell)
                AddLedger(tree.ledger.momentum[face.axis], -face.impulse, face.level);
        }

        // 塊(面の単位)を side へ足す(sign = 1)か引く(−1)。セルでなければ(開いた境界の外)帳簿へ逆向きに
        void AddParcel(GasTree& tree, TreeState& state, const TreeSide& side, uint32_t faceLevel, const GasCell& parcel,
                       int64_t energy, int64_t sign) {
            const uint32_t speciesCount = tree.config.speciesCount;
            if (side.kind != SideKind::Cell) {
                for (uint32_t s = 0; s < speciesCount; ++s)
                    AddLedger(tree.ledger.amounts[s], -sign * (int64_t)parcel.amounts[s], faceLevel);

                AddLedger(tree.ledger.energy, -sign * energy, faceLevel);
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                    AddLedger(tree.ledger.momentum[axis], -sign * parcel.momentum[axis], faceLevel);

                return;
            }

            const uint32_t shift = 3 * (faceLevel - side.level);
            GasCell& cell = state.cells[side.index];
            GasTreeFraction& fraction = state.fractions[side.index];
            for (uint32_t s = 0; s < speciesCount; ++s) {
                assert(sign > 0 || shift != 0 || cell.amounts[s] >= parcel.amounts[s]);
                AddScaledUnsigned(cell.amounts[s], fraction.amounts[s], sign * (int64_t)parcel.amounts[s], shift);
            }

            AddScaled(cell.energy, fraction.energy, sign * energy, shift);
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                AddScaled(cell.momentum[axis], fraction.momentum[axis], sign * parcel.momentum[axis], shift);
        }

        // 重さの仕事(面の単位。1 mg が 1 セル〔0.5 m × 2^-k〕上がる仕事)
        int64_t GravityWork(const GasTree& tree, const TreeFace& face, const GasCell& moved) {
            const GasConfig& config = tree.config;
            if (face.axis != 2 || !config.gravity || config.thermodynamics != GasThermodynamics::Adiabatic)
                return 0;

            const uint64_t mass = gd::CellMass(config, tree.levels[face.level].coefficients, moved);
            const auto work = (int64_t)(mass * tree.levels[face.level].coefficients.gravityWork /
                                        (gd::MICRO << face.level));

            return face.flow > 0 ? work : -work;
        }

        void Transfer(GasTree& tree, const std::vector<GasCell>& forced, const std::vector<GasDerived>& derived,
                      TreeState& next, const TreeFace& face) {
            if (face.fraction == 0)
                return;

            const TreeSide& donor = Donor(face);
            const TreeSide& receiver = face.flow > 0 ? face.right : face.left;

            // --- 風上の中身を面の単位へ(粗い風上は × 8^d)---
            GasCell source = SideCell(tree, forced, donor);
            GasDerived sourceDerived = SideDerived(tree, derived, donor);
            const uint32_t shift = ContentShift(face, donor);
            if (shift != 0) {
                for (uint64_t& amount : source.amounts)
                    amount <<= shift;

                source.energy *= (int64_t)1 << shift;
                for (int64_t& momentum : source.momentum)
                    momentum *= (int64_t)1 << shift;

                sourceDerived.mass <<= shift;
            }

            GasCell moved;
            moved.amounts = face.moved;
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                moved.momentum[axis] = fx::FxMulShiftS64(source.momentum[axis], (int64_t)face.fraction, 32);

            const int64_t energy = gd::MovedEnergy(tree.config, source, sourceDerived, moved, face.fraction);
            const int64_t sent = gd::DivideNearest(energy, 1000);
            const int64_t received = gd::DivideNearest(energy - GravityWork(tree, face, moved), 1000);
            AddLedger(tree.ledger.energy, received - sent, face.level);

            AddParcel(tree, next, donor, face.level, moved, sent, -1);
            AddParcel(tree, next, receiver, face.level, moved, received, 1);
        }

        // (1) 小刻みを始めるレベルのセル: 導く値(整数部だけ)・力を当てる前の値・出した量の数える欄
        void BeginCells(const GasTree& tree, uint64_t micro, TreeWork& work) {
            for (uint32_t index = 0; index < tree.cells.size(); ++index) {
                if (!Starts(tree, tree.places[index].level, micro))
                    continue;

                work.derived[index] = DeriveGasTreeCell(tree, index);
                work.forced.cells[index] = tree.cells[index];
                work.forced.fractions[index] = tree.fractions[index];
                work.committedOutflow[index] = 0;
                work.taken[index] = {};
            }
        }

        // (2) 面の力を先に当てる(同じ小刻みの側は forced へ。始めるセルの next は forced の写し。粗い小刻みの側は next へ)
        void ApplyForces(GasTree& tree, uint64_t micro, std::vector<TreeFace>& faces, TreeWork& work) {
            for (TreeFace& face : faces)
                ComputeFaceForce(tree, work.derived, face);

            for (const TreeFace& face : faces)
                ApplyImpulse(tree, work, face, true, micro);

            for (uint32_t index = 0; index < tree.cells.size(); ++index) {
                if (!Starts(tree, tree.places[index].level, micro))
                    continue;

                work.next.cells[index] = work.forced.cells[index];
                work.next.fractions[index] = work.forced.fractions[index];
            }

            for (const TreeFace& face : faces)
                ApplyImpulse(tree, work, face, false, micro);
        }

        // いちばん細かい小刻み 1 回: 小刻みを始めるレベルの面だけを計算し、小刻みを終えるレベルのセルが次の値を受ける
        // (全部同じ小刻みなら gas_reference の Substep・T-0208 と同じ段・同じ値)
        void MicroStep(GasTree& tree, const std::vector<TreeFace>& layout, uint64_t micro, TreeWork& work) {
            BeginCells(tree, micro, work);

            std::vector<TreeFace> faces;
            faces.reserve(layout.size());
            for (const TreeFace& face : layout) {
                if (Starts(tree, face.level, micro))
                    faces.push_back(face);
            }

            ApplyForces(tree, micro, faces, work);

            // --- (3) 力を当てた後の運動量で流れ(前進・後退の順)・出ていく割合を抑える ---
            for (TreeFace& face : faces)
                ComputeFaceFlow(tree, work.derived, work.forced.cells, face);

            LimitOutflow(tree, work, faces);

            // --- (4) 移す成分の物質量 ---
            ComputeMovedAmounts(tree, work.forced.cells, work.derived, micro, faces);
            LimitMovedAmounts(tree, work, faces);

            // --- (5) 移す ---
            for (const TreeFace& face : faces)
                Transfer(tree, work.forced.cells, work.derived, work.next, face);

            // --- (6) 小刻みを終えるレベルのセルは次の値を受ける ---
            for (uint32_t index = 0; index < tree.cells.size(); ++index) {
                if (!Ends(tree, tree.places[index].level, micro))
                    continue;

                tree.cells[index] = work.next.cells[index];
                tree.fractions[index] = work.next.fractions[index];
            }
        }

        // --- 木を作る ---

        // レベル k の係数: 流れ・拡散・力積は × 2^k(面積 ÷ 体積)。小刻みの数は substeps × 2^σ(T-0320)なので、流れ・拡散・力積・重さの
        // 力積は × 2^-σ(MakeGasCoefficients と同じ式に小刻みの数を入れる)。ほか(物質の質量・α)はレベル 0 と同じ
        GasCoefficients MakeLevelCoefficients(const GasConfig& config, const GasCoefficients& base, uint32_t level,
                                              uint32_t exponent) {
            GasCoefficients coefficients = base;
            const uint64_t substeps = (uint64_t)config.substeps << exponent;
            coefficients.centralFlow = ((uint64_t)1 << (31 + level)) / (30 * substeps);
            coefficients.pressureFlow = fx::FxDivU128By64(fx::FxMulU64Full(25, (uint64_t)1 << (40 + level)),
                                                          24 * substeps * config.soundSpeedMmPerS)
                                            .quotient;
            coefficients.pressureImpulse = ((uint64_t)1 << (31 + level)) / (15 * substeps);
            coefficients.gravityImpulse = ((uint64_t)config.gravityMicrometersPerSecond2 << 36) /
                                          (60 * substeps * gd::MICRO);

            return coefficients;
        }

        // レベル k の静水圧の基準(MakeHydrostaticReference と同じ形で、層の厚さが 2^-k)
        std::vector<GasCell> MakeLevelReference(const GasConfig& config, const GasCoefficients& coefficients,
                                                uint32_t level, std::span<const uint32_t> fractionsQ32,
                                                uint32_t temperature, uint64_t surfacePressure) {
            std::vector<GasCell> layers;
            uint64_t pressure = surfacePressure;
            for (uint32_t z = 0; z < (config.sizeZ << level); ++z) {
                const uint64_t divisor = gd::PRESSURE_PER_AMOUNT_TEMPERATURE_Q32 * temperature;
                const uint64_t total = fx::FxDivU128By64(fx::FxMulU64Full(pressure, gd::ONE_Q32), divisor).quotient;

                GasPerSpecies<uint64_t> amounts;
                for (uint32_t s = 0; s < config.speciesCount && s < fractionsQ32.size(); ++s)
                    amounts[s] = gd::MulShiftU64(total, fractionsQ32[s], 32);

                const GasCell cell = MakeGasCell(config, amounts, temperature);
                layers.push_back(cell);

                const uint64_t mass = config.gravity ? gd::CellMass(config, coefficients, cell) : 0;
                pressure -= mass * coefficients.hydrostatic / ((uint64_t)10000 << level);
            }

            return layers;
        }

        // 開いた境界の外 = 層の基準 + 風(MakeGasBox と同じ)
        GasCell MakeGhost(const GasConfig& config, const GasCell& layer, uint64_t mass) {
            GasCell ghost = layer;
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                ghost.momentum[axis] = (int64_t)mass * config.windVelocity[axis];

            ghost.energy += gd::DivideNearest(gd::KineticEnergy(config, ghost, mass), 1000);

            return ghost;
        }

        // レベル k(≥ 1)の環境から決まる値: 係数・層の基準・導く値・開いた境界の外(大きさ・σ・セルの表は木の形で、ここでは触れない)
        void MakeFinerLevelEnvironment(const GasConfig& config, const GasTreeLevel& root, uint32_t level,
                                       GasTreeLevel& result, std::span<const uint32_t> fractionsQ32,
                                       uint32_t temperature, uint64_t surfacePressure) {
            result.coefficients = MakeLevelCoefficients(config, root.coefficients, level, result.subcycleExponent);
            result.reference = MakeLevelReference(config, root.coefficients, level, fractionsQ32, temperature,
                                                  surfacePressure);
            result.referenceDerived.clear();
            result.ghost.clear();
            for (const GasCell& layer : result.reference) {
                result.referenceDerived.push_back(gd::DeriveRaw(config, result.coefficients, layer));
                result.ghost.push_back(MakeGhost(config, layer, result.referenceDerived.back().mass));
            }
        }

        // 環境から決まる値(係数・層の基準・導く値・開いた境界の外・α・頭打ちの温度)を全部のレベルで作る。木の形(大きさ・σ・セルの表)と
        // 状態(セル・端数・帳簿・刻み)には触れない。MakeGasTree と SetGasTreeEnvironment が同じここを通る(T-0470・ADR-0200)
        void BuildLevelEnvironments(GasTree& tree, std::span<const uint32_t> fractionsQ32, uint32_t temperature,
                                    uint64_t surfacePressure) {
            const GasConfig& config = tree.config;

            // --- レベル 0 は 1 レベルの箱と同じ(係数・基準・α)---
            // 重さが無ければ基準の層は全部地面の層と同じ(gas_reference_test の周期的な箱と同じ扱い。層ごとの基準の差が偽の力にならない)
            std::vector<GasCell> reference = MakeHydrostaticReference(config, fractionsQ32, temperature,
                                                                      surfacePressure);
            if (!config.gravity)
                std::fill(reference.begin(), reference.end(), reference.front());

            GasBox base = MakeGasBox(config, std::move(reference));
            GasTreeLevel& root = tree.levels[0];
            root.coefficients = base.coefficients;

            // 熱いセルの頭打ちの温度(T-0269): ν がいちばん大きいレベルで安定の上限に届く温度(全部のレベルが同じ値を使う)
            if (config.soundCapTemperature == 0) {
                root.coefficients.soundTemperature = GasSoundCapTemperature(config.soundSpeedMmPerS,
                                                                            GasTreeSoundSpeedLimitMmPerS(tree),
                                                                            base.referenceDerived.front().temperature);
            }

            root.reference = std::move(base.reference);
            root.referenceDerived = std::move(base.referenceDerived);
            root.ghost = std::move(base.ghost);

            for (uint32_t level = 1; level < tree.levels.size(); ++level)
                MakeFinerLevelEnvironment(config, root, level, tree.levels[level], fractionsQ32, temperature,
                                          surfacePressure);
        }

        // 範囲を細かくする: 下の粗いセルを覆い、細かいセルを置く(番号は後で振る。置いた印は 0)
        void Refine(GasTree& tree, const GasTreeRegion& region) {
            assert(region.level >= 1 && region.level < tree.levels.size());
            const uint32_t count = region.size[0] * region.size[1] * region.size[2];
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t x = region.origin[0] + (i % region.size[0]);
                const uint32_t y = region.origin[1] + ((i / region.size[0]) % region.size[1]);
                const uint32_t z = region.origin[2] + (i / (region.size[0] * region.size[1]));
                int32_t& parent = tree.levels[region.level - 1]
                                      .grid[tree.GridIndex(region.level - 1, x / 2, y / 2, z / 2)];
                int32_t& fine = tree.levels[region.level].grid[tree.GridIndex(region.level, x, y, z)];
                assert(parent != GAS_TREE_ABSENT && fine == GAS_TREE_ABSENT);
                parent = GAS_TREE_COVERED;
                fine = 0;
            }
        }

        // 置いたセルに番号を振り、層の基準 + 風で埋める(レベル 0 から順に、x が最も速く回る順)
        void NumberCells(GasTree& tree) {
            for (uint32_t level = 0; level < tree.levels.size(); ++level) {
                GasTreeLevel& grid = tree.levels[level];
                for (uint32_t index = 0; index < grid.grid.size(); ++index) {
                    if (grid.grid[index] != 0)
                        continue;

                    const uint32_t x = index % grid.size[0];
                    const uint32_t y = (index / grid.size[0]) % grid.size[1];
                    const uint32_t z = index / (grid.size[0] * grid.size[1]);
                    grid.grid[index] = (int32_t)tree.cells.size();
                    tree.cells.push_back(grid.ghost[z]);
                    tree.places.push_back({.level = level, .coordinate = {x, y, z}});
                }
            }

            tree.fractions.assign(tree.cells.size(), GasTreeFraction{});
        }

    }  // namespace

    GasTree MakeGasTree(const GasConfig& config, std::span<const GasTreeRegion> regions,
                        std::span<const uint32_t> fractionsQ32, uint32_t temperatureMilliKelvin,
                        uint64_t surfacePressureMicroPascal, const GasTreeOptions& options) {
        assert(config.eddyViscosityMm2PerS ==
               0);  // 気体の粘り(T-0758)は 1 レベルの箱と多重解像度の 1 段だけ(凍結の木には入れない)
        uint32_t maxLevel = 0;
        for (const GasTreeRegion& region : regions) {
            assert(region.origin[0] % 2 == 0 && region.origin[1] % 2 == 0 && region.origin[2] % 2 == 0);
            assert(region.size[0] % 2 == 0 && region.size[1] % 2 == 0 && region.size[2] % 2 == 0);
            maxLevel = std::max(maxLevel, region.level);
        }

        // 木の成分の数に上限は無い(GasPerSpecies)。丸めの鍵は設定のまま(欄の番号か物質 ID。T-0598)

        // --- 木の形: レベルごとの大きさ・小刻みの指数 σ(T-0320)・セルの表 ---
        assert(maxLevel <= MAX_TREE_LEVEL);
        GasTree tree;
        tree.config = config;
        tree.levels.resize(maxLevel + 1);
        for (uint32_t level = 0; level <= maxLevel; ++level) {
            GasTreeLevel& grid = tree.levels[level];
            grid.size = {config.sizeX << level, config.sizeY << level, config.sizeZ << level};
            if (level > options.subcycleBaseLevel)
                grid.subcycleExponent = std::min(level - options.subcycleBaseLevel, options.maxSubcycleGap);

            grid.grid.assign((size_t)grid.size[0] * grid.size[1] * grid.size[2], level == 0 ? 0 : GAS_TREE_ABSENT);
        }

        tree.maxSubcycleExponent = tree.levels[maxLevel].subcycleExponent;

        // 安定の条件: どのレベルも ν = c̃ Δt ÷ Δx × 2^(k − σ) ≤ 0.6(σ はレベルごとの小刻みの指数。T-0320)
        assert(config.soundSpeedMmPerS <= GasTreeSoundSpeedLimitMmPerS(tree));

        // --- 環境から決まる値(係数・層の基準・α・頭打ちの温度)---
        BuildLevelEnvironments(tree, fractionsQ32, temperatureMilliKelvin, surfacePressureMicroPascal);

        // --- 粗いレベルの範囲から順に細かくする ---
        for (uint32_t level = 1; level <= maxLevel; ++level) {
            for (const GasTreeRegion& region : regions) {
                if (region.level == level)
                    Refine(tree, region);
            }
        }

        NumberCells(tree);

        return tree;
    }

    uint32_t GasTreeFinestShift(const GasTree& tree) {
        uint32_t shift = 0;
        for (uint32_t level = 0; level < tree.levels.size(); ++level)
            shift = std::max(shift, level - tree.levels[level].subcycleExponent);

        return shift;
    }

    uint64_t GasTreeSoundSpeedLimitMmPerS(const GasTree& tree) {
        return GasSoundSpeedLimitMmPerS(tree.config.substeps) >> GasTreeFinestShift(tree);
    }

    std::expected<void, std::string> CheckGasTreeEnvironment(const GasTree& tree, const GasConfig& next) {
        // 箱の作り・音速の基準 0・重力の上限は 1 レベルの箱と同じ決まり
        if (auto checked = CheckGasEnvironment(tree.config, next); !checked)
            return checked;

        // 音速の基準の上限は木の形(ν がいちばん大きいレベル)から。範囲の外の理由は読み込み・ホットリロードと同じ形(CheckWorldPhysics)
        const WorldPhysics world = {.gravityMicrometersPerSecond2 = next.gravityMicrometersPerSecond2,
                                    .soundSpeedMillimetersPerSecond = next.soundSpeedMmPerS};
        return CheckWorldPhysics(world, next.substeps, GasTreeFinestShift(tree));
    }

    void SetGasTreeEnvironment(GasTree& tree, const GasConfig& config, std::span<const uint32_t> fractionsQ32,
                               uint32_t temperatureMilliKelvin, uint64_t surfacePressureMicroPascal) {
        assert(CheckGasTreeEnvironment(tree, config).has_value());
        tree.config = config;
        BuildLevelEnvironments(tree, fractionsQ32, temperatureMilliKelvin, surfacePressureMicroPascal);
    }

    GasDerived DeriveGasTreeCell(const GasTree& tree, uint32_t cellIndex) {
        const GasTreePlace& place = tree.places[cellIndex];
        const GasTreeLevel& level = tree.levels[place.level];
        GasDerived derived = gd::DeriveRaw(tree.config, level.coefficients, tree.cells[cellIndex]);

        // p̃ = α(p − p_基準)。頭打ちの温度より熱いセルは α × T_頭 ÷ T(DeriveGasCell と同じ)
        const int64_t deviation = derived.pressure - level.referenceDerived[place.coordinate[2]].pressure;
        int64_t scaled = fx::FxMulShiftS64(deviation, (int64_t)level.coefficients.soundScale, 32);
        const uint32_t soundTemperature = level.coefficients.soundTemperature;
        if (derived.temperature > soundTemperature)
            scaled = fx::FxMulDivS64(scaled, soundTemperature, derived.temperature);  // 積は 128bit(T-0676)

        derived.scaledPressure = scaled;

        return derived;
    }

    void StepGasTree(GasTree& tree, GasTreeStepStats* stats) {
        const std::vector<TreeFace> layout = EnumerateFaces(tree);
        const size_t count = tree.cells.size();
        TreeWork work;
        work.derived.resize(count);
        work.forced = {.cells = tree.cells, .fractions = tree.fractions};
        work.next = work.forced;
        work.committedOutflow.assign(count, 0);
        work.taken.assign(count, {});
        work.stats = stats;

        // 1 刻み = いちばん細かい小刻み substeps × 2^σmax 回(どのレベルの小刻みもこの中で始まって終わる)
        const uint64_t micros = (uint64_t)tree.config.substeps << tree.maxSubcycleExponent;
        for (uint64_t micro = 0; micro < micros; ++micro)
            MicroStep(tree, layout, micro, work);

        ++tree.tick;
    }

    std::vector<GasTreeFaceLayout> ListGasTreeFaces(const GasTree& tree) {
        // 面を作る時だけの Finer は一覧に残らない(作らないか、外側の隣なら Mixed にしてある)
        const auto toSide = [](const TreeSide& side) {
            GasTreeFaceSide result{.kind = GasTreeSideKind::Cell, .index = side.index, .level = side.level};
            if (side.kind == SideKind::Ghost)
                result.kind = GasTreeSideKind::Ghost;
            else if (side.kind == SideKind::Wall)
                result.kind = GasTreeSideKind::Wall;
            else if (side.kind != SideKind::Cell)
                result.kind = GasTreeSideKind::Mixed;

            return result;
        };

        std::vector<GasTreeFaceLayout> layout;
        for (const TreeFace& face : EnumerateFaces(tree)) {
            assert(face.left.kind != SideKind::Finer && face.right.kind != SideKind::Finer);
            layout.push_back({.left = toSide(face.left),
                              .right = toSide(face.right),
                              .leftOuter = toSide(face.leftOuter),
                              .rightOuter = toSide(face.rightOuter),
                              .axis = face.axis,
                              .level = face.level,
                              .id = face.id});
        }

        return layout;
    }

    GasTreeTotals SumGasTree(const GasTree& tree) {
        GasTreeTotals totals;
        for (uint32_t index = 0; index < tree.cells.size(); ++index) {
            const uint32_t shift = 3 * tree.places[index].level;
            const GasCell& cell = tree.cells[index];
            const GasTreeFraction& fraction = tree.fractions[index];
            for (uint32_t s = 0; s < tree.config.speciesCount; ++s) {
                AddScaled(totals.amounts[s].whole, totals.amounts[s].fraction, (int64_t)cell.amounts[s], shift);
                AddFraction(totals.amounts[s], fraction.amounts[s], shift);
            }

            AddScaled(totals.energy.whole, totals.energy.fraction, cell.energy, shift);
            AddFraction(totals.energy, fraction.energy, shift);
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                AddScaled(totals.momentum[axis].whole, totals.momentum[axis].fraction, cell.momentum[axis], shift);
                AddFraction(totals.momentum[axis], fraction.momentum[axis], shift);
            }
        }

        return totals;
    }

}  // namespace bicameral::sim
