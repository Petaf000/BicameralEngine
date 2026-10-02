// probe_view.hlsl — 仮の世界(common/probe_sim.hlsli の 64³ のセル)のデバッグ表示(T-0015。前は z = 32 の面だけ: T-0004・T-0005)。
// 色分けする量は 4 通り(T-0089): 温度・O2 の減り・CO2・炭(抽出の 4 語。probe_sim.hlsli)。
//
// データの流れ: シミュ(compute キュー)が抽出の 3 組のどれかに全部のセルと活性の印を書く → 描画(direct キュー)がフェンスを待ってから、
// フレームの定数(frame。CPU がフレームごとに書く。engine/src/render/probe_view_constants.h と同じ並び)が指す組を読む(06 §4)。
// 画面いっぱいの三角形 1 枚で、画素ごとにカメラの光線を作り(engine/src/render/debug_camera.h と同じ式)、格子をセルごとに辿る(DDA)。
// 表示は 3 通り: 吸収と発光・光線の上の最大値・断面。重ね書き: 格子の枠・断面の枠(クリックがつつく面)・活性なブロック・色の凡例。
// 覗き窓(T-0096): 抽出の覗きの欄に影の鎖があれば、断面の上では各点で一番細かい段のセルの色を描き(段の枠と、大きく見えるセルの線も)、
// 立体の表示では段の箱の枠だけを重ねる。潜っている段(flags のビット 16〜19)の枠は明るく。
// 浮動小数点はここ(描画)だけ。決定性は求めない(10 の目的)。
#include "common/probe_sim.hlsli"

StructuredBuffer<uint32_t> extraction0 : register(t0);
StructuredBuffer<uint32_t> extraction1 : register(t1);
StructuredBuffer<uint32_t> extraction2 : register(t2);
ByteAddressBuffer frame : register(t3);  // ProbeViewConstants(96 バイト)

// --- 表示の仕方と重ね書き(render/probe_view_constants.h の DebugViewMode・VIEW_FLAG_*)---
static const uint32_t VIEW_MODE_VOLUME = 0;
static const uint32_t VIEW_MODE_MAXIMUM = 1;
static const uint32_t VIEW_MODE_SLICE = 2;

static const uint32_t VIEW_FLAG_ACTIVE_BLOCKS = 1;
static const uint32_t VIEW_FLAG_LOGARITHMIC = 2;
static const uint32_t
    VIEW_QUANTITY_SHIFT = 8;  // flags のビット 8〜9 = 色分けする量(render/probe_view_constants.h の DebugViewQuantity)
static const uint32_t VIEW_QUANTITY_TEMPERATURE = 0;
static const uint32_t VIEW_QUANTITY_OXYGEN_DEPLETION = 1;
static const uint32_t VIEW_QUANTITY_CARBON_DIOXIDE = 2;
static const uint32_t VIEW_QUANTITY_CARBON = 3;
static const uint32_t VIEW_PEEK_DEPTH_SHIFT = 16;  // flags のビット 16〜19 = 潜っている段(0 = 潜っていない。T-0096)

// --- 量を 0〜1 にする幅 ---
static const float AMBIENT_KELVIN = 300.0;    // 温度はこれより上の分
static const float HOT_KELVIN_SPAN = 1700.0;  // 300 K + 1700 K = 2000 K で 1
static const float
    AIR_OXYGEN_MICROMOLES = 1063800.0;  // 空気のセルの O2(sim/probe_sim.cpp の初めの世界)。O2 の減り = これ − O2
static const float GAS_SPAN_MICROMOLES = 1100000.0;       // CO2 はこれで 1
static const float CARBON_SPAN_MICROMOLES = 240000000.0;  // 炭は木箱の壁のセルが全部炭になった量(約 2.3e8 µmol)で 1

