// gpu_cascade_light.h — 世界の中の光: 距離ごとの段を GPU で(Compute。shaders/sim/cascade_light.hlsl。T-0959・ADR-0432)。
// CPU リファレンスは sim/cascade_light.h の StepCascadeLight(同じ刻みの結果がビット一致)。
// 多重解像度の木(sim/gpu_multires)のルート署名と外のバッファ u4・u5 を借りる(太陽の影の柱と同じ形)。段 0 の密な写し(CascadeGrid)を読む。
// CreateFromTree で作ると、RecordTreeGrid が刻みの初めの木の葉(粗い葉を含む)から GPU の上で写しを作る(CPU の MakeCascadeTreeGrid と
// ビット単位で同じ。T-1117)。粗い葉の中も 0.5 m で歩く(葉の大きさで歩く形・127 の限りを外す形は T-1117 の残り)。
// 仮の世界(sim/probe_sim。T-1154)は CreateStandalone で作る: 同じシェーダーを自分のルート署名(u0〜u5 + 定数 26 語 + デバッグのリング。
// 木のルート署名と同じ並びの部分だけ)で動かし、光の値と温度は呼ぶ側の GPU の段が毎刻み作業のバッファ(Work の workLights・workTemperatures)へ書く。
// 持ち越しの表は刻みの偶奇で選ぶ(刻み t は表 t & 1 を読み、表 1 − (t & 1) へ書く)ので、CPU に刻みをまたぐ状態が無い(写し・巻き戻しは状態のバッファの頭だけを写す)。
// 段 1 からの起動は、前の段が GPU の上で書いた袋の数で要るだけ(間接の起動。T-1235・ADR-0495。袋が無い段はグループ 0)。
//
// データの流れ: CPU の CascadeGrid・CascadeParams・初めの持ち越し → 作業のバッファ(u4。既定のヒープへ写す)
//   → 刻みごとに RecordTick(1 刻みの全部の段。持ち越しは GPU の中で次の刻みへ)→ RecordReadback → Read(CascadeTickResult の形)
// 照り返しと組を待つ光の持ち越しは (セル・面・帯) の鍵で引く密な表 2 つ(刻みの偶奇)で刻みをまたぐ(D-619。溢れない。T-1164・ADR-0484。
// セーブとハッシュに入れる形は CPU と同じ CascadeCarry に読める)。
//
// 使い方:
//   auto light = GpuCascadeLight::Create(device, nest, grid, params, {});
//   light->RecordUpload(list, carry) → 投げて待つ → light->RecordTick(list, nest, ring, tick) …  light->RecordReadback(list) → 待つ → light->Read()
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gpu/com_ptr.h"
#include "gpu/stage_stamps.h"
#include "sim/cascade_light.h"
#include "sim/gpu_multires.h"

namespace bicameral::sim {

    // 置き場の大きさ(溢れたら Read の overflow に印。場面に合わせて呼ぶ側が決める)
    struct GpuCascadeCapacity {
        uint32_t faces = 1u << 15;  // 1 刻みに放つ面の数
        uint32_t bags = 1u << 20;   // 袋の表 1 つの番地(2 の冪。段 1 つの袋の数の 2 倍ほど)
        uint32_t carry =
            1u << 18;  // RecordUpload で写す初めの持ち越しの数の上限(刻みの中の持ち越しは鍵で引く密な表なので溢れない)
    };

    struct GpuCascadeTickResult {
        CascadeTickResult result;  // CPU と同じ形(持ち越しはセル・面・帯の順に並べ直す)
        uint32_t activeFaces = 0;  // 段 0 で放った面の数
        uint32_t overflow = 0;  // 1 = 放つ面・2 = 袋が溢れた・4 = 木を読む写しが 0.5 m より細かい葉に当たった(T-1117)
        uint32_t bagPeak = 0;   // 段 1 つの袋の数の最大(作ってから)
    };

    class GpuCascadeLight {
    public:
        [[nodiscard]] static std::expected<GpuCascadeLight, std::string> Create(ID3D12Device5* device,
                                                                                const GpuMultires& nest,
                                                                                const CascadeGrid& grid,
                                                                                const CascadeParams& params,
                                                                                const GpuCascadeCapacity& capacity);

        // 木を読む形(T-1117): 升目の箱は shape の世界の箱(MakeCascadeTreeBox。形だけ読む)、光の値と温度は毎刻み RecordTreeGrid が
        // 木から書く。物質の光の値(table.speciesLight)は作業のバッファへ写す。読む枠は世界の枠(shape.capacity.worldBlocks)だけ
        [[nodiscard]] static std::expected<GpuCascadeLight, std::string> CreateFromTree(
            ID3D12Device5* device, const GpuMultires& nest, const MultiresNest& shape, const BakedReactionTable& table,
            const CascadeParams& params, const GpuCascadeCapacity& capacity);

