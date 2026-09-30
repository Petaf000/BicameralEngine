// frame_loop.h — 窓を開き、毎フレーム「コマンドを渡して投げて Present する」だけのループを回す(T-0004・T-0012、06 §4)。
//
// CPU は View と Controller(D-107): 窓の入力をコマンドにし、記録済みのリストを投げ、Present する。世界の状態には触れない。
//   - シミュは compute キュー、描画は direct キュー。キューの間の同期は GPU のフェンスだけ(Queue::GpuWait)。
//   - シミュは 1 フレームの予算ぶんの「単位」だけを投げ、その後ろに描画を投げる(ADR-0011)。刻みはフレームをまたいでよい。
//     単位の数は SimScheduler が、読み戻した単位ごとの GPU 時間と描画の GPU 時間から決める(重いと世界が遅くなる。D-202)。
//   - 描画のリストは作るときに 1 度だけ記録して使い回す。シミュのリストはフレームの枠ごとに毎フレーム記録する(sim/probe_sim)。
//   - GPU → CPU は待たない読み戻し(イベント・刻みごとの状態のハッシュ・デバッグの出力・タイムスタンプ)。フェンスが進んでいた分だけ読む。
// 今の世界は仮のもの(sim/probe_sim。クリックした所が熱くなって広がる)。中身は T-0005 以降で本物に。
#pragma once

#include <cstdint>

#include "gpu/device.h"

namespace bicameral::frame {

    struct FrameLoopOptions {
        uint32_t frameLimit = 0;         // 0 なら窓を閉じるまで。自動の確認(job.py run)では有限にする
        bool vsync = true;               // false なら待たずに Present(対応していれば tearing)
        uint32_t maxFrameLatency = 2;    // CPU が GPU より先に進めるフレームの数(2〜3)
        uint32_t targetFps = 60;         // 重いときに描画が保つ fps(ADR-0011。30 以上)
        uint32_t simLoad = 0;            // 重さの試験: 1 刻みに足す繰り返し(0 なら無し)
        uint32_t simSplit = 1;           // 重さの試験を何個の単位に分けるか(R-LOOP-2)
        bool renderHighPriority = true;  // 描画のキューの優先度を HIGH にする
        bool autoClick = false;          // 自動でクリックを入れる(人がいない自動の確認でイベントの流れを通す)
        gpu::AdapterKind adapter = gpu::AdapterKind::Hardware;
    };

    // 終了コード: 0 = 正常、1 = 失敗(作れない・デバイスの喪失・debug layer のエラー)
    int RunFrameLoop(const FrameLoopOptions& options);

}  // namespace bicameral::frame
