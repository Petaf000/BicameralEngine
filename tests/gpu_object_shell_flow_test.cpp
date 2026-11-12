// gpu_object_shell_flow_test.cpp — 物のまわりの空気の層の流れ(D-667 の 2 段目・D-673)の GPU の形を CPU リファレンスとビット一致で確かめ、
// 1 刻みの GPU の時間を測る(T-1200)。GPU は shaders/sim/object_shell_flow.hlsl(物 1 つ = 1 グループで 1 刻みの小刻み・圧力の反復を中で回す)、
// CPU は tests/object_flow_cpu.h の StepShellFlow(式は共有の shaders/common/object_heat.hlsli「層の流れ」)。
// 場面: 縦の紙(骨格 列 × 1 × 高さ・箱 7.8 mm・囲むセル 0.5 m)に層を k 段作り、端の 1/4 の骨格を 900 K・そこに面する層を 1,500 K にする
//   (流れを起こすため。物の熱と反応は回さない = 流れの段だけを比べて測る。費用は回す回数が決まっているので温度にほぼよらない)。
//   既定: 紙 24 列の層 2 段と層 4 段を 2 枚(別々のセル)で 12 刻み、4 刻みごとにボクセル・セル・対の端数・面の運動量・待ち・ブロックの状態・
//         セルの変わった段・数え上げ(出る上限で縮めた数・一番速い面)を比べる。圧力の値を groupshared に置く形と作業場に置く形の両方。
//   --measure(--measure-only は比べを飛ばす): 紙 24 列・A4(27 × 38)の層 4・6 段を、物 1 つと 46 個(RTX 3070 Ti の SM の数)で、
//         暖機 30 刻みの後 20 刻みの 1 刻みの平均の GPU の時間を出す。
//   圧力の反復の規則(T-1201): 比べは「0 から決まった 24 回」(T-1200 まで)・「0 から収まるまで」(既定)・「温めた始まり + 収まるまで」。
//         測るのは 0 から決まった回数(--sweeps)・0 から収まるまで・温めた始まり + 収まるまで の 3 つ(--rule 0 / 1 / 3 で 1 つだけ)。
//   --profile(T-1223。比べを飛ばす): 紙 24 列の層 2・4・6 段を物 1・4・16・46 個で、段を 1 つずつだけ回した時の 1 刻みの時間を出し、
//         段ごとの費用(段なしとの差)・固定の費用・物 1 つあたり・ボクセル 1 つあたりに分ける(段を外すと結果は変わるので測るだけ)。
//   --threads 256 / 512 / 1024: グループのスレッドの数を変えた形(object_shell_flow_t256 など。費用の比べ)。
//   --scale(T-1320 の段 0。比べを飛ばす): 紙 24 列の層 2・4 段を燃える物 1・10・100・1000 個で、散らばり(物ごとに別のセル)と
//         密な山(8 個で 1 つのセルを分ける)に置いて 1 刻みの時間を出し、物 1 つあたり・ボクセル 1 つあたりと、
//         山の並べ方での層の箱の和の体積と重なりを除いた体積(燃えている空気の体積)を出す。
//   1 つのセルに物が 2・3・8 個(T-1332): セルへの受け渡しは物ごとに粗い量を貯め、刻みの終わりに OhShellApplyCells がセルごとに物の順で足して
//         1 回だけ当てる(ADR-0500 の集めて書く段)。比べに、熱い層の温度を物ごとに変えた 2・3・8 個 × 2 セル + 1 個の場面を足し、
//         GPU を 2 回回して状態のハッシュが同じか(回ごとに変わらないか)も見る。
//   --grow(T-1331 の小さく試す 2。比べを飛ばす): 骨格を大きくした紙 1 枚で、箱の大きさと赤黒の回数(0 から収まるまで・多重格子)を出す。
//         切り分け(T-1349): --grow-cpu N(CPU だけで N 刻み・上限で止まった解きの中身)・--grow-shape 列 高さ 層(その 1 つだけ)・--grow-sweeps N(0 から決まった N 回だけ)・--grow-ticks 暖機 測る・--grow-groupshared。
//   分けた刻み(T-1350): 1 つの Dispatch が長くなる箱(ボクセル × 赤黒の上限 × 小刻みが GROUP_VOXEL_SWEEPS を超える)は、1 刻みを小刻みごとの
//         Dispatch〔⓪〜④ / 赤黒を続きから n 回ずつ / ⑥〜⑨〕に分けて回す(既定 = 自動)。比べに、分けた形(n = 7 で小刻みの途中で区切る)を
//         1 つの Dispatch の形と同じハッシュになるかも足した。--large-box: 紙 96 × 48・層 8 段(121,856 ボクセル)を 0 から決まった 1,024 回(上限)で
//         2 刻み回して CPU とビット一致・一番長い Dispatch の時間を出す(HW だけ。WARP では数時間かかる)。
//         --split auto|whole|split(測りの形を決める)・--split-sweeps N(分けた形の 1 つの Dispatch の赤黒の回数)。
// 引数は gpu_test_options.h と --measure / --measure-only / --profile / --scale / --grow / --large-box / --threads N / --sweeps N / --rule N / --split / --split-sweeps。
#include <cstdlib>
#include <cstring>
#include <optional>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "gpu_object_heat_stages.h"
#include "gpu_test_options.h"
#include "object_flow_cpu.h"
#include "sim/reaction_table.h"
#include "sun_column_scene.h"

using namespace bicameral;
namespace rx = bicameral::reaction;

namespace {

    // --- 試験の値(公開側の試験データ。ゲームの中身ではない。D-006。object_shell_test と同じ)---
    constexpr int32_t BODY_LEVEL = 6;  // 箱 7.8 mm
    constexpr int32_t CELL_LEVEL = 0;  // 囲むセル 0.5 m
    constexpr uint32_t FILM_UM = 3906;
    constexpr uint64_t PAPER_CELLULOSE_UNITS = 14809600;  // 紙 0.1 mm(ボクセルの単位)
    constexpr uint64_t AIR_OXYGEN_UNITS = 1092000;
    constexpr uint64_t AIR_NITROGEN_UNITS = 4108000;
    constexpr uint32_t DEFAULT_DIFFUSIVITY_MM2_PER_S = 20;
    constexpr int32_t HOT_SKELETON_MK = 900000;
    constexpr int32_t HOT_SHELL_MK = 1500000;
    constexpr uint32_t COMPARE_TICKS = 12;
    constexpr uint32_t COMPARE_EVERY = 4;
    constexpr uint32_t WARM_TICKS = 30;  // GPU の時計が上がるまで
    constexpr uint32_t MEASURE_TICKS = 20;
    constexpr uint32_t SM_COUNT = 46;  // RTX 3070 Ti

