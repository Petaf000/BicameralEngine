// probe_sim.hlsli — フレームループと刻みのループ(T-0004・T-0012・T-0086)と、Work Graphs の伝播(T-0005)を確かめるための仮の小さな世界。
// HLSL と C++ で共通(04 §3 と同じ作り)。
//
// 本物の 1 刻み(06 §2 の 9 段)の中身は、ここを足場に段ごとに置き換えていく。ここにあるのは
// 「1 刻みを単位の列に分けて、フレームの予算ぶんずつ投げ、刻みごとのハッシュと、コマンド → イベントの流れが CPU に戻る」形と、
// 「熱が広がっている所だけを Work Graphs が起動する」伝播の最小形(07 §1 の伝導を、材質が 1 つの整数で)。
//
// 世界(T-0089): PROBE_GRID_SIZE³ のセル。セル = 成分 + エネルギー(reaction.hlsli の RxCell)と、その熱のキャッシュ
//   (heat_conduction.hlsli の HcThermalCache。セルから決まる導出値)。世代同期(ADR-0003)のため 2 世代を持つ。
//   刻み t の始めの状態 S(t) は世代 (t & 1) にある。格子の外の面は断熱(流れ 0)。エネルギーは伝導と反応で構造的に保存され、
//   変わるのはつつき(明示的な湧き出し。刻みごとに数える。D-206)だけ。元素の数は反応でも保存される。
//   初めの状態(空気の中の木箱)は CPU が作って最初のフレームで写す(sim/probe_sim.cpp の MakeProbeInitialWorld)。
// 活性(06 §2 段 2): セルを PROBE_BLOCK_SIZE³ のブロックにまとめ、刻み t で計算するのは
//   「刻み t − 1 で値が変わったブロック・刻み t につつかれたブロック・次に評価の要る刻みが t のブロック」と、その 6 面の隣だけ(= 予定したブロック)。
//   反応は待ちの丸め(ADR-0018・D-429。T-0122): ブロックが最後に変わった刻み tc から幾何分布の待ちで「次に 1 単位進む刻み」が決まるので、
//   変わらなかったブロックは「次に評価の要る刻み」(セルの最小)まで眠らせる(遅い反応も、その刻みに起きる)。
//   それ以外のブロックは、自分と隣の値が前の刻みから変わっておらず、待ちもまだ来ないので、計算しても結果が変わらない(だから計算しなくてよい)。
//   予定していないブロックは 2 世代とも S(t) と同じ値を持つ(変わった刻みの次の刻みは必ず予定されるので)。
//   → CPU のリファレンスは全部のセルを毎刻み計算し、GPU の結果とビット一致する(活性の取り方が正しいことの試験になる)。
// 1 刻み t = 単位の列(06 §4・ADR-0011。単位の間は UAV バリア。フレームの切れ目はどの単位の間にも来てよい):
//   [0] コマンドの適用: GPU のコマンドキューの先頭から targetTick == t のものを番号順に適用する(06 §3。T-0086)。
//       つつき = 世代 (t & 1) のセルを約 2700 K 温める熱(熱容量 × PROBE_POKE_HEATING_MILLIKELVIN)を足し、そのセルの熱のキャッシュを作り直す。適用したらイベントを刻みの一時置き場へ、
//       つついたブロックを刻み t の活性の一覧へ。刻み t + 1 の一覧を空にし、ハッシュの表の S(t + 1) の欄を用意する
//   [1] 伝導と反応(Work Graph。shaders/sim/probe_conduct.hlsl): 刻み t の一覧を GPU の入力として DispatchGraph。
//       WakeBlocks(スレッド起動)が一覧のブロックと 6 面の隣を予定し(1 刻みに 1 回だけ)、ConductBlock(1 ブロック = 1 グループ)が
//       温度の差で熱を受け渡してから、その場で反応を評価し(06 §2 の段 3・4 を 1 つに。反応はセルの中で閉じるので結果は同じ)、
//       世代 ((t + 1) & 1) に S(t + 1) を書く。値が変わったか、まだ進める規則があるブロックは刻み t + 1 の一覧へ
//   [2] 物理(入れたときだけ。T-0098): 物の 1 刻み(sim/gpu_physics。広域と接触は Work Graph、色ごとの解は Compute)。
//       押すコマンドは [0] の適用が速度に入れる(common/physics_push.hlsli)
//   [.. + k) 重さの試験(--sim-load を k 個に分けたもの。世界の結果に入らない。k = 0 なら無し)
//   [最後] 検査と出力(06 §2 段 9): S(t + 1) の要約(ProbeStateHash)とエネルギーの合計・物の状態のハッシュを、ハッシュの表の (t + 1) % PROBE_HASH_CAPACITY に書き、
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
// つつき 1 回で温める量(mK)。足す熱 = 熱容量(nJ/K)× 2700 K = (熱容量 × 11325) >> 22 mJ(約 2700.1 K。割り算を使わない)。
// 1 セルの木に火をつけて燃え広がる大きさ(900 K では、燃えた 1 セルの熱が隣を点ける前に薄まって消えた。gpu_probe_fire_test。T-0089)
PROBE_CONST uint32_t PROBE_POKE_HEATING_MILLIKELVIN = 2700000;
PROBE_CONST uint64_t PROBE_POKE_ENERGY_MULTIPLIER = 11325;
PROBE_CONST uint32_t PROBE_POKE_ENERGY_SHIFT = 22;
PROBE_CONST uint32_t PROBE_WORLD_SEED_LOW = 0x0B1CA3E7u;  // 反応の端数を丸める乱数の世界のシード(R6)
PROBE_CONST uint32_t PROBE_WORLD_SEED_HIGH = 0x0000C0DEu;

