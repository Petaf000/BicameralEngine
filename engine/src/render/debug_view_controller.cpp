// debug_view_controller.cpp — デバッグ表示の操作。考え方は debug_view_controller.h。
#include "render/debug_view_controller.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include "core/log.h"

namespace bicameral::render {
    namespace {

        constexpr uint32_t SLICE_FAST_STEP = 8;  // Shift を押しながら断面を動かす幅

        // 覗き窓で潜った段 k のカメラの距離 = これ × 2^-k(セル)。画面の高さに段のブロックが約 2.5 個入る
        constexpr float PEEK_BASE_DISTANCE = 24.0f;
        constexpr uint32_t KEY_PAGE_UP = 0x21;    // VK_PRIOR
        constexpr uint32_t KEY_PAGE_DOWN = 0x22;  // VK_NEXT

        std::string_view ModeName(DebugViewMode mode) {
            switch (mode) {
                case DebugViewMode::Volume: return "ボリューム";
                case DebugViewMode::MaximumProjection: return "最大値の投影";
                case DebugViewMode::Slice: return "断面";
            }

            return "?";
        }

        std::string_view QuantityName(DebugViewQuantity quantity) {
            switch (quantity) {
                case DebugViewQuantity::Temperature: return "温度";
                case DebugViewQuantity::OxygenDepletion: return "O2 の減り";
                case DebugViewQuantity::CarbonDioxide: return "CO2";
                case DebugViewQuantity::Carbon: return "炭";
            }

            return "?";
        }

        constexpr std::array<char, 3> AXIS_NAMES = {'x', 'y', 'z'};

        std::array<float, 4> ToFloat4(Vector3 vector) {
            return {vector.x, vector.y, vector.z, 0.0f};
        }

    }  // namespace

    DebugViewController::DebugViewController(uint32_t gridSize, const DebugViewSettings& settings,
                                             const OrbitCameraState& camera)
        : m_gridSize(gridSize), m_settings(settings), m_camera(camera) {
        m_settings.sliceAxis = std::min(m_settings.sliceAxis, 2u);
        m_settings.slicePosition = std::min(m_settings.slicePosition, gridSize - 1);
    }

    std::vector<CellCoordinate> DebugViewController::HandleInput(std::span<const InputEvent> events, uint32_t width,
                                                                 uint32_t height) {
        std::vector<CellCoordinate> pokes;
        for (const InputEvent& event : events) {
            if (event.kind == InputKind::KeyDown)
                HandleKey(event, width, height);
            else if (event.kind == InputKind::Wheel)
                m_camera.Zoom(event.wheelSteps);
            else
                HandlePointer(event, width, height, pokes);
        }

        return pokes;
    }

    // --- ポインタ ---

    void DebugViewController::HandlePointer(const InputEvent& event, uint32_t width, uint32_t height,
                                            std::vector<CellCoordinate>& pokes) {
        const bool down = event.kind == InputKind::ButtonDown;
        if (event.kind == InputKind::PointerMove) {
            const auto deltaX = static_cast<float>(event.x - m_lastX);
            const auto deltaY = static_cast<float>(event.y - m_lastY);
            if (m_rotating)
                m_camera.Orbit(deltaX, deltaY);

            if (m_panning)
                m_camera.Pan(deltaX, deltaY, height);
        } else if (event.button == PointerButton::Right)
            m_rotating = down;
        else if (event.button == PointerButton::Middle)
            m_panning = down;
        else if (down) {
            // 左クリック: 画素の中心を通る光線が断面に当たったセルをつつく。Shift なら光線のまま物を押す(当たる物は GPU が決める)
            const CameraRay ray = RayThroughPixel(m_camera.Basis(width, height), static_cast<float>(event.x) + 0.5f,
                                                  static_cast<float>(event.y) + 0.5f, width, height);
            if (event.shift)
                m_pushRays.push_back(ray);

            const auto cell = PickSliceCell(ray, m_settings.sliceAxis, m_settings.slicePosition, m_gridSize);
            if (cell && !event.shift)
                pokes.push_back(*cell);
        }

        m_lastX = event.x;
        m_lastY = event.y;
    }

