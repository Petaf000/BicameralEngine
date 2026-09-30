// probe_sim.hlsli — フレームループと刻みのループ(T-0004・T-0012・T-0086)と、Work Graphs の伝播(T-0005)を確かめるための仮の小さな世界。
// HLSL と C++ で共通(04 §3 と同じ作り)。
//
// 本物の 1 刻み(06 §2 の 9 段)の中身は、ここを足場に段ごとに置き換えていく。ここにあるのは
// 「1 刻みを単位の列に分けて、フレームの予算ぶんずつ投げ、刻みごとのハッシュと、コマンド → イベントの流れが CPU に戻る」形と、
// 「熱が広がっている所だけを Work Graphs が起動する」伝播の最小形(07 §1 の伝導を、材質が 1 つの整数で)。
//
// 世界: PROBE_GRID_SIZE³ のセルに uint32 の熱(エネルギーの整数の量)。世代同期(ADR-0003)のため 2 世代を持つ。
//   刻み t の始めの状態 S(t) は世代 (t & 1) にある。格子の外の面は断熱(流れ 0)なので、つつき以外で熱の合計は変わらない(D-206)。
// 活性(06 §2 段 2): セルを PROBE_BLOCK_SIZE³ のブロックにまとめ、刻み t で計算するのは
//   「刻み t − 1 で値が変わったブロック・刻み t につつかれたブロック」と、その 6 面の隣だけ(= 予定したブロック)。
//   それ以外のブロックは、自分と隣の値が前の刻みから変わっていないので、計算しても結果が変わらない(だから計算しなくてよい)。
//   予定していないブロックは 2 世代とも S(t) と同じ値を持つ(変わった刻みの次の刻みは必ず予定されるので)。
//   → CPU のリファレンスは全部のセルを毎刻み計算し、GPU の結果とビット一致する(活性の取り方が正しいことの試験になる)。
// 1 刻み t = 単位の列(06 §4・ADR-0011。単位の間は UAV バリア。フレームの切れ目はどの単位の間にも来てよい):
//   [0] コマンドの適用: GPU のコマンドキューの先頭から targetTick == t のものを番号順に適用する(06 §3。T-0086)。
//       つつき = 世代 (t & 1) のセルに PROBE_POKE_AMOUNT の熱を足す(PROBE_HEAT_LIMIT で飽和)。適用したらイベントを刻みの一時置き場へ、
//       つついたブロックを刻み t の活性の一覧へ。刻み t + 1 の一覧を空にし、ハッシュの表の S(t + 1) の欄を用意する
//   [1] 伝導(Work Graph。shaders/sim/probe_conduct.hlsl): 刻み t の一覧を GPU の入力として DispatchGraph。
//       WakeBlocks(スレッド起動)が一覧のブロックと 6 面の隣を予定し(1 刻みに 1 回だけ)、ConductBlock(1 ブロック = 1 グループ)が
//       世代 ((t + 1) & 1) に S(t + 1) を書く。値が変わったブロックは刻み t + 1 の一覧へ(熱が広がる所だけが次を起動する)
//   [2 .. 2 + k) 重さの試験(--sim-load を k 個に分けたもの。世界の結果に入らない。k = 0 なら無し)
//   [最後] 検査と出力(06 §2 段 9): S(t + 1) の要約(ProbeStateHash)と熱の合計を、ハッシュの表の (t + 1) % PROBE_HASH_CAPACITY に書き、
//       刻みの一時置き場のイベントをキー(種類・場所)で並べてからリングへ(T-0086)
// コマンドは各フレームのリストの先頭で GPU のキューに足す(Enqueue。単位ではない)。キューの中で自分の刻みの適用の単位まで待つので、
// フレームの切れ目と刻みの関係に結果が依存しない。
// CPU のリファレンス(sim/probe_sim.cpp の ProbeReference)と GPU(shaders/sim/probe_tick.hlsl)は同じ関数を使う。
#ifndef BICAMERAL_PROBE_SIM_HLSLI
#define BICAMERAL_PROBE_SIM_HLSLI

#ifdef __cplusplus
#include <cstdint>
#define PROBE_FN constexpr inline
#define PROBE_CONST inline constexpr
#define PROBE_NAMESPACE_BEGIN namespace bicameral::sim {
#define PROBE_NAMESPACE_END }
#else
#define PROBE_FN
#define PROBE_CONST static const
#define PROBE_NAMESPACE_BEGIN
#define PROBE_NAMESPACE_END
#endif

