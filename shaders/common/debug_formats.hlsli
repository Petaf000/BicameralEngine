// debug_formats.hlsli — GPU のデバッグ出力(debug_ring.hlsli)の書式の一覧(T-0003、docs/design/16-debug-test.md §1)。
// シェーダーは書式の番号(DebugFormat::名前)と整数の引数だけをリングに書き、CPU(engine/src/gpu/debug_ring.cpp)が
// ここの書式に当てはめて 1 行にする。HLSL は文字列を扱えないので、文字列は CPU だけが持つ。
//
// 1 行 = DEBUG_FORMAT(名前, ログのチャンネル, 場所, 書式)
//   名前       DebugFormat::名前 になる(HLSL と C++ で同じ)。並び順が番号なので、途中に足してよいがセーブには残さない
//   チャンネル core/log.h の Channel の名前(Gpu・WorkGraph・Reaction など)
//   場所       ファイル名かノード名(ログに「場所:行」で出る。行は書いた所の __LINE__)
//   書式       std::format の書式。{} は引数の順。{:#x} や {:>8} も使える(値は整数だけ。04 R1)
//
// インクルードガードは付けない: 読む側が DEBUG_FORMAT を定義してから読み、読んだら #undef する(debug_ring.hlsli・debug_ring.cpp)。

// --- 共通 ---
DEBUG_FORMAT(FxAssert, Gpu, "fixed.hlsli", "FX_ASSERT に失敗(桁あふれ・範囲外の引数)")

// --- テスト(tests/gpu_debug_ring_test.cpp)---
DEBUG_FORMAT(DebugRingProbe, Gpu, "debug_ring_probe/Main", "thread={} negative={} large={:#x} signed64={}")
DEBUG_FORMAT(DebugRingProbeAssert, Gpu, "debug_ring_probe/Main", "thread={} で assert")
DEBUG_FORMAT(DebugRingGraphLeaf, WorkGraph, "debug_ring_graph_probe/Leaf", "value={}")