// --- 色 ---
static const float3 BACKGROUND = float3(0.02, 0.02, 0.03);
static const float3 FRAME_COLOR = float3(0.35, 0.37, 0.45);
static const float3 SLICE_FRAME_COLOR = float3(0.95, 0.8, 0.25);
static const float3 ACTIVE_COLOR = float3(0.15, 0.75, 0.95);
static const float3 PEEK_FRAME_COLOR = float3(0.25, 0.8, 0.45);  // 覗きの段の枠
static const float3 PEEK_DIVE_COLOR = float3(0.6, 1.0, 0.7);     // 潜っている段の枠

// 断面の熱の無いセル(面の広がりが見えるように)
static const float3 COLD_SLICE_COLOR = float3(0.05, 0.06, 0.1);

// --- 光線の進め方 ---
// 1 セルの長さあたりの濃さ(最大の熱で。熱の 3 乗で薄くし、奥の熱い所が透けて見えるように)
static const float VOLUME_DENSITY = 0.25;

// 活性なブロックの薄い色(1 セルの長さあたり)
static const float ACTIVE_DENSITY = 0.012;

// ここまで不透明になったら、奥は見えないので打ち切る
static const float OPAQUE_ENOUGH = 0.995;

// 格子の枠の線の太さ(画素)
static const float LINE_WIDTH_PIXELS = 1.2;

// 覗きの段のセルの線を描くのは、セルが画面でこの画素数より大きいとき(線の濃さ)
static const float PEEK_CELL_LINE_MIN_PIXELS = 12.0;
static const float PEEK_CELL_LINE_SHADE = 0.35;

// 格子を斜めに抜けるときのセルの数の上限
static const uint32_t MAX_STEPS = PROBE_GRID_SIZE * 3 + 4;

// フレームの定数(render/probe_view_constants.h の ProbeViewConstants と同じ並び。96 バイト)
struct ViewConstants {
    // --- 何をどこに描くか ---
    uint32_t extraction;  // 読む抽出(0〜2)
    float2 viewport;      // 描く大きさ(画素)
    uint32_t flags;       // VIEW_FLAG_* の組み合わせ

    // --- 表示の仕方 ---
    uint32_t mode;           // VIEW_MODE_*
    uint32_t sliceAxis;      // 断面の軸(0 = x・1 = y・2 = z)
    uint32_t slicePosition;  // 断面のセルの番号(クリックがつつく面)

    // --- カメラ(render/debug_camera.h の CameraBasis と同じ式)---
    bool orthographic;  // 平行投影か(false なら透視)
    float3 position;    // 目の位置(セル)
    float3 forward;     // 見ている向き
    float3 right;       // 画面の半分の幅の分(透視は向きの傾き、平行は世界の長さ)
    float3 up;          // 画面の半分の高さの分
};

ViewConstants LoadConstants() {
    const uint4 word0 = frame.Load4(0);
    const uint4 word1 = frame.Load4(16);
    ViewConstants constants;
    constants.extraction = word0.x;
    constants.viewport = float2(word0.y, word0.z);
    constants.flags = word0.w;
    constants.mode = word1.x;
    constants.sliceAxis = word1.y;
    constants.slicePosition = word1.z;
    constants.orthographic = word1.w != 0;
    constants.position = asfloat(frame.Load3(32));
    constants.forward = asfloat(frame.Load3(48));
    constants.right = asfloat(frame.Load3(64));
    constants.up = asfloat(frame.Load3(80));

    return constants;
}

uint32_t LoadExtraction(uint32_t extraction, uint32_t index) {
    if (extraction == 0)
        return extraction0[index];

    if (extraction == 1)
        return extraction1[index];

    return extraction2[index];
}

// 世界のセルの 4 語の始まり(覗きの段のセルも同じ 4 語。PeekCellWordBase)
uint32_t CellWordBase(uint3 cell) {
    return ProbeCellIndex(cell.x, cell.y, cell.z) * PROBE_EXTRACTION_CELL_WORDS;
}

bool IsBlockActive(uint32_t extraction, uint3 cell) {
    return LoadExtraction(extraction, PROBE_EXTRACTION_BLOCK_OFFSET + ProbeBlockOfCell(cell.x, cell.y, cell.z)) != 0;
}

