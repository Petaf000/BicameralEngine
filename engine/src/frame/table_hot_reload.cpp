// table_hot_reload.cpp — 反応表のホットリロードを世界の刻みの境界へつなぐ(T-0139・ADR-0047)。
#include "frame/table_hot_reload.h"

#include <algorithm>
#include <format>
#include <utility>

#include "core/aliases.h"
#include "core/log.h"

namespace bicameral::frame {

    namespace {

        // ファイルを見る間隔(フレーム)。見るたびにパッケージのファイルを全部読むので、毎フレームにはしない(60 fps で約 0.25 秒)
        constexpr uint64_t POLL_INTERVAL_FRAMES = 15;

    }  // namespace

    TableHotReload::TableHotReload(script::ReactionTableSource source,
                                   std::shared_ptr<const script::LoadedReactionTable> initial, bool watch,
                                   script::SpeciesChangePolicy policy)
        : m_initialVersion(initial->tableVersion), m_appliedVersion(initial->tableVersion) {
        Remember(initial);
        if (watch)
            m_reload.emplace(std::move(source), std::move(initial), policy);
    }

    void TableHotReload::Remember(const std::shared_ptr<const script::LoadedReactionTable>& table) {
        m_known.emplace(table->tableVersion, table);  // 同じ版は同じ法則なので、先に覚えた方のままでよい
    }

    std::shared_ptr<const script::LoadedReactionTable> TableHotReload::Find(uint64_t version) const {
        const auto known = m_known.find(version);

        return known == m_known.end() ? nullptr : known->second;
    }

    void TableHotReload::Poll(uint64_t frameNumber) {
        if (!m_reload || frameNumber % POLL_INTERVAL_FRAMES != 0)
            return;

        script::HotReloadResult result = m_reload->Poll();
        if (result.state == script::HotReloadState::Failed) {
            ++m_failedCount;
            m_lastFailed = true;
            m_lastMessage = std::move(result.message);
            Log(Channel::Sim, Level::Warning, "反応表を読み直せなかった(今の表のまま): {}", m_lastMessage);
            return;
        }

        if (result.state != script::HotReloadState::Reloaded)
            return;

        Remember(result.table);
        m_waiting = std::move(result.table);
        m_lastFailed = false;
        m_lastMessage = std::format("版 {:016x} を読んだ(物質 {}・規則 {})。次の刻みの境界で当てる{}{}",
                                    m_waiting->tableVersion, m_waiting->table.species.size() - 1,
                                    m_waiting->table.rules.size(), result.message.empty() ? "" : "。", result.message);
        Log(Channel::Sim, Level::Info, "反応表を読み直した: {}", m_lastMessage);
    }

    std::expected<void, std::string> TableHotReload::ScheduleReplayed(std::span<const sim::ProbeCommand> commands) {
        for (const sim::ProbeCommand& command : commands) {
            if (command.type != sim::PROBE_COMMAND_TYPE_TABLE)
                continue;

            const uint64_t version = sim::TableCommandVersion(command);
            const auto known = m_known.find(version);
            if (known == m_known.end()) {
                return std::unexpected(
                    std::format("再生ファイルの刻み {} で反応表が版 {:016x} "
                                "に変わるが、その表の中身が再生ファイルに無い(起動時の表は版 {:016x}。"
                                "表の中身を残さない古い形式〔版 1〕の記録)",
                                command.targetTick, version, m_initialVersion));
            }

            m_scheduled.push_back({.tick = command.targetTick, .table = known->second});
        }

        return {};
    }

    std::expected<std::vector<sim::ProbeTableSwap>, std::string> TableHotReload::TakeSwaps(
        uint64_t firstUnit, uint32_t unitCount, uint32_t unitsPerTick, bool replaying, bool canAddCommand,
        uint32_t& nextSequence, std::vector<sim::ProbeCommand>& commands) {
        m_inFlight.clear();  // 前のフレームの表は RecordFrame が写し終えた

        // --- 新しい差し替え: 再生は印から、生の操作は待っている表をこのフレームの最初の適用の単位へ ---
        const uint64_t applyTick = (firstUnit + unitsPerTick - 1) / unitsPerTick;  // ProbeSim::NextApplyTick と同じ
        if (replaying) {
            if (auto scheduled = ScheduleReplayed(commands); !scheduled)
                return std::unexpected(std::move(scheduled.error()));
        } else if (m_waiting && canAddCommand && applyTick * unitsPerTick < firstUnit + unitCount) {
            commands.push_back(sim::MakeTableCommand(applyTick, nextSequence++, m_waiting->tableVersion));
            rng::sort(commands, sim::CommandPrecedes);
            m_scheduled.push_back({.tick = applyTick, .table = std::move(m_waiting)});
            m_waiting = nullptr;
        }

        // --- このフレームに適用の単位がある差し替えを返す ---
        std::vector<sim::ProbeTableSwap> swaps;
        for (auto it = m_scheduled.begin(); it != m_scheduled.end();) {
            const uint64_t swapUnit = it->tick * unitsPerTick;
            if (swapUnit < firstUnit)
                return std::unexpected(std::format("反応表の差し替えの刻み {} を過ぎてしまった", it->tick));

            if (swapUnit >= firstUnit + unitCount) {
                ++it;
                continue;
            }

            swaps.push_back({.tick = it->tick, .table = &it->table->table});
            m_appliedVersion = it->table->tableVersion;
            ++m_appliedCount;
            Log(Channel::Sim, Level::Info, "反応表を刻み {} の始めに版 {:016x} へ差し替える", it->tick,
                m_appliedVersion);
            m_inFlight.push_back(std::move(it->table));
            it = m_scheduled.erase(it);
        }

        return swaps;
    }

}  // namespace bicameral::frame