// --- 描画用の抽出の組の数(06 §4)---
// 抽出は 1 フレームに 1 回まで、投げた単位の後ろで刻みの境界の状態を写す。描画は終わっている最新を読む。
// 抽出 n は組 n % 3 に書き、終わっている抽出が n − 2 以上のときだけ投げる(frame/frame_loop.cpp)→ 描画が読む組と重ならない
PROBE_CONST uint32_t PROBE_EXTRACTION_COUNT = 3;
// 抽出の中身(uint32 の並び。T-0015・T-0089): [0, セルの数 × 4) 刻みの境界の状態の全部のセル(世界と同じ並び)の 4 語
//   [0] 温度(mK)[1..3] 見る物質 3 つ(フレームの入力の見出しの PROBE_HEADER_VIEW_SPECIES)の物質量(µmol、uint32 で飽和)
//   → [セルの数 × 4, + ブロックの数) ブロックごとの活性の印(1 = 境界の前の刻みで計算した。刻みの途中の抽出ではその刻みの分も)
//   → 覗きの欄(T-0096。sim/probe_peek が抽出の後に書く。覗いていなければ段の数 0 だけを Extract が書く):
//      見出し [0] 段の数 [1..3] 空き、段 i ごとに [4 + 4i] レベル [5 + 4i .. 7 + 4i] 原点 x・y・z(そのレベルのセルの単位、int32)
//      → 段 i のセル(ブロックの中の番号順に 512 個)の 4 語(世界のセルと同じ意味)
//   → 物の欄(T-0098。物理を入れたときだけ中身がある): 見出し [0] 物の数(PROBE_MAX_VIEW_BODIES まで)[1..15] 空き、
//      物 i ごとに 16 語: [0..5] 重心の x・y・z(int64 の下位・上位。2^-20 m、物理の座標)[6..9] 向き (x, y, z, w)(Q1.30)
//      [10..12] 半分の辺(2^-20 m)[13] 印(PROBE_BODY_VIEW_DYNAMIC・PROBE_BODY_VIEW_ACTIVE)[14, 15] 空き
PROBE_CONST uint32_t PROBE_EXTRACTION_CELL_WORDS = 4;
PROBE_CONST uint32_t PROBE_VIEW_SPECIES_COUNT = 3;
PROBE_CONST uint32_t PROBE_EXTRACTION_BLOCK_OFFSET = PROBE_CELL_COUNT * PROBE_EXTRACTION_CELL_WORDS;
PROBE_CONST uint32_t PROBE_PEEK_MAX_LEVELS = 9;     // 影の鎖の段の数の上限(k = 1〜9。0.5 m → 約 1 mm)
PROBE_CONST uint32_t PROBE_PEEK_BLOCK_EDGE = 8;     // 段の 1 辺のセルの数(multires.hlsli の MR_BLOCK_EDGE)
PROBE_CONST uint32_t PROBE_PEEK_BLOCK_CELLS = 512;  // 段のセルの数(multires.hlsli の MR_BLOCK_CELLS)
PROBE_CONST uint32_t PROBE_PEEK_HEADER_WORDS = 64;
PROBE_CONST uint32_t PROBE_PEEK_LEVEL_WORDS = 4;  // 段ごとの見出しの語(レベル・原点 3 つ)
PROBE_CONST uint32_t PROBE_EXTRACTION_PEEK_OFFSET = PROBE_EXTRACTION_BLOCK_OFFSET + PROBE_BLOCK_COUNT;
PROBE_CONST uint32_t PROBE_EXTRACTION_PEEK_CELL_OFFSET = PROBE_EXTRACTION_PEEK_OFFSET + PROBE_PEEK_HEADER_WORDS;
PROBE_CONST uint32_t PROBE_EXTRACTION_PEEK_WORDS = PROBE_PEEK_HEADER_WORDS +
                                                   (PROBE_PEEK_MAX_LEVELS * PROBE_PEEK_BLOCK_CELLS *
                                                    PROBE_EXTRACTION_CELL_WORDS);
