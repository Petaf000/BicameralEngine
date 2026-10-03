// frame_loop.h — 窓を開き、毎フレーム「コマンドを渡して投げて Present する」だけのループを回す(T-0004・T-0012、06 §4)。
//
// CPU は View と Controller(D-107): 窓の入力をコマンドにし、記録済みのリストを投げ、Present する。世界の状態には触れない。
//   - シミュは compute キュー、描画は direct キュー。キューの間の同期は GPU のフェンスだけ(Queue::GpuWait)。
//   - シミュは 1 フレームの予算ぶんの「単位」だけを投げ、その後ろに描画を投げる(ADR-0011)。刻みはフレームをまたいでよい。
//     単位の数は SimScheduler が、読み戻した単位ごとの GPU 時間と描画の GPU 時間から決める(重いと世界が遅くなる。D-202)。
//   - 描画のリストは作るときに 1 度だけ記録して使い回す。シミュのリストはフレームの枠ごとに毎フレーム記録する(sim/probe_sim)。
//   - GPU → CPU は待たない読み戻し(イベント・刻みごとの状態のハッシュ・デバッグの出力・タイムスタンプ)。フェンスが進んでいた分だけ読む。
//   - 窓の操作はコマンド(適用する刻みつき)になり、GPU のコマンドキューで自分の刻みまで待つ(06 §3)。
//     --record でコマンドの列と刻みごとのハッシュを再生ファイルに書き、--replay でそれを流して同じハッシュ列になるかを確かめる(15 §2)。
// 窓の入力は render/debug_view_controller がカメラ・表示の切り替え・つつくセル・トレースの依頼(T)に振り分ける(T-0015・T-0088)。
// カメラとトレースは世界に入らない。
// 今の世界は仮のもの(sim/probe_sim。クリックした所が熱くなって広がる)。中身は T-0005 以降で本物に。
#pragma once

#include <cstdint>
#include <filesystem>

#include "core/aliases.h"
#include "gpu/device.h"
#include "gpu/graph_trace.h"
#include "render/debug_view_controller.h"

namespace bicameral::frame {

    // 連鎖のトレースの容量(1 フレームに GPU が書ける記録。1 MiB)。範囲を実行中に変えるので、いつも確保しておく(T-0088)
    inline constexpr uint32_t TRACE_CAPACITY_PER_FRAME = 1u << 16;

    struct FrameLoopOptions {
        // --- フレームの進め方 ---
        uint32_t frameLimit = 0;         // 0 なら窓を閉じるまで。自動の確認(job.py run)では有限にする
        bool vsync = true;               // false なら待たずに Present(対応していれば tearing)
        uint32_t maxFrameLatency = 2;    // CPU が GPU より先に進めるフレームの数(2〜3)
        uint32_t targetFps = 60;         // 重いときに描画が保つ fps(ADR-0011。30 以上)
        bool renderHighPriority = true;  // 描画のキューの優先度を HIGH にする

        // --- 重さの試験(R-LOOP-2)---
        uint32_t simLoad = 0;   // 1 刻みに足す繰り返し(0 なら無し)
        uint32_t simSplit = 1;  // 何個の単位に分けるか

        // --- 入力・記録・再生 ---
        bool autoClick = false;   // 自動でクリックを入れる(人がいない自動の確認でイベントの流れを通す)
        bool autoPush = false;    // 決まったフレームで積み木を押す(人がいない確認で押すコマンドの流れを通す。T-0098)
        fs::path recordPath;      // 空でなければ、終わるときに再生ファイルを書く
        fs::path replayPath;      // 空でなければ、この再生ファイルのコマンドで進めてハッシュを突き合わせる
        fs::path screenshotPath;  // 空でなければ、最後のフレーム(frameLimit)を BMP に書く

        // --- 連鎖のトレース(T-0087・T-0088。frame/trace_capture.h)---
        // 空でなければ、起動時から trace の範囲を集め、範囲の刻みが終わったら(終わりが無ければ終わるときに)刻みごとの木にして書く
        fs::path tracePath;
        gpu::GraphTraceFilter
            trace;                // その範囲(場所の箱はブロックの座標。main が --trace-ticks・--trace-cells から作る)
        fs::path traceDirectory;  // 窓の T で集めたトレースを書くフォルダ(main の既定は exe の横の traces/)
        bool autoTrace = false;   // 決まったフレームで T を押す(人がいない確認で T の流れを通す)

        // --- 覗き窓(T-0096)---
        bool peek = false;                // 起動時から peekCell を覗く(窓では P)
        render::CellCoordinate peekCell;  // 覗く世界のセル
        uint32_t peekDepth = 0;           // 潜る段(カメラが寄る。0〜9)

        // --- 物理(T-0098)---
        bool physics = true;  // 仮の世界に積み木(sim::MakeProbeStackScene)を入れる。Shift + 左クリックで押す
        // CPU の物理(sim::PhysicsWorld)を並べて走らせ、刻みごとの物のハッシュを突き合わせる(--check-physics。調べる用)
        bool checkPhysics = false;
        bool physicsComputeBroadphase = false;  // 広域の選別を Compute で(--physics-compute。比べる用)

        // --- 表示と GPU ---
        render::DebugViewSettings view;   // 最初のデバッグ表示(T-0015)
        render::OrbitCameraState camera;  // 最初のカメラ
        gpu::AdapterKind adapter = gpu::AdapterKind::Hardware;
    };

    // 終了コード: 0 = 正常、1 = 失敗(作れない・デバイスの喪失・debug layer のエラー・再生のハッシュの不一致・記録を書けない)
    int RunFrameLoop(const FrameLoopOptions& options);

}  // namespace bicameral::frame
