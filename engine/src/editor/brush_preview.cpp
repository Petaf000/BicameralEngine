// brush_preview.cpp — カーソルの下に筆の形を描くための線(T-0279)。流れは brush_preview.h。
#include "editor/brush_preview.h"

#include <algorithm>
#include <cstddef>
#include <optional>

#include "common/probe_place.hlsli"

namespace bicameral::editor {
    namespace {

        using render::Vector3;

        // 軸 axis が a、残りの 2 つの軸 (u, v)(x → y → z の順)の点
        Vector3 SlicePoint(uint32_t axis, float along, float u, float v) {
            if (axis == 0)
                return {along, u, v};

            if (axis == 1)
                return {u, along, v};

            return {u, v, along};
        }

        // 形の大きさが置くコマンドとして読める(物質は見ない。ProbeDecodePlace の形の検査と同じ範囲)
        bool ShapeReadable(const sim::ProbePlace& place) {
            const bool inside = place.x < sim::PROBE_GRID_SIZE && place.y < sim::PROBE_GRID_SIZE &&
                                place.z < sim::PROBE_GRID_SIZE;
            const bool extents = place.radius <= sim::PROBE_PLACE_MAX_RADIUS &&
                                 place.extentX <= sim::PROBE_PLACE_MAX_RADIUS &&
                                 place.extentY <= sim::PROBE_PLACE_MAX_RADIUS &&
                                 place.extentZ <= sim::PROBE_PLACE_MAX_RADIUS;

            return inside && extents && place.shape <= sim::PROBE_PLACE_SHAPE_CYLINDER && place.axis <= 2;
        }

    }  // namespace

    BrushPreview BuildBrushPreview(std::span<const uint32_t> placePayload, uint32_t sliceAxis, uint32_t slicePlane) {
        BrushPreview preview;
        sim::ProbePlacePayload payload{};
        const size_t words = std::min<size_t>(placePayload.size(), sim::PROBE_PLACE_PAYLOAD_WORDS);
        std::copy_n(placePayload.begin(), words, std::begin(payload.words));
        const sim::ProbePlace place = sim::ProbeDecodePlace(payload, UINT32_MAX);
        if (!ShapeReadable(place) || sliceAxis > 2)
            return preview;

        // --- 形を囲む箱(格子の中に切る。置くコマンドが触る範囲と同じ)---
        const std::array<uint32_t, 3> center = {place.x, place.y, place.z};
        const std::array<uint32_t, 3> extent = {place.extentX, place.extentY, place.extentZ};
        std::array<uint32_t, 3> begin{};
        std::array<uint32_t, 3> end{};
        for (size_t axis = 0; axis < 3; ++axis) {
            begin[axis] = sim::ProbePlaceBegin(center[axis], extent[axis]);
            end[axis] = sim::ProbePlaceEnd(center[axis], extent[axis]);
        }

        const auto corner = [&](uint32_t bits) {
            return Vector3(static_cast<float>((bits & 1u) != 0 ? end[0] : begin[0]),
                           static_cast<float>((bits & 2u) != 0 ? end[1] : begin[1]),
                           static_cast<float>((bits & 4u) != 0 ? end[2] : begin[2]));
        };
        for (uint32_t bits = 0; bits < 8; ++bits) {
            for (uint32_t axisBit = 1; axisBit < 8; axisBit <<= 1) {
                if ((bits & axisBit) == 0)
                    preview.segments.push_back({.from = corner(bits), .to = corner(bits | axisBit), .onSlice = false});
            }
        }

        // --- 断面の上で覆うセルの縁(隣が覆われていない辺)---
        if (slicePlane < begin[sliceAxis] || slicePlane >= end[sliceAxis] || slicePlane >= sim::PROBE_GRID_SIZE)
            return preview;

        const uint32_t axisU = sliceAxis == 0 ? 1 : 0;
        const uint32_t axisV = sliceAxis == 2 ? 1 : 2;
        const auto covers = [&](int64_t u, int64_t v) {
            if (u < 0 || v < 0 || u >= sim::PROBE_GRID_SIZE || v >= sim::PROBE_GRID_SIZE)
                return false;

            const render::CellCoordinate cell = render::CellOnSlice(sliceAxis, slicePlane, static_cast<uint32_t>(u),
                                                                    static_cast<uint32_t>(v));
            return sim::ProbePlaceCovers(place, cell.x, cell.y, cell.z);
        };

        const float along = static_cast<float>(slicePlane) + 0.5f;
        for (uint32_t v = begin[axisV]; v < end[axisV]; ++v) {
            for (uint32_t u = begin[axisU]; u < end[axisU]; ++u) {
                if (!covers(u, v))
                    continue;

                ++preview.sliceCells;
                const auto fu = static_cast<float>(u);
                const auto fv = static_cast<float>(v);
                const auto edge = [&](float u0, float v0, float u1, float v1) {
                    preview.segments.push_back({.from = SlicePoint(sliceAxis, along, u0, v0),
                                                .to = SlicePoint(sliceAxis, along, u1, v1),
                                                .onSlice = true});
                };
                if (!covers(static_cast<int64_t>(u) - 1, v))
                    edge(fu, fv, fu, fv + 1.0f);
                if (!covers(static_cast<int64_t>(u) + 1, v))
                    edge(fu + 1.0f, fv, fu + 1.0f, fv + 1.0f);
                if (!covers(u, static_cast<int64_t>(v) - 1))
                    edge(fu, fv, fu + 1.0f, fv);
                if (!covers(u, static_cast<int64_t>(v) + 1))
                    edge(fu, fv + 1.0f, fu + 1.0f, fv + 1.0f);
            }
        }

        return preview;
    }

    std::vector<BrushPreviewLine> ProjectBrushPreview(const BrushPreview& preview, const render::CameraBasis& basis,
                                                      uint32_t width, uint32_t height) {
        std::vector<BrushPreviewLine> lines;
        lines.reserve(preview.segments.size());
        for (const BrushPreviewSegment& segment : preview.segments) {
            const auto from = render::ProjectToPixel(basis, segment.from, width, height);
            const auto to = render::ProjectToPixel(basis, segment.to, width, height);
            if (from && to)
                lines.push_back({.from = *from, .to = *to, .onSlice = segment.onSlice});
        }

        return lines;
    }

}  // namespace bicameral::editor