// --- 色 ---

// 量(幅 span で 1 になる値)を 0〜1 に。対数(広がった先の小さな値まで見える。1/1000 の幅から)か線形
float Normalized(float value, float span, uint32_t flags) {
    const float fraction = saturate(value / span);  // HLSL の linear は補間の修飾子
    if ((flags & VIEW_FLAG_LOGARITHMIC) != 0)
        return saturate(log2(fraction * 1000.0 + 1.0) / log2(1001.0));

    return fraction;
}

// セル(4 語が wordBase から)の色分けする量を 0〜1 に(flags が選ぶ量)。覗きの段のセルは単位が 8^-k だが、
// 同じ濃度・温度なら同じ数なので(17 §1)同じ幅で塗る
float WordsAmount(ViewConstants constants, uint32_t wordBase) {
    const uint32_t quantity = (constants.flags >> VIEW_QUANTITY_SHIFT) & 3;
    const float value = (float)LoadExtraction(constants.extraction, wordBase + quantity);
    if (quantity == VIEW_QUANTITY_TEMPERATURE)
        return Normalized(max(value * 0.001 - AMBIENT_KELVIN, 0.0), HOT_KELVIN_SPAN, constants.flags);

    if (quantity == VIEW_QUANTITY_OXYGEN_DEPLETION)
        return Normalized(max(AIR_OXYGEN_MICROMOLES - value, 0.0), AIR_OXYGEN_MICROMOLES, constants.flags);

    if (quantity == VIEW_QUANTITY_CARBON_DIOXIDE)
        return Normalized(value, GAS_SPAN_MICROMOLES, constants.flags);

    return Normalized(value, CARBON_SPAN_MICROMOLES, constants.flags);
}

float CellAmount(ViewConstants constants, uint3 cell) {
    return WordsAmount(constants, CellWordBase(cell));
}

// 黒 → 赤 → 黄 → 白
float3 HeatColor(float amount) {
    return saturate(float3(amount * 3.0, amount * 3.0 - 1.0, amount * 3.0 - 2.0));
}

// --- 光線 ---

struct Ray {
    float3 origin;
    float3 direction;
};

// 画素の中心(SV_Position)を通る光線(debug_camera.cpp の RayThroughPixel と同じ)
Ray MakeRay(ViewConstants constants, float2 pixel) {
    const float u = 2.0 * pixel.x / constants.viewport.x - 1.0;
    const float v = 1.0 - 2.0 * pixel.y / constants.viewport.y;
    const float3 offset = constants.right * u + constants.up * v;
    Ray ray;
    ray.origin = constants.orthographic ? constants.position + offset : constants.position;
    ray.direction = constants.orthographic ? constants.forward : constants.forward + offset;

    return ray;
}

// 光線の t での 1 画素の世界の大きさ(線の太さに使う)
float PixelSize(ViewConstants constants, float t) {
    const float scale = 2.0 * length(constants.up) / constants.viewport.y;
    return constants.orthographic ? scale : scale * t;
}

// 箱 [boxLow, boxHigh] との交わり(入る t・出る t)。当たらなければ enter > exit
float2 IntersectBox(Ray ray, float3 boxLow, float3 boxHigh) {
    const float3 inverse = 1.0 / ray.direction;  // 0 の成分は ±inf(比較はそのまま効く)
    const float3 near = (boxLow - ray.origin) * inverse;
    const float3 far = (boxHigh - ray.origin) * inverse;
    const float3 low = min(near, far);
    const float3 high = max(near, far);

    return float2(max(max(max(low.x, low.y), low.z), 0.0), min(min(high.x, high.y), high.z));
}

// 箱の面の上の点が、辺(2 つの軸で端)の近くか
bool OnBoxEdge(float3 location, float3 boxLow, float3 boxHigh, float width) {
    const float3 distance = min(location - boxLow, boxHigh - location);
    const uint32_t nearCount = (distance.x < width ? 1 : 0) + (distance.y < width ? 1 : 0) +
                               (distance.z < width ? 1 : 0);

    return nearCount >= 2;
}

