// auto_reload.h — 窓での人がいないホットリロードの確認(--auto-reload。T-0195・14・ADR-0047)。
//
// データの流れ(フレームのループが毎フレーム、ホットリロードの Poll の後に Update を呼ぶ):
//   世界が刻み START_TICK に着いた → パッケージの写しの reactions.luau を壊す(Luau の構文の誤り)
//   → TableHotReload が読み直しに失敗した(古い表のまま。差し替えが起きていない)のを見て、速度を書き換えた正しい中身にする
//   → 差し替わった(AppliedCount)のを見て、そこから RUN_TICKS_AFTER_SWAP 刻み流す
//   → 物質を足す(species.luau にオゾンを足す。後ろの物質の ID がずれる)→ 差し替わってオゾンが表にあるのを見て流す
//   → 物質を消す(オゾンの無い中身に戻す)→ 差し替わってオゾンが無くなったのを見て流す(T-0242・ADR-0065)
//   → 元の中身に戻して終える(Done)。
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
        // withSpecies: 速度の書き換えの後に物質を足す・消す段も(--species-remap。T-0242)
        [[nodiscard]] static std::expected<AutoReload, std::string> Create(const fs::path& packageRoot,
                                                                           bool withSpecies = false);

        // 毎フレーム。tick は世界の次に投げる刻み
        void Update(uint64_t tick, const TableHotReload& tables);

        [[nodiscard]] bool Done() const { return m_stage == Stage::Done; }
        [[nodiscard]] bool Failed() const { return m_stage == Stage::Failed; }
        [[nodiscard]] const std::string& Failure() const { return m_failure; }

    private:
        enum class Stage : uint8_t {
            Start,
            Broken,
            Edited,
            Swapped,
            SpeciesAdded,
            AddApplied,
            SpeciesRemoved,
            RemoveApplied,
            Done,
            Failed
        };

        AutoReload(fs::path file, std::string original, std::string edited, fs::path speciesFile,
                   std::string speciesOriginal, std::string speciesAdded)
            : m_file(std::move(file)),
              m_original(std::move(original)),
              m_edited(std::move(edited)),
              m_speciesFile(std::move(speciesFile)),
              m_speciesOriginal(std::move(speciesOriginal)),
              m_speciesAdded(std::move(speciesAdded)) {}

        void Write(const std::string& text) { WriteFile(m_file, text); }
        void WriteFile(const fs::path& file, const std::string& text);
        void UpdateSpecies(uint64_t tick, const TableHotReload& tables);
        [[nodiscard]] bool WaitApplied(uint64_t tick, const TableHotReload& tables, uint32_t count, bool hasOzone);
        void Fail(std::string why);

        fs::path m_file;
        std::string m_original;  // 元の中身(終える時に戻す)
        std::string m_edited;    // 速度を書き換えた中身
        fs::path m_speciesFile;  // 物質を足す・消す所(combustion_test/species.luau。T-0242)
        std::string m_speciesOriginal;
        std::string m_speciesAdded;  // オゾンを足した中身(空 = 物質を足す・消す段をしない)
        Stage m_stage = Stage::Start;
        uint64_t m_swapTick = 0;  // 差し替わったのを見た刻み
        std::string m_failure;
    };

}  // namespace bicameral::frame