    // --- キー ---

    void DebugViewController::HandleKey(const InputEvent& event, uint32_t width, uint32_t height) {
        if (event.key == 'P' || event.key == KEY_PAGE_UP || event.key == KEY_PAGE_DOWN) {
            HandlePeekKey(event, width, height);
            return;
        }

        const DebugViewSettings before = m_settings;
        const uint32_t step = event.shift ? SLICE_FAST_STEP : 1;
        switch (event.key) {
            case '1': m_settings.mode = DebugViewMode::Volume; break;
            case '2': m_settings.mode = DebugViewMode::MaximumProjection; break;
            case '3': m_settings.mode = DebugViewMode::Slice; break;
            case 'X': m_settings.sliceAxis = 0; break;
            case 'Y': m_settings.sliceAxis = 1; break;
            case 'Z': m_settings.sliceAxis = 2; break;
            case 'Q': m_settings.slicePosition -= std::min(step, m_settings.slicePosition); break;
            case 'E': m_settings.slicePosition = std::min(m_settings.slicePosition + step, m_gridSize - 1); break;
            case 'B': m_settings.showActiveBlocks = !m_settings.showActiveBlocks; break;
            case 'L': m_settings.logarithmic = !m_settings.logarithmic; break;
            case 'C':
                m_settings.quantity = static_cast<DebugViewQuantity>((static_cast<uint32_t>(m_settings.quantity) + 1) %
                                                                     DEBUG_VIEW_QUANTITY_COUNT);
                break;
            case 'O':
                m_camera.ToggleProjection();
                Log(Channel::Render, Level::Info, "カメラ: {}", m_camera.State().orthographic ? "平行投影" : "透視");
                return;
            case 'R':
                m_camera.Reset();
                Log(Channel::Render, Level::Info, "カメラを戻した");
                return;
            case 'T': m_traceRequested = true; return;
            default: return;
        }

        const bool changed = before.mode != m_settings.mode || before.sliceAxis != m_settings.sliceAxis ||
                             before.slicePosition != m_settings.slicePosition ||
                             before.showActiveBlocks != m_settings.showActiveBlocks ||
                             before.logarithmic != m_settings.logarithmic || before.quantity != m_settings.quantity;

        if (changed)
            Log(Channel::Render, Level::Info, "表示: {}", Describe());
    }

    // --- 覗き窓 ---

    // P: 最後のポインタの位置の光線が断面に当たったセルを覗く(Shift + P でやめる)。PageDown・PageUp: 潜る・浮かぶ
    void DebugViewController::HandlePeekKey(const InputEvent& event, uint32_t width, uint32_t height) {
        if (event.key == 'P' && event.shift) {
            m_peekChanged = m_peekChanged || m_peek.peeking;
            m_peek.peeking = false;
            Log(Channel::Render, Level::Info, "覗き窓: やめた");
            return;
        }

        if (event.key == 'P') {
            const CameraRay ray = RayThroughPixel(m_camera.Basis(width, height), static_cast<float>(m_lastX) + 0.5f,
                                                  static_cast<float>(m_lastY) + 0.5f, width, height);
            const auto cell = PickSliceCell(ray, m_settings.sliceAxis, m_settings.slicePosition, m_gridSize);
            if (cell)
                Peek(*cell, m_peek.depth);

            return;
        }

        if (!m_peek.peeking)
            return;

        const uint32_t before = m_peek.depth;
        if (event.key == KEY_PAGE_DOWN)
            m_peek.depth = std::min(m_peek.depth + 1, PEEK_MAX_DEPTH);
        else
            m_peek.depth -= std::min(m_peek.depth, 1u);

        if (m_peek.depth == before)
            return;

        FocusOnPeek();
        Log(Channel::Render, Level::Info, "覗き窓: 段 {}(1 セル = {} mm)", m_peek.depth, 500.0 / (1u << m_peek.depth));
    }