// 格子の箱 [0, 1 辺]³
float2 IntersectGrid(Ray ray) {
    return IntersectBox(ray, 0.0, (float)PROBE_GRID_SIZE);
}

bool OnGridEdge(float3 location, float width) {
    return OnBoxEdge(location, 0.0, (float)PROBE_GRID_SIZE, width);
}

// --- 覗きの段(T-0096。抽出の覗きの欄、並びは probe_sim.hlsli)---

struct PeekLevel {
    float cellsPerWorld;  // この段のセルの数 / 世界のセル 1 つ(2^レベル)
    float3 origin;        // 原点(この段のセルの単位)
    float3 low;           // 箱(世界のセルの単位)
    float3 high;
};

uint32_t PeekLevelCount(uint32_t extraction) {
    return min(LoadExtraction(extraction, PROBE_EXTRACTION_PEEK_OFFSET), PROBE_PEEK_MAX_LEVELS);
}

PeekLevel LoadPeekLevel(uint32_t extraction, uint32_t index) {
    const uint32_t header = PROBE_EXTRACTION_PEEK_OFFSET + PROBE_PEEK_LEVEL_WORDS + index * PROBE_PEEK_LEVEL_WORDS;
    PeekLevel level;
    level.cellsPerWorld = exp2((float)LoadExtraction(extraction, header));
    level.origin = float3((float)(int)LoadExtraction(extraction, header + 1),
                          (float)(int)LoadExtraction(extraction, header + 2),
                          (float)(int)LoadExtraction(extraction, header + 3));
    level.low = level.origin / level.cellsPerWorld;
    level.high = (level.origin + (float)PROBE_PEEK_BLOCK_EDGE) / level.cellsPerWorld;

    return level;
}

bool InsidePeekLevel(PeekLevel level, float3 location) {
    return all(location >= level.low) && all(location < level.high);
}

// 段 index のセル(場所 location を含む)の 4 語の始まり
uint32_t PeekCellWordBase(PeekLevel level, uint32_t index, float3 location) {
    const uint3 local = (uint3)clamp(floor(location * level.cellsPerWorld - level.origin), 0.0,
                                     (float)PROBE_PEEK_BLOCK_EDGE - 1.0);
    const uint32_t cell = local.x + PROBE_PEEK_BLOCK_EDGE * (local.y + PROBE_PEEK_BLOCK_EDGE * local.z);

    return PROBE_EXTRACTION_PEEK_CELL_OFFSET + (index * PROBE_PEEK_BLOCK_CELLS + cell) * PROBE_EXTRACTION_CELL_WORDS;
}

// 潜っている段(1〜9。0 なら潜っていない)の枠の色
float3 PeekFrameColor(ViewConstants constants, PeekLevel level) {
    const uint32_t depth = (constants.flags >> VIEW_PEEK_DEPTH_SHIFT) & 15;
    const bool dive = depth != 0 && abs(level.cellsPerWorld - exp2((float)depth)) < 0.5;

    return dive ? PEEK_DIVE_COLOR : PEEK_FRAME_COLOR;
}

// 光線が当たる段の箱の辺(手前の面)。無ければ false
bool PeekBoxEdges(ViewConstants constants, Ray ray, out float3 color) {
    color = 0;
    const uint32_t count = PeekLevelCount(constants.extraction);
    bool found = false;
    for (uint32_t index = 0; index < count; ++index) {
        const PeekLevel level = LoadPeekLevel(constants.extraction, index);
        const float2 range = IntersectBox(ray, level.low, level.high);
        if (range.x >= range.y)
            continue;

        const float width = LINE_WIDTH_PIXELS * PixelSize(constants, range.x);
        const float3 enter = ray.origin + ray.direction * range.x;
        const float3 exit = ray.origin + ray.direction * range.y;
        if (OnBoxEdge(enter, level.low, level.high, width) || OnBoxEdge(exit, level.low, level.high, width)) {
            color = PeekFrameColor(constants, level);
            found = true;
        }
    }

    return found;
}

