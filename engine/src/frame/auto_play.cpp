// auto_play.cpp — --auto-play の手順と判定(T-0497・T-0509)。手順は auto_play.h。
#include "frame/auto_play.h"

#include <format>

#include "core/log.h"

namespace bicameral::frame {

    AutoPlayAction AutoPlay::Update(const AutoPlayState& state) {
        if (Finished())
            return AutoPlayAction::None;

        if (m_step <= Step::EditStill)
            return UpdateEdit(state);

        if (m_step == Step::Playing || m_step == Step::PlayingAfterC || m_step == Step::PlayingUndone ||
            m_step == Step::Replaying)
            return UpdatePlay(state);

        return UpdateAfterStop(state);
    }

    void AutoPlay::NoteHash(uint64_t tick, uint64_t worldHash) {
        if (m_recording != 0 && tick > m_editTick)
            m_plays[m_recording][tick] = worldHash;
    }

    // --- 起動から最初のプレイまで(止まっている・時間を進めずに置く)---

    AutoPlayAction AutoPlay::UpdateEdit(const AutoPlayState& state) {
        if (!state.editing) {
            Fail(std::format("フレーム {} で編集の状態でない", state.frame));
            return AutoPlayAction::None;
        }

        switch (m_step) {
            case Step::Hold:
                if (state.cursor != SimCursor{}) {
                    Fail(std::format("起動が止まっていない(フレーム {} で刻み {} 単位 {})", state.frame,
                                     state.cursor.tick, state.cursor.unit));
                    return AutoPlayAction::None;
                }

                return state.frame >= HOLD_FRAMES ? Act(Step::PlaceA, AutoPlayAction::StrokeA, state.frame)
                                                  : AutoPlayAction::None;

            case Step::PlaceA:
                return EditSettled(state, 1, false) ? Act(Step::PlaceB, AutoPlayAction::StrokeB, state.frame)
                                                    : AutoPlayAction::None;

            case Step::PlaceB:
                return EditSettled(state, 1, true) ? Act(Step::EditStill, AutoPlayAction::None, state.frame)
                                                   : AutoPlayAction::None;

            case Step::EditStill:
                return StayedStill(state) ? StartPlay(Step::Playing, 1, state.frame) : AutoPlayAction::None;

            default: return AutoPlayAction::None;
        }
    }

    // --- プレイ(プレイ中に置く・やめる・比べる)---

    AutoPlayAction AutoPlay::UpdatePlay(const AutoPlayState& state) {
        if (state.editing || state.playStartTick == UINT64_MAX)
            return AutoPlayAction::None;

        if (state.playStartTick != m_editTick) {
            Fail(std::format("プレイを始めた刻みが {}(期待は編集の境界 {}。編集の状態で時間が進んだ)",
                             state.playStartTick, m_editTick));
            return AutoPlayAction::None;
        }

        const uint64_t elapsed = state.cursor.tick - m_editTick;
        switch (m_step) {
            case Step::Playing:
                return elapsed >= STROKE_C_AFTER ? Act(Step::PlayingAfterC, AutoPlayAction::StrokeC, state.frame)
                                                 : AutoPlayAction::None;

            case Step::PlayingAfterC:
                return elapsed >= STOP_AFTER ? Act(Step::Stopped, AutoPlayAction::Stop, state.frame)
                                             : AutoPlayAction::None;

            case Step::PlayingUndone: {
                if (elapsed < SAME_TICKS + 10)
                    return AutoPlayAction::None;

                const auto [same, differ] = CompareWithFirst(2, m_editTick + 1, m_editTick + SAME_TICKS);
                if (differ == 0 || same + differ < SAME_TICKS / 2) {
                    Fail(std::format("B を元に戻したプレイがプレイ 1 と違わない(同じ {} 刻み・違う {} 刻み)", same,
                                     differ));
                    return AutoPlayAction::None;
                }

                return Act(Step::StoppedUndone, AutoPlayAction::Stop, state.frame);
            }

            case Step::Replaying: {
                if (elapsed < STOP_AFTER + 10)
                    return AutoPlayAction::None;

                const auto [same, early] = CompareWithFirst(3, m_editTick + 1, m_editTick + SAME_TICKS);
                const auto [laterSame, differ] = CompareWithFirst(3, m_editTick + STROKE_C_AFTER,
                                                                  m_editTick + STOP_AFTER);
                if (early != 0 || same < SAME_TICKS / 2 || differ == 0) {
                    Fail(
                        std::format("やり直したプレイ 3 とプレイ 1 の比べ: 初めの同じ {} 刻み・違う {} 刻み・"
                                    "C の後に違う {} 刻み",
                                    same, early, differ));
                    return AutoPlayAction::None;
                }

                m_summary = std::format(
                    "起動は止まっていた・置く 2 回で刻みの番号は {} のまま(時間が進まない)・プレイ(刻み {} から)→ "
                    "やめるで"
                    "置いたものを当て直した境界へ戻った・元に戻す 2 回とやり直す 2 回も刻みが進まず、B を戻したプレイは"
                    "違い、やり直したプレイは初めの {} 刻みが同じ・プレイ中に置いた分が消えて {} 刻みが違う",
                    m_editTick, m_editTick, same, differ);
                return Act(Step::Done, AutoPlayAction::None, state.frame);
            }

            default: return AutoPlayAction::None;
        }
    }