    // --- GPU の並び(object_shell_flow.hlsl と同じ)---
    constexpr uint32_t SHARED_VOXELS = 7168;
    constexpr uint32_t ROOT_CONSTANT_COUNT = 12;
    constexpr uint32_t SUBSTEPS = 4;        // 1 刻みの小刻み(CPU の StepShellFlow と同じ)
    constexpr uint32_t APPLY_THREADS = 64;  // object_shell_flow.hlsl の OH_SHELL_APPLY_THREADS
    constexpr uint32_t SWEEP_STATS = 4;     // g_stats の後ろ: 解いた回数・赤黒の和・一番多い・上限で止まった(T-1201)
    // u0〜u15 / b0 / デバッグのリング / t0〜t8(16 × 2 + 12 + 2 + 9 × 2 = 64 語 = 上限。物の熱の 63 語とは別の署名)
    constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
        .uavCount = 16, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = 9};

    // --- 分けた刻み(object_shell_flow.hlsl の OH_SPLIT_*。T-1350)---
    constexpr uint32_t SPLIT_PREPARE = 1;
    constexpr uint32_t SPLIT_SWEEPS = 2;
    constexpr uint32_t SPLIT_FINISH = 3;
    constexpr uint32_t SPLIT_WORDS = 3;  // g_stats の後ろの物ごとの作業場の語の数
    // 1 グループが 1 つの Dispatch で回してよいボクセル × 赤黒の回数(2^25 ≈ 0.24 s。RTX 3070 Ti で 1 ボクセルの赤黒 1 回 ≈ 7.1 ns・
    // Windows の TDR 2 s の約 1/8。T-1349)。groupshared に載る箱(7,168 ボクセル)× 上限 1,024 回 × 小刻み 4 はちょうど収まる
    constexpr uint64_t GROUP_VOXEL_SWEEPS = 1ull << 25;
    constexpr uint32_t SPLIT_SWEEPS_MAX = (1u << 24) - 1;  // g_split の上の 24 bit
    constexpr uint32_t DISPATCH_TIMESTAMPS = 1024;  // 1 つ目の刻みの Dispatch ごとの時刻(一番長い Dispatch を出す)

    // 段の印(object_shell_flow.hlsl の OH_STAGE_*。1 つずつだけ回して費用を分ける。T-1223)
    constexpr uint32_t ALL_STAGES = 0xFFFFFFFFu;
    constexpr uint32_t STAGE_PRESSURE = 1u << 4;
    constexpr std::array<const char*, 10> STAGE_NAMES = {"① 見立て", "② 面",    "③ 運ぶ運動量", "④ 残り",
                                                         "⑤ 圧力",   "⑥ 力積",  "⑦ 出る割合",   "⑧ 運ぶ",
                                                         "⑨ セルへ", "⑩ 起こす"};

    struct ShellVoxelWork {
        rx::OhFlowCell cell;
        int64_t potential;
        uint64_t total;
        uint64_t outShare;
        uint32_t changed;
        uint32_t padding;
        int64_t residual;
        uint64_t recipMultiplier;
        uint32_t recipShift1;
        uint32_t recipShift2;
        std::array<uint64_t, 6> weights;  // 隣との面の重み(圧力の反復が続けて読む。T-1223)
    };
    static_assert(sizeof(ShellVoxelWork) == 128);

    struct ShellFaceWork {
        uint64_t mass;
        int64_t speed;
        int64_t next;
        uint64_t weight;
        uint32_t wall;
        uint32_t padding;
    };
    static_assert(sizeof(ShellFaceWork) == 40);

    struct ShellObject {
        uint32_t faceBegin[3];
        uint32_t multigridBegin;  // 圧力の多重格子の作業場の始まり(T-1222)
        uint32_t pressureBegin;   // 詰めた圧力の欄の始まり(色 2 つ × 色の並びの数。T-1320 段 1)
    };

    // 詰めた圧力の欄(object_shell_flow.hlsl の OhShellPressure。T-1320 段 1)
    struct ShellPressureWork {
        int64_t residual;
        uint64_t recipMultiplier;
        uint32_t recipShift1;
        uint32_t recipShift2;
        std::array<uint32_t, 6> weights;
    };
    static_assert(sizeof(ShellPressureWork) == 48);

    // 圧力の多重格子の段のボクセル(object_shell_flow.hlsl の OhShellMultigrid。T-1222)
    struct ShellMultigridWork {
        std::array<uint64_t, 6> weights;
        uint64_t total;
        uint64_t recipMultiplier;
        uint32_t recipShift1;
        uint32_t recipShift2;
        int64_t rhs;
        int64_t value;
        int64_t scratch;
    };
    static_assert(sizeof(ShellMultigridWork) == 96);

    enum Uav : uint8_t {
        UavVoxels,
        UavCells,
        UavCarries,
        UavMomentum,
        UavWaits,
        UavBlockStates,
        UavCellChanged,
        UavStats,
        UavVoxelWork,
        UavGasWork,
        UavFaceWork,
        UavMultigrid,
        UavCellViews,  // 物ごとの囲むセルの見立て(T-1332)
        UavExchanges,  // 物ごとのセルへ渡す粗い量の和(T-1332)
        UavPressure,   // 詰めた圧力の欄(T-1320 段 1)
        UavFaceFlows,  // 面ごとの流れ(T-1320 段 1)
        UAV_COUNT,
    };
    constexpr uint32_t SRV_COUNT = 9;

    // --- 場面 ---

    struct Shape {
        const char* name;
        uint32_t columns;  // 骨格の x
        uint32_t sizeZ;    // 骨格の高さ
        uint32_t layers;
    };

    rx::RxCell Reheated(const sim::BakedReactionTable& table, const rx::RxCell& cell, int32_t milliKelvin) {
        std::vector<sim::SpeciesAmount> amounts;
        for (uint32_t s = 0; s < cell.speciesCount; ++s)
            amounts.push_back({.species = RX_SPECIES_AT(cell, s), .amount = RX_AMOUNT_AT(cell, s)});

        return sim::MakeReactionCell(table, amounts, milliKelvin);
    }

    // 端の 1/4 の骨格を 900 K、そこに面する層(骨格から 1 ボクセル)を 1,500 K に
    void Heat(const sim::BakedReactionTable& table, test::OhWorld& world, const Shape& shape,
              int32_t shellMilliKelvin) {
        const rx::OhObject object = world.objects[0];
        const uint32_t sizeZ = object.voxelCount / (object.sizeX * object.sizeY);
        const uint32_t hotEnd = shape.layers + (shape.columns / 4);
        for (uint32_t z = 0; z < sizeZ; ++z) {
            for (uint32_t y = 0; y < object.sizeY; ++y) {
                for (uint32_t x = 0; x < hotEnd; ++x) {
                    const uint32_t v = x + (object.sizeX * (y + (object.sizeY * z)));
                    const bool skeleton = (world.infos[v].shape & rx::OH_SHELL_BIT) == 0;
                    const bool nearY = y + 1 >= shape.layers && y <= shape.layers + 1;
                    const bool nearZ = z + 1 >= shape.layers && z <= shape.layers + shape.sizeZ;
                    if (skeleton)
                        world.voxels[v] = Reheated(table, world.voxels[v], HOT_SKELETON_MK);
                    else if (nearY && nearZ && x + 1 >= shape.layers)
                        world.voxels[v] = Reheated(table, world.voxels[v], shellMilliKelvin);
                }
            }
        }
    }

    std::optional<test::OhWorld> MakePaper(const sim::BakedReactionTable& table, const test::OhCpuTable& shared,
                                           const test::OhGasList& gases, const Shape& shape,
                                           int32_t shellMilliKelvin = HOT_SHELL_MK) {
        test::OhWorld world;
        const uint32_t count = shape.columns * shape.sizeZ;
        world.objects.push_back({.voxelBegin = 0,
                                 .voxelCount = count,
                                 .sizeX = shape.columns,
                                 .sizeY = 1,
                                 .cell = 0,
                                 .bodyLevel = BODY_LEVEL,
                                 .cellLevel = CELL_LEVEL,
                                 .substeps = 1,
                                 .filmUm = FILM_UM,
                                 .shellLayers = 0});
        const std::vector<sim::SpeciesAmount> solid = {
            {.species = table.SpeciesId("cellulose"), .amount = PAPER_CELLULOSE_UNITS},
            {.species = table.SpeciesId("oxygen"), .amount = AIR_OXYGEN_UNITS},
            {.species = table.SpeciesId("nitrogen"), .amount = AIR_NITROGEN_UNITS}};
        const rx::RxCell voxel = sim::MakeReactionCell(table, solid, test::SUN_ROOM_TEMPERATURE_MK);
        for (uint32_t z = 0; z < shape.sizeZ; ++z) {
            for (uint32_t x = 0; x < shape.columns; ++x) {
                const test::OhBoxFace face = test::BoxFaceOf(x, 0, z, shape.columns, 1, shape.sizeZ);
                world.infos.push_back({.object = 0, .shape = face.neighbours | (2u << rx::OH_EXPOSED_SHIFT)});
                world.voxels.push_back(voxel);
                world.light.push_back(0);
            }
        }

        const std::vector<sim::SpeciesAmount> air = {
            {.species = table.SpeciesId("oxygen"), .amount = AIR_OXYGEN_UNITS},
            {.species = table.SpeciesId("nitrogen"), .amount = AIR_NITROGEN_UNITS}};
        world.cells = {sim::MakeReactionCell(table, air, test::SUN_ROOM_TEMPERATURE_MK)};
        world.ranges = {{.cell = 0, .objectBegin = 0, .objectCount = 1}};
        world.carries = {rx::OhZeroFlow()};
        test::FinishObjectHeatWorld(world);
        if (!test::AddObjectShell(shared, gases, world, 0, 1, shape.layers))
            return std::nullopt;

        Heat(table, world, shape, shellMilliKelvin);

        return world;
    }

    // 物 1 つの世界(同じ形で状態だけ違うもの)を copies 個に(物 k は variants[k % 数]。objectsPerCell 個ごとに 1 つのセル。1 = 物ごとに別のセル)
    test::OhWorld Replicate(std::span<const test::OhWorld> variants, uint32_t copies, uint32_t objectsPerCell = 1) {
        test::OhWorld world;
        const test::OhWorld& first = variants[0];
        const auto voxels = static_cast<uint32_t>(first.voxels.size());
        const auto blocks = static_cast<uint32_t>(first.blocks.size());
        for (uint32_t k = 0; k < copies; ++k) {
            const test::OhWorld& one = variants[k % variants.size()];
            rx::OhObject object = one.objects[0];
            object.voxelBegin = k * voxels;
            object.cell = k / objectsPerCell;
            object.blockBegin = one.objects[0].blockBegin + (k * blocks);
            world.objects.push_back(object);
            for (rx::OhVoxelInfo info : one.infos) {
                info.object = k;
                world.infos.push_back(info);
            }

            world.voxels.insert(world.voxels.end(), one.voxels.begin(), one.voxels.end());
            world.light.insert(world.light.end(), one.light.begin(), one.light.end());
            world.waits.insert(world.waits.end(), one.waits.begin(), one.waits.end());
            if (k % objectsPerCell == 0) {
                const uint32_t count = std::min(objectsPerCell, copies - k);
                world.cells.push_back(one.cells[0]);
                world.ranges.push_back({.cell = k / objectsPerCell, .objectBegin = k, .objectCount = count});
                world.cellChangedPasses.push_back(one.cellChangedPasses[0]);
            }

            world.carries.push_back(one.carries[0]);
            for (rx::OhBlock block : one.blocks) {
                block.object = k;
                world.blocks.push_back(block);
            }

            world.blockStates.insert(world.blockStates.end(), one.blockStates.begin(), one.blockStates.end());
        }

        world.pass = first.pass;

        return world;
    }

    test::OhWorld Replicate(const test::OhWorld& one, uint32_t copies, uint32_t objectsPerCell = 1) {
        return Replicate(std::span(&one, 1), copies, objectsPerCell);
    }

    // 物ごとの面の並びの始まり(軸 0・1・2 の順)と面の数
    // 圧力の多重格子の全部の段のボクセルの数(object_shell_flow.hlsl の MgLevelCount・MgLevelOf と同じ。一番粗い段はどの向きも 2 以下)
    uint32_t MultigridVoxels(const test::OhFlowBox& box) {
        std::array<uint32_t, 3> size = {box.size[0], box.size[1], box.size[2]};
        uint32_t count = 0;
        while (true) {
            count += size[0] * size[1] * size[2];
            if (size[0] <= 2 && size[1] <= 2 && size[2] <= 2)
                return count;

            for (uint32_t& s : size)
                s = (s + 1) / 2;
        }
    }

    std::vector<ShellObject> ShellObjectsOf(const test::OhWorld& world, uint32_t& faceCount, uint32_t& multigridCount,
                                            uint32_t& pressureCount) {
        std::vector<ShellObject> shells;
        faceCount = 0;
        multigridCount = 0;
        pressureCount = 0;
        for (const rx::OhObject& object : world.objects) {
            const test::OhFlowBox box = test::FlowBoxOf(object);
            ShellObject shell{};
            for (uint32_t a = 0; a < 3; ++a) {
                shell.faceBegin[a] = faceCount;
                faceCount += static_cast<uint32_t>(box.FaceCount(a));
            }

            shell.multigridBegin = multigridCount;
            multigridCount += MultigridVoxels(box);
            shell.pressureBegin = pressureCount;
            pressureCount += 2 * ((box.size[0] + 1) / 2) * box.size[1] * box.size[2];
            shells.push_back(shell);
        }

        return shells;
    }

    // --- GPU ---

    // 1 刻みの Dispatch の形(T-1350)
    enum class SplitMode : uint8_t {
        Auto,   // 1 つの Dispatch が長くなる箱だけ分ける(既定)
        Whole,  // 1 刻み = 1 つの Dispatch
        Split,  // 段ごとの Dispatch
    };

    struct Gpu {
        ComPtr<ID3D12RootSignature> rootSignature;
        ComPtr<ID3D12PipelineState> pipeline;
        ComPtr<ID3D12PipelineState> applyPipeline;  // 集めて書く段 OhShellApplyCells(T-1332)
        std::array<ComPtr<ID3D12Resource>, UAV_COUNT> uavs;
        std::array<ComPtr<ID3D12Resource>, UAV_COUNT> uploads;
        std::array<gpu::ResidentBuffer, SRV_COUNT> srvs;
        ComPtr<ID3D12QueryHeap> timestamps;
        ComPtr<ID3D12Resource> timestampReadback;
        uint32_t objectCount = 0;
        uint32_t faceCount = 0;
        uint32_t rangeCount = 0;
        uint32_t passBase = 0;
        uint32_t sharedPotential = 0;
        uint32_t pressureSweeps = 1024;  // 圧力の赤黒の上限(CPU の OhShellFlow と同じ既定)
        uint32_t pressureMode =
            1;  // 1 = 収まったら止める・2 = 温めた始まり・4 = 多重格子・8 = 試験だけ 64bit の欄の道(shaders の OH_PRESSURE_*。T-1201・T-1222)
        uint32_t stageMask = ALL_STAGES;  // 回す段(測るだけ。T-1223)

        // --- 分けた刻み(T-1350)---
        uint32_t maxVoxels = 0;  // 一番大きい箱のボクセルの数
        SplitMode split = SplitMode::Auto;
        uint32_t splitSweeps = 0;      // 分けた形の 1 つの Dispatch の赤黒の回数(0 = GROUP_VOXEL_SWEEPS ÷ 一番大きい箱)
        double longestDispatchMs = 0;  // 最後の RunTicks の 1 つ目の刻みで一番長い Dispatch
        uint32_t dispatchesPerTick = 0;
    };

    // 1 刻みを段ごとの Dispatch に分けるか。自動 = 1 つの Dispatch の形で、一番大きい箱が上限まで回すと GROUP_VOXEL_SWEEPS を超える時
    bool UsesSplit(const Gpu& gpuState) {
        if (gpuState.split != SplitMode::Auto)
            return gpuState.split == SplitMode::Split;

        return static_cast<uint64_t>(gpuState.maxVoxels) * gpuState.pressureSweeps * SUBSTEPS > GROUP_VOXEL_SWEEPS;
    }

    uint32_t SplitSweepsOf(const Gpu& gpuState) {
        if (gpuState.splitSweeps != 0)
            return std::min(gpuState.splitSweeps, SPLIT_SWEEPS_MAX);

        const uint64_t sweeps = GROUP_VOXEL_SWEEPS / std::max<uint32_t>(gpuState.maxVoxels, 1);
        return static_cast<uint32_t>(std::clamp<uint64_t>(sweeps, 1, SPLIT_SWEEPS_MAX));
    }

    // 圧力の反復の規則(T-1201。CPU の OhShellFlow と GPU の g_pressureMode に同じものを渡す)
    struct PressureRule {
        const char* name;
        uint32_t sweeps;
        uint32_t mode;
    };

    void ApplyRule(const PressureRule& rule, test::OhShellFlow& flow) {
        flow.pressureSweeps = rule.sweeps;
        flow.pressureUntilQuiet = (rule.mode & 1u) != 0;
        flow.pressureWarmStart = (rule.mode & 2u) != 0;
        flow.pressureMultigrid = (rule.mode & 4u) != 0;
    }

    size_t StatsCount(size_t objects) {
        return 2 + (2 * objects) + SWEEP_STATS + (SPLIT_WORDS * objects);  // 後ろは分けた刻みの作業場(T-1350)
    }

    template <typename T>
    std::span<const std::byte> BytesOf(const std::vector<T>& values) {
        return std::as_bytes(std::span(values));
    }

    std::expected<Gpu, std::string> CreateGpu(ID3D12Device5* device, const sim::BakedReactionTable& table,
                                              const test::OhTables& tables, const test::OhWorld& world,
                                              bool sharedPotential, std::string_view shader) {
        Gpu result;
        result.objectCount = static_cast<uint32_t>(world.objects.size());
        result.rangeCount = static_cast<uint32_t>(world.ranges.size());
        result.passBase = world.pass;
        result.sharedPotential = sharedPotential ? 1u : 0u;
        for (const rx::OhObject& object : world.objects)
            result.maxVoxels = std::max(result.maxVoxels, object.voxelCount);
        result.rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        const auto bytecode = gpu::LoadShader(shader);
        const auto applyBytecode = gpu::LoadShader("sim/object_shell_apply_cells.cso");
        if (!result.rootSignature || !bytecode || !applyBytecode)
            return std::unexpected("ルート署名かシェーダーを作れない");

        result.pipeline = gpu::CreateComputePipeline(device, result.rootSignature.Get(), *bytecode);
        result.applyPipeline = gpu::CreateComputePipeline(device, result.rootSignature.Get(), *applyBytecode);
        if (!result.pipeline || !result.applyPipeline)
            return std::unexpected("パイプラインを作れない");

        uint32_t multigridCount = 0;
        uint32_t pressureCount = 0;
        const std::vector<ShellObject> shells = ShellObjectsOf(world, result.faceCount, multigridCount, pressureCount);
        const std::vector<ShellPressureWork> pressureWork(pressureCount, ShellPressureWork{});
        const std::vector<int64_t> momentum(result.faceCount, 0);
        const std::vector<uint32_t> stats(StatsCount(world.objects.size()), 0);
        const std::vector<ShellVoxelWork> voxelWork(world.voxels.size(), ShellVoxelWork{});
        const std::vector<rx::OhVoxelGas> gasWork(world.voxels.size(), rx::OhVoxelGas{});
        const std::vector<ShellFaceWork> faceWork(result.faceCount, ShellFaceWork{});
        const std::vector<rx::OhFlow> faceFlows(result.faceCount, rx::OhZeroFlow());
        const std::vector<ShellMultigridWork> multigridWork(multigridCount, ShellMultigridWork{});
        const std::vector<rx::RxCell> cellViews(world.objects.size(), rx::RxCell{});
        const std::vector<rx::OhFlow> exchanges(world.objects.size(), rx::OhZeroFlow());
        const std::array<std::span<const std::byte>, UAV_COUNT> initial = {BytesOf(world.voxels),
                                                                           BytesOf(world.cells),
                                                                           BytesOf(world.carries),
                                                                           BytesOf(momentum),
                                                                           BytesOf(world.waits),
                                                                           BytesOf(world.blockStates),
                                                                           BytesOf(world.cellChangedPasses),
                                                                           BytesOf(stats),
                                                                           BytesOf(voxelWork),
                                                                           BytesOf(gasWork),
                                                                           BytesOf(faceWork),
                                                                           BytesOf(multigridWork),
                                                                           BytesOf(cellViews),
                                                                           BytesOf(exchanges),
                                                                           BytesOf(pressureWork),
                                                                           BytesOf(faceFlows)};
        for (size_t i = 0; i < initial.size(); ++i) {
            result.uavs[i] = gpu::CreateBuffer(device, initial[i].size(), gpu::BufferKind::UnorderedAccess);
            result.uploads[i] = gpu::CreateFilledUpload(device, initial[i]);
            if (!result.uavs[i] || !result.uploads[i])
                return std::unexpected("世界のバッファを作れない");
        }

        const std::vector<uint32_t> gasSpecies(tables.gases.species.begin(), tables.gases.species.end());
        const std::array<std::span<const std::byte>, SRV_COUNT> srvs = {
            BytesOf(table.species), BytesOf(tables.species), BytesOf(table.molarMasses),
            BytesOf(gasSpecies),    BytesOf(world.objects),  BytesOf(world.infos),
            BytesOf(shells),        BytesOf(world.blocks),   BytesOf(world.ranges)};
        for (size_t i = 0; i < srvs.size(); ++i) {
            result.srvs[i] = gpu::CreateResidentBuffer(device, srvs[i]);
            if (!result.srvs[i].Valid())
                return std::unexpected("表のバッファを作れない");
        }

        result.timestampReadback = gpu::CreateBuffer(device, (2 + DISPATCH_TIMESTAMPS) * sizeof(uint64_t),
                                                     gpu::BufferKind::Readback);
        const D3D12_QUERY_HEAP_DESC heapDesc{
            .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 2 + DISPATCH_TIMESTAMPS, .NodeMask = 0};
        if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&result.timestamps))) || !result.timestampReadback)
            return std::unexpected("タイムスタンプを作れない");

        return result;
    }

    bool Upload(gpu::ImmediateQueue& queue, const Gpu& gpuState) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return false;

        // 写し元は 256 バイト以上に切り上がる(CreateFilledUpload)ので、CopyResource(同じ大きさの 2 つの間だけ)ではなく UAV の大きさだけ写す
        // (数え上げの欄 40 バイトで大きさが食い違い、HW は通るが WARP はデバイスを失った。T-1284)
        for (size_t i = 0; i < gpuState.uavs.size(); ++i)
            list->CopyBufferRegion(gpuState.uavs[i].Get(), 0, gpuState.uploads[i].Get(), 0,
                                   gpuState.uavs[i]->GetDesc().Width);

        gpu::RecordResidentUploads(list, gpuState.srvs);

        return queue.ExecuteAndWait();
    }

    // 刻み [first, first + count) を 1 本のリストで流す。戻り値 = 1 刻みの平均の GPU の ms
    std::expected<double, std::string> RunTicks(gpu::ImmediateQueue& queue, Gpu& gpuState, gpu::DebugRing& debugRing,
                                                uint64_t first, uint32_t count) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        debugRing.RecordBegin(list);
        list->SetComputeRootSignature(gpuState.rootSignature.Get());
        list->SetPipelineState(gpuState.pipeline.Get());
        for (uint32_t i = 0; i < UAV_COUNT; ++i)
            list->SetComputeRootUnorderedAccessView(i, gpuState.uavs[i]->GetGPUVirtualAddress());

        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing.GpuAddress());
        for (uint32_t i = 0; i < SRV_COUNT; ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i),
                                                   gpuState.srvs[i].buffer->GetGPUVirtualAddress());

        list->EndQuery(gpuState.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        uint32_t stamped = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t tick = first + i;
            // 1 つ目の刻みだけ Dispatch ごとに時刻を打つ(一番長い Dispatch を出す)
            const auto stamp = [&](bool firstTick) {
                if (!firstTick || stamped >= DISPATCH_TIMESTAMPS)
                    return;

                list->EndQuery(gpuState.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 + stamped);
                ++stamped;
            };
            const auto dispatch = [&](uint32_t split) {
                const std::array<uint32_t, ROOT_CONSTANT_COUNT> constants = {gpuState.objectCount,
                                                                             static_cast<uint32_t>(tick),
                                                                             static_cast<uint32_t>(tick >> 32),
                                                                             gpuState.passBase,
                                                                             SUBSTEPS,
                                                                             gpuState.pressureSweeps,
                                                                             1u,
                                                                             gpuState.sharedPotential,
                                                                             gpuState.pressureMode,
                                                                             gpuState.stageMask,
                                                                             gpuState.rangeCount,
                                                                             split};
                list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT,
                                                   constants.data(), 0);
                list->Dispatch(gpuState.objectCount, 1, 1);
                test::object_heat_gpu::Barrier(list);
                stamp(i == 0);
            };

            if (!UsesSplit(gpuState)) {
                dispatch(0);
            } else {
                // 段ごとの Dispatch(T-1350): 小刻みごとに ⓪〜④ / 赤黒を続きから n 回ずつ(上限まで足りる数)/ ⑥〜⑨
                const uint32_t sweeps = SplitSweepsOf(gpuState);
                const uint32_t chunks = (gpuState.pressureSweeps + sweeps - 1) / sweeps;
                for (uint32_t substep = 0; substep < SUBSTEPS; ++substep) {
                    dispatch(SPLIT_PREPARE | (substep << 2));
                    for (uint32_t c = 0; c < chunks; ++c)
                        dispatch(SPLIT_SWEEPS | (substep << 2) | (sweeps << 8));

                    dispatch(SPLIT_FINISH | (substep << 2));
                }
            }

            // 集めて書く段: セルごとに物の順で足して 1 回だけ当てる(T-1332)
            list->SetPipelineState(gpuState.applyPipeline.Get());
            list->Dispatch((gpuState.rangeCount + APPLY_THREADS - 1) / APPLY_THREADS, 1, 1);
            test::object_heat_gpu::Barrier(list);
            stamp(i == 0);
            list->SetPipelineState(gpuState.pipeline.Get());
            gpuState.passBase += gpuState.objectCount;
        }

        list->EndQuery(gpuState.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(gpuState.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2 + stamped,
                               gpuState.timestampReadback.Get(), 0);
        debugRing.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = debugRing.Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        std::vector<uint64_t> times(2 + stamped, 0);
        uint64_t frequency = 0;
        if (!gpu::ReadBuffer(gpuState.timestampReadback.Get(), std::as_writable_bytes(std::span(times))) ||
            FAILED(queue.Native()->GetTimestampFrequency(&frequency)) || frequency == 0)
            return std::unexpected("タイムスタンプを読めない");

        // 一番長い Dispatch(1 つ目の刻み。バリアの待ちも含む)
        uint64_t longest = 0;
        for (uint32_t k = 0; k < stamped; ++k)
            longest = std::max(longest, times[2 + k] - times[k == 0 ? 0 : 1 + k]);

        gpuState.longestDispatchMs = static_cast<double>(longest) * 1000.0 / static_cast<double>(frequency);
        gpuState.dispatchesPerTick = stamped;

        return static_cast<double>(times[1] - times[0]) * 1000.0 / static_cast<double>(frequency) /
               static_cast<double>(count);
    }

    // GPU の真実と数え上げを CPU と比べる。食い違いの数
    std::expected<size_t, std::string> Compare(ID3D12Device5* device, gpu::ImmediateQueue& queue, const Gpu& gpuState,
                                               const test::OhWorld& cpu, const std::vector<test::OhShellFlow>& flows) {
        using test::object_heat_gpu::CountDifferent;
        using test::object_heat_gpu::ReadUav;
        std::vector<int64_t> momentum;
        for (const test::OhShellFlow& flow : flows) {
            for (uint32_t a = 0; a < 3; ++a)
                momentum.insert(momentum.end(), flow.momentum[a].begin(), flow.momentum[a].end());
        }

        const auto voxels = ReadUav<rx::RxCell>(device, queue, gpuState.uavs[UavVoxels].Get(), cpu.voxels.size());
        const auto cells = ReadUav<rx::RxCell>(device, queue, gpuState.uavs[UavCells].Get(), cpu.cells.size());
        const auto carries = ReadUav<rx::OhFlow>(device, queue, gpuState.uavs[UavCarries].Get(), cpu.carries.size());
        const auto faces = ReadUav<int64_t>(device, queue, gpuState.uavs[UavMomentum].Get(), momentum.size());
        const auto waits = ReadUav<rx::OhVoxelWait>(device, queue, gpuState.uavs[UavWaits].Get(), cpu.waits.size());
        const auto states = ReadUav<rx::OhBlockState>(device, queue, gpuState.uavs[UavBlockStates].Get(),
                                                      cpu.blockStates.size());
        const auto changed = ReadUav<uint32_t>(device, queue, gpuState.uavs[UavCellChanged].Get(),
                                               cpu.cellChangedPasses.size());
        const auto stats = ReadUav<uint32_t>(device, queue, gpuState.uavs[UavStats].Get(), StatsCount(flows.size()));
        const auto voxelWork = ReadUav<ShellVoxelWork>(device, queue, gpuState.uavs[UavVoxelWork].Get(),
                                                       cpu.voxels.size());
        if (!voxels || !cells || !carries || !faces || !waits || !states || !changed || !stats || !voxelWork)
            return std::unexpected("読み戻せない");

        size_t different = CountDifferent<rx::RxCell>(cpu.voxels, *voxels, "ボクセル") +
                           CountDifferent<rx::RxCell>(cpu.cells, *cells, "セル") +
                           CountDifferent<rx::OhFlow>(cpu.carries, *carries, "対の端数") +
                           CountDifferent<int64_t>(momentum, *faces, "面の運動量") +
                           CountDifferent<rx::OhVoxelWait>(cpu.waits, *waits, "待ち") +
                           CountDifferent<rx::OhBlockState>(cpu.blockStates, *states, "ブロックの状態");
        for (size_t c = 0; c < cpu.cellChangedPasses.size(); ++c)
            different += cpu.cellChangedPasses[c] != (*changed)[c] ? 1u : 0u;

        if ((*stats)[0] != 0 || (*stats)[1] != 0) {
            Log(Channel::Gpu, Level::Error, "GPU: 量が負・枠があふれた {}・圧力の値が 32bit に入らない {}", (*stats)[0],
                (*stats)[1]);
            ++different;
        }

        // 圧力の反復の回数(T-1201)と、温めた始まりなら解き始めの値(層の状態)
        uint64_t solves = 0;
        uint64_t sweeps = 0;
        uint32_t most = 0;
        uint64_t capped = 0;
        for (size_t o = 0; o < flows.size(); ++o) {
            solves += flows[o].pressureSolves;
            sweeps += flows[o].pressureSweepsUsed;
            most = std::max(most, flows[o].pressureMostSweeps);
            capped += flows[o].pressureCapped;
            if (!flows[o].pressureWarmStart)
                continue;

            const rx::OhObject& object = cpu.objects[o];
            for (uint32_t v = 0; v < object.voxelCount; ++v)
                different += flows[o].potential[v] != (*voxelWork)[object.voxelBegin + v].potential ? 1u : 0u;
        }

        const size_t base = 2 + (2 * flows.size());
        if ((*stats)[base] != solves || (*stats)[base + 1] != sweeps || (*stats)[base + 2] != most ||
            (*stats)[base + 3] != capped) {
            Log(Channel::Gpu, Level::Error,
                "圧力の反復: 解いた cpu {} / gpu {}・赤黒の和 cpu {} / gpu {}・一番多い cpu {} / gpu {}・上限 cpu {} / "
                "gpu {}",
                solves, (*stats)[base], sweeps, (*stats)[base + 1], most, (*stats)[base + 2], capped,
                (*stats)[base + 3]);
            ++different;
        }

        for (size_t o = 0; o < flows.size(); ++o) {
            const uint32_t limited = (*stats)[2 + (2 * o)];
            const uint32_t fastest = (*stats)[3 + (2 * o)];
            if (limited == flows[o].limitedShares && fastest == static_cast<uint64_t>(flows[o].fastestQ16))
                continue;

            Log(Channel::Gpu, Level::Error, "物 {}: 出る上限で縮めた数 cpu {} / gpu {}・一番速い面 cpu {} / gpu {}", o,
                flows[o].limitedShares, limited, flows[o].fastestQ16, fastest);
            ++different;
        }

        return different;
    }

    struct Context {
        ID3D12Device5* device;
        D3D12_COMMAND_LIST_TYPE queueType;
        const sim::BakedReactionTable& table;
        const test::OhTables& tables;
        const test::OhCpuTable& shared;
        uint32_t measureSweeps;  // 測る時の「0 から決まった回数」の回数(切り分け。比べは 24 のまま)
        std::string shader;      // シェーダー(--threads で変える。T-1223)

        // --- 測りの刻みの数(--grow-ticks。T-1349 の切り分け)---
        uint32_t warmTicks = WARM_TICKS;
        uint32_t measureTicks = MEASURE_TICKS;

        // --- 1 刻みの Dispatch の形(--split・--split-sweeps。T-1350)---
        SplitMode split = SplitMode::Auto;
        uint32_t splitSweeps = 0;
    };

    const char* SplitName(const Gpu& gpuState) {
        return UsesSplit(gpuState) ? "段ごとの Dispatch" : "1 つの Dispatch";
    }

    // GPU の真実(ボクセル・セル・対の端数・面の運動量)のハッシュ(FNV-1a。回ごとに変わらないかを見る。T-1332)
    std::expected<uint64_t, std::string> GpuDigest(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                   const Gpu& gpuState, const test::OhWorld& world) {
        using test::object_heat_gpu::ReadUav;
        const auto voxels = ReadUav<rx::RxCell>(device, queue, gpuState.uavs[UavVoxels].Get(), world.voxels.size());
        const auto cells = ReadUav<rx::RxCell>(device, queue, gpuState.uavs[UavCells].Get(), world.cells.size());
        const auto carries = ReadUav<rx::OhFlow>(device, queue, gpuState.uavs[UavCarries].Get(), world.carries.size());
        const auto faces = ReadUav<int64_t>(device, queue, gpuState.uavs[UavMomentum].Get(), gpuState.faceCount);
        if (!voxels || !cells || !carries || !faces)
            return std::unexpected("読み戻せない");

        uint64_t digest = 14695981039346656037ull;
        const auto mix = [&digest](std::span<const std::byte> bytes) {
            for (const std::byte b : bytes)
                digest = (digest ^ static_cast<uint64_t>(b)) * 1099511628211ull;
        };
        mix(BytesOf(*voxels));
        mix(BytesOf(*cells));
        mix(BytesOf(*carries));
        mix(BytesOf(*faces));

        return digest;
    }

    // 比べの場面: 物の数と、何個ごとに 1 つのセルか(1 = 物ごとに別のセル)。熱い層の温度は物ごとに variants の順で変える
    struct CompareScene {
        uint32_t copies = 2;
        uint32_t objectsPerCell = 1;
        bool vary = false;               // 物ごとに熱い層の温度を変える(1,500 K・1,200 K・1,800 K の順)
        uint32_t ticks = COMPARE_TICKS;  // 比べる刻みの数(every ごとに比べる)
        uint32_t every = COMPARE_EVERY;
        double maxDispatchMs = 0;  // 0 でなければ、1 つ目の刻みの一番長い Dispatch がこれを超えたら失敗(T-1350)
    };

    // CPU と GPU を同じ場面から回して比べる。戻り値 = 最後の GPU の真実のハッシュ
    std::expected<uint64_t, std::string> RunCompare(const Context& context, const Shape& shape, bool sharedPotential,
                                                    const PressureRule& rule, const CompareScene& scene = {}) {
        std::vector<test::OhWorld> variants;
        for (const int32_t shellMk : {HOT_SHELL_MK, 1200000, 1800000}) {
            auto one = MakePaper(context.table, context.shared, context.tables.gases, shape, shellMk);
            if (!one)
                return std::unexpected("層を作れない");

            variants.push_back(std::move(*one));
            if (!scene.vary)
                break;
        }

        const test::OhWorld* one = &variants[0];
        test::OhWorld world = Replicate(variants, scene.copies, scene.objectsPerCell);
        std::vector<test::OhShellFlow> flows(world.objects.size());
        for (uint32_t o = 0; o < flows.size(); ++o) {
            test::ResetShellFlow(world, o, flows[o]);
            ApplyRule(rule, flows[o]);
        }

        auto queue = gpu::ImmediateQueue::Create(context.device, context.queueType);
        auto debugRing = gpu::DebugRing::Create(context.device);
        auto gpuState = CreateGpu(context.device, context.table, context.tables, world, sharedPotential,
                                  context.shader);
        if (!queue || !debugRing || !gpuState || !Upload(*queue, *gpuState))
            return std::unexpected(gpuState ? "GPU の準備に失敗" : gpuState.error());

        gpuState->pressureSweeps = rule.sweeps;
        gpuState->pressureMode = rule.mode;
        gpuState->split = context.split;
        gpuState->splitSweeps = context.splitSweeps;
        uint64_t limited = 0;
        for (uint32_t tick = 2; tick < 2 + scene.ticks; tick += scene.every) {
            const auto ran = RunTicks(*queue, *gpuState, *debugRing, tick, scene.every);
            if (!ran)
                return std::unexpected(ran.error());

            for (uint32_t t = tick; t < tick + scene.every; ++t) {
                if (!test::StepShellFlows(context.shared, context.tables.gases, world, flows, t))
                    return std::unexpected(std::format("CPU の流れが崩れた(刻み {})", t));
            }

            const auto different = Compare(context.device, *queue, *gpuState, world, flows);
            if (!different)
                return std::unexpected(different.error());

            if (*different != 0)
                return std::unexpected(
                    std::format("刻み {} で GPU と CPU が食い違った({} 件)", tick + scene.every - 1, *different));
        }

        for (const test::OhShellFlow& flow : flows)
            limited += flow.limitedShares;

        const auto digest = GpuDigest(context.device, *queue, *gpuState, world);
        if (!digest)
            return std::unexpected(digest.error());

        if (scene.maxDispatchMs > 0 && gpuState->longestDispatchMs > scene.maxDispatchMs)
            return std::unexpected(std::format("一番長い Dispatch {:.1f} ms が {:.0f} ms を超えた",
                                               gpuState->longestDispatchMs, scene.maxDispatchMs));

        Log(Channel::Gpu, Level::Info,
            "{}・圧力の値は{}・{}・{}: {} ボクセル × {} 枚({} 枚で 1 セル)・{} 刻みでビット一致(一番速い面 "
            "{}・出る上限で縮めた {}・"
            "赤黒 平均 {:.1f} 一番多い {} 上限 {}・ハッシュ {:016x}・Dispatch 1 刻み {} 回 一番長い {:.3f} ms)",
            shape.name, sharedPotential ? " groupshared" : "作業場", rule.name, SplitName(*gpuState),
            one->voxels.size(), scene.copies, scene.objectsPerCell, scene.ticks, flows[0].fastestQ16, limited,
            static_cast<double>(flows[0].pressureSweepsUsed) /
                static_cast<double>(std::max<uint64_t>(flows[0].pressureSolves, 1)),
            flows[0].pressureMostSweeps, flows[0].pressureCapped, *digest, gpuState->dispatchesPerTick,
            gpuState->longestDispatchMs);

        return *digest;
    }

    // 1 つのセルに物が 2・3・8 個(T-1332): 2 セル + 1 個(最後のセルは 1 個)・物ごとに熱い層の温度を変えて CPU とビット一致、
    // GPU を 2 回回してハッシュが同じ(回ごとに変わらない)。圧力は既定(0 から収まるまで)・groupshared
    std::expected<void, std::string> CompareSharedCells(const Context& context) {
        const Shape layers2{"紙 24 列・層 2 段", 24, 3, 2};
        const Shape layers4{"紙 24 列・層 4 段", 24, 3, 4};
        const PressureRule rule{"0 から収まるまで", 1024, 1};
        const std::array<std::pair<const Shape*, uint32_t>, 4> cases = {
            std::pair{&layers2, 2u}, std::pair{&layers2, 3u}, std::pair{&layers2, 8u}, std::pair{&layers4, 3u}};
        for (const auto& [shape, perCell] : cases) {
            const CompareScene scene{.copies = (2 * perCell) + 1, .objectsPerCell = perCell, .vary = true};
            const auto first = RunCompare(context, *shape, true, rule, scene);
            if (!first)
                return std::unexpected(std::format("{}・{} 個で 1 セル: {}", shape->name, perCell, first.error()));

            const auto again = RunCompare(context, *shape, true, rule, scene);
            if (!again)
                return std::unexpected(
                    std::format("{}・{} 個で 1 セル(2 回目): {}", shape->name, perCell, again.error()));

            if (*first != *again)
                return std::unexpected(
                    std::format("{}・{} 個で 1 セル: GPU のハッシュが回ごとに違う({:016x} / {:016x})", shape->name,
                                perCell, *first, *again));
        }

        return {};
    }

    // 段ごとの Dispatch に分けた形(T-1350): 赤黒を 7 回ずつに区切り(収まるのは区切りの途中・ちょうど区切り・上限のどれも起きる)、
    // CPU とビット一致で、1 つの Dispatch の形(wholeDigests。[規則][groupshared = 0・作業場 = 1])と同じハッシュになる
    std::expected<void, std::string> CompareSplit(const Context& context, const Shape& shape,
                                                  std::span<const PressureRule> rules,
                                                  const std::array<std::array<uint64_t, 2>, 4>& wholeDigests) {
        Context split = context;
        split.split = SplitMode::Split;
        split.splitSweeps = 7;
        for (size_t r = 0; r < rules.size(); ++r) {
            for (const bool sharedPotential : {true, false}) {
                const auto compared = RunCompare(split, shape, sharedPotential, rules[r]);
                if (!compared)
                    return std::unexpected(
                        std::format("分けた形・{}・{}: {}", shape.name, rules[r].name, compared.error()));

                const uint64_t whole = wholeDigests[r][sharedPotential ? 0 : 1];
                if (*compared != whole)
                    return std::unexpected(
                        std::format("分けた形・{}・{}: ハッシュが 1 つの Dispatch の形と違う({:016x} / {:016x})",
                                    shape.name, rules[r].name, *compared, whole));
            }
        }

        // 1 つのセルに物が 3 個(集めて書く段と一緒に)
        const CompareScene scene{.copies = 7, .objectsPerCell = 3, .vary = true};
        Context wholeContext = context;
        wholeContext.split = SplitMode::Whole;
        const PressureRule rule{"0 から収まるまで", 1024, 1};
        const auto whole = RunCompare(wholeContext, shape, true, rule, scene);
        const auto parts = RunCompare(split, shape, true, rule, scene);
        if (!whole || !parts)
            return std::unexpected(std::format("分けた形・3 個で 1 セル: {}", !whole ? whole.error() : parts.error()));

        if (*whole != *parts)
            return std::unexpected(
                std::format("分けた形・3 個で 1 セル: ハッシュが違う({:016x} / {:016x})", *whole, *parts));

        return {};
    }

    // 大きい箱(T-1350): 紙 96 × 48・層 8 段(121,856 ボクセル)を 0 から決まった 1,024 回(上限)で 2 刻み。1 つの Dispatch の形なら
    // 1 刻み約 3.5 s で TDR に当たる箱(T-1349)。自動で段ごとの Dispatch になり、CPU とビット一致・一番長い Dispatch が 2 s より十分短い
    std::expected<void, std::string> CompareLargeBox(const Context& context) {
        const Shape shape{"紙 96 × 48・層 8 段", 96, 48, 8};
        const PressureRule rule{"0 から決まった 1,024 回", 1024, 0};
        const CompareScene scene{
            .copies = 1, .objectsPerCell = 1, .vary = false, .ticks = 2, .every = 1, .maxDispatchMs = 1000.0};
        Context automatic = context;
        automatic.split = SplitMode::Auto;
        const auto compared = RunCompare(automatic, shape, false, rule, scene);
        if (!compared)
            return std::unexpected(std::format("{}: {}", shape.name, compared.error()));

        return {};
    }

    // 戻り値 = 1 刻みの平均の GPU の ms。stageMask を全部でなくすると暖機の後だけ段を外す(結果は変わるので数え上げは見ない・書き出さない)
    std::expected<double, std::string> Measure(const Context& context, const Shape& shape, uint32_t copies,
                                               bool sharedPotential, const PressureRule& rule,
                                               uint32_t stageMask = ALL_STAGES, uint32_t objectsPerCell = 1) {
        const auto one = MakePaper(context.table, context.shared, context.tables.gases, shape);
        if (!one)
            return std::unexpected("層を作れない");

        const test::OhWorld world = Replicate(*one, copies, objectsPerCell);
        auto queue = gpu::ImmediateQueue::Create(context.device, context.queueType);
        auto debugRing = gpu::DebugRing::Create(context.device);
        auto gpuState = CreateGpu(context.device, context.table, context.tables, world, sharedPotential,
                                  context.shader);
        if (!queue || !debugRing || !gpuState || !Upload(*queue, *gpuState))
            return std::unexpected(gpuState ? "GPU の準備に失敗" : gpuState.error());

        gpuState->pressureSweeps = rule.sweeps;
        gpuState->pressureMode = rule.mode;
        gpuState->split = context.split;
        gpuState->splitSweeps = context.splitSweeps;
        const size_t statsCount = StatsCount(world.objects.size());
        const size_t base = 2 + (2 * world.objects.size());
        const auto warm = RunTicks(*queue, *gpuState, *debugRing, 2, context.warmTicks);
        std::expected<std::vector<uint32_t>, std::string> before = std::unexpected(std::string("暖機に失敗"));
        if (warm)
            before = test::object_heat_gpu::ReadUav<uint32_t>(context.device, *queue, gpuState->uavs[UavStats].Get(),
                                                              statsCount);

        gpuState->stageMask = stageMask;
        const auto measured = before
                                  ? RunTicks(*queue, *gpuState, *debugRing, 2 + context.warmTicks, context.measureTicks)
                                  : warm;
        if (!measured)
            return std::unexpected(measured.error());

        if (stageMask != ALL_STAGES)
            return *measured;

        const auto stats = test::object_heat_gpu::ReadUav<uint32_t>(context.device, *queue,
                                                                    gpuState->uavs[UavStats].Get(), statsCount);
        if (!before || !stats || (*stats)[0] != 0 || (*stats)[1] != 0)
            return std::unexpected(std::format("{}: 量が負・枠があふれた・圧力の値が 32bit に入らない", shape.name));

        // 赤黒の回数(測った 20 刻みと、暖機の 30 刻みの一番多い回数)
        const uint32_t solves = (*stats)[base] - (*before)[base];
        const uint32_t sweeps = (*stats)[base + 1] - (*before)[base + 1];
        const uint32_t cappedMeasured = (*stats)[base + 3] - (*before)[base + 3];
        Log(Channel::Gpu, Level::Info,
            "測る: {}({} ボクセル・面 {})× {}・圧力の値は{}・{}: 1 刻み {:.3f} ms・赤黒 平均 {:.1f} 回"
            "(暖機からの一番多い {}・上限で止まった {}・測った {} 回の解きのうち上限で止まった {})・{}"
            "(1 刻み {} 回・一番長い {:.3f} ms)",
            shape.name, one->voxels.size(), gpuState->faceCount / copies, copies,
            sharedPotential ? " groupshared" : "作業場", rule.name, *measured,
            static_cast<double>(sweeps) / static_cast<double>(std::max<uint32_t>(solves, 1)), (*stats)[base + 2],
            (*stats)[base + 3], solves, cappedMeasured, SplitName(*gpuState), gpuState->dispatchesPerTick,
            gpuState->longestDispatchMs);

        return *measured;
    }

    // 段ごとの費用(T-1223): 段を 1 つずつだけ回した時間 − 段なしの時間。圧力は決まった回数(--sweeps)・groupshared
    std::expected<void, std::string> Profile(const Context& context) {
        const std::array<Shape, 3> shapes = {Shape{"紙 24 列・層 2 段", 24, 3, 2}, Shape{"紙 24 列・層 4 段", 24, 3, 4},
                                             Shape{"紙 24 列・層 6 段", 24, 3, 6}};
        const PressureRule rule{"決まった回数", context.measureSweeps, 0};
        for (const Shape& shape : shapes) {
            const uint32_t voxels = (shape.columns + (2 * shape.layers)) * (1 + (2 * shape.layers)) *
                                    (shape.sizeZ + (2 * shape.layers));
            for (const uint32_t copies : {1u, 4u, 16u, SM_COUNT}) {
                const auto all = Measure(context, shape, copies, true, rule, (1u << STAGE_NAMES.size()) - 1);
                const auto none = Measure(context, shape, copies, true, rule, 0);
                const auto withoutPressure = Measure(context, shape, copies, true, rule, ALL_STAGES & ~STAGE_PRESSURE);
                if (!all || !none || !withoutPressure)
                    return std::unexpected(!all ? all.error() : (!none ? none.error() : withoutPressure.error()));

                std::string line = std::format(
                    "段の費用: {}({} ボクセル)× {}・{}: 全部 {:.3f} ms・圧力なし {:.3f}・段なし {:.3f}", shape.name,
                    voxels, copies, context.shader, *all, *withoutPressure, *none);
                for (uint32_t stage = 0; stage < STAGE_NAMES.size(); ++stage) {
                    const auto only = Measure(context, shape, copies, true, rule, 1u << stage);
                    if (!only)
                        return std::unexpected(only.error());

                    line += std::format("・{} {:.3f}", STAGE_NAMES[stage], *only - *none);
                }

                Log(Channel::Gpu, Level::Info, "{}", line);
            }
        }

        return {};
    }

    // --- 規模の測り(T-1320 の段 0)---
    // 密な山の並べ方: 骨格(列 × 1 × 高さ)を x に列 + 2・y に 1 + 2・z に高さ + 2 ボクセルの間を空けて、
    // 1 辺 ⌈∛n⌉ 個の立方の並びに置く(紙の間 15.6 mm。層 2 段 = 15.6 mm・層 4 段 = 31 mm の層は隣と重なる)。
    // 戻り値 = { 箱の体積の和, 重なりを除いた体積 }(ボクセル)
    std::pair<uint64_t, uint64_t> HeapVolumes(const Shape& shape, uint32_t count) {
        uint32_t side = 1;
        while (side * side * side < count)
            side += 1;

        const std::array<uint32_t, 3> body = {shape.columns, 1u, shape.sizeZ};
        std::array<uint32_t, 3> extent{};
        for (uint32_t axis = 0; axis < 3; ++axis)
            extent[axis] = (side * (body[axis] + 2)) + (2 * shape.layers);

        std::vector<uint8_t> covered(static_cast<size_t>(extent[0]) * extent[1] * extent[2], 0);
        uint64_t sum = 0;
        for (uint32_t k = 0; k < count; ++k) {
            const std::array<uint32_t, 3> slot = {k % side, (k / side) % side, k / (side * side)};
            std::array<uint32_t, 3> low{};
            std::array<uint32_t, 3> size{};
            for (uint32_t axis = 0; axis < 3; ++axis) {
                low[axis] = slot[axis] * (body[axis] + 2);
                size[axis] = body[axis] + (2 * shape.layers);
            }

            sum += static_cast<uint64_t>(size[0]) * size[1] * size[2];
            for (uint32_t z = 0; z < size[2]; ++z) {
                for (uint32_t y = 0; y < size[1]; ++y) {
                    for (uint32_t x = 0; x < size[0]; ++x) {
                        const size_t index = (((static_cast<size_t>(low[2]) + z) * extent[1] + low[1] + y) *
                                              extent[0]) +
                                             low[0] + x;
                        covered[index] = 1;
                    }
                }
            }
        }

        uint64_t merged = 0;
        for (const uint8_t value : covered)
            merged += value;

        return {sum, merged};
    }

    std::expected<void, std::string> Scale(const Context& context) {
        const std::array<Shape, 2> shapes = {Shape{"紙 24 列・層 2 段", 24, 3, 2},
                                             Shape{"紙 24 列・層 4 段", 24, 3, 4}};
        const std::array<PressureRule, 2> rules = {PressureRule{"0 から収まるまで(既定)", 1024, 1},
                                                   PressureRule{"多重格子 + 収まるまで", 1024, 5}};
        for (const Shape& shape : shapes) {
            const uint64_t voxels = (shape.columns + (2 * shape.layers)) * (1 + (2 * shape.layers)) *
                                    (shape.sizeZ + (2 * shape.layers));
            for (const PressureRule& rule : rules) {
                for (const uint32_t copies : {1u, 10u, 100u, 1000u}) {
                    for (const uint32_t perCell : {1u, 8u}) {
                        if (perCell != 1 && copies == 1)
                            continue;

                        const auto measured = Measure(context, shape, copies, true, rule, ALL_STAGES, perCell);
                        if (!measured)
                            return std::unexpected(measured.error());

                        const auto [sum, merged] = HeapVolumes(shape, copies);
                        const double perObject = *measured / static_cast<double>(copies);
                        const double perVoxelNs = *measured * 1.0e6 / static_cast<double>(voxels * copies);
                        Log(Channel::Gpu, Level::Info,
                            "規模: {}・{}・{} 個・{}: 1 刻み {:.3f} ms・1 物 {:.4f} ms・1 ボクセル {:.1f} ns・"
                            "山の体積 和 {} / 重なりを除く {}({:.2f} 倍)",
                            shape.name, rule.name, copies, perCell == 1 ? "散らばり" : "密な山(8 個で 1 セル)",
                            *measured, perObject, perVoxelNs, sum, merged,
                            static_cast<double>(sum) / static_cast<double>(std::max<uint64_t>(merged, 1)));
                    }
                }
            }
        }

        return {};
    }

    // --- 箱の大きさと赤黒の回数(T-1331 の小さく試す 2。捨てる測り)---
    // 共有の空気の格子は 1 つの箱が大きい(山 1 つで 1 辺 100 ボクセル級)。0 から収まるまで・多重格子の赤黒の回数が箱の大きさでどう増えるかを、
    // 骨格を大きくした紙 1 枚(物 1 つ = 1 グループ・圧力の値は作業場)で見る。時間は SM 1 つの値なので比べには使わない(回数を見る)。
    struct GrowOptions {
        std::optional<Shape> shape;  // --grow-shape 列 高さ 層(1 つだけ測る)
        uint32_t fixedSweeps = 0;    // --grow-sweeps N(0 から決まった N 回だけで測る。0 = 既定の 2 つの規則)
        bool groupshared = false;    // --grow-groupshared(箱が groupshared に載る時だけ意味がある)
        uint32_t cpuTicks = 0;  // --grow-cpu N(GPU を使わず CPU で N 刻み回し、上限で止まった解きの中身を出す。T-1349)
    };

    // CPU で回して、上限で止まった解きの最後の赤黒を見る(T-1349 の ①: ゆっくり収まっているのか、整数の行き来で収まらないのか)
    std::expected<void, std::string> GrowCpu(const Context& context, const Shape& shape, const PressureRule& rule,
                                             uint32_t ticks) {
        auto one = MakePaper(context.table, context.shared, context.tables.gases, shape);
        if (!one)
            return std::unexpected("層を作れない");

        test::OhWorld world = std::move(*one);
        std::vector<test::OhShellFlow> flows(world.objects.size());
        for (uint32_t o = 0; o < flows.size(); ++o) {
            test::ResetShellFlow(world, o, flows[o]);
            ApplyRule(rule, flows[o]);
        }

        uint64_t reportedSolves = 0;
        uint64_t reportedCapped = 0;
        for (uint32_t t = 2; t < 2 + ticks; ++t) {
            if (!test::StepShellFlows(context.shared, context.tables.gases, world, flows, t))
                return std::unexpected(std::format("CPU の流れが崩れた(刻み {})", t));

            const test::OhShellFlow& flow = flows[0];
            if (flow.pressureCapped != reportedCapped || t + 1 == 2 + ticks) {
                Log(Channel::Gpu, Level::Info,
                    "CPU: {}({} ボクセル)・{}: 刻み {} まで 解き {}・赤黒 平均 {:.1f}・上限で止まった {}"
                    "(最後の赤黒で動いたボクセル 平均 {:.1f}・幅の最大 {}・2 回前と同じ値 {})",
                    shape.name, world.voxels.size(), rule.name, t, flow.pressureSolves,
                    static_cast<double>(flow.pressureSweepsUsed) /
                        static_cast<double>(std::max<uint64_t>(flow.pressureSolves, 1)),
                    flow.pressureCapped,
                    static_cast<double>(flow.cappedLastMoved) /
                        static_cast<double>(std::max<uint64_t>(flow.pressureCapped, 1)),
                    flow.cappedLastStep, flow.cappedCycle2);
                reportedCapped = flow.pressureCapped;
            }

            reportedSolves = flow.pressureSolves;
        }

        return reportedSolves != 0 ? std::expected<void, std::string>{} : std::unexpected("解きが 0 回");
    }

    std::expected<void, std::string> GrowOne(const Context& context, const Shape& shape, const GrowOptions& options) {
        const std::array<PressureRule, 2> rules = {PressureRule{"0 から収まるまで(既定)", 1024, 1},
                                                   PressureRule{"多重格子 + 収まるまで", 1024, 5}};
        if (options.fixedSweeps != 0) {
            const PressureRule fixed{"0 から決まった回数", options.fixedSweeps, 0};
            const auto measured = Measure(context, shape, 1, options.groupshared, fixed);
            return measured ? std::expected<void, std::string>{} : std::unexpected(measured.error());
        }

        for (const PressureRule& rule : rules) {
            const auto measured = Measure(context, shape, 1, options.groupshared, rule);
            if (!measured)
                return std::unexpected(measured.error());
        }

        return {};
    }

    std::expected<void, std::string> Grow(const Context& context, const GrowOptions& options) {
        if (options.cpuTicks != 0) {
            const Shape shape = options.shape.value_or(Shape{"紙 48 × 12・層 4 段", 48, 12, 4});
            for (const PressureRule& rule :
                 {PressureRule{"0 から収まるまで(既定)", 1024, 1}, PressureRule{"多重格子 + 収まるまで", 1024, 5}}) {
                if (auto ran = GrowCpu(context, shape, rule, options.cpuTicks); !ran)
                    return ran;
            }

            return {};
        }

        if (options.shape)
            return GrowOne(context, *options.shape, options);

        const std::array<Shape, 3> shapes = {Shape{"紙 24 × 3・層 4 段", 24, 3, 4},
                                             Shape{"紙 48 × 12・層 4 段", 48, 12, 4},
                                             Shape{"紙 96 × 24・層 4 段", 96, 24, 4}};
        // 紙 96 × 48・層 8 段(121,856 ボクセル)は GPU の実行に失敗した(原因は未確認。T-1331 の 5b)

        const std::array<PressureRule, 2> rules = {PressureRule{"0 から収まるまで(既定)", 1024, 1},
                                                   PressureRule{"多重格子 + 収まるまで", 1024, 5}};
        for (const Shape& shape : shapes) {
            for (const PressureRule& rule : rules) {
                const auto measured = Measure(context, shape, 1, false, rule);
                if (!measured)
                    return std::unexpected(measured.error());
            }
        }

        return {};
    }

    int Run(std::span<char*> arguments) {
        std::vector<char*> rest;
        bool measure = false;
        bool compare = true;
        bool profile = false;
        bool scale = false;
        bool grow = false;
        GrowOptions growOptions;
        uint32_t warmTicks = WARM_TICKS;
        uint32_t measureTicks = MEASURE_TICKS;
        uint32_t threads = 1024;
        uint32_t measureSweeps = 24;
        int32_t onlyRule = -1;
        bool largeBox = false;
        SplitMode split = SplitMode::Auto;
        uint32_t splitSweeps = 0;
        for (size_t i = 0; i < arguments.size(); ++i) {
            char* argument = arguments[i];
            const std::string_view text = argument;
            if (text == "--sweeps" && i + 1 < arguments.size()) {
                measureSweeps = static_cast<uint32_t>(std::strtoul(arguments[++i], nullptr, 10));
            } else if (text == "--rule" && i + 1 < arguments.size()) {
                onlyRule = static_cast<int32_t>(std::strtol(arguments[++i], nullptr, 10));
            } else if (text == "--threads" && i + 1 < arguments.size()) {
                threads = static_cast<uint32_t>(std::strtoul(arguments[++i], nullptr, 10));
            } else if (text == "--profile") {
                profile = true;
                compare = false;
            } else if (text == "--scale") {
                scale = true;
                compare = false;
            } else if (text == "--grow") {
                grow = true;
                compare = false;
            } else if (text == "--grow-shape" && i + 3 < arguments.size()) {
                const auto columns = static_cast<uint32_t>(std::strtoul(arguments[++i], nullptr, 10));
                const auto sizeZ = static_cast<uint32_t>(std::strtoul(arguments[++i], nullptr, 10));
                const auto layers = static_cast<uint32_t>(std::strtoul(arguments[++i], nullptr, 10));
                growOptions.shape = Shape{"指定の紙", columns, sizeZ, layers};
            } else if (text == "--grow-sweeps" && i + 1 < arguments.size()) {
                growOptions.fixedSweeps = static_cast<uint32_t>(std::strtoul(arguments[++i], nullptr, 10));
            } else if (text == "--grow-groupshared") {
                growOptions.groupshared = true;
            } else if (text == "--grow-cpu" && i + 1 < arguments.size()) {
                growOptions.cpuTicks = static_cast<uint32_t>(std::strtoul(arguments[++i], nullptr, 10));
            } else if (text == "--grow-ticks" && i + 2 < arguments.size()) {
                warmTicks = static_cast<uint32_t>(std::strtoul(arguments[++i], nullptr, 10));
                measureTicks = static_cast<uint32_t>(std::strtoul(arguments[++i], nullptr, 10));
            } else if (text == "--large-box") {
                largeBox = true;
                compare = false;
            } else if (text == "--split" && i + 1 < arguments.size()) {
                const std::string_view mode = arguments[++i];
                split = mode == "whole" ? SplitMode::Whole : (mode == "split" ? SplitMode::Split : SplitMode::Auto);
            } else if (text == "--split-sweeps" && i + 1 < arguments.size()) {
                splitSweeps = static_cast<uint32_t>(std::strtoul(arguments[++i], nullptr, 10));
            } else if (text == "--measure") {
                measure = true;
            } else if (text == "--measure-only") {
                measure = true;
                compare = false;
            } else {
                rest.push_back(argument);
            }
        }

        const auto options = test::ParseGpuTestOptions(rest);
        if (!options) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_object_shell_flow_test [--warp] [--queue direct|compute] [--measure | --measure-only]");
            return 2;
        }

        const sim::BakedReactionTable table = test::MakeSunTable();
        const auto tables = test::MakeObjectHeatTables(table, DEFAULT_DIFFUSIVITY_MM2_PER_S);
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!tables || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_object_shell_flow_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        const test::OhCpuTable shared{
            .view = table.View(), .objectSpecies = tables->species, .molarMasses = table.molarMasses};
        const Context context{.device = device->Get(),
                              .queueType = options->queueType,
                              .table = table,
                              .tables = *tables,
                              .shared = shared,
                              .measureSweeps = measureSweeps,
                              .shader = threads == 1024 ? std::string("sim/object_shell_flow.cso")
                                                        : std::format("sim/object_shell_flow_t{}.cso", threads),
                              .warmTicks = warmTicks,
                              .measureTicks = measureTicks,
                              .split = split,
                              .splitSweeps = splitSweeps};
        if (compare) {
            const std::array<Shape, 2> shapes = {Shape{"紙 24 列・層 2 段", 24, 3, 2},
                                                 Shape{"紙 24 列・層 4 段", 24, 3, 4}};
            const std::array<PressureRule, 4> rules = {
                PressureRule{"0 から決まった 24 回", 24, 0}, PressureRule{"0 から収まるまで", 1024, 1},
                PressureRule{"温めた始まり + 収まるまで", 1024, 3}, PressureRule{"多重格子 + 収まるまで", 1024, 5}};
            // 1 つの Dispatch の形のハッシュ(紙 24 列・層 2 段。分けた形と比べる。T-1350)
            std::array<std::array<uint64_t, 2>, 4> wholeDigests{};
            for (size_t r = 0; r < rules.size(); ++r) {
                for (size_t s = 0; s < shapes.size(); ++s) {
                    for (const bool sharedPotential : {true, false}) {
                        const auto compared = RunCompare(context, shapes[s], sharedPotential, rules[r]);
                        if (!compared) {
                            Log(Channel::Gpu, Level::Error, "gpu_object_shell_flow_test: FAILED({}・{}: {})",
                                shapes[s].name, rules[r].name, compared.error());
                            return 1;
                        }

                        if (s == 0)
                            wholeDigests[r][sharedPotential ? 0 : 1] = *compared;
                    }
                }
            }

            // 段ごとの Dispatch に分けた形(T-1350): 赤黒 7 回ずつで小刻みの途中で区切り、CPU とビット一致・1 つの Dispatch の形と同じハッシュ
            if (auto splitCompared = CompareSplit(context, shapes[0], rules, wholeDigests); !splitCompared) {
                Log(Channel::Gpu, Level::Error, "gpu_object_shell_flow_test: FAILED({})", splitCompared.error());
                return 1;
            }

            // 重みが 32bit に入らないボクセルの道(64bit の欄)も CPU と比べる(試験の印 8 = x が 3 の倍数のボクセルをその道へ。T-1320 段 1)
            const std::array<PressureRule, 2> wideRules = {
                PressureRule{"0 から収まるまで・64bit の欄の道も", 1024, 1 | 8},
                PressureRule{"多重格子 + 収まるまで・64bit の欄の道も", 1024, 5 | 8}};
            for (const PressureRule& rule : wideRules) {
                for (const bool sharedPotential : {true, false}) {
                    if (auto compared = RunCompare(context, shapes[0], sharedPotential, rule); !compared) {
                        Log(Channel::Gpu, Level::Error, "gpu_object_shell_flow_test: FAILED({}・{}: {})",
                            shapes[0].name, rule.name, compared.error());
                        return 1;
                    }
                }
            }

            if (auto sharedCells = CompareSharedCells(context); !sharedCells) {
                Log(Channel::Gpu, Level::Error, "gpu_object_shell_flow_test: FAILED({})", sharedCells.error());
                return 1;
            }
        }

        if (largeBox) {
            if (auto large = CompareLargeBox(context); !large) {
                Log(Channel::Gpu, Level::Error, "gpu_object_shell_flow_test: FAILED({})", large.error());
                return 1;
            }
        }

        if (grow) {
            if (auto grown = Grow(context, growOptions); !grown) {
                Log(Channel::Gpu, Level::Error, "gpu_object_shell_flow_test: FAILED({})", grown.error());
                return 1;
            }
        }

        if (profile) {
            if (auto profiled = Profile(context); !profiled) {
                Log(Channel::Gpu, Level::Error, "gpu_object_shell_flow_test: FAILED({})", profiled.error());
                return 1;
            }
        }

        if (scale) {
            if (auto scaled = Scale(context); !scaled) {
                Log(Channel::Gpu, Level::Error, "gpu_object_shell_flow_test: FAILED({})", scaled.error());
                return 1;
            }
        }

        if (measure) {
            const std::array<Shape, 4> shapes = {Shape{"紙 24 列・層 4 段", 24, 3, 4},
                                                 Shape{"紙 24 列・層 6 段", 24, 3, 6}, Shape{"A4・層 4 段", 27, 38, 4},
                                                 Shape{"A4・層 6 段", 27, 38, 6}};
            for (const Shape& shape : shapes) {
                const uint32_t outer = (shape.columns + (2 * shape.layers)) * (1 + (2 * shape.layers)) *
                                       (shape.sizeZ + (2 * shape.layers));
                const std::array<PressureRule, 4> rules = {
                    PressureRule{"0 から決まった回数", measureSweeps, 0}, PressureRule{"0 から収まるまで", 1024, 1},
                    PressureRule{"温めた始まり + 収まるまで", 1024, 3}, PressureRule{"多重格子 + 収まるまで", 1024, 5}};
                for (const PressureRule& rule : rules) {
                    if (onlyRule >= 0 && static_cast<uint32_t>(onlyRule) != rule.mode)
                        continue;

                    for (const uint32_t copies : {1u, SM_COUNT}) {
                        for (const bool sharedPotential : {true, false}) {
                            if (sharedPotential && outer > SHARED_VOXELS)
                                continue;

                            // 収まるまで解くと A4 の 46 個は 1 回の Dispatch が 1 秒を超えうる(TDR の 2 秒に近い)ので測らない
                            if (rule.mode != 0 && copies != 1 && outer > SHARED_VOXELS)
                                continue;

                            if (auto measured = Measure(context, shape, copies, sharedPotential, rule); !measured) {
                                Log(Channel::Gpu, Level::Error, "gpu_object_shell_flow_test: FAILED({})",
                                    measured.error());
                                return 1;
                            }
                        }
                    }
                }
            }
        }

        Log(Channel::Gpu, Level::Info, "gpu_object_shell_flow_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
