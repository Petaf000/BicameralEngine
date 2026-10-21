// cascade_light.cpp — 距離ごとの段の CPU リファレンス(cascade_light.h)。光線 1 本の本体は shaders/common/cascade_light.hlsli の RcWalkRay。
// 足し算はすべて可換な整数の足し算(袋の量・量 × 位置の和・セルごとの吸った・持ち越し)なので、光線と袋をどの順に回しても同じ結果(R3・R4)。
// 袋と持ち越しは CPU では std::map(GPU は T-0959 で 64bit の atomic の足し算の表にする)。
#include "sim/cascade_light.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace bicameral::sim {

    namespace mr = ::bicameral::multires;
    namespace rx = ::bicameral::reaction;

    namespace {

        constexpr uint32_t TEXEL_LIMIT = 1u << 10;  // 升目の 1 辺の上限(RcFaceWeight の約束)
        constexpr uint32_t LEVEL_LIMIT = 8;
        constexpr int64_t CELL_POSITION = int64_t{1} << rx::RC_POSITION_SHIFT;
        constexpr uint32_t CHILD_TEXELS = 4;

        // 乱数の用途(R6)
        constexpr uint32_t PURPOSE_JITTER = 0x52430001u;
        constexpr uint32_t PURPOSE_REMAINDER = 0x52430002u;
        constexpr uint32_t PURPOSE_RESERVOIR = 0x52430003u;
        constexpr uint32_t PURPOSE_FACE_POINT = 0x52430004u;

        using Cell3 = std::array<int64_t, 3>;

        // --- RcWalkRay に渡す道具(cascade_light.hlsli の約束。値で渡るので指すだけ)---

        struct GridView {
            const CascadeGrid* grid = nullptr;

            [[nodiscard]] bool Contains(int64_t x, int64_t y, int64_t z) const { return grid->Contains(x, y, z); }

            [[nodiscard]] rx::RcCellLight Light(int64_t x, int64_t y, int64_t z) const {
                return grid->lights[grid->IndexOf(x, y, z)];
            }
        };

        // 持ち越しの鍵 = セル × 12 + 面 × 2 + 帯(昇順 = セル・面・帯の順)
        using CarryMap = std::map<uint64_t, uint64_t>;

        uint64_t CarryKey(uint64_t cell, uint32_t face, uint32_t band) {
            return (cell * rx::RC_FACES * rx::LT_BANDS) + (uint64_t{face} * rx::LT_BANDS) + band;
        }

        struct SinkView {
            const CascadeGrid* grid = nullptr;
            std::vector<uint64_t>* absorbed = nullptr;
            CarryMap* carry = nullptr;

            void Deposit(int64_t x, int64_t y, int64_t z, uint32_t band, uint32_t face, uint64_t absorbedAmount,
                         uint64_t reflectedAmount) const {
                const size_t cell = grid->IndexOf(x, y, z);
                (*absorbed)[(cell * rx::LT_BANDS) + band] += absorbedAmount;
                if (reflectedAmount != 0)
                    (*carry)[CarryKey(cell, face, band)] += reflectedAmount;
            }
        };

        // --- 袋(段 k ≥ 1 の場所 = レベル −k のブロック × 升目)---

        // (ブロック x・y・z, 升目の面・i・j)。std::map の順 = 回す順(結果は順によらない)
        using BagKey = std::tuple<int64_t, int64_t, int64_t, uint32_t, uint32_t, uint32_t>;

        // 袋の出どころは、袋に入った光線のうち 1 本を量に比例した確率で選んだその出どころ(重み付きの溜め。ReSTIR の reservoir と同じ形)。
        // 鍵 = −ln(u) ÷ 量(u = (種・刻み・光線) のハッシュ)の一番小さいもの。min の畳み込みなので順によらない(R3・R4。GPU は 64bit の atomic min)。
        // 重心 1 点から出すと向かいの壁の真ん中が焦点になり、広がりで散らすと遮る物の中から出る光線ができた(どちらも閉じた箱で正味の流れ。T-0958)
        struct Bag {
            uint64_t energyShort = 0;
            uint64_t energyLong = 0;
            uint64_t chosenKey = UINT64_MAX;
            Cell3 chosenOrigin{};
        };

        using BagMap = std::map<BagKey, Bag>;

        // 量 × 重み ÷ 重みの和(床。重み ≤ 重みの和なので商は 64bit に収まる)
        uint64_t Share(uint64_t energy, uint64_t weight, uint64_t weightSum) {
            return fx::FxDivU128By64(fx::FxMulU64Full(energy, weight), weightSum).quotient;
        }

        uint64_t BagId(uint32_t level, const BagKey& key) {
            uint64_t hash = fx::FxHashCombine(level, static_cast<uint64_t>(std::get<0>(key)));
            hash = fx::FxHashCombine(hash, static_cast<uint64_t>(std::get<1>(key)));
            hash = fx::FxHashCombine(hash, static_cast<uint64_t>(std::get<2>(key)));
            hash = fx::FxHashCombine(hash, std::get<3>(key));
            hash = fx::FxHashCombine(hash, std::get<4>(key));

            return fx::FxHashCombine(hash, std::get<5>(key));
        }

        // --- 段 0 の重み(放つ面ごとに、升目の Lambert の重み。セルによらないので前もって作る)---

        struct TexelWeight {
            uint32_t face = 0;
            uint32_t i = 0;
            uint32_t j = 0;
            uint64_t weight = 0;
        };

        struct FaceWeights {
            std::array<std::vector<TexelWeight>, rx::RC_FACES> texels;  // 重みが 0 でない升目だけ
            std::array<uint64_t, rx::RC_FACES> sums{};
        };

        FaceWeights MakeFaceWeights(uint32_t texels) {
            FaceWeights weights;
            for (uint32_t source = 0; source < rx::RC_FACES; ++source) {
                for (uint32_t face = 0; face < rx::RC_FACES; ++face) {
                    for (uint32_t j = 0; j < texels; ++j) {
                        for (uint32_t i = 0; i < texels; ++i) {
                            const uint64_t weight = rx::RcFaceWeight(texels, face, i, j, source);
                            if (weight == 0)
                                continue;

                            weights.texels[source].push_back({.face = face, .i = i, .j = j, .weight = weight});
                            weights.sums[source] += weight;
                        }
                    }
                }
            }

            return weights;
        }

        // --- 1 刻みの作業 ---

        struct TickWork {
            const CascadeGrid* grid = nullptr;
            const CascadeParams* params = nullptr;
            uint64_t tick = 0;
            CascadeTickResult* result = nullptr;
            CarryMap carry;
            std::vector<BagMap> bags;  // [段]
        };

        // 段 level の光線(升目 face・i・j)。home = その段のブロックの番号(レベル −level の座標)
        struct LevelRay {
            rx::RcRay ray{};
            uint32_t level = 0;
            uint32_t face = 0;
            uint32_t i = 0;
            uint32_t j = 0;
            Cell3 home{};
            uint64_t id = 0;  // ずらし・余りの乱数の鍵
        };

        void AddLedger(CascadeTickResult& result, const rx::RcWalk& walk) {
            result.ledger[rx::LT_BAND_SHORT].absorbed += walk.absorbedShort;
            result.ledger[rx::LT_BAND_SHORT].reflected += walk.reflectedShort;
            result.ledger[rx::LT_BAND_SHORT].escaped += walk.escapedShort;
            result.ledger[rx::LT_BAND_LONG].absorbed += walk.absorbedLong;
            result.ledger[rx::LT_BAND_LONG].reflected += walk.reflectedLong;
            result.ledger[rx::LT_BAND_LONG].escaped += walk.escapedLong;
            result.steps += walk.steps;
        }

        // 段 level の区間(向きに沿った座標)。段 0 は放つセルから、段 k ≥ 1 は段 k のブロックの始まり + 区間の長さから。
        // 終わり = 段 k + 1 のブロックの始まり + 区間の長さ × 2^(k+1)(RcIntervalEnd)。最後の段は世界の端まで
        void SetInterval(rx::RcRay& ray, const Cell3& home, uint32_t level, const CascadeParams& params) {
            const int64_t start = rx::RcBlockStartAlong(home[ray.axis], level, ray.sign);
            const int64_t interval = int64_t{params.intervalBlocks} << level;
            ray.firstA = level == 0 ? start : start + interval;
            ray.endA = level + 1 >= params.levels ? rx::RC_ENDLESS
                                                  : rx::RcIntervalEnd(start, level, params.intervalBlocks);
        }

        // 鍵 = −ln(u) ÷ 量(Q32 × 2^32 ÷ 量。指数分布の最小なので、量に比例した確率で選ばれる)。u は (0, 1] の Q32。
        // 量が 64 未満の光線はほぼ選ばれない(鍵を最大の手前に)。同じ鍵は位置の小さい方(AddToBag)
        uint64_t ReservoirKey(uint64_t hash, uint64_t energy) {
            constexpr uint64_t LN_2_POW_32_Q32 = 95265423104;  // 32 ln 2(Q32)
            constexpr uint64_t SMALL_ENERGY = 64;
            if (energy < SMALL_ENERGY)
                return UINT64_MAX - 1;

            const uint64_t unit = (hash >> 32) + 1;  // (0, 2^32]
            const auto lnUnit = static_cast<uint64_t>(fx::FxLnU64(unit));
            const uint64_t minusLn = LN_2_POW_32_Q32 > lnUnit ? LN_2_POW_32_Q32 - lnUnit : 0;  // −ln(u / 2^32)(< 2^37)
            const fx::FxU128 numerator = {.hi = minusLn >> 32, .lo = minusLn << 32};

            return fx::FxDivU128By64(numerator, energy).quotient;
        }

        void AddToBag(Bag& bag, uint64_t energyShort, uint64_t energyLong, const Cell3& origin, uint64_t hash) {
            bag.energyShort += energyShort;
            bag.energyLong += energyLong;
            const uint64_t key = ReservoirKey(hash, energyShort + energyLong);
            if (key < bag.chosenKey || (key == bag.chosenKey && origin < bag.chosenOrigin)) {
                bag.chosenKey = key;
                bag.chosenOrigin = origin;
            }
        }

        // 光線を升目へ向ける(升目の中はハッシュでずらす。R6)
        rx::RcRay AimLevelRay(const TickWork& work, const LevelRay& levelRay) {
            const uint32_t texels = work.params->baseTexels << levelRay.level;
            const uint64_t hash = fx::FxHash64(work.params->seed, work.tick, levelRay.id, PURPOSE_JITTER);
            const auto jitterU = static_cast<uint32_t>(hash & (rx::RC_JITTER - 1));
            const auto jitterV = static_cast<uint32_t>((hash >> 4) & (rx::RC_JITTER - 1));

            return rx::RcAimRay(levelRay.ray, levelRay.face, texels, levelRay.i, levelRay.j, jitterU, jitterV);
        }

        // 区間の終わりに残った量を段 level + 1 の袋(子の升目 4 つ)へ立体角の重みで分ける(余りはハッシュで決めた子へ)
        void PassToNextLevel(TickWork& work, const LevelRay& from, const rx::RcWalk& walk) {
            if (walk.remainingShort == 0 && walk.remainingLong == 0)
                return;

            // 子の袋の鍵と、その袋の光線が向く向き(ずらしは袋の鍵から決まるので、出どころによらず先に分かる)の立体角の密度で分ける
            const uint32_t next = from.level + 1;
            const Cell3 home = {from.home[0] >> 1, from.home[1] >> 1, from.home[2] >> 1};
            std::array<BagKey, CHILD_TEXELS> keys{};
            std::array<uint64_t, CHILD_TEXELS> weights{};
            uint64_t weightSum = 0;
            for (uint32_t child = 0; child < CHILD_TEXELS; ++child) {
                keys[child] = {
                    home[0], home[1], home[2], from.face, (2 * from.i) + (child & 1u), (2 * from.j) + (child >> 1)};
                const LevelRay probe{.level = next,
                                     .face = from.face,
                                     .i = std::get<4>(keys[child]),
                                     .j = std::get<5>(keys[child]),
                                     .id = BagId(next, keys[child])};
                weights[child] = rx::RcAimedSolidAngleWeight(AimLevelRay(work, probe));
                weightSum += weights[child];
            }

            std::array<uint64_t, CHILD_TEXELS> shortShares{};
            std::array<uint64_t, CHILD_TEXELS> longShares{};
            uint64_t givenShort = 0;
            uint64_t givenLong = 0;
            for (uint32_t child = 0; child < CHILD_TEXELS; ++child) {
                shortShares[child] = Share(walk.remainingShort, weights[child], weightSum);
                longShares[child] = Share(walk.remainingLong, weights[child], weightSum);
                givenShort += shortShares[child];
                givenLong += longShares[child];
            }

            const uint64_t hash = fx::FxHash64(work.params->seed, work.tick, from.id, PURPOSE_REMAINDER);
            const uint32_t lucky = fx::FxRandomBelow(hash, CHILD_TEXELS);
            shortShares[lucky] += walk.remainingShort - givenShort;
            longShares[lucky] += walk.remainingLong - givenLong;

            const Cell3 origin = {from.ray.originX, from.ray.originY, from.ray.originZ};
            for (uint32_t child = 0; child < CHILD_TEXELS; ++child) {
                if (shortShares[child] == 0 && longShares[child] == 0)
                    continue;

                const BagKey& key = keys[child];
                const uint64_t pick = fx::FxHash64(work.params->seed, work.tick, fx::FxHashCombine(from.id, child),
                                                   PURPOSE_RESERVOIR);
                AddToBag(work.bags[next][key], shortShares[child], longShares[child], origin, pick);
            }
        }

        // 向けた光線の区間を歩かせ、残りを次の段へ
        void WalkLevelRay(TickWork& work, LevelRay levelRay) {
            const CascadeParams& params = *work.params;
            levelRay.ray = AimLevelRay(work, levelRay);
            SetInterval(levelRay.ray, levelRay.home, levelRay.level, params);
            // 段 k ≥ 1 の光線は区間の始まりの板(向きの軸の面)から最初のセルに入る
            if (levelRay.level != 0)
                levelRay.ray.firstFace = rx::RcEnteredFace(levelRay.ray.axis, levelRay.ray.sign);

            const SinkView sink{.grid = work.grid, .absorbed = &work.result->absorbed, .carry = &work.carry};
            const rx::RcWalk walk = rx::RcWalkRay(GridView{.grid = work.grid}, sink, levelRay.ray);
            work.result->rays += 1;
            work.result->raysPerLevel[levelRay.level] += 1;
            AddLedger(*work.result, walk);
            if (levelRay.level + 1 < params.levels) {
                PassToNextLevel(work, levelRay, walk);
                return;
            }

            // 最後の段は世界の端まで歩くので残らない(歩みの上限で止まった時の保険: 外へ出たとして数える)
            work.result->ledger[rx::LT_BAND_SHORT].escaped += walk.remainingShort;
            work.result->ledger[rx::LT_BAND_LONG].escaped += walk.remainingLong;
        }

        // --- 段 0: 面から放つ ---

        struct FaceSource {
            uint64_t energyShort = 0;
            uint64_t energyLong = 0;
        };

        // 鍵 = セル × 6 + 面
        using SourceMap = std::map<uint64_t, FaceSource>;

        Cell3 CellOf(const CascadeGrid& grid, size_t index) {
            const auto flat = static_cast<int64_t>(index);
            const int64_t x = flat % grid.size[0];
            const int64_t y = (flat / grid.size[0]) % grid.size[1];
            const int64_t z = flat / (grid.size[0] * grid.size[1]);

            return {grid.low[0] + x, grid.low[1] + y, grid.low[2] + z};
        }

        // 光線の出どころ: 面の上の点(1/256 セル)。面の中の位置は光線ごとにハッシュで散らす(R6)。
        // 面の真ん中の 1 点から放つと、点から見た面と面から見た点の見え方が向き合う 2 面で揃わず、同じ温度の閉じた箱で
        // 隣り合うセルの間に正味の流れが残る(T-0958 の測り: 17 %)。面の上で一様に散らすと期待値で対称(相反)になる
        Cell3 FacePoint(const TickWork& work, const Cell3& cell, uint32_t face, uint64_t rayId) {
            const uint64_t hash = fx::FxHash64(work.params->seed, work.tick, rayId, PURPOSE_FACE_POINT);
            const uint32_t axis = face >> 1;
            Cell3 point{};
            for (uint32_t k = 0; k < 3; ++k) {
                const auto inside = static_cast<int64_t>((hash >> (k * 8)) & (CELL_POSITION - 1));
                point[k] = (cell[k] * CELL_POSITION) + inside;
            }

            point[axis] = (cell[axis] + ((face & 1u) == 0 ? 1 : 0)) * CELL_POSITION;

            return point;
        }

        // 面 1 つの光を、外向きの升目へ「ずらした向きの重み」(RcAimedFaceWeight)で整数に分けて放つ(余りはハッシュで決めた升目へ)
        void EmitFace(TickWork& work, const FaceWeights& weights, uint64_t sourceKey, const FaceSource& source) {
            const auto cellIndex = static_cast<size_t>(sourceKey / rx::RC_FACES);
            const auto sourceFace = static_cast<uint32_t>(sourceKey % rx::RC_FACES);
            const std::vector<TexelWeight>& texels = weights.texels[sourceFace];
            std::vector<uint64_t> aimedWeights(texels.size());
            uint64_t weightSum = 0;
            for (size_t t = 0; t < texels.size(); ++t) {
                const LevelRay probe{.level = 0,
                                     .face = texels[t].face,
                                     .i = texels[t].i,
                                     .j = texels[t].j,
                                     .id = fx::FxHashCombine(sourceKey, t)};
                aimedWeights[t] = rx::RcAimedFaceWeight(AimLevelRay(work, probe), sourceFace);
                weightSum += aimedWeights[t];
            }

            if (weightSum == 0)
                return;

            std::vector<uint64_t> shortShares(texels.size());
            std::vector<uint64_t> longShares(texels.size());
            uint64_t givenShort = 0;
            uint64_t givenLong = 0;
            for (size_t t = 0; t < texels.size(); ++t) {
                shortShares[t] = Share(source.energyShort, aimedWeights[t], weightSum);
                longShares[t] = Share(source.energyLong, aimedWeights[t], weightSum);
                givenShort += shortShares[t];
                givenLong += longShares[t];
            }

            const uint64_t hash = fx::FxHash64(work.params->seed, work.tick, sourceKey, PURPOSE_REMAINDER);
            uint32_t lucky = fx::FxRandomBelow(hash, static_cast<uint32_t>(texels.size()));
            while (aimedWeights[lucky] == 0)
                lucky = (lucky + 1) % static_cast<uint32_t>(texels.size());

            shortShares[lucky] += source.energyShort - givenShort;
            longShares[lucky] += source.energyLong - givenLong;

            const Cell3 cell = CellOf(*work.grid, cellIndex);
            LevelRay levelRay{.level = 0, .home = cell};
            levelRay.ray.firstFace = sourceFace ^ 1u;  // 面を越えた先のセルには、向かい合う面から入る
            for (size_t t = 0; t < texels.size(); ++t) {
                if (shortShares[t] == 0 && longShares[t] == 0)
                    continue;

                levelRay.id = fx::FxHashCombine(sourceKey, t);
                const Cell3 origin = FacePoint(work, cell, sourceFace, levelRay.id);
                levelRay.ray.originX = origin[0];
                levelRay.ray.originY = origin[1];
                levelRay.ray.originZ = origin[2];
                levelRay.ray.energyShort = shortShares[t];
                levelRay.ray.energyLong = longShares[t];
                levelRay.face = texels[t].face;
                levelRay.i = texels[t].i;
                levelRay.j = texels[t].j;
                WalkLevelRay(work, levelRay);
            }
        }

        rx::RcCellLight LightAcross(const CascadeGrid& grid, const Cell3& cell, uint32_t face) {
            Cell3 across = cell;
            across[face >> 1] += (face & 1u) == 0 ? 1 : -1;
            if (!grid.Contains(across[0], across[1], across[2]))
                return {};

            return grid.lights[grid.IndexOf(across[0], across[1], across[2])];
        }

        // 熱で放つ面(セルから引く量)と、前の刻みに返された光を、(セル・面) ごとに 1 つの放つ面にまとめる
        SourceMap CollectSources(TickWork& work, const CascadeCarry& carry) {
            const CascadeGrid& grid = *work.grid;
            CascadeTickResult& result = *work.result;
            SourceMap sources;
            for (size_t index = 0; index < grid.CellCount(); ++index) {
                const uint32_t temperature = grid.temperatureMk[index];
                if (temperature == 0 || temperature < work.params->quietBelowMk)
                    continue;

                const Cell3 cell = CellOf(grid, index);
                for (uint32_t face = 0; face < rx::RC_FACES; ++face) {
                    const rx::RcBands bands = rx::RcFaceEmission(temperature, grid.lights[index],
                                                                 LightAcross(grid, cell, face));
                    if (bands.shortBand == 0 && bands.longBand == 0)
                        continue;

                    result.emitted[(index * rx::LT_BANDS) + rx::LT_BAND_SHORT] += bands.shortBand;
                    result.emitted[(index * rx::LT_BANDS) + rx::LT_BAND_LONG] += bands.longBand;
                    result.ledger[rx::LT_BAND_SHORT].emitted += bands.shortBand;
                    result.ledger[rx::LT_BAND_LONG].emitted += bands.longBand;
                    FaceSource& source = sources[(uint64_t{index} * rx::RC_FACES) + face];
                    source.energyShort += bands.shortBand;
                    source.energyLong += bands.longBand;
                }
            }

            for (const CascadeCarryEntry& entry : carry.entries) {
                result.ledger[entry.band].carriedIn += entry.amount;
                FaceSource& source = sources[(uint64_t{entry.cell} * rx::RC_FACES) + entry.face];
                (entry.band == rx::LT_BAND_SHORT ? source.energyShort : source.energyLong) += entry.amount;
            }

            return sources;
        }

        // 段 level の袋を全部、袋の選んだ出どころから歩かせる
        void WalkBags(TickWork& work, uint32_t level, bool reverse) {
            const BagMap bags = std::move(work.bags[level]);
            auto walkOne = [&](const BagKey& key, const Bag& bag) {
                const uint64_t total = bag.energyShort + bag.energyLong;
                LevelRay levelRay{.level = level,
                                  .face = std::get<3>(key),
                                  .i = std::get<4>(key),
                                  .j = std::get<5>(key),
                                  .home = {std::get<0>(key), std::get<1>(key), std::get<2>(key)},
                                  .id = BagId(level, key)};
                if (total == 0)
                    return;

                const Cell3& origin = bag.chosenOrigin;
                levelRay.ray.originX = origin[0];
                levelRay.ray.originY = origin[1];
                levelRay.ray.originZ = origin[2];
                levelRay.ray.energyShort = bag.energyShort;
                levelRay.ray.energyLong = bag.energyLong;
                WalkLevelRay(work, levelRay);
            };

            if (reverse) {
                for (auto it = bags.rbegin(); it != bags.rend(); ++it)
                    walkOne(it->first, it->second);

                return;
            }

            for (const auto& [key, bag] : bags)
                walkOne(key, bag);
        }

        CascadeCarry ToCarry(const CarryMap& map) {
            CascadeCarry carry;
            carry.entries.reserve(map.size());
            for (const auto& [key, amount] : map) {
                if (amount == 0)
                    continue;

                const uint64_t cell = key / (rx::RC_FACES * rx::LT_BANDS);
                const uint64_t rest = key % (rx::RC_FACES * rx::LT_BANDS);
                carry.entries.push_back({.cell = static_cast<uint32_t>(cell),
                                         .face = static_cast<uint8_t>(rest / rx::LT_BANDS),
                                         .band = static_cast<uint8_t>(rest % rx::LT_BANDS),
                                         .amount = amount});
            }

            return carry;
        }

    }  // namespace

    std::expected<CascadeGrid, std::string> MakeCascadeGrid(const MultiresNest& nest, const BakedReactionTable& table) {
        const mr::WeWorld& world = nest.edges.world;
        if (world.highX <= world.lowX || world.highY <= world.lowY || world.highZ <= world.lowZ)
            return std::unexpected("世界の箱(nest.edges.world)が空");

        if (world.rootLevel != 0 || nest.capacity.rootLevel != 0)
            return std::unexpected("今はレベル 0 の根だけの木(細かい葉・粗い根は T-0972)");

        CascadeGrid grid;
        grid.low = {world.lowX, world.lowY, world.lowZ};
        grid.size = {world.highX - world.lowX, world.highY - world.lowY, world.highZ - world.lowZ};
        const auto count = static_cast<size_t>(grid.size[0] * grid.size[1] * grid.size[2]);
        if (count >= (size_t{1} << 31))
            return std::unexpected("世界の箱のセルが多すぎる(2^31 まで)");

        grid.lights.assign(count, {});
        grid.temperatureMk.assign(count, 0);
        std::vector<bool> seen(count, false);
        const ReactionTableView view = table.View();
        struct Lights {
            const std::vector<rx::LtSpeciesLight>* lights = nullptr;

            [[nodiscard]] rx::LtSpeciesLight Light(uint32_t species) const { return (*lights)[species]; }
        };
        const Lights lights{.lights = &table.speciesLight};
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const mr::MrBlock& block = nest.blocks[slot];
            if (block.kind != mr::MR_BLOCK_REAL)
                continue;

            if (block.level != 0)
                return std::unexpected("細かくした木はまだ読めない(T-0972)");

            for (uint32_t index = 0; index < mr::MR_BLOCK_CELLS; ++index) {
                const int64_t x = block.originX + mr::MrCellX(index);
                const int64_t y = block.originY + mr::MrCellY(index);
                const int64_t z = block.originZ + mr::MrCellZ(index);
                if (!grid.Contains(x, y, z))
                    continue;

                const rx::RxCell cell = LoadNestCell(nest, slot, index);
                const rx::LtCellBand shortBand = rx::LtCellBandOf(lights, cell, rx::LT_BAND_SHORT);
                const rx::LtCellBand longBand = rx::LtCellBandOf(lights, cell, rx::LT_BAND_LONG);
                const size_t at = grid.IndexOf(x, y, z);
                grid.lights[at] = {.depthShortQ32 = rx::RdOpticalDepthQ32(shortBand.absorption),
                                   .depthLongQ32 = rx::RdOpticalDepthQ32(longBand.absorption),
                                   .reflectanceShortQ16 = shortBand.reflectanceQ16,
                                   .reflectanceLongQ16 = longBand.reflectanceQ16};
                const int32_t temperature = rx::RxComputeThermal(view, cell).temperature;
                grid.temperatureMk[at] = temperature > 0 ? static_cast<uint32_t>(temperature) : 0u;
                seen[at] = true;
            }
        }

        if (std::find(seen.begin(), seen.end(), false) != seen.end())
            return std::unexpected("世界の箱の中に根の無い所がある");

        return grid;
    }

    std::expected<void, std::string> CheckCascadeParams(const CascadeParams& params) {
        if (params.levels == 0 || params.levels > LEVEL_LIMIT)
            return std::unexpected("段の数は 1〜8");

        if (params.baseTexels == 0 || (params.baseTexels << (params.levels - 1)) > TEXEL_LIMIT)
            return std::unexpected("升目の 1 辺(n0 × 2^(段 − 1))は 1〜1024");

        if (params.intervalBlocks == 0 || params.intervalBlocks > 64)
            return std::unexpected("区間の長さは 1〜64 ブロック");

        return {};
    }

    CascadeTickResult StepCascadeLight(const CascadeGrid& grid, const CascadeCarry& carry, const CascadeParams& params,
                                       uint64_t tick, bool reverse) {
        CascadeTickResult result;
        result.emitted.assign(grid.CellCount() * rx::LT_BANDS, 0);
        result.absorbed.assign(grid.CellCount() * rx::LT_BANDS, 0);
        TickWork work{.grid = &grid, .params = &params, .tick = tick, .result = &result, .carry = {}, .bags = {}};
        work.bags.resize(params.levels);

        // --- 段 0: 面から放つ ---
        const FaceWeights weights = MakeFaceWeights(params.baseTexels);
        const SourceMap sources = CollectSources(work, carry);
        if (reverse) {
            for (auto it = sources.rbegin(); it != sources.rend(); ++it)
                EmitFace(work, weights, it->first, it->second);
        } else {
            for (const auto& [key, source] : sources)
                EmitFace(work, weights, key, source);
        }

        // --- 段 1 から: 袋の重心から ---
        for (uint32_t level = 1; level < params.levels; ++level)
            WalkBags(work, level, reverse);

        result.carry = ToCarry(work.carry);

        return result;
    }

    uint64_t HashCascadeCarry(const CascadeCarry& carry) {
        uint64_t hash = fx::FxMix64(carry.entries.size());
        for (const CascadeCarryEntry& entry : carry.entries) {
            hash = fx::FxHashCombine(hash, entry.cell);
            hash = fx::FxHashCombine(hash, (uint64_t{entry.face} << 8) | entry.band);
            hash = fx::FxHashCombine(hash, entry.amount);
        }

        return hash;
    }

    uint64_t CascadeCarrySaveBytes(const CascadeCarry& carry) {
        constexpr uint64_t CELL_BYTES = 4 + 2;  // 番号 + (面 × 帯)の 12bit の印
        constexpr uint64_t VALUE_BYTES = 4;
        uint64_t bytes = 0;
        for (size_t at = 0; at < carry.entries.size(); ++at) {
            const bool newCell = at == 0 || carry.entries[at - 1].cell != carry.entries[at].cell;
            bytes += (newCell ? CELL_BYTES : 0) + VALUE_BYTES;
        }

        return bytes;
    }

}  // namespace bicameral::sim
