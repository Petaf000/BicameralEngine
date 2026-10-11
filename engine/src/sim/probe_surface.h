// probe_surface.h — 筆の「面に沿って置く」(T-0332・docs/design/14 §2「実装(T-0332)」)のための、物のセルの印(64³ ビット = 32 KiB)を
// GPU で作って CPU へ読み戻す。View のためだけ(世界の結果にも再生ファイルにも入らない。D-107・D-403)。
//
// データの流れ(ProbeSim の抽出の後のフック。ProbeFrameInput::afterExtract。覗き窓と同じ道):
//   世界のセル S(t) → shaders/sim/probe_surface.hlsl(物とみなすのは common/probe_surface.hlsli の ProbeIsSolidCell)→ 印のバッファ
//   → フレームの枠ごとの読み戻し → そのフレームのリストが終わったら Read(枠)→ editor/surface_pick が光線を格子に通す。
// 有効な間(筆が「面に沿って」の時・--auto-place)だけ記録する。数フレーム遅れてよい(置く位置はコマンドとして記録されるので、
// 再生は印を使わない)。CPU リファレンスは BuildProbeSurfaceMask(同じ関数。gpu_probe_place の試験が比べる)。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/probe_surface.hlsli"
#include "gpu/com_ptr.h"
#include "sim/probe_sim.h"

namespace bicameral::sim {

    // 読み戻した印(語 w のビット b = セルの番号 32w + b。PROBE_SURFACE_WORDS 語)と、それが表す状態の刻み S(tick)
    struct ProbeSurfaceMask {
        uint64_t tick = 0;
        std::vector<uint32_t> words;
    };

    // CPU リファレンス: 世界のセル(1 世代 PROBE_CELL_COUNT 個)の印
    [[nodiscard]] std::vector<uint32_t> BuildProbeSurfaceMask(std::span<const reaction::RxCell> cells);

    class ProbeSurface {
    public:
        [[nodiscard]] static std::expected<ProbeSurface, std::string> Create(ID3D12Device* device);

        // 記録するか(筆が「面に沿って」の時だけ。止めても前に読んだ印は呼ぶ側が持つ)
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        [[nodiscard]] bool Enabled() const { return m_enabled; }

        // 抽出の後ろ(ProbeFrameInput::afterExtract)で呼ぶ。有効なら S(context.tick) の印を作って context.slot の読み戻しへ写す
        void RecordAfterExtract(ID3D12GraphicsCommandList10* list, const ProbeExtractContext& context);

        // 枠 slot のリストが GPU で終わった後に呼ぶ。そのリストで記録していれば印(取ったら忘れる)
        [[nodiscard]] std::optional<ProbeSurfaceMask> Read(uint32_t slot);

    private:
        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_pipeline;
        ComPtr<ID3D12Resource> m_mask;  // 印(PROBE_SURFACE_WORDS 語。UAV)

        struct Slot {
            ComPtr<ID3D12Resource> readback;
            uint64_t tick = 0;
            bool recorded = false;
        };
        std::array<Slot, ProbeSim::FRAME_SLOT_COUNT> m_slots;
        bool m_enabled = false;
    };

}  // namespace bicameral::sim
