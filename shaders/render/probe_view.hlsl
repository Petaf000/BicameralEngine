// probe_view.hlsl — 仮の世界(common/probe_sim.hlsli の 64³ の熱)のデバッグ表示(T-0015。前は z = 32 の面だけ: T-0004・T-0005)。
//
// データの流れ: シミュ(compute キュー)が抽出の 3 組のどれかに全部のセルと活性の印を書く → 描画(direct キュー)がフェンスを待ってから、
// フレームの定数(frame。CPU がフレームごとに書く。engine/src/render/probe_view_constants.h と同じ並び)が指す組を読む(06 §4)。
// 画面いっぱいの三角形 1 枚で、画素ごとにカメラの光線を作り(engine/src/render/debug_camera.h と同じ式)、格子をセルごとに辿る(DDA)。
// 表示は 3 通り: 吸収と発光・光線の上の最大値・断面。重ね書き: 格子の枠・断面の枠(クリックがつつく面)・活性なブロック・色の凡例。
// 浮動小数点はここ(描画)だけ。決定性は求めない(10 の目的)。
#include "common/probe_sim.hlsli"

StructuredBuffer<uint32_t> extraction0 : register(t0);
StructuredBuffer<uint32_t> extraction1 : register(t1);
StructuredBuffer<uint32_t> extraction2 : register(t2);
ByteAddressBuffer frame : register(t3);  // ProbeViewConstants(96 バイト)

static const uint32_t VIEW_MODE_VOLUME = 0;
static const uint32_t VIEW_MODE_MAXIMUM = 1;
static const uint32_t VIEW_MODE_SLICE = 2;
static const uint32_t VIEW_FLAG_ACTIVE_BLOCKS = 1;
static const uint32_t VIEW_FLAG_LOGARITHMIC = 2;

static const float3 BACKGROUND = float3(0.02, 0.02, 0.03);
static const float3 FRAME_COLOR = float3(0.35, 0.37, 0.45);
static const float3 SLICE_FRAME_COLOR = float3(0.95, 0.8, 0.25);
static const float3 ACTIVE_COLOR = float3(0.15, 0.75, 0.95);
static const float3 COLD_SLICE_COLOR = float3(0.05, 0.06, 0.1);  // 断面の熱の無いセル(面の広がりが見えるように)
static const float VOLUME_DENSITY =
    0.25;  // 1 セルの長さあたりの濃さ(最大の熱で。熱の 3 乗で薄くし、奥の熱い所が透けて見えるように)
static const float ACTIVE_DENSITY = 0.012;  // 活性なブロックの薄い色(1 セルの長さあたり)
static const float OPAQUE_ENOUGH = 0.995;
static const float LINE_WIDTH_PIXELS = 1.2;
static const uint32_t MAX_STEPS = PROBE_GRID_SIZE * 3 + 4;  // 格子を斜めに抜けるときのセルの数の上限

struct ViewConstants {
    uint32_t extraction;
    float2 viewport;
    uint32_t flags;
    uint32_t mode;
    uint32_t sliceAxis;
    uint32_t slicePosition;
    bool orthographic;
    float3 position;
    float3 forward;
    float3 right;
    float3 up;
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
    if (extraction == 0) return extraction0[index];
    if (extraction == 1) return extraction1[index];
    return extraction2[index];
}

uint32_t LoadCell(uint32_t extraction, uint3 cell) {
    return LoadExtraction(extraction, ProbeCellIndex(cell.x, cell.y, cell.z));
}

bool IsBlockActive(uint32_t extraction, uint3 cell) {
    return LoadExtraction(extraction, PROBE_EXTRACTION_BLOCK_OFFSET + ProbeBlockOfCell(cell.x, cell.y, cell.z)) != 0;
}

// --- 色 ---

