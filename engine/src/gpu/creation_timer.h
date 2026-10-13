// creation_timer.h — パイプライン・Work Graph の状態オブジェクトの作成にかかった時間を測る(T-0427)。
// 壁時計の時間と、プロセス全体の CPU 時間(ドライバがコンパイルに使うスレッドも含む。GetProcessTimes)を並べて出す:
//   CPU 時間が壁時計と同じくらいかそれ以上 → 作成そのものが重い(ドライバのコンパイル。キャッシュに当たっていない)
//   壁時計だけ長く CPU 時間が短い           → 待ち(ほかのプロセスとの CPU の取り合い・ドライバの中のロック)
// 測るだけで、作成の手順には何も足さない。
#pragma once

#include <cstdint>

#include "core/aliases.h"

namespace bicameral::gpu {

    struct CreationCost {
        int64_t wallMs = 0;
        int64_t processCpuMs = 0;  // プロセスの全スレッドの user + kernel の合計(並列なら壁時計より長くなる)
    };

    class CreationTimer {
    public:
        CreationTimer() : m_startWall(chr::steady_clock::now()), m_startCpu100ns(ProcessCpu100ns()) {}

        // 作ってからの時間
        CreationCost Elapsed() const {
            const auto wall = chr::duration_cast<chr::milliseconds>(chr::steady_clock::now() - m_startWall);
            const uint64_t cpu100ns = ProcessCpu100ns() - m_startCpu100ns;
            return {.wallMs = wall.count(), .processCpuMs = static_cast<int64_t>(cpu100ns / 10'000)};
        }

    private:
        // プロセスの CPU 時間(100 ns 単位)。取れなければ 0
        static uint64_t ProcessCpu100ns() {
            FILETIME creation{};
            FILETIME exit{};
            FILETIME kernel{};
            FILETIME user{};
            if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
                return 0;

            const auto toTicks = [](const FILETIME& time) {
                return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
            };
            return toTicks(kernel) + toTicks(user);
        }

        chr::steady_clock::time_point m_startWall;
        uint64_t m_startCpu100ns = 0;
    };

}  // namespace bicameral::gpu
