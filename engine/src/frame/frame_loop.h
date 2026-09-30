// frame_loop.h — 窓を開き、毎フレーム「コマンドを渡して投げて Present する」だけのループを回す(T-0004、06 §4)。
//
// CPU は View と Controller(D-107): 窓の入力をコマンドにし、記録済みのリストを投げ、Present する。世界の状態には触れない。
//   - シミュは compute キュー、描画は direct キュー。キューの間の同期は GPU のフェンスだけ(Queue::GpuWait)。
//   - コマンドリストは作るときに 1 度だけ記録し、毎フレーム使い回す(CPU はアップロードのバッファに値を書くだけ)。
//   - GPU → CPU は待たない読み戻し(イベント・デバッグの出力・タイムスタンプ)。フェンスが進んでいた分だけ読む。
//   - 1 フレームに投げる刻みの数は TickPacer が、読み戻したタイムスタンプから決める(重いと世界が遅くなる。D-202)。
// 今の世界は仮のもの(sim/probe_sim。クリックした所が熱くなって広がる)。本物の刻みは T-0012 から。
#pragma once

#include <cstdint>

#include "gpu/device.h"

namespace bicameral::frame {

    struct FrameLoopOptions {
        uint32_t frameLimit = 0;       // 0 なら窓を閉じるまで。自動の確認(job.py run)では有限にする
        bool vsync = true;             // false なら待たずに Present(対応していれば tearing)
        uint32_t maxFrameLatency = 2;  // CPU が GPU より先に進めるフレームの数(2〜3)
        uint32_t simLoad = 0;          // 重さの試験: 刻みの拡散に足す繰り返し(0 なら無し)
        bool autoClick = false;        // 自動でクリックを入れる(人がいない自動の確認でイベントの流れを通す)
        gpu::AdapterKind adapter = gpu::AdapterKind::Hardware;
    };

    // 終了コード: 0 = 正常、1 = 失敗(作れない・デバイスの喪失・debug layer のエラー)
    int RunFrameLoop(const FrameLoopOptions& options);

}  // namespace bicameral::frame
