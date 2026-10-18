// transport_packet.h — 当てる段の包み(common/transport_apply.hlsli の TxPacket)をセルごとに足し合わせ、セルに当てる CPU(T-0770)。
//
// データの流れ: 物質を運ぶ種類(液体・気体)が面ごとに包みを作る → 世界の AddPacket が上流・下流のセルの変化(TxCellChange)に足す
//   → 刻みの終わりに ApplyCellChange でセルへ(成分は物質 ID の順・0 の成分は消す)。運ぶ欄の値(carried)は種類が自分の欄へ当てる。
// 足し合わせは整数の和なので、包みを足す順によらない(Jacobi)。仮の世界の液体の CellDelta・ApplyDelta(probe_liquid.cpp)と同じ式
// (仮の世界は基準として残す。D-513)。
// 浮動小数点は使わない(04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "common/transport_apply.hlsli"

namespace bicameral::sim {

    // セル 1 つの変化(包みの和。物質 ID → 増減)
    struct TxCellChange {
        std::vector<std::pair<uint32_t, int64_t>> amounts;
        int64_t energy = 0;
        std::array<int64_t, multires::TX_PACKET_CARRIED> carried = {};
        bool touched = false;  // 物質が出入りした(エネルギーはこれが立った時だけ当てる)
    };

    // 包みを変化に足す(incoming = 下流 = 足す、そうでなければ上流 = 引く)
    void AddPacket(TxCellChange& change, const multires::TxPacket& packet, bool incoming);

    // 変化をセルに当てる(物質とエネルギー。成分は物質 ID の順・0 の成分は消す)。値が変わったら true。
    // セルのインラインの成分の数を超える変化は断る(FX_ASSERT。溢れのセルの世界は呼ぶ側が断る)
    bool ApplyCellChange(reaction::RxCell& cell, const TxCellChange& change);

}  // namespace bicameral::sim
