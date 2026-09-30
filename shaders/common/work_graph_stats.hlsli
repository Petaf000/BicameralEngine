// work_graph_stats.hlsli — Work Graphs のノードごとのカウンタと、上限の手前で止める仕組み(T-0008、docs/design/16-debug-test.md §1)。
// GPU の中だけで走り切る連鎖を、CPU から「どのノードが何回起動し、何件出し、どこで上限に当たったか」で見られるようにする。
//
// データの流れ:
//   ノード(や compute): WgCountLaunch / WgGrantOutputs / WgTryRecurse / WgGaugePeak
//     → u1 space1 の RWByteAddressBuffer(ノード WG_STATS_MAX_NODES 個 × WG_NODE_WORDS 語 + 計器 WG_STATS_MAX_GAUGES 個)
//     に atomic で足す(ウェーブでまとめてから 1 回)
//   CPU: gpu::WorkGraphStats(engine/src/gpu/work_graph_stats.h)がフレームの終わりに読み戻して 0 に戻し、ログへ要約と Warning を出す。
//
// 上限(仕様。16 §1): 出力のレコードの数は宣言した MaxRecords を超えると未定義の動作。スレッド起動のノードは 8 まで、
// ブロードキャスト/合体は 256 まで。再帰は NodeMaxRecursionDepth を超えると未定義の結果。だから「超える前にノードの側で止めて数える」:
//   const uint32_t granted = WgGrantOutputs(node, 欲しい数, 宣言した MaxRecords);  // 越えた分は出さずに数える
//   if (WgTryRecurse(node, GetRemainingRecursionLevels(), 宣言した深さ, 自分へ出したいか)) { 自分へ出す }
// 止めると結果が変わるので、止めたことは必ず Warning になる(CPU)。
//
// 数は整数だけで、実行の順番に依存しない(足し算と最大値だけ。04 R1〜R8)。同じ入力なら毎回同じ数になる(T-0087 の比較に使う)。
// デバッグだけのリング(debug_ring.hlsli)と違い、Release でも有効(上限で止める仕組みは結果の正しさの一部なので)。
#ifndef BICAMERAL_WORK_GRAPH_STATS_HLSLI
#define BICAMERAL_WORK_GRAPH_STATS_HLSLI

#ifdef __cplusplus
#include <cstdint>
#define WG_STATS_CONST inline constexpr
#define WG_STATS_NAMESPACE_BEGIN namespace bicameral {
#define WG_STATS_NAMESPACE_END }
#else
#define WG_STATS_CONST static const
#define WG_STATS_NAMESPACE_BEGIN
#define WG_STATS_NAMESPACE_END
#endif

WG_STATS_NAMESPACE_BEGIN

// --- レイアウト(HLSL と C++ で共通)--------------------------------------------------------------
// ノード n の語 w は (n × WG_NODE_WORDS + w) 番目。計器 g は (WG_STATS_MAX_NODES × WG_NODE_WORDS + g) 番目。全部 0 から始まる
WG_STATS_CONST uint32_t WG_STATS_MAX_NODES = 16;  // 1 つのバッファで数えられるノードの数(グラフをまたいでもよい)
WG_STATS_CONST uint32_t WG_STATS_MAX_GAUGES = 8;  // 容量の計器(一覧・キューの使った量の最大)の数
WG_STATS_CONST uint32_t WG_NODE_WORDS = 8;

WG_STATS_CONST uint32_t WG_NODE_LAUNCHES = 0;       // 起動の数(スレッド起動はスレッド、ブロードキャスト/合体はグループ)
WG_STATS_CONST uint32_t WG_NODE_INPUT_RECORDS = 1;  // 受け取ったレコードの数
WG_STATS_CONST uint32_t WG_NODE_OUTPUT_RECORDS = 2;      // 出したレコードの数(止めた分は入らない)
WG_STATS_CONST uint32_t WG_NODE_REFUSED_OUTPUTS = 3;     // 上限を超えるので出さなかったレコードの数
WG_STATS_CONST uint32_t WG_NODE_PEAK_REQUESTED = 4;      // 1 回の起動が出したがった数の最大
WG_STATS_CONST uint32_t WG_NODE_DEEPEST_RECURSION = 5;   // 再帰した段の数の最大(一番上 = 0)
WG_STATS_CONST uint32_t WG_NODE_REFUSED_RECURSIONS = 6;  // 深さの上限にいて、自分へ出したかったのに止めた数
// [7] は予約(0)

