// surface_detail.h — 表面の細かい形(23 §7・ADR-0384。研究 R-T1・T-0879)の区画を CPU で持つ。GPU を知らない。
// 形の関数は shaders/common/surface_detail.hlsli(GPU と同じソース)。ここは区画のデータ(柱の高さ・差分)の持ち方と、
// 差分の 2 つの候補の作り方・セーブの形・ハッシュを持つ。
//
// 差分(プレイヤー・出来事が変えた所。真実に記録する)の候補:
//   候補 B(SD_MODE_LIST): 彫った形の一覧が真実。柱ごとの登録(carveBegin・registered)は導かれる値で、形が変わった時に作り直す
//   候補 A(SD_MODE_GRID): 高さの差の格子が真実(彫った柱だけ 17 × 17 点)。彫る操作は格子の点に深さを書き込む(一覧は残らない)
// どちらも「差分を消すと自然の形に戻る」(差分を空にすると SD_MODE_NATURAL と同じ高さ)。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "common/surface_detail.hlsli"

namespace bicameral::sim {

    // 区画(柱の高さ + 差分の 2 つの候補を並べて持つ。試作なので両方を同じ操作で作り、比べる)
    struct SurfaceDetailScene {
        surface::SdConfig config{};
        std::vector<uint32_t> columnHeights;  // 柱の地面の高さ(µm)

        // --- 候補 B: 彫った形の一覧(真実)と柱ごとの登録(導かれる値)---
        std::vector<surface::SdCarve> carves;
        std::vector<uint32_t> carveBegin;  // 柱の数 + 1
        std::vector<surface::SdCarve> registered;

        // --- 候補 A: 高さの差の格子(真実)---
        std::vector<uint32_t> gridIndex;  // 柱 → 格子の番号(SD_NO_GRID = 無い)
        std::vector<int32_t> gridNodes;   // 格子の番号 × 289 + j × 17 + i(µm)

        [[nodiscard]] uint32_t ColumnCount() const { return config.columnsX * config.columnsZ; }
        [[nodiscard]] surface::SdSceneData Data() const {
            return {columnHeights, carveBegin, registered, gridIndex, gridNodes};
        }
    };

    // 平らな区画を作る(差分なし)。柱の数は 2 以上
    [[nodiscard]] SurfaceDetailScene MakeSurfaceDetailScene(const surface::SdConfig& config, uint32_t groundUm);

    // 彫る。候補 B は一覧に足して、触れた柱の登録を作り直す。候補 A は触れた柱の格子の点に深さを書く。
    // 断る: 線分が長すぎる・半径か深さが大きすぎる・区画の外
    [[nodiscard]] std::expected<void, std::string> CarveSurface(SurfaceDetailScene& scene,
                                                                const surface::SdCarve& carve);

    // 候補 B の登録を一覧から作り直す(全部。形が変わった時の導かれる値)
    void RegisterCarves(SurfaceDetailScene& scene);

    // 差分を消す(両方の候補。自然の形に戻る)
    void ClearSurfaceDiff(SurfaceDetailScene& scene);

    // 差分のセーブの形(バイト列)。mode = SD_MODE_LIST は一覧・SD_MODE_GRID は格子
    [[nodiscard]] std::vector<uint8_t> SaveSurfaceDiff(const SurfaceDetailScene& scene, uint32_t mode);
    [[nodiscard]] std::expected<void, std::string> LoadSurfaceDiff(SurfaceDetailScene& scene, uint32_t mode,
                                                                   std::span<const uint8_t> bytes);

    // 区画の地面の高さを sample × sample の点で調べたハッシュ(差分の持ち方によらない比べ)
    [[nodiscard]] uint64_t HashSurfaceHeights(const SurfaceDetailScene& scene, uint32_t mode,
                                              uint32_t samplesPerColumn);

}  // namespace bicameral::sim