PROBE_CONST uint32_t PROBE_EXTRACTION_BODY_OFFSET = PROBE_EXTRACTION_PEEK_OFFSET + PROBE_EXTRACTION_PEEK_WORDS;
PROBE_CONST uint32_t PROBE_BODY_VIEW_HEADER_WORDS = 16;
PROBE_CONST uint32_t PROBE_BODY_VIEW_WORDS = 16;
PROBE_CONST uint32_t PROBE_MAX_VIEW_BODIES = 64;
PROBE_CONST uint32_t PROBE_BODY_VIEW_DYNAMIC = 1;  // 物の欄の印: 動く物(地面・壁でない)
PROBE_CONST uint32_t PROBE_BODY_VIEW_ACTIVE = 2;   // 世界にいる
PROBE_CONST uint32_t PROBE_EXTRACTION_WORDS = PROBE_EXTRACTION_BODY_OFFSET + PROBE_BODY_VIEW_HEADER_WORDS +
                                              PROBE_MAX_VIEW_BODIES * PROBE_BODY_VIEW_WORDS;

// --- 物理(T-0098。sim/gpu_physics の整数の AVBD を刻みの単位に入れる)---
// 物理の座標(2^-20 m、y が上)と格子の座標(セルの番号の空間。描画は y が画面の下向き)の対応:
//   格子 x = 物理 x / 0.5 m、格子 y = PROBE_GRID_SIZE − 物理 y / 0.5 m、格子 z = 物理 z / 0.5 m。
//   物理の地面の上の面(y = 0)が格子の底(y = PROBE_GRID_SIZE)。物と世界のセルのやり取り(燃える・熱)は無い(M6)
PROBE_CONST uint32_t PROBE_PHYSICS_CELL_SHIFT = 19;  // 1 セル = 0.5 m = 2^19 × 2^-20 m

// --- 1 刻みの単位(06 §4・ADR-0011)---
PROBE_CONST uint32_t PROBE_UNIT_APPLY = 0;
PROBE_CONST uint32_t PROBE_UNIT_CONDUCT = 1;  // Work Graph(DispatchGraph を 1 回)
PROBE_CONST uint32_t PROBE_UNIT_PHYSICS = 2;  // 物理の 1 刻み(物理を入れたときだけ。T-0098)。重さの試験はその後ろ
PROBE_CONST uint32_t PROBE_FIXED_UNITS_PER_TICK = 3;  // 適用・伝導・ハッシュ(物理・重さの試験の単位は別に数える)

