// auto_play.cpp — --auto-play の手順と判定(T-0497)。手順は auto_play.h。
#include "frame/auto_play.h"

#include <format>

#include "core/log.h"

namespace bicameral::frame {

    AutoPlayAction AutoPlay::Update(const AutoPlayState& state) {
        if (Finished())
            return AutoPlayAction::None;

        return m_step <= Step::Check ? UpdateEdit(state) : UpdatePlay(state);
    }

    void AutoPlay::NoteHash(uint64_t tick, uint64_t worldHash) {
        m_hashes[tick] = {worldHash, m_generation};
        if (m_recordFirst && tick >= m_playStart)
            m_firstPlay[tick] = worldHash;

        if (m_recordSecond && tick >= m_playStart)
            m_secondPlay[tick] = worldHash;
    }

    // --- 編集の状態(起動が止まっている・置く・元に戻す・やり直す)---

    AutoPlayAction AutoPlay::UpdateEdit(const AutoPlayState& state) {
        if (!state.editing) {
            Fail(std::format("フレーム {} で編集の状態でない", state.frame));
            return AutoPlayAction::None;
        }

        // 読み戻すハッシュは刻みの境界の状態 S(t)(刻み t − 1 を終えた状態)。今のカーソルの境界の状態を比べる
        const uint64_t boundary = state.cursor.tick;
        switch (m_step) {
            case Step::Hold:
                if (state.cursor != SimCursor{}) {
                    Fail(std::format("起動が止まっていない(フレーム {} で刻み {})", state.frame, state.cursor.tick));
                    return AutoPlayAction::None;
                }

                return state.frame >= HOLD_FRAMES ? Act(Step::PlaceA, AutoPlayAction::StrokeA) : AutoPlayAction::None;

            case Step::PlaceA:
                return boundary > 0 && Settled(state, boundary) ? Act(Step::PlaceB, AutoPlayAction::StrokeB)
                                                                : AutoPlayAction::None;

            case Step::PlaceB:
                if (!Settled(state, boundary))
                    return AutoPlayAction::None;

                m_editTick = boundary;
                m_hashAB = HashAt(m_editTick);
                return Act(Step::UndoB, AutoPlayAction::Undo);

            case Step::UndoB:
                if (!Settled(state, m_editTick))
                    return AutoPlayAction::None;

                m_hashUndoB = HashAt(m_editTick);
                if (m_hashUndoB == m_hashAB)
                    Fail("B を元に戻しても刻み E の状態が変わらない");

                return Act(Step::UndoA, AutoPlayAction::Undo);

            case Step::UndoA:
                if (!Settled(state, m_editTick))
                    return AutoPlayAction::None;

                m_hashUndoA = HashAt(m_editTick);
                if (m_hashUndoA == m_hashUndoB)
                    Fail("A を元に戻しても刻み E の状態が変わらない");

                return Act(Step::RedoA, AutoPlayAction::Redo);

            case Step::RedoA:
                if (!Settled(state, m_editTick))
                    return AutoPlayAction::None;

                if (HashAt(m_editTick) != m_hashUndoB)
                    Fail("A をやり直した状態が、B だけを元に戻した状態と違う");

                return Act(Step::RedoB, AutoPlayAction::Redo);

            case Step::RedoB:
                if (!Settled(state, m_editTick))
                    return AutoPlayAction::None;

                if (HashAt(m_editTick) != m_hashAB)
                    Fail("B をやり直した状態が、元に戻す前と違う");

                return Act(Step::Check, AutoPlayAction::None);

            case Step::Check:
                if (state.cursor != SimCursor{.tick = m_editTick, .unit = 0}) {
                    Fail(std::format("編集の状態で刻みが進んだ(刻み {}。期待は {})", state.cursor.tick, m_editTick));
                    return AutoPlayAction::None;
                }

                m_recordFirst = true;
                m_playStart = m_editTick;
                return Act(Step::Playing, AutoPlayAction::Play);

            default: return AutoPlayAction::None;
        }
    }

    // --- プレイ(プレイ中に置く・やめると戻る・もう一度プレイして比べる)---

