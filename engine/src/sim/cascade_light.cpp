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
        constexpr uint32_t RAYS_PER_BAG_LIMIT = 64;
        // 溜めの出どころを 15bit × 3 に詰めるので、世界の 1 辺は 127 セルまで(RcPackReservoir。ADR-0432)
        constexpr int64_t SIZE_LIMIT = ((int64_t{1} << rx::RC_ORIGIN_BITS) >> rx::RC_POSITION_SHIFT) - 1;
        constexpr uint32_t PURPOSE_REMAINDER = rx::RC_PURPOSE_REMAINDER;

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

        // 袋の量・量が出た軸ごとの和・溜め(RcPackReservoir の min。重み付きの溜め: 袋に入った光線から量に比例して 1 本の出どころを選ぶ。
        // min の畳み込みなので順によらない。GPU は 64bit の atomic min 1 回。ADR-0419・ADR-0432)
        struct Bag {
            uint64_t energyShort = 0;
            uint64_t energyLong = 0;
            std::array<uint64_t, 3> lobeShort{};
            std::array<uint64_t, 3> lobeLong{};
            uint64_t chosen = rx::RC_RESERVOIR_EMPTY;
        };

        using BagMap = std::map<BagKey, Bag>;

        // RcPassToNextLevel に渡す道具
        struct BagSink {
            BagMap* bags = nullptr;

            void Add(const rx::RcBagShare& share) const {
                Bag& bag = (*bags)[{share.homeX, share.homeY, share.homeZ, share.face, share.i, share.j}];
                bag.energyShort += share.energyShort;
                bag.energyLong += share.energyLong;
                bag.lobeShort[0] += share.lobeShortX;
                bag.lobeShort[1] += share.lobeShortY;
                bag.lobeShort[2] += share.lobeShortZ;
                bag.lobeLong[0] += share.lobeLongX;
                bag.lobeLong[1] += share.lobeLongY;
                bag.lobeLong[2] += share.lobeLongZ;
                bag.chosen = std::min(bag.chosen, share.chosen);
            }
        };

        rx::RcBagShare ShareOf(uint32_t level, const BagKey& key, const Bag& bag) {
            rx::RcBagShare share{};
            share.homeX = std::get<0>(key);
            share.homeY = std::get<1>(key);
            share.homeZ = std::get<2>(key);
            share.energyShort = bag.energyShort;
            share.energyLong = bag.energyLong;
            share.lobeShortX = bag.lobeShort[0];
            share.lobeShortY = bag.lobeShort[1];
            share.lobeShortZ = bag.lobeShort[2];
            share.lobeLongX = bag.lobeLong[0];
            share.lobeLongY = bag.lobeLong[1];
            share.lobeLongZ = bag.lobeLong[2];
            share.chosen = bag.chosen;
            share.level = level;
            share.face = std::get<3>(key);
            share.i = std::get<4>(key);
            share.j = std::get<5>(key);

            return share;
        }

        // --- 段 0 の升目(放つ面ごとに、升目の真ん中の向きが外を向くもの。セルによらないので前もって作る)---

        struct TexelOfFace {
            uint32_t face = 0;
            uint32_t i = 0;
            uint32_t j = 0;
        };

        std::array<std::vector<TexelOfFace>, rx::RC_FACES> MakeFaceTexels(uint32_t texels) {
            std::array<std::vector<TexelOfFace>, rx::RC_FACES> list;
            for (uint32_t source = 0; source < rx::RC_FACES; ++source) {
                for (uint32_t face = 0; face < rx::RC_FACES; ++face) {
                    for (uint32_t j = 0; j < texels; ++j) {
                        for (uint32_t i = 0; i < texels; ++i) {
                            if (rx::RcFaceWeight(texels, face, i, j, source) != 0)
                                list[source].push_back({.face = face, .i = i, .j = j});
                        }
                    }
                }
            }

            return list;
        }

        // --- 1 刻みの作業 ---

        struct TickWork {
            const CascadeGrid* grid = nullptr;
            const CascadeParams* params = nullptr;
            rx::RcStepConstants constants{};
            CascadeTickResult* result = nullptr;
            CarryMap carry;
            std::vector<BagMap> bags;  // [段]
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

        // 段の光線 1 本を歩かせ、残りを次の段の袋へ(最後の段は世界の端まで)
        void WalkLevelRay(TickWork& work, const rx::RcLevelRay& levelRay) {
            const CascadeGrid& grid = *work.grid;
            const SinkView sink{.grid = &grid, .absorbed = &work.result->absorbed, .carry = &work.carry};
            const rx::RcWalk walk = rx::RcWalkLevelRay(work.constants, levelRay, GridView{.grid = &grid}, sink);
            work.result->rays += 1;
            work.result->raysPerLevel[levelRay.level] += 1;
            AddLedger(*work.result, walk);
            if (levelRay.level + 1 < work.params->levels) {
                const BagSink bags{.bags = &work.bags[levelRay.level + 1]};
                rx::RcPassToNextLevel(work.constants, levelRay, walk, grid.low[0], grid.low[1], grid.low[2], bags);
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

        rx::RcLevelRay Level0Ray(const TickWork& work, uint64_t sourceKey, const Cell3& cell, uint32_t sourceFace,
                                 size_t texelIndex, const TexelOfFace& texel) {
            return rx::RcLevel0Ray(work.constants, sourceKey, cell[0], cell[1], cell[2], sourceFace,
                                   static_cast<uint32_t>(texelIndex), texel.face, texel.i, texel.j);
        }

        // 面 1 つの光を、外向きの升目へ「ずらした向きの重み」(RcLevel0Weight)で整数に分けて放つ(余りはハッシュで決めた升目へ)
        void EmitFace(TickWork& work, const std::vector<TexelOfFace>& texels, uint64_t sourceKey,
                      const FaceSource& source) {
            const auto sourceFace = static_cast<uint32_t>(sourceKey % rx::RC_FACES);
            const Cell3 cell = CellOf(*work.grid, static_cast<size_t>(sourceKey / rx::RC_FACES));
            std::vector<uint64_t> weights(texels.size());
            uint64_t weightSum = 0;
            for (size_t t = 0; t < texels.size(); ++t) {
                weights[t] = rx::RcLevel0Weight(work.constants,
                                                Level0Ray(work, sourceKey, cell, sourceFace, t, texels[t]), sourceFace);
                weightSum += weights[t];
            }

            if (weightSum == 0)
                return;

            std::vector<uint64_t> shortShares(texels.size());
            std::vector<uint64_t> longShares(texels.size());
            uint64_t givenShort = 0;
            uint64_t givenLong = 0;
            for (size_t t = 0; t < texels.size(); ++t) {
                shortShares[t] = rx::RcShare(source.energyShort, weights[t], weightSum);
                longShares[t] = rx::RcShare(source.energyLong, weights[t], weightSum);
                givenShort += shortShares[t];
                givenLong += longShares[t];
            }

            const uint64_t hash = fx::FxHash64(work.constants.seed, work.constants.tick, sourceKey, PURPOSE_REMAINDER);
            uint32_t lucky = fx::FxRandomBelow(hash, static_cast<uint32_t>(texels.size()));
            while (weights[lucky] == 0)
                lucky = (lucky + 1) % static_cast<uint32_t>(texels.size());

            shortShares[lucky] += source.energyShort - givenShort;
            longShares[lucky] += source.energyLong - givenLong;

            for (size_t t = 0; t < texels.size(); ++t) {
                if (shortShares[t] == 0 && longShares[t] == 0)
                    continue;

                WalkLevelRay(work, rx::RcLevel0WithEnergy(Level0Ray(work, sourceKey, cell, sourceFace, t, texels[t]),
                                                          sourceFace, shortShares[t], longShares[t]));
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

        // 段 level の袋を全部、袋の選んだ出どころから raysPerBag 本ずつ歩かせる
        void WalkBags(TickWork& work, uint32_t level, bool reverse) {
            const BagMap bags = std::move(work.bags[level]);
            const CascadeGrid& grid = *work.grid;
            const uint32_t rays = work.params->raysPerBag;
            auto walkOne = [&](const BagKey& key, const Bag& bag) {
                const rx::RcBagShare share = ShareOf(level, key, bag);
                for (uint32_t sub = 0; sub < rays; ++sub) {
                    const rx::RcLevelRay levelRay = rx::RcBagRay(share, sub, rays, grid.low[0], grid.low[1],
                                                                 grid.low[2]);
                    if (levelRay.ray.energyShort == 0 && levelRay.ray.energyLong == 0)
                        continue;

                    WalkLevelRay(work, levelRay);
                }
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

        if (params.raysPerBag == 0 || params.raysPerBag > RAYS_PER_BAG_LIMIT)
            return std::unexpected("袋 1 つの光線は 1〜64 本");

        return {};
    }

    std::expected<void, std::string> CheckCascadeGrid(const CascadeGrid& grid) {
        if (std::ranges::any_of(grid.size, [](int64_t side) { return side <= 0 || side > SIZE_LIMIT; }))
            return std::unexpected("世界の箱の 1 辺は 1〜127 セル(溜めの出どころを 15bit × 3 に詰める。ADR-0432)");

        return {};
    }

    CascadeTickResult StepCascadeLight(const CascadeGrid& grid, const CascadeCarry& carry, const CascadeParams& params,
                                       uint64_t tick, bool reverse) {
        CascadeTickResult result;
        result.emitted.assign(grid.CellCount() * rx::LT_BANDS, 0);
        result.absorbed.assign(grid.CellCount() * rx::LT_BANDS, 0);
        if (std::ranges::any_of(grid.size, [](int64_t side) { return side > SIZE_LIMIT; }))
            return result;  // 溜めの出どころを詰められない(CheckCascadeGrid が先に断る)

        const rx::RcStepConstants constants{.seed = params.seed,
                                            .tick = tick,
                                            .baseTexels = params.baseTexels,
                                            .levels = params.levels,
                                            .intervalBlocks = params.intervalBlocks,
                                            .raysPerBag = params.raysPerBag};
        TickWork work{
            .grid = &grid, .params = &params, .constants = constants, .result = &result, .carry = {}, .bags = {}};
        work.bags.resize(params.levels);

        // --- 段 0: 面から放つ ---
        const std::array<std::vector<TexelOfFace>, rx::RC_FACES> texels = MakeFaceTexels(params.baseTexels);
        const SourceMap sources = CollectSources(work, carry);
        if (reverse) {
            for (auto it = sources.rbegin(); it != sources.rend(); ++it)
                EmitFace(work, texels[it->first % rx::RC_FACES], it->first, it->second);
        } else {
            for (const auto& [key, source] : sources)
                EmitFace(work, texels[key % rx::RC_FACES], key, source);
        }

        // --- 段 1 から: 袋の溜めが選んだ出どころから ---
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