    // --- やめた後(編集の状態へ戻る・元に戻す・やり直す)---

    AutoPlayAction AutoPlay::UpdateAfterStop(const AutoPlayState& state) {
        if (!state.editing)
            return AutoPlayAction::None;  // やめるが届くのを待つ

        switch (m_step) {
            case Step::Stopped:
                if (!EditSettled(state, 1, true) || !StayedStill(state))
                    return AutoPlayAction::None;

                return Act(Step::UndoB, AutoPlayAction::Undo, state.frame);

            case Step::UndoB:
                return EditSettled(state, 1, true) ? Act(Step::UndoA, AutoPlayAction::Undo, state.frame)
                                                   : AutoPlayAction::None;

            case Step::UndoA:  // 置いたものが無くなった: 刻み P の前 (P, 0) で止まる
                return EditSettled(state, 0, true) ? Act(Step::RedoA, AutoPlayAction::Redo, state.frame)
                                                   : AutoPlayAction::None;

            case Step::RedoA:
                return EditSettled(state, 1, true) ? StartPlay(Step::PlayingUndone, 2, state.frame)
                                                   : AutoPlayAction::None;

            case Step::StoppedUndone:
                return EditSettled(state, 1, true) ? Act(Step::RedoB, AutoPlayAction::Redo, state.frame)
                                                   : AutoPlayAction::None;

            case Step::RedoB:
                return EditSettled(state, 1, true) ? StartPlay(Step::Replaying, 3, state.frame) : AutoPlayAction::None;

            default: return AutoPlayAction::None;
        }
    }

    // --- 補助 ---

    bool AutoPlay::EditSettled(const AutoPlayState& state, uint32_t unit, bool rewound) const {
        if (!state.editing || state.settling || state.frame < m_actionFrame + ACTION_FRAMES)
            return false;

        if (rewound && m_rewinds == m_rewindsAtAction)
            return false;

        return state.cursor == SimCursor{.tick = m_editTick, .unit = unit};
    }

    bool AutoPlay::StayedStill(const AutoPlayState& state) {
        if (m_stillFrame == 0) {
            m_stillFrame = state.frame;
            m_stillCursor = state.cursor;
            return false;
        }

        if (state.cursor != m_stillCursor) {
            Fail(std::format("編集の状態で世界が進んだ(刻み {} 単位 {} → 刻み {} 単位 {})", m_stillCursor.tick,
                             m_stillCursor.unit, state.cursor.tick, state.cursor.unit));
            return false;
        }

        if (state.frame < m_stillFrame + STILL_FRAMES)
            return false;

        m_stillFrame = 0;
        return true;
    }

    std::pair<uint32_t, uint32_t> AutoPlay::CompareWithFirst(uint32_t play, uint64_t from, uint64_t to) const {
        uint32_t same = 0;
        uint32_t differ = 0;
        for (uint64_t tick = from; tick <= to; ++tick) {
            const auto first = m_plays[1].find(tick);
            const auto other = m_plays[play].find(tick);
            if (first == m_plays[1].end() || other == m_plays[play].end())
                continue;

            if (first->second == other->second)
                ++same;
            else
                ++differ;
        }

        return {same, differ};
    }

    AutoPlayAction AutoPlay::Act(Step next, AutoPlayAction action, uint64_t frame) {
        m_step = next;
        m_actionFrame = frame;
        m_rewindsAtAction = m_rewinds;

        return action;
    }

    AutoPlayAction AutoPlay::StartPlay(Step next, uint32_t play, uint64_t frame) {
        m_recording = play;
        m_plays[play].clear();

        return Act(next, AutoPlayAction::Play, frame);
    }

    void AutoPlay::Fail(std::string reason) {
        if (m_failed)
            return;

        m_failed = true;
        m_summary = std::move(reason);
        Log(Channel::Sim, Level::Error, "--auto-play: {}", m_summary);
    }

}  // namespace bicameral::frame
