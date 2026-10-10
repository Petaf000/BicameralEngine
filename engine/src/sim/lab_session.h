// lab_session.h — 実験室(14 §2・T-0142)の 1 回の実験: 同じ箱を GPU(sim/gpu_lab_box)と CPU リファレンス(sim/lab_box の StepLabBox)で
// 並べて刻み、刻みごとに状態の全部を比べる。食い違ったら最初の刻みとセルを残して止まる。エディタのパネル(editor/lab_panel)とテストが使う。
//
// データの流れ:
//   パネルの「置く」「温度」→ Place・SetTemperature(次の刻みのコマンドにする。CPU は箱に触れない)
//   → Step(n): 刻みごとに 1 本のリスト(コマンドを当てる → 刻む → 読み戻す)を自分のキューに投げて待つ → CPU も同じ刻み → 比べる
//   → Recording()(コマンドの列 + ハッシュの列)を保存 → Replay で初めの箱から流し直し、ハッシュの列が記録と同じかも確かめる。
// 反応表(T-0218・ADR-0055): ホットリロードで替わったら、次の刻みから新しい表で続ける(表を替えた印のコマンドを置く)。
//   印のある刻みは、コマンドを当てる前に GPU と CPU の両方の表を替え、印が箱をつつく。記録は刻み 0 の表の版 + 印の列。
//   今まで見た表は版ごとに持つので、途中で表を替えた記録もこの実験室の中なら再生できる。記録には表の中身(script::TableBytes)も残り
//   (T-0217)、別の起動では呼ぶ側が中身から表を作り直して AddTable で渡してから Replay する(editor/lab_panel)。
//   物質を足す・消す表(T-0242・ADR-0065): 印のある刻みに、GPU と CPU の箱の全部のセルを名前で付け替える(RemapLabBox。コマンドの前)。
//   付け替えは表 2 つだけから決まるので、記録は今までどおり印の列と表の中身だけ(再生は同じ付け替えを作り直す)。
// 計器(T-0221): 刻むたびに読み戻した GPU の箱から見るセルと箱全体の値を取る(sim/lab_gauge。世界に何も返さない)。
// 箱は小さい(512 セル)ので、1 刻みずつ待つ(エディタの道具。ゲームの世界のフレームの歩調〔ADR-0011〕とは別のキュー)。
#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gpu/debug_ring.h"
#include "gpu/immediate_queue.h"
#include "sim/gpu_lab_box.h"
#include "sim/lab_box.h"
#include "sim/lab_gauge.h"

namespace bicameral::sim {

    class LabSession {
    public:
        // table は写して持つ(呼ぶ側の表が消えてもよい)。tableVersion は記録に残す表の版(0 = 分からない)。
        // tableBytes は表の中身(script::TableBytes。記録に残す。空 = 分からない〔試験の表〕。T-0217)
        [[nodiscard]] static std::expected<LabSession, std::string> Create(ID3D12Device5* device,
                                                                           D3D12_COMMAND_LIST_TYPE queueType,
                                                                           const BakedReactionTable& table,
                                                                           uint64_t tableVersion = 0,
                                                                           std::string tableBytes = {});

        // 反応表を替える(ホットリロード。T-0218・ADR-0055): 次の刻み NextTick() から新しい表で続ける(表を替えた印を置く)。
        // 刻む前にもう一度替えたら、印は最後の表の 1 つだけ(今の表に戻したなら印を消す)。tableVersion は 0 でないこと(記録の印が表を指す)。
        // 物質の一覧が違う表は、今の表から名前で付け替えられること(sim::BuildSpeciesRemap が通る。T-0242)。
        // まだ刻んでいない置く操作の材料も新しい表の ID に付け替える(消えた物質の材料は落とす)
        [[nodiscard]] std::expected<void, std::string> ChangeTable(const BakedReactionTable& table,
                                                                   uint64_t tableVersion, std::string tableBytes = {});

        // 記録を再生するための表を足す(T-0217。記録の中身から作り直した表。最新の表は変えない)。物質の一覧は違ってよい
        // (表から表へ付け替えられるかは Replay が流す前に確かめる。T-0242)
        [[nodiscard]] std::expected<void, std::string> AddTable(const BakedReactionTable& table, uint64_t tableVersion,
                                                                std::string tableBytes);
        [[nodiscard]] bool HasTable(uint64_t tableVersion) const { return m_tables.contains(tableVersion); }

        // 今までの操作(表を替えた印を除く)を、初めの箱から最新の表で同じ刻みまで流し直す(T-0194 の案 A。同じ置き方で法則だけ比べる)。
        // 置いてまだ刻んでいない操作は残る。物質の一覧が違う表で置いた材料は名前で最新の表の ID にする(消えた物質の材料は落とす。T-0242)
        [[nodiscard]] std::expected<void, std::string> RerunWithLatestTable();

        // --- 置く(次の刻み NextTick() のコマンドにする。1 刻みに LAB_MAX_COMMANDS_PER_TICK まで)---
        bool Place(LabCellPosition cell, std::span<const SpeciesAmount> contents, uint32_t temperatureMilliKelvin);
        bool SetTemperature(LabCellPosition cell, uint32_t temperatureMilliKelvin);

        // 範囲のセル全部(T-0220。1 つのコマンド)
        bool PlaceRegion(LabCellRange range, std::span<const SpeciesAmount> contents, uint32_t temperatureMilliKelvin);
        bool SetTemperatureRegion(LabCellRange range, uint32_t temperatureMilliKelvin);

        // tickCount 刻み進める。食い違ったらその刻みで止まり(Mismatch())、以後は Reset まで進めない。GPU の失敗はエラー
        [[nodiscard]] std::expected<void, std::string> Step(uint32_t tickCount);

