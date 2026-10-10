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

        // 物質を足す・消す(T-0242): species.luau の carbon の前にオゾンを足す(名前のバイト順で oxygen と water_vapor の間に入り、
        // 水蒸気の ID がずれる)。値は文献値を丸めた試験の値(未確認)。tests/reaction_replay_table_test.cpp と同じ
        constexpr std::string_view SPECIES_FILE = "combustion_test/species.luau";
        constexpr std::string_view SPECIES_ANCHOR = "    carbon = {";
        constexpr std::string_view OZONE_SPECIES =
            "    ozone = {\n"
            "        composition = { O = 3 },\n"
            "        formation_enthalpy_j_per_mol = 142700,\n"
            "        heat_capacity_mj_per_mol_k = 39200,\n"
            "        thermal_conductivity_mw_per_m_k = gas(19),\n"
            "    },\n";
        constexpr std::string_view OZONE = "ozone";
        constexpr uint64_t RUN_TICKS_AFTER_SPECIES = 10;  // 物質を足した・消した表で流す刻み

        std::expected<std::string, std::string> ReadWhole(const fs::path& file) {
            std::ifstream input(file, std::ios::binary);
            if (!input)
                return std::unexpected(std::format("--auto-reload: {} を読めない", file.string()));

            return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        }

    }  // namespace

    std::expected<AutoReload, std::string> AutoReload::Create(const fs::path& packageRoot, bool withSpecies) {
        const fs::path file = packageRoot / PACKAGE_FILE;
        auto read = ReadWhole(file);
        if (!read)
            return std::unexpected(read.error());

        std::string original = std::move(*read);
        const size_t at = original.find(RATE_BEFORE);
        if (at == std::string::npos)
            return std::unexpected(
                std::format("--auto-reload: {} に書き換える所({})が無い", file.string(), RATE_BEFORE));

        std::string edited = original;
        edited.replace(at, RATE_BEFORE.size(), RATE_AFTER);

        // --- 物質を足した中身(T-0242)---
        const fs::path speciesFile = packageRoot / SPECIES_FILE;
        auto species = ReadWhole(speciesFile);
        if (!species)
            return std::unexpected(species.error());

        const size_t anchor = species->find(SPECIES_ANCHOR);
        if (anchor == std::string::npos)
            return std::unexpected(
                std::format("--auto-reload: {} に物質を足す所({})が無い", speciesFile.string(), SPECIES_ANCHOR));

        std::string added;
        if (withSpecies) {
            added = *species;
            added.insert(anchor, OZONE_SPECIES);
        }

        return AutoReload(file, std::move(original), std::move(edited), speciesFile, std::move(*species),
                          std::move(added));
    }

    void AutoReload::WriteFile(const fs::path& file, const std::string& text) {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output)
            Fail(std::format("{} を書けない", file.string()));
    }

    // 差し替えが count 回目まで当たり、当てた表にオゾンがある(hasOzone)/ 無いのを見たら true。読めない・違う表なら Fail
    bool AutoReload::WaitApplied(uint64_t tick, const TableHotReload& tables, uint32_t count, bool hasOzone) {
        if (tables.LastFailed()) {
            Fail(std::format("物質を足す・消す表が読めない・当てられない: {}", tables.LastMessage()));
            return false;
        }

        if (tables.AppliedCount() < count)
            return false;

        const auto applied = tables.Find(tables.AppliedVersion());
        if (applied == nullptr || (applied->table.SpeciesId(OZONE) != 0) != hasOzone) {
            Fail(std::format("刻み {} で当てた表(版 {:016x})にオゾンが{}", tick, tables.AppliedVersion(),
                             hasOzone ? "無い" : "残っている"));
            return false;
        }

        m_swapTick = tick;
        Log(Channel::Tool, Level::Info, "--auto-reload: 刻み {} の頃に物質を{}表(版 {:016x}・物質 {})へ差し替わった",
            tick, hasOzone ? "足した" : "消した", tables.AppliedVersion(), applied->table.species.size() - 1);

        return true;
    }

    // 物質を足す → 当たったら流す → 消す → 当たったら流す(T-0242)
    void AutoReload::UpdateSpecies(uint64_t tick, const TableHotReload& tables) {
        switch (m_stage) {
            case Stage::SpeciesAdded:
                if (WaitApplied(tick, tables, 2, true))
                    m_stage = Stage::AddApplied;
                return;

            case Stage::AddApplied:
                if (tick < m_swapTick + RUN_TICKS_AFTER_SPECIES)
                    return;

                Log(Channel::Tool, Level::Info, "--auto-reload: 刻み {} でオゾンを消す", tick);
                m_stage = Stage::SpeciesRemoved;
                WriteFile(m_speciesFile, m_speciesOriginal);
                return;

            case Stage::SpeciesRemoved:
                if (WaitApplied(tick, tables, 3, false))
                    m_stage = Stage::RemoveApplied;
                return;

            case Stage::RemoveApplied:
                if (tick < m_swapTick + RUN_TICKS_AFTER_SPECIES)
                    return;

                Write(m_original);
                if (m_stage == Stage::RemoveApplied)
                    m_stage = Stage::Done;

                Log(Channel::Tool, Level::Info, "--auto-reload: 刻み {} まで流した。元の中身に戻して終える", tick);
                return;

            default: return;
        }
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

            // --- 差し替わった後もしばらく流してから、物質を足す(--species-remap。T-0242)か、元の中身に戻して終える ---
            case Stage::Swapped:
                if (tick < m_swapTick + RUN_TICKS_AFTER_SWAP)
                    return;

                if (m_speciesAdded.empty()) {
                    Write(m_original);
                    if (m_stage == Stage::Swapped)
                        m_stage = Stage::Done;

                    Log(Channel::Tool, Level::Info, "--auto-reload: 刻み {} まで流した。元の中身に戻して終える", tick);
                    return;
                }

                Log(Channel::Tool, Level::Info, "--auto-reload: 刻み {} でオゾンを足す", tick);
                m_stage = Stage::SpeciesAdded;
                WriteFile(m_speciesFile, m_speciesAdded);
                return;

            case Stage::SpeciesAdded:
            case Stage::AddApplied:
            case Stage::SpeciesRemoved:
            case Stage::RemoveApplied: UpdateSpecies(tick, tables); return;

            case Stage::Done:
            case Stage::Failed: return;
        }
    }

}  // namespace bicameral::frame