// 熱を 0〜1 に。対数(3D で広がると値がすぐ小さくなるので、広がった先まで見える)か線形(つつき 1 回分 = 1)
float Normalized(uint32_t value, uint32_t flags) {
    if ((flags & VIEW_FLAG_LOGARITHMIC) != 0)
        return saturate(log2((float)value + 1.0) / log2((float)PROBE_POKE_AMOUNT));
    return saturate((float)value / (float)PROBE_POKE_AMOUNT);
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

// 格子の箱 [0, 1 辺]³ との交わり(入る t・出る t)。当たらなければ enter > exit
float2 IntersectGrid(Ray ray) {
    const float3 inverse = 1.0 / ray.direction;  // 0 の成分は ±inf(比較はそのまま効く)
    const float3 near = (0.0 - ray.origin) * inverse;
    const float3 far = ((float)PROBE_GRID_SIZE - ray.origin) * inverse;
    const float3 low = min(near, far);
    const float3 high = max(near, far);
    return float2(max(max(max(low.x, low.y), low.z), 0.0), min(min(high.x, high.y), high.z));
}

// 箱の面の上の点が、辺(2 つの軸で端)の近くか
bool OnGridEdge(float3 location, float width) {
    const float3 distance = min(location, (float)PROBE_GRID_SIZE - location);
    const uint32_t nearCount =
        (distance.x < width ? 1 : 0) + (distance.y < width ? 1 : 0) + (distance.z < width ? 1 : 0);
    return nearCount >= 2;
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
        const float amount = Normalized(LoadCell(constants.extraction, current), constants.flags);
        const bool active =
            (constants.flags & VIEW_FLAG_ACTIVE_BLOCKS) != 0 && IsBlockActive(constants.extraction, current);

        march.maximum = max(march.maximum, amount);
        march.activeLength += active ? segment : 0.0;
        const float alpha =
            1.0 - exp(-(amount * amount * amount * VOLUME_DENSITY + (active ? ACTIVE_DENSITY : 0.0)) * segment);
        const float3 emitted = amount > 0.0 ? HeatColor(amount) * 1.5 : ACTIVE_COLOR;
        march.color += (1.0 - march.opacity) * alpha * emitted;
        march.opacity += (1.0 - march.opacity) * alpha;
        if (constants.mode == VIEW_MODE_VOLUME && march.opacity > OPAQUE_ENOUGH) break;

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
        if (any(cell < 0) || any(cell >= (int)PROBE_GRID_SIZE)) break;
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
    if (abs(along) < 1e-6) return result;
    result.t = ((float)constants.slicePosition + 0.5 - ray.origin[constants.sliceAxis]) / along;
    result.location = ray.origin + ray.direction * result.t;
    result.hit = result.t >= 0.0 && all(result.location >= 0.0) && all(result.location < (float)PROBE_GRID_SIZE);
    return result;
}

float3 SliceColor(ViewConstants constants, float3 location) {
    const uint3 cell = min((uint3)location, PROBE_GRID_SIZE - 1);
    const float amount = Normalized(LoadCell(constants.extraction, cell), constants.flags);
    float3 color = amount > 0.0 ? HeatColor(amount) : COLD_SLICE_COLOR;
    const bool active = (constants.flags & VIEW_FLAG_ACTIVE_BLOCKS) != 0 && IsBlockActive(constants.extraction, cell);
    return active ? lerp(color, ACTIVE_COLOR, 0.25) : color;
}

// 断面の四角の縁(軸以外の 2 つの軸のどちらかが格子の端)
bool OnSliceFrame(ViewConstants constants, float3 location, float width) {
    const float3 distance = min(location, (float)PROBE_GRID_SIZE - location);
    float nearest = 1e30;
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        if (axis != constants.sliceAxis) nearest = min(nearest, distance[axis]);
    }
    return nearest < width;
}

// --- 凡例(左下の色の帯)---

bool Legend(float2 pixel, float2 viewport, uint32_t flags, out float3 color) {
    const float2 size = float2(256.0, 12.0);
    const float2 inside = pixel - float2(16.0, viewport.y - 28.0);
    color = 0;
    if (any(inside < -1.0) || any(inside > size + 1.0)) return false;
    if (any(inside < 0.0) || any(inside > size)) {
        color = FRAME_COLOR;  // 帯の縁(1 画素)
        return true;
    }
    const float2 local = inside / size;
    color = HeatColor(local.x);
    // 目盛り: 対数なら 2^8 ごと(2^0・2^8・2^16・2^24)、線形なら 1/4 ごと
    const float divisions = (flags & VIEW_FLAG_LOGARITHMIC) != 0 ? 3.0 : 4.0;
    const float tickDistancePixels = abs(frac(local.x * divisions + 0.5) - 0.5) / divisions * size.x;
    if (tickDistancePixels < 0.75 && local.y > 0.5) color = 1.0;
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
    if (Legend(pixel, constants.viewport, constants.flags, legend)) return float4(legend, 1);

    const Ray ray = MakeRay(constants, pixel);
    const float2 range = IntersectGrid(ray);
    if (range.x >= range.y) return float4(BACKGROUND, 1);

    const float3 enter = ray.origin + ray.direction * range.x;
    const float3 exit = ray.origin + ray.direction * range.y;
    const bool frontEdge = OnGridEdge(enter, LINE_WIDTH_PIXELS * PixelSize(constants, range.x));
    const bool backEdge = OnGridEdge(exit, LINE_WIDTH_PIXELS * PixelSize(constants, range.y));
    const SliceHit slice = IntersectSlice(constants, ray);
    const bool sliceFrame =
        slice.hit && OnSliceFrame(constants, slice.location, LINE_WIDTH_PIXELS * PixelSize(constants, slice.t));

    // 断面の表示: 断面だけを不透明に(枠は上に)
    if (constants.mode == VIEW_MODE_SLICE) {
        if (sliceFrame) return float4(SLICE_FRAME_COLOR, 1);
        if (slice.hit) return float4(SliceColor(constants, slice.location), 1);
        return float4(frontEdge || backEdge ? FRAME_COLOR : BACKGROUND, 1);
    }

    const March march = MarchGrid(constants, ray, range);
    float3 color;
    if (constants.mode == VIEW_MODE_MAXIMUM) {
        color = march.maximum > 0.0 ? HeatColor(march.maximum) : BACKGROUND;
        color = lerp(color, ACTIVE_COLOR, (1.0 - exp(-march.activeLength * ACTIVE_DENSITY * 2.0)) * 0.5);
    } else {
        color = march.color + (1.0 - march.opacity) * BACKGROUND;
    }
    // 奥の枠は中身の後ろ(暗く)、手前の枠と断面の枠は上に
    if (backEdge) color = lerp(color, FRAME_COLOR, (1.0 - march.opacity) * 0.6);
    if (sliceFrame) color = lerp(color, SLICE_FRAME_COLOR, 0.8);
    if (frontEdge) color = FRAME_COLOR;
    return float4(color, 1);
}