        // 初めの箱(300 K の空気)に戻し、記録を空にする。表は最新の表(ChangeTable で最後に渡したもの)
        [[nodiscard]] std::expected<void, std::string> Reset();

        // 初めの箱から記録のコマンドを流し直す(記録の刻みの数まで)。記録のハッシュと違った最初の刻みは ReplayDivergence()。
        // 刻み 0 の表は記録の版の表(0 なら最新の表)。記録の表(刻み 0 と印)にこの実験室が持たない版があれば流さずにエラー。
        // 流し終えて最新の表と違えば、次の刻みから最新の表に替える印を置く(実験室の表は世界と同じ。ADR-0054 の 3)
        [[nodiscard]] std::expected<void, std::string> Replay(const LabRecording& recording);

        // --- 見る ---
        [[nodiscard]] uint64_t NextTick() const { return m_tick; }
        [[nodiscard]] const MultiresNest& Cpu() const { return m_cpu; }
        [[nodiscard]] const MultiresNest& Gpu() const { return m_read; }
        [[nodiscard]] const BakedReactionTable& Table() const { return m_table; }
        [[nodiscard]] uint64_t TableVersion() const { return m_tableVersion; }  // 次の刻みの前の箱の表
        [[nodiscard]] uint64_t LatestTableVersion() const { return m_latestVersion; }
        [[nodiscard]] bool TableChangePending() const;
        [[nodiscard]] const std::optional<LabMismatch>& Mismatch() const { return m_mismatch; }
        [[nodiscard]] std::optional<uint64_t> ReplayDivergence() const { return m_replayDivergence; }
        [[nodiscard]] bool Replaying() const { return m_replayEnd > m_tick; }
        [[nodiscard]] size_t PendingCommands() const { return m_pending.size(); }
        [[nodiscard]] LabRecording Recording() const;
        // 最後に物質を付け替えた刻みの報告(CPU の箱の全部のセルの合計。T-0242)。付け替えていなければ空
        [[nodiscard]] const std::optional<SpeciesRemapReport>& LastRemapReport() const { return m_lastRemapReport; }

        // --- 計器(T-0221)---
        // 見るセルを替える(今までの値は捨てる: 前のセルの値と混ぜない)
        void SetGaugeCell(uint32_t cellIndex);
        [[nodiscard]] uint32_t GaugeCell() const { return m_gaugeCell; }
        [[nodiscard]] const std::vector<LabGaugeSample>& Gauge() const { return m_gauge; }

    private:
        LabSession(BakedReactionTable table, gpu::ImmediateQueue queue, gpu::DebugRing ring, GpuLabBox box);

        bool Queue(const Command& command);
        void DropPendingTableChange();
        [[nodiscard]] std::vector<Command> TakeCommands(uint64_t tick);
        // remapBox: 物質の一覧が違えば次の刻みで箱を付け替える(StepOne の印。ResetTo は箱を作り直すので要らない)
        [[nodiscard]] std::expected<void, std::string> SwitchTable(uint64_t tableVersion, bool remapBox);
        [[nodiscard]] std::expected<void, std::string> ResetTo(uint64_t tableVersion);
        [[nodiscard]] std::expected<void, std::string> StepOne();
        void RecordGauge(uint64_t tick);
        [[nodiscard]] std::expected<void, std::string> CheckTableChain(std::span<const uint64_t> versions) const;

        // --- 反応表 ---
        BakedReactionTable m_table;                       // 箱が今使っている表(GPU の箱と同じ)
        uint64_t m_tableVersion = 0;                      // m_table の版
        uint64_t m_latestVersion = 0;                     // 最新の表(世界の表。Create・ChangeTable)
        uint64_t m_initialVersion = 0;                    // この実験の刻み 0 の表(記録に残す)
        std::map<uint64_t, BakedReactionTable> m_tables;  // 今まで見た表(版ごと。再生で使う)
        std::map<uint64_t, std::string> m_tableBytes;     // その中身(分かるものだけ。記録に残す。T-0217)
        std::optional<SpeciesRemap> m_boxRemap;           // 次の刻みで箱に当てる付け替え(SwitchTable が作る。T-0242)
        std::optional<SpeciesRemapReport> m_lastRemapReport;
        gpu::ImmediateQueue m_queue;
        gpu::DebugRing m_ring;
        GpuLabBox m_box;
        uint32_t
            m_ledgerColumns = 0;  // GPU の箱を作った時の帳簿の列(物質の数の違う表で作り直す箱もこれに合わせる。T-0242)

        // --- 箱 ---
        MultiresNest m_initial;  // 初めの箱(刻み 0 の表で作る。刻み 0 に表を替えても GPU に写すのはこれ)
        MultiresNest m_cpu;      // CPU リファレンス
        MultiresNest m_read;     // GPU から読み戻した箱
        bool m_uploaded = false;
        uint64_t m_tick = 0;  // 次に刻む刻み
        uint32_t m_sequence = 0;

        // --- コマンド ---
        std::vector<Command> m_pending;    // 置いたが、まだ刻んでいない
        std::vector<Command> m_scheduled;  // 再生中の記録のうち、まだ刻んでいない
        std::vector<Command> m_history;    // 刻んだ(記録に入る)

        // --- 比べた結果 ---
        std::vector<uint64_t> m_hashes;  // 刻みごとの CPU の HashWholeNest
        std::optional<LabMismatch> m_mismatch;
        std::vector<uint64_t> m_replayHashes;
        uint64_t m_replayEnd = 0;
        std::optional<uint64_t> m_replayDivergence;

        // --- 計器(読み戻した GPU の箱の値。刻みの順)---
        uint32_t m_gaugeCell = multires::MrCellIndex(3, 3, 3);
        std::vector<LabGaugeSample> m_gauge;
    };

}  // namespace bicameral::sim
