// surface_detail.cpp — 表面の細かい形の区画(差分の 2 つの候補・セーブ・ハッシュ)。約束は surface_detail.h。
#include "sim/surface_detail.h"

#include <algorithm>
#include <format>

#include "core/aliases.h"

namespace bicameral::sim {

    using namespace bicameral::surface;

    namespace {

        // 彫った形が触れる矩形(µm。両端を含む)
        struct CarveBounds {
            uint32_t minX = 0;
            uint32_t minZ = 0;
            uint32_t maxX = 0;
            uint32_t maxZ = 0;
        };

        CarveBounds BoundsOf(const SdCarve& carve) {
            const uint32_t lowX = std::min(carve.ax, carve.bx);
            const uint32_t lowZ = std::min(carve.az, carve.bz);

            return {.minX = lowX > carve.radiusUm ? lowX - carve.radiusUm : 0,
                    .minZ = lowZ > carve.radiusUm ? lowZ - carve.radiusUm : 0,
                    .maxX = std::max(carve.ax, carve.bx) + carve.radiusUm,
                    .maxZ = std::max(carve.az, carve.bz) + carve.radiusUm};
        }

        // 触れる柱の範囲(区画の中に切る)を回す
        template <typename Visit>
        void ForEachTouchedColumn(const SurfaceDetailScene& scene, const SdCarve& carve, Visit&& visit) {
            const CarveBounds bounds = BoundsOf(carve);
            const uint32_t lastX = std::min(bounds.maxX / SD_CELL_UM, scene.config.columnsX - 1);
            const uint32_t lastZ = std::min(bounds.maxZ / SD_CELL_UM, scene.config.columnsZ - 1);
            for (uint32_t columnZ = bounds.minZ / SD_CELL_UM; columnZ <= lastZ; ++columnZ) {
                for (uint32_t columnX = bounds.minX / SD_CELL_UM; columnX <= lastX; ++columnX)
                    visit(columnX, columnZ);
            }
        }

        // 候補 A: 柱の格子に彫った形の深さを書く(格子が無ければ作る)
        void CarveGrid(SurfaceDetailScene& scene, const SdCarve& carve, uint32_t columnX, uint32_t columnZ) {
            const uint32_t column = columnX + (columnZ * scene.config.columnsX);
            if (scene.gridIndex[column] == SD_NO_GRID) {
                scene.gridIndex[column] = (uint32_t)(scene.gridNodes.size() / SD_GRID_NODE_COUNT);
                scene.gridNodes.resize(scene.gridNodes.size() + SD_GRID_NODE_COUNT, 0);
            }

            const size_t base = (size_t)scene.gridIndex[column] * SD_GRID_NODE_COUNT;
            for (uint32_t j = 0; j < SD_GRID_NODES; ++j) {
                for (uint32_t i = 0; i < SD_GRID_NODES; ++i) {
                    const uint32_t x = (columnX * SD_CELL_UM) + (i * SD_GRID_SPACING_UM);
                    const uint32_t z = (columnZ * SD_CELL_UM) + (j * SD_GRID_SPACING_UM);
                    scene.gridNodes[base + i + (j * SD_GRID_NODES)] -= (int32_t)SdCarveDepthUm(carve, x, z);
                }
            }
        }

        void AppendU32(std::vector<uint8_t>& bytes, uint32_t value) {
            for (uint32_t shift = 0; shift < 32; shift += 8)
                bytes.push_back((uint8_t)(value >> shift));
        }

        uint32_t ReadU32(std::span<const uint8_t> bytes, size_t& at) {
            uint32_t value = 0;
            for (uint32_t shift = 0; shift < 32; shift += 8)
                value |= (uint32_t)bytes[at++] << shift;

            return value;
        }

    }  // namespace

    SurfaceDetailScene MakeSurfaceDetailScene(const SdConfig& config, uint32_t groundUm) {
        SurfaceDetailScene scene;
        scene.config = config;
        scene.columnHeights.assign(scene.ColumnCount(), groundUm);
        scene.gridIndex.assign(scene.ColumnCount(), SD_NO_GRID);
        RegisterCarves(scene);

        return scene;
    }

    std::expected<void, std::string> CarveSurface(SurfaceDetailScene& scene, const SdCarve& carve) {
        const int64_t dx = (int64_t)carve.bx - (int64_t)carve.ax;
        const int64_t dz = (int64_t)carve.bz - (int64_t)carve.az;
        const int64_t limit = SD_MAX_CARVE_LENGTH_UM;
        if ((dx * dx) + (dz * dz) > limit * limit)
            return std::unexpected("彫る線分が長すぎる(折れ線に分ける)");

        if (carve.radiusUm == 0 || carve.radiusUm > SD_MAX_CARVE_RADIUS_UM || carve.depthUm > SD_MAX_CARVE_DEPTH_UM)
            return std::unexpected(
                std::format("彫る半径か深さが範囲の外(半径 {} µm・深さ {} µm)", carve.radiusUm, carve.depthUm));

        const uint32_t sizeX = scene.config.columnsX * SD_CELL_UM;
        const uint32_t sizeZ = scene.config.columnsZ * SD_CELL_UM;
        if (std::max(carve.ax, carve.bx) >= sizeX || std::max(carve.az, carve.bz) >= sizeZ)
            return std::unexpected("彫る線分が区画の外");

        scene.carves.push_back(carve);
        ForEachTouchedColumn(scene, carve,
                             [&](uint32_t columnX, uint32_t columnZ) { CarveGrid(scene, carve, columnX, columnZ); });
        RegisterCarves(scene);

        return {};
    }