// --- ルート定数(b0。単位を記録するときに埋め込む)---
// [0] 刻みの下位 [1] 刻みの上位 [2] 引数(抽出: 書き先の組。適用: その刻みの表の物質の数〔置くコマンドの検査。T-0222〕)
PROBE_CONST uint32_t PROBE_ROOT_CONSTANT_COUNT = 3;

// --- コマンド(CPU → GPU。06 §3 の 64 バイトの形。C++ は sim/command.h)---
// 語: [0] targetTick の下位 [1] targetTick の上位 [2] sequence [3] type(下位 16bit)| size(上位 16bit)[4..15] payload 48 バイト
PROBE_CONST uint32_t PROBE_MAX_COMMANDS = 256;  // 1 フレームに GPU のキューへ足せるコマンドの数
PROBE_CONST uint32_t PROBE_COMMAND_WORDS = 16;
PROBE_CONST uint32_t PROBE_COMMAND_BYTES = PROBE_COMMAND_WORDS * 4;
PROBE_CONST uint32_t PROBE_COMMAND_TYPE_POKE = 1;  // payload: [0] x [1] y [2] z
// 押す(T-0098。common/physics_push.hlsli): payload: [0..5] 光線の原点 x・y・z(int64 の下位・上位。2^-20 m、物理の座標)
//   [6..8] 光線の向き(長さ 1 の Q1.30、int32)[9] 力積の大きさ(mN·s)。物理を入れていなければ何もしない
PROBE_CONST uint32_t PROBE_COMMAND_TYPE_PUSH = 2;
// 反応表の差し替え(ホットリロード。T-0139・ADR-0047): payload: [0] 新しい表の版の下位 [1] 上位。
//   GPU の適用の単位は何もしない(印として記録と再生に残る)。表を写すのと、熱のキャッシュを作り直して全部のブロックを起こすのは、
//   CPU がその刻みの適用の単位の前後に記録する写しと RefreshTable(sim/probe_sim の ProbeFrameInput::tableSwap)
PROBE_CONST uint32_t PROBE_COMMAND_TYPE_TABLE = 3;
// 物を置く筆(T-0222・D-449。common/probe_place.hlsli): payload は ProbePlacePayload(中心・半径・置き換えか足すか・温度・物質 3 つまで)。
//   球の中のセルを置き換える・足す(エディタの明示的な湧き出し。変わったエネルギーは湧き出しの欄へ)。触ったブロックを起こす
PROBE_CONST uint32_t PROBE_COMMAND_TYPE_PLACE = 4;

// --- GPU のコマンドキュー(06 §3。T-0086)---
// 環状のバッファ。見出し 16 バイト([0] 末尾 = 足した総数 [1] 先頭 = 取り出した総数。どちらも 2^32 で一周する)+ コマンド × 容量。
// CPU だけが足す(末尾を CPU が知っているので、足す場所を入力で渡す)。取り出すのは適用の単位だけ。
// 並びは (targetTick, sequence) の昇順(CPU が守る)。だから適用の単位は先頭から「targetTick が今の刻み以下」の間だけ読めばよい。
// 容量を超えないことも CPU が守る(sim/probe_sim の FreeCommandSlots)。
PROBE_CONST uint32_t PROBE_COMMAND_QUEUE_CAPACITY = 1024;  // 2 の冪(番号を下位ビットで取る)
PROBE_CONST uint32_t PROBE_COMMAND_QUEUE_HEADER_BYTES = 16;
PROBE_CONST uint32_t PROBE_COMMAND_QUEUE_TAIL = 0;  // 見出しの語の位置(× 4 バイト)
PROBE_CONST uint32_t PROBE_COMMAND_QUEUE_HEAD = 1;
PROBE_CONST uint32_t PROBE_COMMAND_QUEUE_BYTES = PROBE_COMMAND_QUEUE_HEADER_BYTES +
                                                 PROBE_COMMAND_QUEUE_CAPACITY * PROBE_COMMAND_BYTES;

