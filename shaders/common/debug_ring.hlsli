// debug_ring.hlsli — シェーダー(Work Graphs のノードも)から書ける printf / assert のリング(T-0003、docs/design/16-debug-test.md §1)。
//
// データの流れ:
//   シェーダー: DEBUG_PRINT(DebugFormat::名前, 引数...) / DEBUG_ASSERT(条件, DebugFormat::名前, 引数...)
//     → リング(u0 space1 の RWByteAddressBuffer)の空きを atomic で 1 つ取り、書式の番号・行・引数を 64 バイトで書く
//   CPU: gpu::DebugRing(engine/src/gpu/debug_ring.h)がフレームの終わりに読み戻して空にし、書式(debug_formats.hlsli)に
//     当てはめてログ(ADR-0006)に出す。溢れた分は書かずに数だけ数える(落とした数 = 要求の数 − 容量)。
//
// 使い方(HLSL):
//   #include "common/debug_ring.hlsli"
//   DEBUG_PRINT(DebugFormat::DebugRingGraphLeaf, value);          // 引数は 0〜6 個。uint32_t / int32_t / uint64_t / int64_t / bool
//   DEBUG_ASSERT(count <= limit, DebugFormat::名前, count, limit);  // 条件が偽のときだけ書く(ログでは Error)
//   ルート署名に u0 space1 のルートの UAV が要る(gpu::RootSignatureLayout::debugRing)。
//   引数は 2 回評価しない(関数に渡す)が、副作用のある式は避ける(デバッグを切ると評価されなくなる)。
//
// 有効になるのは BICAMERAL_GPU_DEBUG が 1 のときだけ(shaders/CMakeLists.txt が Debug のシミュのシェーダーに付ける)。
// 0 なら DEBUG_PRINT・DEBUG_ASSERT は何もしない(リングも宣言しない)。C++ から読むとレイアウトの定数と DebugFormat だけになる。
// 値は整数だけ(04 R1)。浮動小数点を出したいときは固定小数点の整数のまま出し、書式の側で読み方を書く。
#ifndef BICAMERAL_DEBUG_RING_HLSLI
#define BICAMERAL_DEBUG_RING_HLSLI

#ifdef __cplusplus
#include <cstdint>
#define DEBUG_RING_CONST inline constexpr
#define DEBUG_RING_NAMESPACE_BEGIN namespace bicameral {
#define DEBUG_RING_NAMESPACE_END }
#else
#define DEBUG_RING_CONST static const
#define DEBUG_RING_NAMESPACE_BEGIN
#define DEBUG_RING_NAMESPACE_END
#endif

DEBUG_RING_NAMESPACE_BEGIN

// --- リングのレイアウト(HLSL と C++ で共通)-----------------------------------------------------
// 先頭 16 バイトが見出し: [0] 書こうとした数(atomic で増やす)。残りは予約(0)。
// その後ろに 64 バイトのレコードが DEBUG_RING_CAPACITY 個。レコードの 16 語:
//   [0] 書式の番号  [1] 種類(bit 0〜3)| 引数の数(bit 4〜7)| 引数の型(bit 8 から 2bit ずつ)  [2] 行  [3] 予約(0)
//   [4..15] 引数 6 個 × 64bit(下位の語、上位の語の順。32bit の値は符号を広げて 64bit にする)
DEBUG_RING_CONST uint32_t DEBUG_RING_CAPACITY = 4096;  // 1 フレームに入るレコードの数(256 KiB)
DEBUG_RING_CONST uint32_t DEBUG_RING_HEADER_BYTES = 16;
DEBUG_RING_CONST uint32_t DEBUG_RECORD_WORDS = 16;
DEBUG_RING_CONST uint32_t DEBUG_RECORD_BYTES = DEBUG_RECORD_WORDS * 4;
DEBUG_RING_CONST uint32_t DEBUG_RECORD_ARG_WORD = 4;  // 引数が始まる語
DEBUG_RING_CONST uint32_t DEBUG_RECORD_MAX_ARGS = 6;
DEBUG_RING_CONST uint32_t DEBUG_RING_BYTES = DEBUG_RING_HEADER_BYTES + DEBUG_RING_CAPACITY * DEBUG_RECORD_BYTES;
DEBUG_RING_CONST uint32_t DEBUG_RING_REGISTER_SPACE = 1;  // u0 space1。space0 はシェーダーそれぞれが自由に使う

