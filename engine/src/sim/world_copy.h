// world_copy.h — 世界の写しの早送り(T-0412・ADR-0198。D-470・D-482 ④・docs/design/14 §2「重さの検査と写しの早送り」)。
//
// 今の世界(ProbeSim)の刻みの境界の状態 S(t) を、別に作った同じ形の世界(写し)へ GPU → GPU で写し、写しだけを早送りして
// 「何刻みで条件に着くか」(何分で火が消えるか)に答える。写しは本物の世界に返さない(D-403。写す元のバッファは読むだけ)。
// 決定性(ADR-0008)があるので、写しに本物と同じコマンドの列を当てれば本物と同じハッシュ列になる(tests/gpu_world_copy_test)。
//
// 流れ(呼ぶ側が写す元と同じキューに順に投げる。キューの順で写す元の状態が決まるので、別のキューには投げない):
//   1. Create(写す元の Table() と、写す元を作った時と同じ物理の場面)→ RecordInitialization を 1 回投げる(写しの世界の初期化)
//   2. RecordStart(写す元, t, 問い): 写す元の最後のリストが刻み t の境界で終わった後・写す元の次のリストより前に投げる
//   3. RecordNext(最大の刻み数) → 投げる → GPU が終えたら Complete() → Finished() まで繰り返す
//   どれも返したリストを順に 1 回で投げ、GPU が終えて Complete() を呼ぶまで次を記録しない(GPU にある写しのリストは 1 組だけ)。
//   早送りは本物の刻みの合間(フレームの余った予算)で回す(D-468: 写しのせいで本物が遅れてはいけない。frame/frame_loop)。
//   同じ写しは何度でも RecordStart から使い直せる(VRAM は写し 1 つ分のまま)。
// 測り方: 刻みごとの要約(ProbeTickHash: ハッシュ・エネルギー・計算したブロックの数)と、決まった間隔の刻みのセルと熱のキャッシュ
// (読み戻し)を条件(until)に渡す。エンジンは中身を持たない(D-454)ので、「火が消えた」のような意味は呼ぶ側(エディタ・遊びの指標
// 〔T-0432〕)が条件で書く。下の小さな道具(物質の量・最高温度)はその材料。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。GPU の時間はタイムスタンプの刻みのまま返す。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "sim/probe_sim.h"
#include "sim/tick_cost.h"

namespace bicameral::sim {

    // 条件に渡す 1 刻みの様子
    struct WorldCopySample {
        uint64_t tick = 0;                       // S(tick)
        const ProbeTickHash* summary = nullptr;  // S(tick) の要約(写しの始めの刻みは nullptr。GPU の表は前の刻みが書く)
        // 読み戻す刻み(始めの刻みと、そこから sampleIntervalTicks ごと)だけ PROBE_CELL_COUNT 個。ほかの刻みは空
        std::span<const reaction::RxCell> cells;
        std::span<const reaction::HcThermalCache> thermal;
    };

    // 早送りの問い
    struct WorldCopyQuery {
        uint64_t maxTicks = 0;             // 早送りの上限(始めの刻みから。1 以上)
        uint32_t sampleIntervalTicks = 0;  // セルと熱を読み戻す間隔(始めの刻みから数える。0 = 読まない)
        // 写しに当てるコマンド((targetTick, sequence) の昇順・targetTick は始めの刻み以上)。本物に当てるのと同じ列なら本物と同じ結果
        std::vector<ProbeCommand> commands;
        // 真になった最初の刻みで止める(無ければ上限まで)。始めの刻み S(t) から刻みの順に呼ぶ。読み戻さない刻みは cells が空
        std::function<bool(const WorldCopySample&)> until;
    };

    struct WorldCopyResult {
        uint64_t startTick = 0;
        uint64_t endTick = 0;               // 止めた刻み(S(endTick) で条件が真になった、か上限)
        bool reached = false;               // 条件が真になった
        bool failed = false;                // 記録できなかった・コマンドが入らなかった(理由はログ)
        std::vector<ProbeTickHash> hashes;  // S(startTick + 1)〜S(endTick) の要約(刻みの順)

        // --- 重さ(重さの検査 T-0413 の材料。docs/perf.md)---
        uint64_t gpuTimestampTicks = 0;  // 早送りのリストの GPU の時間の和(写す・読み戻しを除く。タイムスタンプの刻み)
        uint64_t ticksRun = 0;           // 早送りした刻みの数(条件の刻みより先まで走った分を含む)
        uint32_t frames = 0;             // 投げたリストの組の数
        uint32_t debugAssertCount = 0;   // シェーダーの assert(写しの世界の分)
        // hashes[i] と同じ並びで、S(hashes[i].tick) を作った刻みの GPU の時間(その刻みの単位の和。タイムスタンプの刻み。
        // 数えられなかった刻みは 0。sim/tick_cost。T-0413)。活性のブロックの数は hashes[i].scheduledBlocks
        std::vector<uint64_t> tickGpuTimestampTicks;
    };