// --- フレームの入力(アップロードのバッファ。CPU がフレームの枠ごとに書く)のレイアウト ---
// [0]    見出し: このフレームにキューへ足すコマンドの数、重さの試験の 1 個あたりの繰り返し回数、足す場所(キューの末尾)、
//        伝導のグラフの入口の番号、活性の一覧 2 組の GPU のアドレス(D3D12_NODE_GPU_INPUT に書き込む。CPU しか知らない)、
//        抽出に写す物質 3 つの ID(描画の色分け。世界の結果に入らない)
// [256]  コマンド × PROBE_MAX_COMMANDS
PROBE_CONST uint32_t PROBE_INPUT_HEADER_OFFSET = 0;
PROBE_CONST uint32_t PROBE_INPUT_COMMANDS_OFFSET = 256;
PROBE_CONST uint32_t PROBE_INPUT_BYTES = PROBE_INPUT_COMMANDS_OFFSET + PROBE_MAX_COMMANDS * PROBE_COMMAND_BYTES;
PROBE_CONST uint32_t PROBE_HEADER_COMMAND_COUNT = 0;  // 見出しの語の位置(× 4 バイト)
PROBE_CONST uint32_t PROBE_HEADER_BUSY_ITERATIONS = 1;
PROBE_CONST uint32_t PROBE_HEADER_ENQUEUE_BASE = 2;
PROBE_CONST uint32_t PROBE_HEADER_CONDUCT_ENTRYPOINT = 3;
PROBE_CONST uint32_t PROBE_HEADER_ACTIVE_LIST_ADDRESS = 4;  // 組 p のアドレスの下位・上位は [4 + 2p]・[5 + 2p]
PROBE_CONST uint32_t PROBE_HEADER_VIEW_SPECIES = 8;         // [8..10]
PROBE_CONST uint32_t PROBE_HEADER_BODY_COUNT = 11;          // 物理の物の数(0 = 物理なし。T-0098)
PROBE_CONST uint32_t PROBE_HEADER_PHYSICS_RATE = 12;        // 物理の 1 秒あたりの小刻みの数(押す力積の計算に使う)
PROBE_CONST uint32_t PROBE_HEADER_WORDS = 13;

