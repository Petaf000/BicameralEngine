// face_opening.cpp — セルの面の開き具合の欄(T-0937)。約束は face_opening.h。
#include "sim/face_opening.h"

#include <array>
#include <vector>

#include "common/world_edge.hlsli"
#include "sim/multires_fields.h"

namespace bicameral::sim {

    namespace {

        using multires::MR_BLOCK_CELLS;
        using namespace bicameral::surface;
        namespace sf = state_field;
        namespace fo = face_opening;

        // 横の 2 軸は x・z、縦は y(区画の高さ)。上の軸を替えたらここの写しも替える
        static_assert(multires::WE_UP_AXIS == 1);

        constexpr uint32_t AXES = 3;

        // 世界のセル (x, up, z) の + 側の 3 面の塞ぐ割合(区画の外は 0)
        std::array<uint32_t, AXES> BlockedAt(const SurfaceDetailScene& scene, uint32_t mode,
                                             const SurfacePlacement& placement, uint32_t edgeUm, int64_t x, int64_t up,
                                             int64_t z) {
            const int64_t localX = x - placement.originX;
            const int64_t localZ = z - placement.originZ;
            const int64_t sizeX = (int64_t)scene.config.columnsX * SD_CELL_UM;
            const int64_t sizeZ = (int64_t)scene.config.columnsZ * SD_CELL_UM;
            if (localX < 0 || localZ < 0 || localX * edgeUm >= sizeX || localZ * edgeUm >= sizeZ)
                return {};

            const int64_t bottomUm = (up - placement.originUp) * (int64_t)edgeUm;
            const SdCellOpenness open = SdCellOpennessAt(scene.Data(), scene.config, mode, (uint32_t)(localX * edgeUm),
                                                         (uint32_t)(localZ * edgeUm), bottomUm, edgeUm);

            return {fo::FoBlockedFromOpen(open.openXQ16), fo::FoBlockedFromOpen(open.openTopQ16),
                    fo::FoBlockedFromOpen(open.openZQ16)};
        }

        // ブロック 1 つの値を書く(一様なブロックで全部のセルが同じなら値 1 つ・違えば頁に広げて)
        bool StoreBlock(MultiresNest& nest, uint32_t field, uint32_t slot,
                        const std::vector<std::array<uint32_t, AXES>>& values) {
            bool same = true;
            for (uint32_t index = 1; index < MR_BLOCK_CELLS && same; ++index)
                same = values[index] == values[0];

            if (multires::MrIsUniform(nest.blocks[slot]) && same) {
                for (uint32_t axis = 0; axis < AXES; ++axis)
                    StoreNestFieldUniform(nest, field, slot, axis, values[0][axis]);

                return true;
            }

            if (!ExpandNestPage(nest, slot))
                return false;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                for (uint32_t axis = 0; axis < AXES; ++axis)
                    StoreNestField(nest, field, slot, index, axis, values[index][axis]);
            }

            return true;
        }

    }  // namespace

    uint32_t RegisterFaceBlockedField(MultiresNest& nest) {
        const sf::SfFieldDesc desc = sf::SfMakeDerivedDesc(sf::SF_PLACE_FACE, sf::SF_TYPE_U32, sf::SF_PHASE_NONE, false,
                                                           3);

        return RegisterNestField(nest, FACE_BLOCKED_FIELD_NAME, desc);
    }

    bool DeriveFaceBlocked(MultiresNest& nest, uint32_t field, const SurfaceDetailScene& scene, uint32_t mode,
                           const SurfacePlacement& placement) {
        const int32_t level = nest.capacity.rootLevel;
        if (level < 0 || level > 18)
            return false;

        const uint32_t edgeUm = surface::SD_CELL_UM >> (uint32_t)level;
        std::vector<std::array<uint32_t, AXES>> values(MR_BLOCK_CELLS);
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const multires::MrBlock& block = nest.blocks[slot];
            if (block.kind != multires::MR_BLOCK_REAL)
                continue;

            if (block.level != level)
                return false;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                values[index] = BlockedAt(scene, mode, placement, edgeUm, block.originX + multires::MrCellX(index),
                                          block.originY + multires::MrCellY(index),
                                          block.originZ + multires::MrCellZ(index));

            if (!StoreBlock(nest, field, slot, values))
                return false;
        }

        return true;
    }

    uint32_t LoadFaceBlockedQ16(const MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t index,
                                uint32_t axis) {
        return (uint32_t)LoadNestField(nest, field, slot, index, axis);
    }

}  // namespace bicameral::sim