    void DebugViewController::Peek(CellCoordinate cell, uint32_t depth) {
        m_peekChanged = m_peekChanged || !m_peek.peeking || m_peek.cell != cell;
        m_peek = {.peeking = true, .cell = cell, .depth = std::min(depth, PEEK_MAX_DEPTH)};
        FocusOnPeek();
        Log(Channel::Render, Level::Info, "覗き窓: ({}, {}, {}) を覗く・段 {}", cell.x, cell.y, cell.z, m_peek.depth);
    }

    // 潜った段 k のブロックの真ん中に寄る(距離 PEEK_BASE_DISTANCE × 2^-k)。段 0 は覗いているセルの真ん中。
    // 段 k のブロックは、点(セルの真ん中 = c × 2^k + 2^(k-1))を含む 8 の倍数に揃ったブロック
    // (子の原点 = 2 × (親の原点 + 4 × 八分の一) なので、どの段の原点も 8 の倍数。multires.hlsli の MrChildOrigin・ADR-0015)
    void DebugViewController::FocusOnPeek() {
        const uint32_t depth = m_peek.depth;
        const auto scale = static_cast<float>(1u << depth);
        const auto center = [depth, scale](uint32_t cell) {
            if (depth == 0)
                return static_cast<float>(cell) + 0.5f;

            const uint64_t point = (uint64_t{cell} << depth) + (uint64_t{1} << (depth - 1));
            const uint64_t blockCenter = (point & ~uint64_t{7}) + 4;

            return static_cast<float>(blockCenter) / scale;
        };

        m_camera.Focus({center(m_peek.cell.x), center(m_peek.cell.y), center(m_peek.cell.z)},
                       PEEK_BASE_DISTANCE / scale);
    }

    bool DebugViewController::TakePeekChange() {
        const bool changed = m_peekChanged;
        m_peekChanged = false;

        return changed;
    }

    bool DebugViewController::TakeTraceRequest() {
        const bool requested = m_traceRequested;
        m_traceRequested = false;

        return requested;
    }

    std::vector<CameraRay> DebugViewController::TakePushRays() {
        return std::exchange(m_pushRays, {});
    }

    // --- 描画へ ---

    ProbeViewConstants DebugViewController::Constants(uint32_t extractionIndex, uint32_t width, uint32_t height) const {
        const CameraBasis basis = m_camera.Basis(width, height);
        uint32_t flags = 0;
        if (m_settings.showActiveBlocks)
            flags |= VIEW_FLAG_ACTIVE_BLOCKS;

        if (m_settings.logarithmic)
            flags |= VIEW_FLAG_LOGARITHMIC;

        flags |= static_cast<uint32_t>(m_settings.quantity) << VIEW_QUANTITY_SHIFT;
        if (m_peek.peeking)
            flags |= m_peek.depth << VIEW_PEEK_DEPTH_SHIFT;

        return {.extractionIndex = extractionIndex,
                .width = width,
                .height = height,
                .flags = flags,
                .mode = static_cast<uint32_t>(m_settings.mode),
                .sliceAxis = m_settings.sliceAxis,
                .slicePosition = m_settings.slicePosition,
                .orthographic = basis.orthographic ? 1u : 0u,
                .position = ToFloat4(basis.position),
                .forward = ToFloat4(basis.forward),
                .right = ToFloat4(basis.right),
                .up = ToFloat4(basis.up)};
    }

    std::string DebugViewController::Describe() const {
        return std::format("{}  断面 {} = {}  活性なブロック {}  色 {}({})", ModeName(m_settings.mode),
                           AXIS_NAMES[m_settings.sliceAxis], m_settings.slicePosition,
                           m_settings.showActiveBlocks ? "あり" : "なし", QuantityName(m_settings.quantity),
                           m_settings.logarithmic ? "対数" : "線形");
    }

    bool ParseDebugViewMode(std::string_view name, DebugViewMode& mode) {
        if (name == "volume")
            mode = DebugViewMode::Volume;
        else if (name == "mip")
            mode = DebugViewMode::MaximumProjection;
        else if (name == "slice")
            mode = DebugViewMode::Slice;
        else
            return false;

        return true;
    }

}  // namespace bicameral::render
