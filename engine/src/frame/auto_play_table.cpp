// auto_play_table.cpp — --auto-play-table の手順と判定(T-0548・ADR-0246)。手順は auto_play.h の「表の手順」。
// 表の差し替え(ファイル・パネル・ミキサーと同じホットリロードの道)を編集として扱えているかを、刻みの番号・世界が使う表の版・
// プレイのハッシュ列で確かめる。
#include <format>

#include "core/log.h"
#include "frame/auto_play.h"

namespace bicameral::frame {

    AutoPlayAction AutoPlay::UpdateTable(const AutoPlayState& state) {
        if (state.editing)
            return UpdateTableEdit(state);

        return UpdateTablePlay(state);
    }

    // --- 編集の状態(置く・表を書き換える・元に戻す・やり直す・やめた後)---

    AutoPlayAction AutoPlay::UpdateTableEdit(const AutoPlayState& state) {
        const bool settled = EditSettled(state, 1, true);
        switch (m_tableStep) {
            case TableStep::Hold:
                if (state.cursor != SimCursor{}) {
                    Fail(std::format("起動が止まっていない(刻み {} 単位 {})", state.cursor.tick, state.cursor.unit));
                    return AutoPlayAction::None;
                }

                return state.frame >= HOLD_FRAMES ? ActTable(TableStep::PlaceA, AutoPlayAction::StrokeA, state.frame)
                                                  : AutoPlayAction::None;

            case TableStep::PlaceA:
                if (!EditSettled(state, 1, false))
                    return AutoPlayAction::None;

                m_baseVersion = state.tableVersion;
                m_recording = 1;
                m_plays[1].clear();
                return ActTable(TableStep::Play1, AutoPlayAction::Play, state.frame);

            case TableStep::Stopped1:
                if (!settled || !StayedStill(state))
                    return AutoPlayAction::None;

                return ActTable(TableStep::Edited, AutoPlayAction::EditTable, state.frame);

            case TableStep::Edited:  // 書き換えた表が、時間を進めずに刻み 0 の始めに当たった(S(0) へ戻して当て直した)
                if (!settled || state.tableVersion == m_baseVersion || !StayedStill(state))
                    return AutoPlayAction::None;

                m_editedVersion = state.tableVersion;
                return ActTable(TableStep::UndoA, AutoPlayAction::Undo, state.frame);

            case TableStep::UndoA:  // A が無くなっても、表の印が残るので (0, 1)。表は T1 のまま
            case TableStep::RedoA:
            case TableStep::Stopped2:
                if (!settled)
                    return AutoPlayAction::None;

                if (state.tableVersion != m_editedVersion) {
                    Fail(std::format("元に戻す・やり直す・やめた後の表が {:016x}(期待は書き換えた表 {:016x})",
                                     state.tableVersion, m_editedVersion));
                    return AutoPlayAction::None;
                }

                if (m_tableStep == TableStep::UndoA)
                    return ActTable(TableStep::RedoA, AutoPlayAction::Redo, state.frame);

                m_recording = m_tableStep == TableStep::RedoA ? 2 : 3;
                m_plays[m_recording].clear();
                return ActTable(m_tableStep == TableStep::RedoA ? TableStep::Play2 : TableStep::Play3,
                                AutoPlayAction::Play, state.frame);

            case TableStep::Stopped3:  // プレイ中に表を元に戻してからやめた: 表はファイルの今の中身(T0)
                if (!settled)
                    return AutoPlayAction::None;

                if (state.tableVersion != m_baseVersion) {
                    Fail(std::format("プレイ中に表を元に戻してやめた後の表が {:016x}(期待はファイルの今の中身 {:016x})",
                                     state.tableVersion, m_baseVersion));
                    return AutoPlayAction::None;
                }

                return StayedStill(state) ? ActTable(TableStep::EditedAgain, AutoPlayAction::EditTable, state.frame)
                                          : AutoPlayAction::None;

            case TableStep::EditedAgain:
                if (!settled || state.tableVersion != m_editedVersion)
                    return AutoPlayAction::None;

                m_recording = 4;
                m_plays[4].clear();
                return ActTable(TableStep::Play4, AutoPlayAction::Play, state.frame);

            default: return AutoPlayAction::None;  // やめるが届くのを待つ
        }
    }