WG_STATS_CONST uint32_t WG_STATS_WORDS = WG_STATS_MAX_NODES * WG_NODE_WORDS + WG_STATS_MAX_GAUGES;
WG_STATS_CONST uint32_t WG_STATS_BYTES = WG_STATS_WORDS * 4;
WG_STATS_CONST uint32_t WG_STATS_REGISTER = 1;  // u1 space1(u0 space1 はデバッグのリング)
WG_STATS_CONST uint32_t WG_STATS_REGISTER_SPACE = 1;

// 仕様の上限(D3D12 Work Graphs「Node output limits」「Node count limits」。16 §1)
WG_STATS_CONST uint32_t WG_SPEC_THREAD_MAX_RECORDS = 8;
WG_STATS_CONST uint32_t WG_SPEC_GROUP_MAX_RECORDS = 256;
WG_STATS_CONST uint32_t WG_SPEC_MAX_CHAIN = 32;

WG_STATS_NAMESPACE_END

#ifndef __cplusplus  // --- HLSL ---

RWByteAddressBuffer bicameralGraphStats : register(u1, space1);

uint32_t WgNodeAddress(uint32_t node, uint32_t word) {
    return (node * WG_NODE_WORDS + word) * 4;
}

uint32_t WgGaugeAddress(uint32_t gauge) {
    return (WG_STATS_MAX_NODES * WG_NODE_WORDS + gauge) * 4;
}

// ウェーブの中の値を足してから 1 回だけ atomic(同じ番地への atomic を減らす)。分岐の中で呼んでよい(生きているレーンだけ)
void WgAddAcrossWave(uint32_t address, uint32_t value) {
    const uint32_t total = WaveActiveSum(value);
    if (WaveIsFirstLane() && total != 0)
        bicameralGraphStats.InterlockedAdd(address, total);
}

void WgMaxAcrossWave(uint32_t address, uint32_t value) {
    const uint32_t peak = WaveActiveMax(value);
    if (WaveIsFirstLane() && peak != 0)
        bicameralGraphStats.InterlockedMax(address, peak);
}

// 1 回の起動を数える。スレッド起動は全部のスレッドが、ブロードキャスト/合体はグループの 1 スレッドだけが呼ぶ
void WgCountLaunch(uint32_t node, uint32_t inputRecords) {
    WgAddAcrossWave(WgNodeAddress(node, WG_NODE_LAUNCHES), 1);
    WgAddAcrossWave(WgNodeAddress(node, WG_NODE_INPUT_RECORDS), inputRecords);
}

// 1 回の起動が出したがった数(requested)と実際に出した数(granted)を数える。出す数が構造で決まるノード(上限を越えようがない)はこちら
void WgCountOutputs(uint32_t node, uint32_t requested, uint32_t granted) {
    WgAddAcrossWave(WgNodeAddress(node, WG_NODE_OUTPUT_RECORDS), granted);
    WgAddAcrossWave(WgNodeAddress(node, WG_NODE_REFUSED_OUTPUTS), requested - granted);
    WgMaxAcrossWave(WgNodeAddress(node, WG_NODE_PEAK_REQUESTED), requested);
}

// 出してよい数(宣言した MaxRecords で切る)。切った分は数える。呼んだ後に GetThreadNodeOutputRecords(granted) などで出す
uint32_t WgGrantOutputs(uint32_t node, uint32_t requested, uint32_t maxRecords) {
    const uint32_t granted = min(requested, maxRecords);
    WgCountOutputs(node, requested, granted);

    return granted;
}

// 再帰するノードの起動ごとに呼ぶ。remainingLevels = GetRemainingRecursionLevels()、declaredDepth = NodeMaxRecursionDepth。
// 自分へ出してよければ true。深さの上限にいて出したかった(wants)なら false にして数える
bool WgTryRecurse(uint32_t node, uint32_t remainingLevels, uint32_t declaredDepth, bool wants) {
    const uint32_t depth = declaredDepth - min(remainingLevels, declaredDepth);
    WgMaxAcrossWave(WgNodeAddress(node, WG_NODE_DEEPEST_RECURSION), depth);
    const bool allowed = wants && remainingLevels > 0;
    WgAddAcrossWave(WgNodeAddress(node, WG_NODE_REFUSED_RECURSIONS), wants && !allowed ? 1u : 0u);

    return allowed;
}

// 容量の計器: 使った量の最大を残す(CPU が容量と比べる)
void WgGaugePeak(uint32_t gauge, uint32_t value) {
    WgMaxAcrossWave(WgGaugeAddress(gauge), value);
}

#endif  // !__cplusplus

#endif  // BICAMERAL_WORK_GRAPH_STATS_HLSLI