// 64bit の定数(HLSL のリテラル接尾辞に頼らない。fixed.hlsli の FX_U64 と同じ)
#define PROBE_U64(high32, low32) ((((uint64_t)(high32)) << 32) | ((uint64_t)(low32)))

PROBE_NAMESPACE_BEGIN

// --- 世界の大きさ(3D。T-0005)---
PROBE_CONST uint32_t PROBE_GRID_SIZE = 64;  // 1 辺のセルの数(x・y・z。場所の 1 軸は 8bit に収める)
PROBE_CONST uint32_t PROBE_CELL_COUNT = PROBE_GRID_SIZE * PROBE_GRID_SIZE * PROBE_GRID_SIZE;
PROBE_CONST uint32_t PROBE_SLICE_CELL_COUNT = PROBE_GRID_SIZE * PROBE_GRID_SIZE;  // 1 つの面(z が同じ)のセルの数
PROBE_CONST uint32_t PROBE_VIEW_Z = PROBE_GRID_SIZE / 2;  // 既定の断面・自動のクリックがつつく面(描画は立体。T-0015)
PROBE_CONST uint32_t PROBE_BLOCK_SIZE = 4;  // 活性の単位のブロック(4³ セル = 伝導の 1 グループ 64 スレッド)
PROBE_CONST uint32_t PROBE_BLOCKS_PER_AXIS = PROBE_GRID_SIZE / PROBE_BLOCK_SIZE;
PROBE_CONST uint32_t PROBE_BLOCK_COUNT = PROBE_BLOCKS_PER_AXIS * PROBE_BLOCKS_PER_AXIS * PROBE_BLOCKS_PER_AXIS;
PROBE_CONST uint32_t PROBE_GROUP_SIZE = 8;          // 重さの試験は 8×8 のスレッドグループ(捨て場は 1 つの面の大きさ)
PROBE_CONST uint32_t PROBE_LINEAR_GROUP_SIZE = 64;  // コマンド・ハッシュ・抽出は 1 次元の 64
PROBE_CONST uint32_t PROBE_POKE_AMOUNT = 1u << 24;  // つつき 1 回で足す熱
PROBE_CONST uint32_t PROBE_HEAT_LIMIT = 1u << 30;   // 1 セルの熱の上限(つつきは飽和させる。伝導は隣の最大を超えない)
PROBE_CONST uint32_t PROBE_CONDUCT_SHIFT = 3;       // 面の流れ = |差| >> 3(1/8。3D の陽解法が単調になる 1/6 以下)

// --- 描画用の抽出の組の数(06 §4)---
// 抽出は 1 フレームに 1 回まで、投げた単位の後ろで刻みの境界の状態を写す。描画は終わっている最新を読む。
// 抽出 n は組 n % 3 に書き、終わっている抽出が n − 2 以上のときだけ投げる(frame/frame_loop.cpp)→ 描画が読む組と重ならない
PROBE_CONST uint32_t PROBE_EXTRACTION_COUNT = 3;
// 抽出の中身(uint32 の並び。T-0015): [0, セルの数) 刻みの境界の状態の全部のセル(世界と同じ並び)
//   → [セルの数, + ブロックの数) ブロックごとの活性の印(1 = 境界の前の刻みで伝導を計算した。刻みの途中の抽出ではその刻みの分も)
PROBE_CONST uint32_t PROBE_EXTRACTION_BLOCK_OFFSET = PROBE_CELL_COUNT;
PROBE_CONST uint32_t PROBE_EXTRACTION_WORDS = PROBE_CELL_COUNT + PROBE_BLOCK_COUNT;

// --- 1 刻みの単位(06 §4・ADR-0011)---
PROBE_CONST uint32_t PROBE_UNIT_APPLY = 0;
PROBE_CONST uint32_t PROBE_UNIT_CONDUCT = 1;          // Work Graph(DispatchGraph を 1 回)
PROBE_CONST uint32_t PROBE_UNIT_BUSY_FIRST = 2;       // 重さの試験の単位はここから k 個。その次がハッシュ
PROBE_CONST uint32_t PROBE_FIXED_UNITS_PER_TICK = 3;  // 適用・伝導・ハッシュ