// --- 活性のブロックの一覧(06 §2 段 2。T-0005)---
// 刻みの偶奇で 2 組。組 (t & 1) は、刻み t の伝導の Work Graph へ GPU の入力として渡す「変わった(つつかれた)ブロック」の一覧。
// 見出し 32 バイトの先頭 24 バイトが D3D12_NODE_GPU_INPUT そのもの(DispatchGraph が実行の時に GPU のメモリから読む):
//   [0] 入口の番号 [1] レコードの数(= 一覧の長さ。atomic で足す)[2,3] レコードのアドレス(= 見出しの直後)[4,5] レコードの間隔(4)
// その後にブロックの番号(uint32)の列。同じブロックが 2 度入ってもよい(予定するときに 1 刻み 1 回にまとめる)。
// 先頭の 1 件は必ず PROBE_NO_BLOCK(何もしない)にして、レコードの数を 0 にしない
// (WARP は GPU の入力のレコードが 0 件の DispatchGraph で固まった。T-0005。ハードウェアの GPU は 0 件でも動く)。
// 容量: 伝導は 1 刻みに 1 ブロック 1 回なので「変わった・次の刻みに起こす」はブロックの数まで(起こす刻みの来たブロックは、
// 前の刻みに一覧へ足したブロックと重ならない。probe_tick.hlsl の WakeDueBlocks)、つつきはキューの容量まで、+ 先頭の 1 件。
// 置くコマンド(T-0222)は触ったブロックの起こす刻みを今にするので、前の刻みに足したブロックとも重なりうる(重なりはブロックの数まで)
PROBE_CONST uint32_t PROBE_ACTIVE_LIST_ENTRYPOINT = 0;  // 見出しの語の位置(× 4 バイト)
PROBE_CONST uint32_t PROBE_ACTIVE_LIST_COUNT = 1;
PROBE_CONST uint32_t PROBE_ACTIVE_LIST_ADDRESS = 2;
PROBE_CONST uint32_t PROBE_ACTIVE_LIST_STRIDE = 4;
PROBE_CONST uint32_t PROBE_ACTIVE_LIST_HEADER_BYTES = 32;
PROBE_CONST uint32_t PROBE_ACTIVE_LIST_CAPACITY = 1 + (2 * PROBE_BLOCK_COUNT) + PROBE_COMMAND_QUEUE_CAPACITY;
PROBE_CONST uint32_t PROBE_NO_BLOCK = 0xFFFFFFFFu;  // 一覧の先頭の「何もしない」1 件
PROBE_CONST uint32_t PROBE_ACTIVE_LIST_BYTES = PROBE_ACTIVE_LIST_HEADER_BYTES + PROBE_ACTIVE_LIST_CAPACITY * 4;
// 予定の印: ブロックごとに「最後に予定した刻み + 1」の下位 32bit(0 = まだ無い)。刻みごとに消さなくてよい
// (2^32 刻み = 2 年あまり後に一周して、1 刻みだけ予定を取りこぼしうる。本物の活性の整理(T-0018)で置き換える)
// その後ろに、ブロックごとの待ちの丸めの印(ADR-0018。T-0122)を 64bit(下位・上位の 2 語)ずつ:
//   [PROBE_BLOCK_COUNT, 3 × PROBE_BLOCK_COUNT)  ブロックが最後に変わった刻みの印(tc。初めは 0。つつきは「刻みの直前に変わった」= 刻み t)
//   [3 × PROBE_BLOCK_COUNT, 5 × PROBE_BLOCK_COUNT)  次に評価の要る刻みの印(一覧に足したブロックは RX_WAIT_NEVER。初めは CPU が作る。sim/probe_sim の ProbeInitialBlockWakes)
PROBE_CONST uint32_t PROBE_SCHEDULE_CHANGED_WORD = PROBE_BLOCK_COUNT;
PROBE_CONST uint32_t PROBE_SCHEDULE_WAKE_WORD = PROBE_BLOCK_COUNT * 3;
PROBE_CONST uint32_t PROBE_SCHEDULE_BYTES = PROBE_BLOCK_COUNT * 5 * 4;

// --- Work Graphs のカウンタ(common/work_graph_stats.hlsli。T-0008)の番号。ProbeSim の GraphStatsLayout(probe_sim.cpp)と同じ順 ---
PROBE_CONST uint32_t PROBE_STATS_NODE_WAKE = 0;            // WakeBlocks
PROBE_CONST uint32_t PROBE_STATS_NODE_CONDUCT = 1;         // ConductBlock
PROBE_CONST uint32_t PROBE_STATS_GAUGE_ACTIVE_LIST = 0;    // 刻みの活性の一覧の長さ(容量 PROBE_ACTIVE_LIST_CAPACITY)
PROBE_CONST uint32_t PROBE_STATS_GAUGE_COMMAND_QUEUE = 1;  // 適用の時にキューで待っていたコマンドの数
// WakeBlocks の MaxRecords(自分と 6 面の隣。probe_conduct.hlsl の属性と同じ)
PROBE_CONST uint32_t PROBE_WAKE_MAX_RECORDS = 7;

// --- 連鎖のトレース(common/graph_trace.hlsli。T-0087)の記録の種類。場所の単位はブロックの座標。木に組むのは sim/probe_trace ---
// 書くのは順番に依存しない事実だけ。「どの起こしが先に予定したか」は毎回変わるので書かない(親は CPU が決まった規則で選ぶ)
PROBE_CONST uint32_t PROBE_TRACE_POKE = 1;  // 適用がつついた。主 = ブロック、従 = セルの番号(ProbeCellIndex)
PROBE_CONST uint32_t
    PROBE_TRACE_WAKE = 2;  // WakeBlocks が起こそうとした。主 = 一覧のブロック、従 = 自分か 6 面の隣(格子の中を全部)
PROBE_CONST uint32_t
    PROBE_TRACE_CONDUCT = 3;  // ConductBlock が計算した。主 = ブロック、従 = PROBE_BLOCK_FLAG_* の組み合わせ