// --- 格子を辿る ---

struct March {
    float3 color;        // 前から重ねた色(ボリューム)
    float opacity;       // 前から重ねた不透明度
    float maximum;       // 光線の上の最大(0〜1)
    float activeLength;  // 活性なブロックの中を通った長さ(セル)
};

// セルごとに前から重ねる(吸収と発光)。最大値と活性なブロックの長さも数える
March MarchGrid(ViewConstants constants, Ray ray, float2 range) {
    March march;
    march.color = 0;
    march.opacity = 0;
    march.maximum = 0;
    march.activeLength = 0;
    const float directionLength = length(ray.direction);
    const float3 inverse = 1.0 / ray.direction;
    const float3 start = ray.origin + ray.direction * range.x;
    int3 cell = clamp((int3)floor(start + ray.direction * 1e-4), 0, (int)PROBE_GRID_SIZE - 1);
    const int3 step = int3(sign(ray.direction));
    const float3 nextBoundary = (float3)cell + max(float3(step), 0.0);
    float3 tMax = select(step != 0, (nextBoundary - ray.origin) * inverse, (float3)1e30);
    const float3 tDelta = select(step != 0, abs(inverse), (float3)1e30);
    float t = range.x;

    for (uint32_t index = 0; index < MAX_STEPS && t < range.y; ++index) {
        const float tNext = min(min(min(tMax.x, tMax.y), tMax.z), range.y);
        const float segment = max(tNext - t, 0.0) * directionLength;
        const uint3 current = (uint3)cell;
        const float amount = CellAmount(constants, current);
        const bool active = (constants.flags & VIEW_FLAG_ACTIVE_BLOCKS) != 0 &&
                            IsBlockActive(constants.extraction, current);

        march.maximum = max(march.maximum, amount);
        march.activeLength += active ? segment : 0.0;
        const float alpha = 1.0 - exp(-(amount * amount * amount * VOLUME_DENSITY + (active ? ACTIVE_DENSITY : 0.0)) *
                                      segment);
        const float3 emitted = amount > 0.0 ? HeatColor(amount) * 1.5 : ACTIVE_COLOR;
        march.color += (1.0 - march.opacity) * alpha * emitted;
        march.opacity += (1.0 - march.opacity) * alpha;
        if (constants.mode == VIEW_MODE_VOLUME && march.opacity > OPAQUE_ENOUGH)
            break;

        // 次のセルへ(いちばん近い境界の軸)
        if (tMax.x <= tMax.y && tMax.x <= tMax.z) {
            cell.x += step.x;
            tMax.x += tDelta.x;
        } else if (tMax.y <= tMax.z) {
            cell.y += step.y;
            tMax.y += tDelta.y;
        } else {
            cell.z += step.z;
            tMax.z += tDelta.z;
        }

        t = tNext;
        if (any(cell < 0) || any(cell >= (int)PROBE_GRID_SIZE))
            break;
    }

    return march;
}

// --- 断面 ---

struct SliceHit {
    bool hit;
    float t;
    float3 location;
};

// 断面(軸 sliceAxis の座標 = 位置 + 0.5)と光線の交わり(格子の中だけ)
SliceHit IntersectSlice(ViewConstants constants, Ray ray) {
    SliceHit result;
    result.hit = false;
    result.t = 0;
    result.location = 0;
    const float along = ray.direction[constants.sliceAxis];
    if (abs(along) < 1e-6)
        return result;

    const float plane = (float)constants.slicePosition + 0.5;
    result.t = (plane - ray.origin[constants.sliceAxis]) / along;
    result.location = ray.origin + ray.direction * result.t;
    // 軸の座標はちょうど面に(覗きの細かい段では面がセルの境目に来るので、丸めの揺れで上下のセルが混ざらないように。
    // 境目では上のセル = 覗いた点を含むセルを取る)
    result.location[constants.sliceAxis] = plane;
    result.hit = result.t >= 0.0 && all(result.location >= 0.0) && all(result.location < (float)PROBE_GRID_SIZE);

    return result;
}