DEBUG_RING_CONST uint32_t DEBUG_KIND_PRINT = 0;
DEBUG_RING_CONST uint32_t DEBUG_KIND_ASSERT = 1;

DEBUG_RING_CONST uint32_t DEBUG_ARG_U32 = 0;
DEBUG_RING_CONST uint32_t DEBUG_ARG_I32 = 1;
DEBUG_RING_CONST uint32_t DEBUG_ARG_U64 = 2;
DEBUG_RING_CONST uint32_t DEBUG_ARG_I64 = 3;

// --- 書式の番号(debug_formats.hlsli の並び順)----------------------------------------------------
// 型はレコードの語と同じ 32bit(HLSL と C++ で同じにする)
// NOLINTNEXTLINE(performance-enum-size)
enum class DebugFormat : uint32_t {
#define DEBUG_FORMAT(name, channel, where, text) name,
#include "common/debug_formats.hlsli"
#undef DEBUG_FORMAT
    Count
};

DEBUG_RING_NAMESPACE_END

#ifdef __cplusplus
// CPU のリファレンスでは、同じコードの DEBUG_ASSERT を C++ の assert にする(DEBUG_PRINT は何もしない)
#include <cassert>
#define DEBUG_PRINT(...) ((void)0)
#define DEBUG_ASSERT(condition, ...) assert(condition)

#else  // HLSL

#ifndef BICAMERAL_GPU_DEBUG
#define BICAMERAL_GPU_DEBUG 0
#endif

#if BICAMERAL_GPU_DEBUG

RWByteAddressBuffer bicameralDebugRing : register(u0, space1);

// --- 引数を 64bit の 2 語と型の番号にする(型ごとの多重定義)---
uint32_t DebugArgKind(uint32_t value) {
    return DEBUG_ARG_U32;
}
uint32_t DebugArgKind(int32_t value) {
    return DEBUG_ARG_I32;
}
uint32_t DebugArgKind(uint64_t value) {
    return DEBUG_ARG_U64;
}
uint32_t DebugArgKind(int64_t value) {
    return DEBUG_ARG_I64;
}
uint32_t DebugArgKind(bool value) {
    return DEBUG_ARG_U32;
}

uint2 DebugArgWords(uint32_t value) {
    return uint2(value, 0u);
}
uint2 DebugArgWords(int32_t value) {
    return uint2((uint32_t)value, value < 0 ? 0xFFFFFFFFu : 0u);  // 符号を上位の語へ広げる
}
uint2 DebugArgWords(uint64_t value) {
    return uint2((uint32_t)value, (uint32_t)(value >> 32));
}
uint2 DebugArgWords(int64_t value) {
    return DebugArgWords((uint64_t)value);
}
uint2 DebugArgWords(bool value) {
    return uint2(value ? 1u : 0u, 0u);
}

// --- 1 レコードを書く ---
// 空きの番号を atomic で取る。容量を超えたら書かない(数だけが増え、CPU が落とした数として出す)
void DebugWrite(uint32_t kind, uint32_t sourceLine, DebugFormat format, uint32_t argCount, uint32_t argKinds,
                uint4 args01, uint4 args23, uint4 args45) {
    uint32_t index;
    bicameralDebugRing.InterlockedAdd(0, 1u, index);
    if (index >= DEBUG_RING_CAPACITY) return;
    const uint32_t offset = DEBUG_RING_HEADER_BYTES + index * DEBUG_RECORD_BYTES;
    bicameralDebugRing.Store4(offset,
                              uint4((uint32_t)format, kind | (argCount << 4) | (argKinds << 8), sourceLine, 0u));
    bicameralDebugRing.Store4(offset + 16, args01);
    bicameralDebugRing.Store4(offset + 32, args23);
    bicameralDebugRing.Store4(offset + 48, args45);
}

// --- 引数の数ごとの入口(DEBUG_PRINT / DEBUG_ASSERT から呼ぶ)---
void DebugEmitAt(uint32_t kind, uint32_t sourceLine, DebugFormat format) {
    DebugWrite(kind, sourceLine, format, 0, 0, uint4(0, 0, 0, 0), uint4(0, 0, 0, 0), uint4(0, 0, 0, 0));
}