// --- ルート定数(b0。単位を記録するときに埋め込む)---
// [0] 刻みの下位 [1] 刻みの上位 [2] 引数(抽出: 書き先の組)
PROBE_CONST uint32_t PROBE_ROOT_CONSTANT_COUNT = 3;

// --- コマンド(CPU → GPU。06 §3 の 64 バイトの形。C++ は sim/command.h)---
// 語: [0] targetTick の下位 [1] targetTick の上位 [2] sequence [3] type(下位 16bit)| size(上位 16bit)[4..15] payload 48 バイト
PROBE_CONST uint32_t PROBE_MAX_COMMANDS = 256;  // 1 フレームに GPU のキューへ足せるコマンドの数
PROBE_CONST uint32_t PROBE_COMMAND_WORDS = 16;
PROBE_CONST uint32_t PROBE_COMMAND_BYTES = PROBE_COMMAND_WORDS * 4;
PROBE_CONST uint32_t PROBE_COMMAND_TYPE_POKE = 1;  // payload: [0] x [1] y [2] z

// --- GPU のコマンドキュー(06 §3。T-0086)---
// 環状のバッファ。見出し 16 バイト([0] 末尾 = 足した総数 [1] 先頭 = 取り出した総数。どちらも 2^32 で一周する)+ コマンド × 容量。
// CPU だけが足す(末尾を CPU が知っているので、足す場所を入力で渡す)。取り出すのは適用の単位だけ。
// 並びは (targetTick, sequence) の昇順(CPU が守る)。だから適用の単位は先頭から「targetTick が今の刻み以下」の間だけ読めばよい。
// 容量を超えないことも CPU が守る(sim/probe_sim の FreeCommandSlots)。
PROBE_CONST uint32_t PROBE_COMMAND_QUEUE_CAPACITY = 1024;  // 2 の冪(番号を下位ビットで取る)
PROBE_CONST uint32_t PROBE_COMMAND_QUEUE_HEADER_BYTES = 16;
PROBE_CONST uint32_t PROBE_COMMAND_QUEUE_TAIL = 0;  // 見出しの語の位置(× 4 バイト)
PROBE_CONST uint32_t PROBE_COMMAND_QUEUE_HEAD = 1;
PROBE_CONST uint32_t PROBE_COMMAND_QUEUE_BYTES =
    PROBE_COMMAND_QUEUE_HEADER_BYTES + PROBE_COMMAND_QUEUE_CAPACITY * PROBE_COMMAND_BYTES;

// --- フレームの入力(アップロードのバッファ。CPU がフレームの枠ごとに書く)のレイアウト ---
// [0]    見出し: このフレームにキューへ足すコマンドの数、重さの試験の 1 個あたりの繰り返し回数、足す場所(キューの末尾)、
//        伝導のグラフの入口の番号、活性の一覧 2 組の GPU のアドレス(D3D12_NODE_GPU_INPUT に書き込む。CPU しか知らない)
// [256]  コマンド × PROBE_MAX_COMMANDS
PROBE_CONST uint32_t PROBE_INPUT_HEADER_OFFSET = 0;
PROBE_CONST uint32_t PROBE_INPUT_COMMANDS_OFFSET = 256;
PROBE_CONST uint32_t PROBE_INPUT_BYTES = PROBE_INPUT_COMMANDS_OFFSET + PROBE_MAX_COMMANDS * PROBE_COMMAND_BYTES;
PROBE_CONST uint32_t PROBE_HEADER_COMMAND_COUNT = 0;  // 見出しの語の位置(× 4 バイト)
PROBE_CONST uint32_t PROBE_HEADER_BUSY_ITERATIONS = 1;
PROBE_CONST uint32_t PROBE_HEADER_ENQUEUE_BASE = 2;
PROBE_CONST uint32_t PROBE_HEADER_CONDUCT_ENTRYPOINT = 3;
PROBE_CONST uint32_t PROBE_HEADER_ACTIVE_LIST_ADDRESS = 4;  // 組 p のアドレスの下位・上位は [4 + 2p]・[5 + 2p]