// 計算したブロックの結果(トレースの従・CPU リファレンスの BlockFlags)。どちらかがあれば次の刻みの一覧に入る
PROBE_CONST uint32_t PROBE_BLOCK_FLAG_CHANGED = 1;  // 値が 1 つでも変わった
PROBE_CONST uint32_t
    PROBE_BLOCK_FLAG_POSSIBLE = 2;  // 次の刻みに評価が要る(待ちの最小が次の刻みの印以下。T-0089・T-0122)

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
PROBE_CONST uint32_t PROBE_TICK_EVENT_BYTES = PROBE_TICK_EVENT_HEADER_BYTES +
                                              PROBE_TICK_EVENT_CAPACITY * PROBE_TICK_EVENT_RECORD_BYTES;
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
// 刻みを過ぎてから届いたコマンド(CPU の約束違反。捨てた)。場所: コマンドの種類
PROBE_CONST uint32_t PROBE_EVENT_COMMAND_LATE = 2;
// 押すコマンドを適用した(T-0098)。場所: 押した物の番号(何も押さなかった = PROBE_PUSH_NOTHING)
PROBE_CONST uint32_t PROBE_EVENT_BODY_PUSHED = 3;
PROBE_CONST uint32_t PROBE_PUSH_NOTHING = 0xFFFFFFFFu;
// 置くコマンドを適用した(T-0222)。場所: 球の中心(ProbePokePlace)。当てられないコマンドは出さない
PROBE_CONST uint32_t PROBE_EVENT_PLACE_APPLIED = 4;

// --- 刻みごとの状態のハッシュ(GPU → CPU。06 §2 段 9)---
// 表: PROBE_HASH_CAPACITY 個 × 32 バイト([0,1] 刻み [2,3] ハッシュ [4,5] エネルギーの合計(mJ、mod 2^64)
//   [6] その状態を作った刻みで予定したブロックの数 [7] 0)+ 32 バイト([8,9] その状態を作った刻みのつつきが足したエネルギー(mJ)
//   [10,11] 物の状態のハッシュ(物理を入れたとき。PhysicsWorld::StateHash と同じ式。無ければ 0。T-0098)[12..15] 0)。
// 欄は刻み t の適用の単位が用意し(刻み・0)、適用がつつきのエネルギーを、伝導が予定の数を、刻みの最後の単位がハッシュとエネルギーの合計を足す。
// 保存則の検査(D-206): エネルギーの合計(t + 1) = エネルギーの合計(t) + つつきのエネルギー(t + 1 の欄)。
// S(t) のハッシュは (t % PROBE_HASH_CAPACITY) 番目。フレームの終わりに表を丸ごと読み戻し、CPU はそのフレームで終えた刻みの分だけ読む。
// 1 フレームに積める単位は 256 まで(3 単位/刻みでも 86 刻み)なので、同じフレームの中で番号が重なることはない
PROBE_CONST uint32_t PROBE_HASH_CAPACITY = 256;  // 2 の冪(番号を下位ビットで取る)
PROBE_CONST uint32_t PROBE_HASH_ENTRY_BYTES = 64;
PROBE_CONST uint32_t PROBE_HASH_OFFSET_HASH = 8;  // 欄の中のバイトの位置
PROBE_CONST uint32_t PROBE_HASH_OFFSET_ENERGY = 16;
PROBE_CONST uint32_t PROBE_HASH_OFFSET_SCHEDULED = 24;
PROBE_CONST uint32_t PROBE_HASH_OFFSET_SOURCE = 32;
PROBE_CONST uint32_t PROBE_HASH_OFFSET_BODIES = 40;
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

// 64bit を混ぜる(splitmix64 の仕上げ。入力の 1 ビットの違いが全体に広がる)
PROBE_FN uint64_t ProbeMix64(uint64_t value) {
    value ^= value >> 30;
    value *= PROBE_U64(0xBF58476Du, 0x1CE4E5B9u);
    value ^= value >> 27;
    value *= PROBE_U64(0x94D049BBu, 0x133111EBu);
    value ^= value >> 31;

    return value;
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
