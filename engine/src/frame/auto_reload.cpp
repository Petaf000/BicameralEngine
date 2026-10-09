// auto_reload.cpp — 窓での人がいないホットリロードの確認(auto_reload.h)。
#include "frame/auto_reload.h"

#include <format>
#include <fstream>
#include <iterator>

#include "core/log.h"

namespace bicameral::frame {

    namespace {

        constexpr uint64_t START_TICK = 10;            // 壊すのは、世界がこの刻みに着いてから
        constexpr uint64_t RUN_TICKS_AFTER_SWAP = 30;  // 差し替わってから流す刻み

        // 書き換える所(data/packages/combustion_test/reactions.luau の木の燃焼の前指数因子を 2 倍に)
        constexpr std::string_view PACKAGE_FILE = "combustion_test/reactions.luau";
        constexpr std::string_view RATE_BEFORE = "rate = { a = \"2e10\"";
        constexpr std::string_view RATE_AFTER = "rate = { a = \"4e10\"";
        constexpr std::string_view BROKEN_TAIL = "\n=== --auto-reload が壊した行(return の後ろの文は構文の誤り)\n";

    }  // namespace

    std::expected<AutoReload, std::string> AutoReload::Create(const fs::path& packageRoot) {
        const fs::path file = packageRoot / PACKAGE_FILE;
        std::ifstream input(file, std::ios::binary);
        if (!input)
            return std::unexpected(std::format("--auto-reload: {} を読めない", file.string()));

        std::string original{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const size_t at = original.find(RATE_BEFORE);
        if (at == std::string::npos)
            return std::unexpected(
                std::format("--auto-reload: {} に書き換える所({})が無い", file.string(), RATE_BEFORE));

        std::string edited = original;
        edited.replace(at, RATE_BEFORE.size(), RATE_AFTER);

        return AutoReload(file, std::move(original), std::move(edited));
    }

    void AutoReload::Write(const std::string& text) {
        std::ofstream output(m_file, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output)
            Fail(std::format("{} を書けない", m_file.string()));
    }

    void AutoReload::Fail(std::string why) {
        m_failure = std::move(why);
        m_stage = Stage::Failed;
        Log(Channel::Tool, Level::Error, "--auto-reload: {}", m_failure);
    }

    void AutoReload::Update(uint64_t tick, const TableHotReload& tables) {
        switch (m_stage) {
            // --- 壊す: 読み直しに失敗して、古い表のまま続くはず ---
            case Stage::Start:
                if (tick < START_TICK)
                    return;

                Log(Channel::Tool, Level::Info, "--auto-reload: 刻み {} で {} を壊す", tick, m_file.string());
                m_stage = Stage::Broken;
                Write(m_original + std::string(BROKEN_TAIL));
                return;

            // --- 直す(速度を変える): 次の刻みの境界で差し替わるはず ---
            case Stage::Broken:
                if (tables.FailedCount() == 0)
                    return;

                if (tables.AppliedCount() != 0) {
                    Fail("壊した表で差し替わった");
                    return;
                }

                Log(Channel::Tool, Level::Info, "--auto-reload: 古い表のまま(「{}」)。速度を変えて直す",
                    tables.LastMessage());
                m_stage = Stage::Edited;
                Write(m_edited);
                return;

            case Stage::Edited:
                if (tables.FailedCount() > 1) {
                    Fail(std::format("直した表が読めない: {}", tables.LastMessage()));
                    return;
                }

                if (tables.AppliedCount() == 0)
                    return;

                m_swapTick = tick;
                m_stage = Stage::Swapped;
                Log(Channel::Tool, Level::Info, "--auto-reload: 刻み {} の頃に版 {:016x} へ差し替わった", tick,
                    tables.AppliedVersion());
                return;

            // --- 差し替わった後もしばらく流してから、元の中身に戻して終える ---
            case Stage::Swapped:
                if (tick < m_swapTick + RUN_TICKS_AFTER_SWAP)
                    return;

                Write(m_original);
                if (m_stage == Stage::Swapped)
                    m_stage = Stage::Done;

                Log(Channel::Tool, Level::Info, "--auto-reload: 刻み {} まで流した。元の中身に戻して終える", tick);
                return;

            case Stage::Done:
            case Stage::Failed: return;
        }
    }

}  // namespace bicameral::frame