// --- 活性のブロックの一覧(06 §2 段 2。T-0005)---
// 刻みの偶奇で 2 組。組 (t & 1) は、刻み t の伝導の Work Graph へ GPU の入力として渡す「変わった(つつかれた)ブロック」の一覧。
// 見出し 32 バイトの先頭 24 バイトが D3D12_NODE_GPU_INPUT そのもの(DispatchGraph が実行の時に GPU のメモリから読む):
//   [0] 入口の番号 [1] レコードの数(= 一覧の長さ。atomic で足す)[2,3] レコードのアドレス(= 見出しの直後)[4,5] レコードの間隔(4)
// その後にブロックの番号(uint32)の列。同じブロックが 2 度入ってもよい(予定するときに 1 刻み 1 回にまとめる)。
// 先頭の 1 件は必ず PROBE_NO_BLOCK(何もしない)にして、レコードの数を 0 にしない
// (WARP は GPU の入力のレコードが 0 件の DispatchGraph で固まった。T-0005。ハードウェアの GPU は 0 件でも動く)。
// 容量: 伝導は 1 刻みに 1 ブロック 1 回なので「変わった」はブロックの数まで、つつきはキューの容量まで、+ 先頭の 1 件
PROBE_CONST uint32_t PROBE_ACTIVE_LIST_ENTRYPOINT = 0;  // 見出しの語の位置(× 4 バイト)
PROBE_CONST uint32_t PROBE_ACTIVE_LIST_COUNT = 1;
PROBE_CONST uint32_t PROBE_ACTIVE_LIST_ADDRESS = 2;
PROBE_CONST uint32_t PROBE_ACTIVE_LIST_STRIDE = 4;
PROBE_CONST uint32_t PROBE_ACTIVE_LIST_HEADER_BYTES = 32;
PROBE_CONST uint32_t PROBE_ACTIVE_LIST_CAPACITY = 1 + PROBE_BLOCK_COUNT + PROBE_COMMAND_QUEUE_CAPACITY;
PROBE_CONST uint32_t PROBE_NO_BLOCK = 0xFFFFFFFFu;  // 一覧の先頭の「何もしない」1 件
PROBE_CONST uint32_t PROBE_ACTIVE_LIST_BYTES = PROBE_ACTIVE_LIST_HEADER_BYTES + PROBE_ACTIVE_LIST_CAPACITY * 4;
// 予定の印: ブロックごとに「最後に予定した刻み + 1」の下位 32bit(0 = まだ無い)。刻みごとに消さなくてよい
// (2^32 刻み = 2 年あまり後に一周して、1 刻みだけ予定を取りこぼしうる。本物の活性の整理(T-0018)で置き換える)
PROBE_CONST uint32_t PROBE_SCHEDULE_BYTES = PROBE_BLOCK_COUNT * 4;

// --- Work Graphs のカウンタ(common/work_graph_stats.hlsli。T-0008)の番号。ProbeSim の GraphStatsLayout(probe_sim.cpp)と同じ順 ---
PROBE_CONST uint32_t PROBE_STATS_NODE_WAKE = 0;            // WakeBlocks
PROBE_CONST uint32_t PROBE_STATS_NODE_CONDUCT = 1;         // ConductBlock
PROBE_CONST uint32_t PROBE_STATS_GAUGE_ACTIVE_LIST = 0;    // 刻みの活性の一覧の長さ(容量 PROBE_ACTIVE_LIST_CAPACITY)
PROBE_CONST uint32_t PROBE_STATS_GAUGE_COMMAND_QUEUE = 1;  // 適用の時にキューで待っていたコマンドの数
PROBE_CONST uint32_t PROBE_WAKE_MAX_RECORDS =
    7;  // WakeBlocks の MaxRecords(自分と 6 面の隣。probe_conduct.hlsl の属性と同じ)

// 重さの試験(--sim-load)の繰り返しの上限(1 刻みの合計。--sim-split で分けたときは 1 個あたりがこれを分けた数で割ったもの)。
// これ以上の値は CPU が送らないので、シェーダーの「使わない分岐」は決して通らない
PROBE_CONST uint32_t PROBE_BUSY_ITERATIONS_LIMIT = 1u << 24;
PROBE_CONST uint32_t PROBE_MAX_BUSY_PIECES = 64;  // 1 刻みの重さを分ける数の上限(--sim-split)