    class WorldCopy {
    public:
        // table: 写す元の Table()(GPU が今使っている表)。options: 写す元を作った時と同じ物理の場面・物理の値
        // (重さの試験・トレースの値は使わない)。写しの VRAM は ProbeSim 1 つ分(VramBytes)
        [[nodiscard]] static std::expected<WorldCopy, std::string> Create(ID3D12Device5* device,
                                                                          D3D12_COMMAND_LIST_TYPE listType,
                                                                          const BakedReactionTable& table,
                                                                          const ProbeSimOptions& options);

        // 写しの世界を初期化するリスト(作った後に 1 回。いつ投げてもよい)。投げて終えたら Complete()
        [[nodiscard]] std::vector<ID3D12CommandList*> RecordInitialization();

        // source の S(tick) を写して問いを始めるリスト(写す元の約束はファイルの先頭)。問いが不正なら空(理由はログ)
        [[nodiscard]] std::vector<ID3D12CommandList*> RecordStart(const ProbeSim& source, uint64_t tick,
                                                                  WorldCopyQuery query);

        // 次の早送りのリスト(maxTicks 刻みまで。1 以上。読み戻す刻み・上限・1 リストの単位の上限で短く切る)。走らせる物が無ければ空
        [[nodiscard]] std::vector<ID3D12CommandList*> RecordNext(uint64_t maxTicks);

        // 返したリストを GPU が終えた後に呼ぶ(読み戻し・条件の判定)
        void Complete();

        [[nodiscard]] bool Pending() const { return m_pending; }  // 返したリストの Complete() を待っている
        [[nodiscard]] bool Initialized() const { return m_initialized; }
        [[nodiscard]] bool Running() const { return m_running; }    // RecordStart の後で、まだ終わっていない
        [[nodiscard]] bool Finished() const { return m_finished; }  // 問いが終わった(Result が揃った)
        [[nodiscard]] uint64_t Tick() const { return m_tick; }      // 写しが次に始める刻み(早送りの進み)
        [[nodiscard]] const WorldCopyQuery& Query() const { return m_query; }
        [[nodiscard]] const WorldCopyResult& Result() const { return m_result; }
        [[nodiscard]] uint32_t UnitsPerTick() const { return m_simulation->UnitsPerTick(); }
        [[nodiscard]] uint64_t VramBytes() const { return m_vramBytes; }

        // 写しの世界(試験がセルを読み戻すだけ。フレームの間は COMMON)
        [[nodiscard]] const ProbeSim& Simulation() const { return *m_simulation; }

    private:
        WorldCopy() = default;

        [[nodiscard]] bool CreateReadback(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType);
        [[nodiscard]] ID3D12CommandList* RecordReadback(uint64_t tick);
        [[nodiscard]] std::vector<ProbeCommand> TakeCommands(uint64_t& ticks);
        [[nodiscard]] bool IsSampleTick(uint64_t tick) const;
        bool Judge(uint64_t tick, const ProbeTickHash* summary, bool withCells);
        void Finish(uint64_t endTick, bool reached);
        void Fail(std::string_view reason);

        std::unique_ptr<ProbeSim> m_simulation;
        uint64_t m_vramBytes = 0;
        TickCostAccumulator m_costs{1};  // 単位の GPU 時間を刻みごとに(Create で写しの単位の数に)

        // --- 読み戻し(S(t) のセルと熱。写しの世界の COMMON のバッファから写す)---
        ComPtr<ID3D12CommandAllocator> m_allocator;
        ComPtr<ID3D12GraphicsCommandList10> m_list;
        ComPtr<ID3D12Resource> m_cellReadback;
        ComPtr<ID3D12Resource> m_thermalReadback;
        std::vector<reaction::RxCell> m_cells;
        std::vector<reaction::HcThermalCache> m_thermal;

        // --- 問いと進み ---
        WorldCopyQuery m_query;
        WorldCopyResult m_result;
        uint64_t m_tick = 0;                    // 次に記録する刻み(写しのカーソル。いつも刻みの境界)
        uint64_t m_judgedTick = 0;              // 条件を判定し終えた刻み + 1
        size_t m_nextCommand = 0;               // まだ足していないコマンドの添字
        uint64_t m_pendingSample = UINT64_MAX;  // 返したリストで読み戻す刻み(無ければ UINT64_MAX)
        uint64_t m_pendingEnd = 0;              // 返したリストが終えた後のカーソルの刻み
        bool m_pendingStart = false;            // 返したリストが写しを取るリスト(始めの刻みを判定する)
        bool m_pending = false;
        bool m_initialized = false;
        bool m_running = false;
        bool m_finished = false;
    };

    // --- 条件を書く小さな道具(中身を持たない。物質の ID と温度は呼ぶ側が決める)---

    // 物質 species の世界全体の量(µmol の和)
    [[nodiscard]] uint64_t ProbeSpeciesAmount(std::span<const reaction::RxCell> cells, uint32_t species);

    // 最高温度(mK)と、temperatureMillikelvin 以上のセルの数
    [[nodiscard]] uint32_t ProbeMaxTemperature(std::span<const reaction::HcThermalCache> thermal);
    [[nodiscard]] uint32_t ProbeCellsAtOrAbove(std::span<const reaction::HcThermalCache> thermal,
                                               uint32_t temperatureMillikelvin);

}  // namespace bicameral::sim