    void RegisterCarves(SurfaceDetailScene& scene) {
        const uint32_t count = scene.ColumnCount();
        std::vector<std::vector<uint32_t>> perColumn(count);
        for (uint32_t index = 0; index < (uint32_t)scene.carves.size(); ++index) {
            ForEachTouchedColumn(scene, scene.carves[index], [&](uint32_t columnX, uint32_t columnZ) {
                perColumn[columnX + (columnZ * scene.config.columnsX)].push_back(index);
            });
        }

        scene.carveBegin.assign(count + 1, 0);
        scene.registered.clear();
        for (uint32_t column = 0; column < count; ++column) {
            scene.carveBegin[column] = (uint32_t)scene.registered.size();
            for (const uint32_t index : perColumn[column])
                scene.registered.push_back(scene.carves[index]);
        }

        scene.carveBegin[count] = (uint32_t)scene.registered.size();
    }

    void ClearSurfaceDiff(SurfaceDetailScene& scene) {
        scene.carves.clear();
        scene.gridNodes.clear();
        rng::fill(scene.gridIndex, SD_NO_GRID);
        RegisterCarves(scene);
    }

    std::vector<uint8_t> SaveSurfaceDiff(const SurfaceDetailScene& scene, uint32_t mode) {
        std::vector<uint8_t> bytes;
        if (mode == SD_MODE_LIST) {
            AppendU32(bytes, (uint32_t)scene.carves.size());
            for (const SdCarve& carve : scene.carves) {
                for (const uint32_t value : {carve.ax, carve.az, carve.bx, carve.bz, carve.radiusUm, carve.depthUm})
                    AppendU32(bytes, value);
            }

            return bytes;
        }

        // 候補 A: 格子のある柱の番号と 289 点
        const uint32_t count = scene.ColumnCount();
        AppendU32(bytes, (uint32_t)(scene.gridNodes.size() / SD_GRID_NODE_COUNT));
        for (uint32_t column = 0; column < count; ++column) {
            if (scene.gridIndex[column] == SD_NO_GRID)
                continue;

            AppendU32(bytes, column);
            const size_t base = (size_t)scene.gridIndex[column] * SD_GRID_NODE_COUNT;
            for (uint32_t node = 0; node < SD_GRID_NODE_COUNT; ++node)
                AppendU32(bytes, (uint32_t)scene.gridNodes[base + node]);
        }

        return bytes;
    }

    std::expected<void, std::string> LoadSurfaceDiff(SurfaceDetailScene& scene, uint32_t mode,
                                                     std::span<const uint8_t> bytes) {
        if (bytes.size() < 4)
            return std::unexpected("差分が短すぎる");

        size_t at = 0;
        const uint32_t count = ReadU32(bytes, at);
        const size_t itemBytes = mode == SD_MODE_LIST ? 24 : (size_t)4 * (1 + SD_GRID_NODE_COUNT);
        if (bytes.size() != 4 + (itemBytes * count))
            return std::unexpected("差分の長さが合わない");

        if (mode == SD_MODE_LIST) {
            scene.carves.clear();
            for (uint32_t i = 0; i < count; ++i) {
                SdCarve carve{};
                carve.ax = ReadU32(bytes, at);
                carve.az = ReadU32(bytes, at);
                carve.bx = ReadU32(bytes, at);
                carve.bz = ReadU32(bytes, at);
                carve.radiusUm = ReadU32(bytes, at);
                carve.depthUm = ReadU32(bytes, at);
                scene.carves.push_back(carve);
            }

            RegisterCarves(scene);
            return {};
        }

        scene.gridNodes.clear();
        rng::fill(scene.gridIndex, SD_NO_GRID);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t column = ReadU32(bytes, at);
            if (column >= scene.ColumnCount() || scene.gridIndex[column] != SD_NO_GRID)
                return std::unexpected("差分の柱の番号が正しくない");

            scene.gridIndex[column] = i;
            for (uint32_t node = 0; node < SD_GRID_NODE_COUNT; ++node)
                scene.gridNodes.push_back((int32_t)ReadU32(bytes, at));
        }

        return {};
    }

    uint64_t HashSurfaceHeights(const SurfaceDetailScene& scene, uint32_t mode, uint32_t samplesPerColumn) {
        const SdSceneData data = scene.Data();
        const uint32_t step = SD_CELL_UM / samplesPerColumn;
        uint64_t hash = 0;
        for (uint32_t z = step / 2; z < scene.config.columnsZ * SD_CELL_UM; z += step) {
            for (uint32_t x = step / 2; x < scene.config.columnsX * SD_CELL_UM; x += step)
                hash = fx::FxHashCombine(hash, (uint64_t)SdGroundHeightUm(data, scene.config, mode, x, z));
        }

        return hash;
    }

}  // namespace bicameral::sim