// --- イベント(GPU → CPU。06 §3・§5。T-0086)---
// 刻みの中で出たイベントは、まず刻みの一時置き場(順不同。atomic で空きを取る)に入る。刻みの最後の単位がキー(種類・場所)で並べ、
// リング(readback_ring の追記バッファ。フレームごとに読み戻す)へ刻みの順に写す。だから CPU が読む並びは (刻み, 種類, 場所) の順で、
// フレームへの分け方にも GPU の中の順番にも依存しない。
// 一時置き場: 見出し 16 バイト([0] 書こうとした数)+ 8 バイトのレコード([0] 種類 [1] 場所)× PROBE_TICK_EVENT_CAPACITY
PROBE_CONST uint32_t PROBE_TICK_EVENT_CAPACITY = 256;  // 並べる 1 グループのスレッド数と同じ(2 の冪。bitonic sort)
PROBE_CONST uint32_t PROBE_TICK_EVENT_HEADER_BYTES = 16;
PROBE_CONST uint32_t PROBE_TICK_EVENT_RECORD_BYTES = 8;
PROBE_CONST uint32_t PROBE_TICK_EVENT_BYTES =
    PROBE_TICK_EVENT_HEADER_BYTES + PROBE_TICK_EVENT_CAPACITY * PROBE_TICK_EVENT_RECORD_BYTES;
// リング: 見出し 16 バイト([0] 書こうとした数 [1] 一時置き場で落とした数)+ 16 バイトのレコード × PROBE_EVENT_CAPACITY
// レコード: [0] 刻みの下位 [1] 刻みの上位 [2] 種類 [3] 場所
// 落とした数 = (書こうとした数 − 容量)+ 一時置き場で落とした数。溢れたときにどれが残るかは決めない(数えるだけ。イベントは View に渡すだけで世界の結果に入らない)
PROBE_CONST uint32_t PROBE_EVENT_CAPACITY = 1024;
PROBE_CONST uint32_t PROBE_EVENT_HEADER_BYTES = 16;
PROBE_CONST uint32_t PROBE_EVENT_WORDS = 4;
PROBE_CONST uint32_t PROBE_EVENT_BYTES = PROBE_EVENT_HEADER_BYTES + PROBE_EVENT_CAPACITY * PROBE_EVENT_WORDS * 4;
PROBE_CONST uint32_t PROBE_EVENT_HEADER_REQUESTED = 0;  // 見出しの語の位置(× 4 バイト)
PROBE_CONST uint32_t PROBE_EVENT_HEADER_TICK_DROPPED = 1;
PROBE_CONST uint32_t PROBE_EVENT_POKE_APPLIED = 1;  // 場所: x | y << 8 | z << 16(ProbePokePlace)
PROBE_CONST uint32_t PROBE_EVENT_COMMAND_LATE =
    2;  // 刻みを過ぎてから届いたコマンド(CPU の約束違反。捨てた)。場所: コマンドの種類

// --- 刻みごとの状態のハッシュ(GPU → CPU。06 §2 段 9)---
// 表: PROBE_HASH_CAPACITY 個 × 32 バイト([0,1] 刻み [2,3] ハッシュ [4,5] 熱の合計 [6] その状態を作った刻みで予定したブロックの数 [7] 0)。
// 欄は刻み t の適用の単位が用意し(刻み・0)、伝導が予定の数を、刻みの最後の単位がハッシュと熱の合計を足す。
// S(t) のハッシュは (t % PROBE_HASH_CAPACITY) 番目。フレームの終わりに表を丸ごと読み戻し、CPU はそのフレームで終えた刻みの分だけ読む。
// 1 フレームに積める単位は 256 まで(3 単位/刻みでも 86 刻み)なので、同じフレームの中で番号が重なることはない
PROBE_CONST uint32_t PROBE_HASH_CAPACITY = 256;  // 2 の冪(番号を下位ビットで取る)
PROBE_CONST uint32_t PROBE_HASH_ENTRY_BYTES = 32;
PROBE_CONST uint32_t PROBE_HASH_OFFSET_HASH = 8;  // 欄の中のバイトの位置
PROBE_CONST uint32_t PROBE_HASH_OFFSET_HEAT = 16;
PROBE_CONST uint32_t PROBE_HASH_OFFSET_SCHEDULED = 24;
PROBE_CONST uint32_t PROBE_HASH_BYTES = PROBE_HASH_CAPACITY * PROBE_HASH_ENTRY_BYTES;

// --- 規則(CPU と GPU で同じ)---

