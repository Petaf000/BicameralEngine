// command.h — CPU → GPU のコマンド(06 §3 の 64 バイトの形。T-0086)。
//
// CPU(View と Controller。D-107)が世界に何かを頼むときの唯一の形。窓の入力・魔法・会話の選択・エディタの変更がこれになる。
//   - CPU は「適用する刻み」(targetTick)と、同じ刻みの中の順番(sequence。CPU が通しで増やす)を付けて GPU のコマンドキューへ足す。
//   - GPU は刻み targetTick の適用の単位で (targetTick, sequence) の順に適用する(sim/probe_sim・shaders/sim/probe_tick.hlsl)。
//   - 再生ファイル(save/replay_file)は初期状態とこの列を持つ。これだけで世界が再現できる(D-205・D-110)。
// payload の中身の意味は type ごと(種類の一覧はシミュごとの .hlsli。今は common/probe_sim.hlsli の PROBE_COMMAND_TYPE_*)。
// ここは GPU を知らない(save からも使う)。
#pragma once

#include <array>
#include <cstdint>

namespace bicameral::sim {

    inline constexpr uint32_t COMMAND_BYTES = 64;
    inline constexpr uint32_t COMMAND_PAYLOAD_WORDS = 12;  // 48 バイト

    struct Command {
        uint64_t targetTick = 0;  // 適用する刻み
        uint32_t sequence = 0;    // 同じ刻みの中の順番(CPU が通しで増やす)
        uint16_t type = 0;
        uint16_t size = 0;  // payload の使っているバイト数
        std::array<uint32_t, COMMAND_PAYLOAD_WORDS> payload{};

        friend bool operator==(const Command&, const Command&) = default;
    };

    static_assert(sizeof(Command) == COMMAND_BYTES);

    // キューと再生ファイルの並び: (targetTick, sequence) の昇順。a が b より前なら true
    [[nodiscard]] constexpr bool CommandPrecedes(const Command& a, const Command& b) {
        return a.targetTick != b.targetTick ? a.targetTick < b.targetTick : a.sequence < b.sequence;
    }

}  // namespace bicameral::sim