        // 仮の世界用(T-1154): 自分のルート署名で動く。grid の光の値と温度は初めの値(毎刻み呼ぶ側が作業のバッファへ書き直す)
        [[nodiscard]] static std::expected<GpuCascadeLight, std::string> CreateStandalone(
            ID3D12Device5* device, const CascadeGrid& grid, const CascadeParams& params,
            const GpuCascadeCapacity& capacity);

        // 作業(段の形・光の値・温度・初めの持ち越し)を写し、状態を 0 にする。1 本のリストに 1 回
        [[nodiscard]] bool RecordUpload(ID3D12GraphicsCommandList10* list, GpuMultires& nest,
                                        D3D12_GPU_VIRTUAL_ADDRESS debugRing, const CascadeCarry& carry);

        // 1 刻み(前の刻みの持ち越しを読み、次の刻みへの持ち越しを書く)
        void RecordTick(ID3D12GraphicsCommandList10* list, GpuMultires& nest, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                        uint64_t tick);

        // 木の葉 → 作業の光の値と温度(CreateFromTree で作った時だけ。RecordTick の前に刻みごとに 1 回)。
        // 0.5 m より細かい葉は書かずに Read の overflow に 4 を立てる。CreateFromTree でなければ false
        [[nodiscard]] bool RecordTreeGrid(ID3D12GraphicsCommandList10* list, GpuMultires& nest,
                                          D3D12_GPU_VIRTUAL_ADDRESS debugRing);
        // 写しの形(比べ用・T-1117): false = 1 スレッド = 葉のセル 1 つ(既定)・true = 1 スレッド = 升目のセル 1 つ。結果は同じ
        void SetTreeGridPerGridCell(bool perGridCell) { m_treeGridPerGridCell = perGridCell; }

        // --- 仮の世界用(CreateStandalone で作った時だけ)---
        // 初めの持ち越しは表 0(刻み 0 から始める世界)。作業のバッファは既定のヒープへ写し、UAV の状態で返す
        [[nodiscard]] bool RecordUpload(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                        const CascadeCarry& carry);
        // 刻み tick: 表 tick & 1 を読み、表 1 − (tick & 1) へ書く。
        // stamps があれば起動ごとの後ろに段の印(firstStage + パイプラインの番号。StageName。測り用・T-1161)
        void RecordTick(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t tick,
                        gpu::StageStamps* stamps = nullptr, uint8_t firstStage = 0);

        // 起動の予告(仮の世界用・T-1229): この刻みに流す物が無ければ、RecordTick と呼ぶ側の段を GPU が飛ばす(CPU は読み戻さない)。
        // RecordPlan は RcPlanTick で「流す」の印を作って予告のバッファへ書く(刻みごとに RecordTick の前に 1 回。放つセルの印は
        // 呼ぶ側の写しの段が作業の RC_GPU_WORK_EMITTERS へ書く)。BeginSkipIfIdle〜EndSkipIfIdle の間の起動は、印が 0 なら流れない。
        // 飛ばすのは「前に流した刻みが何も放たず受けず・入る持ち越しが空・放つセルが無い」時だけなので、結果は流した時と同じ(D-428)
        void RecordPlan(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t tick,
                        gpu::StageStamps* stamps = nullptr, uint8_t firstStage = 0);
        void BeginSkipIfIdle(ID3D12GraphicsCommandList10* list) const;
        void EndSkipIfIdle(ID3D12GraphicsCommandList10* list) const;

        // 段 1 からの間接の起動(仮の世界用・T-1235。既定は切る: 64³ で速くならなかった。ADR-0495)。入れても結果は同じ
        void SetIndirectLevels(bool enabled) { m_indirectLevels = enabled; }

        // 段の印の番号の数(パイプラインの数)と名前(測りのログ用)
        static constexpr uint32_t STAGE_COUNT = 12;
        [[nodiscard]] static std::string_view StageName(uint32_t stage);

        [[nodiscard]] ID3D12Resource* Work() const { return m_work.Get(); }
        [[nodiscard]] ID3D12Resource* State() const { return m_state.Get(); }
        [[nodiscard]] const reaction::RcGpuParams& Params() const { return m_params; }
        // 刻みをまたぐ状態(頭の語・放った・吸った・持ち越しの表 2 つ)のバイト数。状態のバッファの頭から。ほかは刻みの中で作って空に戻す
        [[nodiscard]] uint64_t PersistentStateBytes() const { return m_readbackBytes; }

        // 最後の刻みの結果(頭の語・放った・吸った・持ち越し)を読み戻す
        void RecordReadback(ID3D12GraphicsCommandList10* list);