PROBE_FN uint32_t ProbeCellIndex(uint32_t x, uint32_t y, uint32_t z) {
    return (z * PROBE_GRID_SIZE + y) * PROBE_GRID_SIZE + x;
}

// ブロックの番号(ブロックの座標から)と、セルが入っているブロック
PROBE_FN uint32_t ProbeBlockIndex(uint32_t blockX, uint32_t blockY, uint32_t blockZ) {
    return (blockZ * PROBE_BLOCKS_PER_AXIS + blockY) * PROBE_BLOCKS_PER_AXIS + blockX;
}

PROBE_FN uint32_t ProbeBlockOfCell(uint32_t x, uint32_t y, uint32_t z) {
    return ProbeBlockIndex(x / PROBE_BLOCK_SIZE, y / PROBE_BLOCK_SIZE, z / PROBE_BLOCK_SIZE);
}

// つつき: 熱を足して PROBE_HEAT_LIMIT で止める(amount ≤ PROBE_HEAT_LIMIT)
PROBE_FN uint32_t ProbeAddHeat(uint32_t value, uint32_t amount) {
    return value >= PROBE_HEAT_LIMIT - amount ? PROBE_HEAT_LIMIT : value + amount;
}

// 面の流れ(self → neighbor の向きが正)。大きさ |差| >> PROBE_CONDUCT_SHIFT に符号を付けるので、
// 面の両側のセルがそれぞれ計算しても、大きさが同じで向きが逆になる → 足し引きが打ち消し、熱の合計が構造的に保存される(06 §2・07)
PROBE_FN int32_t ProbeFaceFlow(uint32_t self, uint32_t neighbor) {
    return self >= neighbor ? (int32_t)((self - neighbor) >> PROBE_CONDUCT_SHIFT)
                            : -(int32_t)((neighbor - self) >> PROBE_CONDUCT_SHIFT);
}

// 伝導の 1 セル: 自分 − 6 面の流れの和(gather。格子の外の面は隣に self を渡す = 流れ 0 = 断熱)。
// 流れは差の 1/8 以下なので、結果は 6 面の隣と自分の最小〜最大の間に収まる(負にならず、PROBE_HEAT_LIMIT を超えない)
PROBE_FN uint32_t ProbeConductValue(uint32_t self, uint32_t minusX, uint32_t plusX, uint32_t minusY, uint32_t plusY,
                                    uint32_t minusZ, uint32_t plusZ) {
    const int32_t outflow = ProbeFaceFlow(self, minusX) + ProbeFaceFlow(self, plusX) + ProbeFaceFlow(self, minusY) +
                            ProbeFaceFlow(self, plusY) + ProbeFaceFlow(self, minusZ) + ProbeFaceFlow(self, plusZ);
    return (uint32_t)((int32_t)self - outflow);
}

// 64bit を混ぜる(splitmix64 の仕上げ。入力の 1 ビットの違いが全体に広がる)
PROBE_FN uint64_t ProbeMix64(uint64_t value) {
    value ^= value >> 30;
    value *= PROBE_U64(0xBF58476Du, 0x1CE4E5B9u);
    value ^= value >> 27;
    value *= PROBE_U64(0x94D049BBu, 0x133111EBu);
    value ^= value >> 31;
    return value;
}

// 1 セルの寄与。状態のハッシュ = 全セルの寄与の和(mod 2^64)。和は足す順番に依存しないので、
// GPU が並列に(wave の和 + 64bit の atomic)足しても CPU が順に足しても同じ値になる(04 R2)
PROBE_FN uint64_t ProbeCellHash(uint32_t cellIndex, uint32_t value) {
    return ProbeMix64(PROBE_U64(cellIndex, value));
}

// イベントの場所(つつき。1 軸 8bit)
PROBE_FN uint32_t ProbePokePlace(uint32_t x, uint32_t y, uint32_t z) {
    return x | (y << 8) | (z << 16);
}

// 刻みの中のイベントを並べるキー(種類が上位、場所が下位)。同じ刻みの中ではキーが同じならレコードも同じ
PROBE_FN uint64_t ProbeEventKey(uint32_t type, uint32_t place) {
    return PROBE_U64(type, place);
}

PROBE_NAMESPACE_END

#endif  // BICAMERAL_PROBE_SIM_HLSLI