// 覗きの段のセルの色(一番細かい段から。どの段にも入っていなければ false)。セルが画面で大きければ境目の線も
bool PeekSliceColor(ViewConstants constants, float3 location, float pixelSize, out float3 color) {
    color = 0;
    const uint32_t count = PeekLevelCount(constants.extraction);
    for (uint32_t step = 0; step < count; ++step) {
        const uint32_t index = count - 1 - step;
        const PeekLevel level = LoadPeekLevel(constants.extraction, index);
        if (!InsidePeekLevel(level, location))
            continue;

        const float amount = WordsAmount(constants, PeekCellWordBase(level, index, location));
        color = amount > 0.0 ? HeatColor(amount) : COLD_SLICE_COLOR;

        // --- 段のセルの境目(断面の 2 つの軸)---
        const float cellPixels = 1.0 / (level.cellsPerWorld * pixelSize);
        if (cellPixels > PEEK_CELL_LINE_MIN_PIXELS) {
            const float3 fine = location * level.cellsPerWorld;
            const float3 edgePixels = abs(fine - round(fine)) * cellPixels;
            bool onLine = false;
            [unroll] for (uint32_t axis = 0; axis < 3; ++axis)
                onLine = onLine || (axis != constants.sliceAxis && edgePixels[axis] < 0.75);

            color *= onLine ? 1.0 - PEEK_CELL_LINE_SHADE : 1.0;
        }

        return true;
    }

    return false;
}

// 断面の上の覗きの段の枠(段の箱の内側の縁)
bool OnPeekSliceFrame(ViewConstants constants, float3 location, float width, out float3 color) {
    color = 0;
    const uint32_t count = PeekLevelCount(constants.extraction);
    bool found = false;
    for (uint32_t index = 0; index < count; ++index) {
        const PeekLevel level = LoadPeekLevel(constants.extraction, index);
        if (!InsidePeekLevel(level, location))
            continue;

        const float3 distance = min(location - level.low, level.high - location);
        float nearest = 1e30;
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
            if (axis != constants.sliceAxis)
                nearest = min(nearest, distance[axis]);
        }

        if (nearest < width) {
            color = PeekFrameColor(constants, level);
            found = true;
        }
    }

    return found;
}

// 断面の色(覗きの段があればその色。活性なブロックの薄い色は世界のブロックで、覗きの段の上にも同じく重ねる)
float3 SliceColor(ViewConstants constants, float3 location, float pixelSize) {
    const uint3 cell = min((uint3)location, PROBE_GRID_SIZE - 1);
    float3 color;
    if (!PeekSliceColor(constants, location, pixelSize, color)) {
        const float amount = CellAmount(constants, cell);
        color = amount > 0.0 ? HeatColor(amount) : COLD_SLICE_COLOR;
    }

    const bool active = (constants.flags & VIEW_FLAG_ACTIVE_BLOCKS) != 0 && IsBlockActive(constants.extraction, cell);

    return active ? lerp(color, ACTIVE_COLOR, 0.25) : color;
}

// 断面の四角の縁(軸以外の 2 つの軸のどちらかが格子の端)
bool OnSliceFrame(ViewConstants constants, float3 location, float width) {
    const float3 distance = min(location, (float)PROBE_GRID_SIZE - location);
    float nearest = 1e30;
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        if (axis != constants.sliceAxis)
            nearest = min(nearest, distance[axis]);
    }

    return nearest < width;
}

// --- 凡例(左下の色の帯)---

