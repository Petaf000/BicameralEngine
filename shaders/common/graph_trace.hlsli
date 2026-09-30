// graph_trace.hlsli — Work Graphs の連鎖のトレース: 「どのレコードがどのレコードを生んだか」を GPU のバッファに書く(T-0087、16 §1.3)。
// 反応の連鎖は GPU の中だけで走り切る(CPU は途中を知らない)ので、選んだ範囲(刻み・場所)だけ事実を書き残し、後から CPU で木に組み直す。
//
// データの流れ:
//   ノード(や compute): GtWants(刻み, 場所) で範囲を見て、GtRecord / GtReserve + GtStore で 16 バイトの記録を追記する
//     → u2 space1 の RWByteAddressBuffer(見出し 64 バイト + 記録 × 容量)
//   CPU: gpu::GraphTrace(engine/src/gpu/graph_trace.h)がフレームの終わりに読み戻して数を 0 に戻す(範囲の指定は残す)。
//     記録の並びは atomic の順(毎回変わる)なので、CPU が (刻み, 種類, 主, 従) の順に並べ替えてから比べる・木にする。
//
// 記録に入れるのは「実行の順番に依存しない事実」だけ(04 R1〜R8)。例えば伝導で「誰が先に予定したか」は毎回変わるので書かず、
// 「このブロックがあのブロックを起こそうとした」を全部書く(どれが親かは CPU が決まった規則で選ぶ。sim/probe_trace)。
// 同じ入力なら、並べ替えた記録の列は毎回同じになる(容量を越えて落とした場合を除く。落とした数は CPU に出る)。
// 範囲の指定が無効なら(既定)、見出しの 1 語を読むだけで何も書かない。Release でも使える(費用は docs/perf.md)。
#ifndef BICAMERAL_GRAPH_TRACE_HLSLI
#define BICAMERAL_GRAPH_TRACE_HLSLI

#ifdef __cplusplus
#include <cstdint>
#define GT_CONST inline constexpr
#define GT_NAMESPACE_BEGIN namespace bicameral {
#define GT_NAMESPACE_END }
#else
#define GT_CONST static const
#define GT_NAMESPACE_BEGIN
#define GT_NAMESPACE_END
#endif

GT_NAMESPACE_BEGIN

// --- レイアウト(HLSL と C++ で共通)--------------------------------------------------------------
// 見出し(語): [0] 書こうとした数(毎フレーム 0 に戻す)[1〜3] 予約
//   範囲(CPU が書き、フレームをまたいで残る): [4] 有効なら 1 [5] 容量 [6, 7] 刻みの始め(含む)の下位・上位
//   [8, 9] 刻みの終わり(含まない)の下位・上位 [10〜12] 場所の箱の最小(含む)x, y, z [13〜15] 最大(含まない)x, y, z
// 記録(16 バイト): [0, 1] 刻みの下位・上位 [2] 種類 << 24 | 主(24bit)[3] 従(32bit)。主と従の意味は種類ごとに使う側が決める
GT_CONST uint32_t GT_HEADER_BYTES = 64;
GT_CONST uint32_t GT_RESET_BYTES = 16;  // 毎フレーム 0 に戻す部分(数)
GT_CONST uint32_t GT_FILTER_OFFSET = 16;
GT_CONST uint32_t GT_FILTER_BYTES = GT_HEADER_BYTES - GT_FILTER_OFFSET;
GT_CONST uint32_t GT_RECORD_BYTES = 16;
GT_CONST uint32_t GT_SUBJECT_BITS = 24;
GT_CONST uint32_t GT_SUBJECT_MASK = (1u << GT_SUBJECT_BITS) - 1;

GT_CONST uint32_t GT_WORD_REQUESTED = 0;
GT_CONST uint32_t GT_WORD_ENABLED = 4;
GT_CONST uint32_t GT_WORD_CAPACITY = 5;
GT_CONST uint32_t GT_WORD_TICK_BEGIN = 6;
GT_CONST uint32_t GT_WORD_TICK_END = 8;
GT_CONST uint32_t GT_WORD_BOX_MIN = 10;
GT_CONST uint32_t GT_WORD_BOX_MAX = 13;

GT_CONST uint32_t GT_REGISTER = 2;  // u2 space1(u0 はデバッグのリング、u1 は Work Graphs のカウンタ)
GT_CONST uint32_t GT_REGISTER_SPACE = 1;

GT_NAMESPACE_END

#ifndef __cplusplus  // --- HLSL ---

RWByteAddressBuffer bicameralGraphTrace : register(u2, space1);

uint64_t GtLoadTick(uint32_t word) {
    const uint2 halves = bicameralGraphTrace.Load2(word * 4);

    return (uint64_t)halves.x | ((uint64_t)halves.y << 32);
}

// 刻み tick を記録するか(範囲が有効で、[始め, 終わり) に入る)。グループやウェーブで同じ値なので分岐の外で 1 回呼べばよい
bool GtWantsTick(uint64_t tick) {
    if (bicameralGraphTrace.Load(GT_WORD_ENABLED * 4) == 0)
        return false;

    return tick >= GtLoadTick(GT_WORD_TICK_BEGIN) && tick < GtLoadTick(GT_WORD_TICK_END);
}

// 場所 place(使う側の単位。伝導ならブロックの座標)が箱 [最小, 最大) に入るか
bool GtWantsPlace(uint3 place) {
    const uint3 boxMin = bicameralGraphTrace.Load3(GT_WORD_BOX_MIN * 4);
    const uint3 boxMax = bicameralGraphTrace.Load3(GT_WORD_BOX_MAX * 4);

    return all(place >= boxMin) && all(place < boxMax);
}

void GtStore(uint32_t slot, uint64_t tick, uint32_t kind, uint32_t subject, uint32_t object) {
    if (slot >= bicameralGraphTrace.Load(GT_WORD_CAPACITY * 4))
        return;  // 溢れた分は書かない(書こうとした数との差を CPU が数える)

    const uint32_t address = GT_HEADER_BYTES + slot * GT_RECORD_BYTES;
    bicameralGraphTrace.Store4(address, uint4((uint32_t)tick, (uint32_t)(tick >> 32),
                                              (kind << GT_SUBJECT_BITS) | (subject & GT_SUBJECT_MASK), object));
}

// 1 件書く(分岐の中で、1 スレッドだけが呼ぶ所向け)。範囲は呼ぶ側が GtWantsTick / GtWantsPlace で見ておく
void GtRecord(uint64_t tick, uint32_t kind, uint32_t subject, uint32_t object) {
    uint32_t slot;
    bicameralGraphTrace.InterlockedAdd(GT_WORD_REQUESTED * 4, 1, slot);
    GtStore(slot, tick, kind, subject, object);
}

// count 件ぶんの場所を取り、最初の番号を返す(ウェーブで足してから 1 回だけ atomic)。
// ウェーブの生きているレーンが全部呼ぶこと(書かないレーンは count = 0)。返した番号から count 件を GtStore で書く
uint32_t GtReserve(uint32_t count) {
    const uint32_t prefix = WavePrefixSum(count);
    const uint32_t total = WaveActiveSum(count);
    uint32_t base = 0;
    if (WaveIsFirstLane() && total != 0)
        bicameralGraphTrace.InterlockedAdd(GT_WORD_REQUESTED * 4, total, base);

    return WaveReadLaneFirst(base) + prefix;
}

#endif  // !__cplusplus

#endif  // BICAMERAL_GRAPH_TRACE_HLSLI
