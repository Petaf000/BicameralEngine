// debug_view_controller.cpp — デバッグ表示の操作。考え方は debug_view_controller.h。
#include "render/debug_view_controller.h"

#include <algorithm>
#include <array>
#include <format>

#include "core/aliases.h"
#include "core/log.h"

namespace bicameral::render {
    namespace {

        constexpr uint32_t SLICE_FAST_STEP = 8;  // Shift を押しながら断面を動かす幅

        string_view ModeName(DebugViewMode mode) {
            switch (mode) {
                case DebugViewMode::Volume: return "ボリューム";
                case DebugViewMode::MaximumProjection: return "最大値の投影";
                case DebugViewMode::Slice: return "断面";
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

    std::vector<CellCoordinate> DebugViewController::HandleInput(span<const InputEvent> events, uint32_t width,
                                                                 uint32_t height) {
        std::vector<CellCoordinate> pokes;
        for (const InputEvent& event : events) {
            if (event.kind == InputKind::KeyDown)
                HandleKey(event);
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
            // 左クリック: 画素の中心を通る光線が断面に当たったセルをつつく
            const CameraRay ray = RayThroughPixel(m_camera.Basis(width, height), static_cast<float>(event.x) + 0.5f,
                                                  static_cast<float>(event.y) + 0.5f, width, height);
            const auto cell = PickSliceCell(ray, m_settings.sliceAxis, m_settings.slicePosition, m_gridSize);
            if (cell)
                pokes.push_back(*cell);
        }

        m_lastX = event.x;
        m_lastY = event.y;
    }

    // --- キー ---

    void DebugViewController::HandleKey(const InputEvent& event) {
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
            case 'O':
                m_camera.ToggleProjection();
                Log(Channel::Render, Level::Info, "カメラ: {}", m_camera.State().orthographic ? "平行投影" : "透視");
                return;
            case 'R':
                m_camera.Reset();
                Log(Channel::Render, Level::Info, "カメラを戻した");
                return;
            default: return;
        }

        const bool changed = before.mode != m_settings.mode || before.sliceAxis != m_settings.sliceAxis ||
                             before.slicePosition != m_settings.slicePosition ||
                             before.showActiveBlocks != m_settings.showActiveBlocks ||
                             before.logarithmic != m_settings.logarithmic;

        if (changed)
            Log(Channel::Render, Level::Info, "表示: {}", Describe());
    }

    // --- 描画へ ---

    ProbeViewConstants DebugViewController::Constants(uint32_t extractionIndex, uint32_t width, uint32_t height) const {
        const CameraBasis basis = m_camera.Basis(width, height);
        uint32_t flags = 0;
        if (m_settings.showActiveBlocks)
            flags |= VIEW_FLAG_ACTIVE_BLOCKS;

        if (m_settings.logarithmic)
            flags |= VIEW_FLAG_LOGARITHMIC;

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
        return format("{}  断面 {} = {}  活性なブロック {}  色 {}", ModeName(m_settings.mode),
                      AXIS_NAMES[m_settings.sliceAxis], m_settings.slicePosition,
                      m_settings.showActiveBlocks ? "あり" : "なし", m_settings.logarithmic ? "対数" : "線形");
    }

    bool ParseDebugViewMode(string_view name, DebugViewMode& mode) {
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