bool Legend(float2 pixel, float2 viewport, uint32_t flags, out float3 color) {
    const float2 size = float2(256.0, 12.0);
    const float2 inside = pixel - float2(16.0, viewport.y - 28.0);
    color = 0;
    if (any(inside < -1.0) || any(inside > size + 1.0))
        return false;

    if (any(inside < 0.0) || any(inside > size)) {
        color = FRAME_COLOR;  // 帯の縁(1 画素)
        return true;
    }

    const float2 local = inside / size;
    color = HeatColor(local.x);
    // 目盛り: 対数なら 10 倍ごと(幅の 1/1000・1/100・1/10・1)、線形なら 1/4 ごと
    const float divisions = (flags & VIEW_FLAG_LOGARITHMIC) != 0 ? 3.0 : 4.0;
    const float tickDistancePixels = abs(frac(local.x * divisions + 0.5) - 0.5) / divisions * size.x;
    if (tickDistancePixels < 0.75 && local.y > 0.5)
        color = 1.0;

    return true;
}

// --- 入口 ---

struct VertexOutput {
    float4 position : SV_Position;
};

// 頂点バッファなしで画面を覆う三角形(頂点 3 つ)
VertexOutput VSMain(uint vertexId : SV_VertexID) {
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VertexOutput output;
    output.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);

    return output;
}

float4 PSMain(VertexOutput input) : SV_Target {
    const ViewConstants constants = LoadConstants();
    const float2 pixel = input.position.xy;
    float3 legend;
    if (Legend(pixel, constants.viewport, constants.flags, legend))
        return float4(legend, 1);

    const Ray ray = MakeRay(constants, pixel);
    const float2 range = IntersectGrid(ray);
    if (range.x >= range.y)
        return float4(BACKGROUND, 1);

    const float3 enter = ray.origin + ray.direction * range.x;
    const float3 exit = ray.origin + ray.direction * range.y;
    const bool frontEdge = OnGridEdge(enter, LINE_WIDTH_PIXELS * PixelSize(constants, range.x));
    const bool backEdge = OnGridEdge(exit, LINE_WIDTH_PIXELS * PixelSize(constants, range.y));
    const SliceHit slice = IntersectSlice(constants, ray);
    const bool sliceFrame = slice.hit &&
                            OnSliceFrame(constants, slice.location, LINE_WIDTH_PIXELS * PixelSize(constants, slice.t));

    // 断面の表示: 断面だけを不透明に(枠は上に)
    if (constants.mode == VIEW_MODE_SLICE) {
        const float slicePixel = PixelSize(constants, slice.t);
        float3 peekFrame;
        if (sliceFrame)
            return float4(SLICE_FRAME_COLOR, 1);

        if (slice.hit && OnPeekSliceFrame(constants, slice.location, LINE_WIDTH_PIXELS * slicePixel, peekFrame))
            return float4(peekFrame, 1);

        if (slice.hit)
            return float4(SliceColor(constants, slice.location, slicePixel), 1);

        return float4(frontEdge || backEdge ? FRAME_COLOR : BACKGROUND, 1);
    }

    const March march = MarchGrid(constants, ray, range);
    float3 color;
    if (constants.mode == VIEW_MODE_MAXIMUM) {
        color = march.maximum > 0.0 ? HeatColor(march.maximum) : BACKGROUND;
        color = lerp(color, ACTIVE_COLOR, (1.0 - exp(-march.activeLength * ACTIVE_DENSITY * 2.0)) * 0.5);
    } else
        color = march.color + (1.0 - march.opacity) * BACKGROUND;

    // 奥の枠は中身の後ろ(暗く)、手前の枠と断面の枠は上に
    if (backEdge)
        color = lerp(color, FRAME_COLOR, (1.0 - march.opacity) * 0.6);

    if (sliceFrame)
        color = lerp(color, SLICE_FRAME_COLOR, 0.8);

    // 覗きの段の箱(立体の表示では枠だけ。中身は断面の表示で)
    float3 peekEdge;
    if (PeekBoxEdges(constants, ray, peekEdge))
        color = peekEdge;

    if (frontEdge)
        color = FRAME_COLOR;

    return float4(color, 1);
}