template <typename A0>
void DebugEmitAt(uint32_t kind, uint32_t sourceLine, DebugFormat format, A0 a0) {
    DebugWrite(kind, sourceLine, format, 1, DebugArgKind(a0), uint4(DebugArgWords(a0), 0, 0), uint4(0, 0, 0, 0),
               uint4(0, 0, 0, 0));
}

template <typename A0, typename A1>
void DebugEmitAt(uint32_t kind, uint32_t sourceLine, DebugFormat format, A0 a0, A1 a1) {
    DebugWrite(kind, sourceLine, format, 2, DebugArgKind(a0) | (DebugArgKind(a1) << 2),
               uint4(DebugArgWords(a0), DebugArgWords(a1)), uint4(0, 0, 0, 0), uint4(0, 0, 0, 0));
}

template <typename A0, typename A1, typename A2>
void DebugEmitAt(uint32_t kind, uint32_t sourceLine, DebugFormat format, A0 a0, A1 a1, A2 a2) {
    DebugWrite(kind, sourceLine, format, 3, DebugArgKind(a0) | (DebugArgKind(a1) << 2) | (DebugArgKind(a2) << 4),
               uint4(DebugArgWords(a0), DebugArgWords(a1)), uint4(DebugArgWords(a2), 0, 0), uint4(0, 0, 0, 0));
}

template <typename A0, typename A1, typename A2, typename A3>
void DebugEmitAt(uint32_t kind, uint32_t sourceLine, DebugFormat format, A0 a0, A1 a1, A2 a2, A3 a3) {
    const uint32_t kinds =
        DebugArgKind(a0) | (DebugArgKind(a1) << 2) | (DebugArgKind(a2) << 4) | (DebugArgKind(a3) << 6);
    DebugWrite(kind, sourceLine, format, 4, kinds, uint4(DebugArgWords(a0), DebugArgWords(a1)),
               uint4(DebugArgWords(a2), DebugArgWords(a3)), uint4(0, 0, 0, 0));
}

template <typename A0, typename A1, typename A2, typename A3, typename A4>
void DebugEmitAt(uint32_t kind, uint32_t sourceLine, DebugFormat format, A0 a0, A1 a1, A2 a2, A3 a3, A4 a4) {
    const uint32_t kinds = DebugArgKind(a0) | (DebugArgKind(a1) << 2) | (DebugArgKind(a2) << 4) |
                           (DebugArgKind(a3) << 6) | (DebugArgKind(a4) << 8);
    DebugWrite(kind, sourceLine, format, 5, kinds, uint4(DebugArgWords(a0), DebugArgWords(a1)),
               uint4(DebugArgWords(a2), DebugArgWords(a3)), uint4(DebugArgWords(a4), 0, 0));
}

template <typename A0, typename A1, typename A2, typename A3, typename A4, typename A5>
void DebugEmitAt(uint32_t kind, uint32_t sourceLine, DebugFormat format, A0 a0, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5) {
    const uint32_t kinds = DebugArgKind(a0) | (DebugArgKind(a1) << 2) | (DebugArgKind(a2) << 4) |
                           (DebugArgKind(a3) << 6) | (DebugArgKind(a4) << 8) | (DebugArgKind(a5) << 10);
    DebugWrite(kind, sourceLine, format, 6, kinds, uint4(DebugArgWords(a0), DebugArgWords(a1)),
               uint4(DebugArgWords(a2), DebugArgWords(a3)), uint4(DebugArgWords(a4), DebugArgWords(a5)));
}

// 行は書いた所の __LINE__(マクロの中の __LINE__ は呼んだ行になる)
#define DEBUG_PRINT(...) DebugEmitAt(DEBUG_KIND_PRINT, __LINE__, __VA_ARGS__)
#define DEBUG_ASSERT(condition, ...)                                             \
    do {                                                                         \
        if (!(condition)) DebugEmitAt(DEBUG_KIND_ASSERT, __LINE__, __VA_ARGS__); \
    } while (false)

#else  // BICAMERAL_GPU_DEBUG == 0: 何もしない(引数も評価しない)

#define DEBUG_PRINT(...)
#define DEBUG_ASSERT(condition, ...)

#endif  // BICAMERAL_GPU_DEBUG
#endif  // __cplusplus

#endif  // BICAMERAL_DEBUG_RING_HLSLI
