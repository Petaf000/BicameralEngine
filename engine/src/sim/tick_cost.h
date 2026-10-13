// tick_cost.h — 単位の GPU 時間を刻みごとにまとめ、刻みの要約(活性のブロックの数)と組にする(T-0413・ADR-0204。D-468・14 §2)。
//
// 重さの検査(写しの早送り。sim/world_copy)と、本物の世界で予算を超えた刻みの警告(frame/frame_loop)が同じ部品で 1 刻みの重さを集める。
// データ: ProbeSim::ReadFrame の読み戻し(投げた単位ごとの GPU 時間 + このフレームで終えた刻みの要約)を投げた順に渡す →
// 終えた刻みごとの TickCost(刻み・GPU の時間・計算したブロックの数)。刻みはフレームをまたいでよい(途中の和を持ち越す)。
// 刻みの最後の単位(HashUnit)が要約を書くので、「終えた刻み」と要約は同じ数・同じ順に並ぶ。食い違ったら(読み落とし・戻した後)
// そのフレームの刻みは数えない(重さの値だけのことで、世界の結果には入らない。D-107)。
// GPU の時間はタイムスタンプの刻みのまま(浮動小数点は使わない。engine/src/sim は検査の対象)。決定的に比べられる値は blocks と刻みの数。
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "sim/probe_sim.h"

namespace bicameral::sim {

    struct TickCost {
        uint64_t tick = 0;               // S(tick) を作った刻み(tick − 1)の重さ(ProbeTickHash::tick と同じ数え方)
        uint64_t gpuTimestampTicks = 0;  // その刻みの単位の GPU の時間の和(タイムスタンプの刻み)
        uint32_t scheduledBlocks = 0;    // 伝導と反応を計算したブロックの数(ProbeTickHash::scheduledBlocks)
    };

    class TickCostAccumulator {
    public:
        explicit TickCostAccumulator(uint32_t unitsPerTick) : m_unitsPerTick(unitsPerTick) {}

        // 投げた順の 1 フレームの読み戻し(firstUnit: unitGpuTicks[0] の刻みの中の番号)。終えた刻みを out の後ろに足す。
        // 終えた刻みと要約の数が食い違えば何も足さず false(次の刻みの始めから数え直す)
        bool Add(uint32_t firstUnit, std::span<const uint64_t> unitGpuTicks, std::span<const ProbeTickHash> hashes,
                 std::vector<TickCost>& out);

        // ProbeFrameReadback をそのまま渡す形
        bool Add(const ProbeFrameReadback& readback, std::vector<TickCost>& out) {
            return Add(readback.firstUnit, readback.unitGpuTicks, readback.hashes, out);
        }

        // 刻みの境界へ戻した後(保存点へ戻す・写しを取り直す)。持ち越した途中の和を捨てる
        void Reset();

    private:
        uint32_t m_unitsPerTick = 1;
        uint32_t m_nextUnit = 0;     // 次に届くはずの単位の番号
        uint64_t m_partial = 0;      // 途中の刻みの GPU の時間の和
        bool m_partialValid = true;  // 途中の刻みの単位を始めから全部見た
    };

}  // namespace bicameral::sim