    // --- プレイ(比べる・プレイ中に表を元に戻す・やめる)---

    AutoPlayAction AutoPlay::UpdateTablePlay(const AutoPlayState& state) {
        const std::optional<uint64_t> elapsed = PlayElapsed(state);
        if (!elapsed)
            return AutoPlayAction::None;

        switch (m_tableStep) {
            case TableStep::Play1:
                return *elapsed >= RUN_TICKS ? ActTable(TableStep::Stopped1, AutoPlayAction::Stop, state.frame)
                                             : AutoPlayAction::None;

            case TableStep::Play2: {
                if (*elapsed < RUN_TICKS + 10)
                    return AutoPlayAction::None;

                const auto [same, differ] = Compare(1, 2, m_editTick + 1, m_editTick + RUN_TICKS);
                if (differ == 0) {
                    Fail(std::format("表を書き換えたプレイ 2 がプレイ 1 と違わない(同じ {} 刻み)", same));
                    return AutoPlayAction::None;
                }

                return ActTable(TableStep::Stopped2, AutoPlayAction::Stop, state.frame);
            }

            case TableStep::Play3:
                return *elapsed >= REVERT_AFTER
                           ? ActTable(TableStep::Play3Reverted, AutoPlayAction::RevertTable, state.frame)
                           : AutoPlayAction::None;

            case TableStep::Play3Reverted: {
                if (*elapsed < RUN_TICKS + 10 || state.tableVersion != m_baseVersion)
                    return AutoPlayAction::None;  // プレイ中の差し替えが当たるのを待つ

                const auto [same, differ] = Compare(2, 3, m_editTick + 1, m_editTick + REVERT_AFTER);
                if (differ != 0 || same < REVERT_AFTER / 2) {
                    Fail(std::format("やめて戻ったプレイ 3 が、元に戻す前までプレイ 2 と同じでない(同じ {}・違う {})",
                                     same, differ));
                    return AutoPlayAction::None;
                }

                return ActTable(TableStep::Stopped3, AutoPlayAction::Stop, state.frame);
            }

            case TableStep::Play4: {
                if (*elapsed < RUN_TICKS + 10)
                    return AutoPlayAction::None;

                const auto [same, differ] = Compare(2, 4, m_editTick + 1, m_editTick + RUN_TICKS);
                if (differ != 0 || same < RUN_TICKS / 2) {
                    Fail(std::format("もう一度書き換えたプレイ 4 がプレイ 2 と同じでない(同じ {}・違う {})", same,
                                     differ));
                    return AutoPlayAction::None;
                }

                m_summary = std::format(
                    "表 {:016x} → {:016x}: 編集の状態の書き換えは刻み {} のまま当たり、元に戻す・やり直すでも表は残り、"
                    "書き換えたプレイは違う・やめて戻ったプレイは同じ・プレイ中に元に戻してやめると表はファイルの中身"
                    "・もう一度書き換えたプレイは {} 刻みがプレイ 2 と同じ",
                    m_baseVersion, m_editedVersion, m_editTick, same);
                m_step = Step::Done;
                return AutoPlayAction::None;
            }

            default: return AutoPlayAction::None;  // 編集の状態へ戻るのを待つ
        }
    }

    // --- 補助 ---

    std::optional<uint64_t> AutoPlay::PlayElapsed(const AutoPlayState& state) {
        if (state.editing || state.playStartTick == UINT64_MAX)
            return std::nullopt;

        if (state.playStartTick != m_editTick) {
            Fail(std::format("プレイを始めた刻みが {}(期待は編集の境界 {}。編集の状態で時間が進んだ)",
                             state.playStartTick, m_editTick));
            return std::nullopt;
        }

        return state.cursor.tick - m_editTick;
    }

    AutoPlayAction AutoPlay::ActTable(TableStep next, AutoPlayAction action, uint64_t frame) {
        m_tableStep = next;
        m_actionFrame = frame;
        m_rewindsAtAction = m_rewinds;

        return action;
    }

    void AutoPlay::WriteTable(bool edited) {
        if (const auto written = WritePackageEdits(m_tableEdits, edited); !written)
            Fail(written.error());
    }

}  // namespace bicameral::frame