        [[nodiscard]] std::expected<GpuCascadeTickResult, std::string> Read() const;

        // 置いたバッファの大きさ(作業 + 状態。読み戻しは除く)
        [[nodiscard]] uint64_t VramBytes() const { return m_workBytes + m_stateBytes; }

    private:
        GpuCascadeLight() = default;

        // nest が nullptr なら自分のルート署名で(CreateStandalone)
        // ownU3・after は自分のルート署名の時だけ(u3 に結ぶ物・起動の後ろの UAV のバリアにまとめる状態の遷移。T-1235)
        void Dispatch(ID3D12GraphicsCommandList10* list, GpuMultires* nest, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                      size_t pipeline, uint64_t items, std::array<uint32_t, 4> external,
                      ID3D12Resource* ownU3 = nullptr, std::span<const D3D12_RESOURCE_BARRIER> after = {}) const;
        // 自分のルート署名で結ぶ(external[2] に置いたグループの数の歩幅を入れる)。置いたグループの数を返す
        uint64_t BindOwn(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, size_t pipeline,
                         uint64_t items, std::array<uint32_t, 4> external, ID3D12Resource* ownU3) const;
        // 段 1 から(自分のルート署名・T-1235): 引数の表のグループの数で起動する
        void DispatchIndirect(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, size_t pipeline,
                              uint64_t items, std::array<uint32_t, 4> external, uint64_t argsOffset,
                              std::span<const D3D12_RESOURCE_BARRIER> after) const;
        static void BarrierAfter(ID3D12GraphicsCommandList10* list, std::span<const D3D12_RESOURCE_BARRIER> after);
        void RecordLevelsIndirect(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                  uint32_t tickLow, uint32_t out, gpu::StageStamps* stamps, uint8_t firstStage) const;
        [[nodiscard]] bool CreateLevelArgs(ID3D12Device5* device);
        bool RecordUploadWith(ID3D12GraphicsCommandList10* list, GpuMultires* nest, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                              const CascadeCarry& carry);
        void RecordTickWith(ID3D12GraphicsCommandList10* list, GpuMultires* nest, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                            uint64_t tick, uint32_t in, gpu::StageStamps* stamps = nullptr, uint8_t firstStage = 0);
        // treeLights は木を読む形の物質の光の値(作業のバッファの温度の後ろへ。空なら置かない)
        static std::expected<GpuCascadeLight, std::string> CreateWith(
            ID3D12Device5* device, ID3D12RootSignature* rootSignature, const CascadeGrid& grid,
            const CascadeParams& params, const GpuCascadeCapacity& capacity,
            std::span<const reaction::LtSpeciesLight> treeLights = {});

        ComPtr<ID3D12RootSignature> m_ownRoot;  // CreateStandalone の時だけ
        std::vector<ComPtr<ID3D12PipelineState>> m_pipelines;
        ComPtr<ID3D12Resource> m_work;        // u4(既定のヒープ)
        ComPtr<ID3D12Resource> m_workUpload;  // その写しの元
        ComPtr<ID3D12Resource> m_state;       // u5
        ComPtr<ID3D12Resource> m_readback;
        ComPtr<ID3D12Resource>
            m_predicate;  // 起動の予告(uint64 1 語。COMMON で置き、使う間だけ写し先・予告にする。T-1229)

        // --- 段 1 からの間接の起動(CreateStandalone の時だけ。T-1235)---
        ComPtr<ID3D12Resource>
            m_levelArgs;  // 段ごとの起動の引数(u3。COMMON で置き、刻みの中で書く間 UAV・読む間 INDIRECT_ARGUMENT)
        ComPtr<ID3D12CommandSignature> m_dispatchSignature;  // 起動 1 回だけの形(ルートの引数は変えない)
        bool m_indirectLevels = false;

        // --- 木を読む形(CreateFromTree の時だけ。T-1117)---
        ComPtr<ID3D12PipelineState> m_treeGrid;
        ComPtr<ID3D12PipelineState> m_treeGridCells;
        bool m_treeGridPerGridCell = false;
        uint32_t m_treeWorldBlocks = 0;
        uint32_t m_treeLightsOffset = 0;          // 作業の中の物質の光の値(バイト)
        std::vector<std::byte> m_workBytesImage;  // 作業の中身(初めの持ち越しの前まで)
        reaction::RcGpuParams m_params{};
        uint64_t m_workBytes = 0;
        uint64_t m_stateBytes = 0;
        uint64_t m_readbackBytes = 0;
        uint32_t m_parity = 0;  // 次の刻みが読む持ち越しの表
        size_t m_cellCount = 0;
    };

}  // namespace bicameral::sim