    AutoPlayAction AutoPlay::UpdatePlay(const AutoPlayState& state) {
        switch (m_step) {
            case Step::Playing:
                if (state.editing || state.playStartTick == UINT64_MAX)
                    return AutoPlayAction::None;

                if (state.playStartTick != m_playStart) {
                    Fail(std::format("プレイを始めた刻みが {}(期待は {})", state.playStartTick, m_playStart));
                    return AutoPlayAction::None;
                }

                return state.cursor.tick >= m_playStart + STROKE_C_AFTER
                           ? Act(Step::PlayingAfterC, AutoPlayAction::StrokeC)
                           : AutoPlayAction::None;

            case Step::PlayingAfterC:
                if (state.cursor.tick < m_playStart + STOP_AFTER)
                    return AutoPlayAction::None;

                m_recordFirst = false;
                return Act(Step::Stopped, AutoPlayAction::Stop);

            case Step::Stopped:
                if (!state.editing || state.settling)
                    return AutoPlayAction::None;

                if (state.cursor != SimCursor{.tick = m_playStart, .unit = 0}) {
                    Fail(std::format("やめた後の刻みが {}(単位 {})。期待はプレイを始めた刻み {}", state.cursor.tick,
                                     state.cursor.unit, m_playStart));
                    return AutoPlayAction::None;
                }

                if (m_stillFrame == 0) {
                    m_stillFrame = state.frame;
                    return AutoPlayAction::None;
                }

                if (state.frame < m_stillFrame + STILL_FRAMES)
                    return AutoPlayAction::None;

                m_recordSecond = true;
                return Act(Step::Replaying, AutoPlayAction::Play);

            case Step::Replaying: {
                if (state.editing || state.cursor.tick < m_playStart + STOP_AFTER + 10)
                    return AutoPlayAction::None;

                uint32_t same = 0;
                for (uint64_t tick = m_playStart; tick <= m_playStart + SAME_TICKS; ++tick) {
                    const auto first = m_firstPlay.find(tick);
                    const auto second = m_secondPlay.find(tick);
                    if (first == m_firstPlay.end() || second == m_secondPlay.end())
                        continue;

                    if (first->second != second->second) {
                        Fail(std::format("2 回のプレイで刻み {} の状態が違う(プレイ前の世界に戻っていない)", tick));
                        return AutoPlayAction::None;
                    }

                    ++same;
                }

                uint32_t differ = 0;
                for (uint64_t tick = m_playStart + STROKE_C_AFTER; tick <= m_playStart + STOP_AFTER; ++tick) {
                    const auto first = m_firstPlay.find(tick);
                    const auto second = m_secondPlay.find(tick);
                    if (first != m_firstPlay.end() && second != m_secondPlay.end() && first->second != second->second)
                        ++differ;
                }

                if (same < SAME_TICKS / 2 || differ == 0) {
                    Fail(std::format("2 回のプレイの比べ: 同じ {} 刻み・C の後に違う {} 刻み", same, differ));
                    return AutoPlayAction::None;
                }

                m_summary = std::format(
                    "起動は止まっていた・編集の状態で元に戻す 2 回とやり直す 2 回が合った(刻み {})・"
                    "プレイ(刻み {} から)→ やめるで戻り、2 回のプレイの {} 刻みが同じ・プレイ中に置いた分が"
                    "消えて {} 刻みが違う",
                    m_editTick, m_playStart, same, differ);
                m_recordSecond = false;
                return Act(Step::Done, AutoPlayAction::None);
            }

            default: return AutoPlayAction::None;
        }
    }

    // --- 補助 ---

    bool AutoPlay::Settled(const AutoPlayState& state, uint64_t tick) const {
        if (state.settling || state.cursor.unit != 0 || state.cursor.tick < tick)
            return false;

        const auto found = m_hashes.find(tick);

        return found != m_hashes.end() && found->second.second == m_generation;
    }

    uint64_t AutoPlay::HashAt(uint64_t tick) const {
        const auto found = m_hashes.find(tick);

        return found == m_hashes.end() ? 0 : found->second.first;
    }

    AutoPlayAction AutoPlay::Act(Step next, AutoPlayAction action) {
        m_step = next;
        ++m_generation;

        return action;
    }

    void AutoPlay::Fail(std::string reason) {
        if (m_failed)
            return;

        m_failed = true;
        m_summary = std::move(reason);
        Log(Channel::Sim, Level::Error, "--auto-play: {}", m_summary);
    }

}  // namespace bicameral::frame
