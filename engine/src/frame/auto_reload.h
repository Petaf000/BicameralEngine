// auto_reload.h — 窓での人がいないホットリロードの確認(--auto-reload。T-0195・14・ADR-0047)。
//
// データの流れ(フレームのループが毎フレーム、ホットリロードの Poll の後に Update を呼ぶ):
//   世界が刻み START_TICK に着いた → パッケージの写しの reactions.luau を壊す(Luau の構文の誤り)
//   → TableHotReload が読み直しに失敗した(古い表のまま。差し替えが起きていない)のを見て、速度を書き換えた正しい中身にする
//   → 差し替わった(AppliedCount)のを見て、そこから RUN_TICKS_AFTER_SWAP 刻み流す → 元の中身に戻して終える(Done)。
// 記録(--record)と一緒に使い、記録の再生(--replay。表の中身は再生ファイルにある。T-0193)でハッシュ列が一致するかを
// 別の起動で確かめる(ctest window_hot_reload_*)。書き換えるのは --packages で渡した写しだけ(ゲーム本体のデータは書かない)。
#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "core/aliases.h"
#include "frame/table_hot_reload.h"

namespace bicameral::frame {

    class AutoReload {
    public:
        // packageRoot の中の試験のパッケージ(combustion_test/reactions.luau)を書き換える準備。読めない・書き換える所が無ければ error
        [[nodiscard]] static std::expected<AutoReload, std::string> Create(const fs::path& packageRoot);

        // 毎フレーム。tick は世界の次に投げる刻み
        void Update(uint64_t tick, const TableHotReload& tables);

        [[nodiscard]] bool Done() const { return m_stage == Stage::Done; }
        [[nodiscard]] bool Failed() const { return m_stage == Stage::Failed; }
        [[nodiscard]] const std::string& Failure() const { return m_failure; }

    private:
        enum class Stage : uint8_t { Start, Broken, Edited, Swapped, Done, Failed };

        AutoReload(fs::path file, std::string original, std::string edited)
            : m_file(std::move(file)), m_original(std::move(original)), m_edited(std::move(edited)) {}

        void Write(const std::string& text);
        void Fail(std::string why);

        fs::path m_file;
        std::string m_original;  // 元の中身(終える時に戻す)
        std::string m_edited;    // 速度を書き換えた中身
        Stage m_stage = Stage::Start;
        uint64_t m_swapTick = 0;  // 差し替わったのを見た刻み
        std::string m_failure;
    };

}  // namespace bicameral::frame
