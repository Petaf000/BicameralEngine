// patch_panel.cpp — 魔法のパッチを置くパネル(patch_panel.h。T-0325)。値はすべて試験用の仮のもの(D-006)。
#include "editor/patch_panel.h"

#include <algorithm>
#include <format>
#include <iterator>

#include <imgui.h>

#include "common/magic_patch.hlsli"
#include "core/log.h"
#include "editor/lab_panel.h"

namespace bicameral::editor {

    namespace {

        // --- 効き方の一覧(MP_KIND_* の順。転換〔T-0359〕などを足す時はここに行を足す)---
        constexpr std::array<PatchKind, 3> PATCH_KINDS = {
            PatchKind{.kind = reaction::MP_KIND_SCALE, .label = "速さを 2^shift 倍", .strength = PatchStrength::Shift},
            PatchKind{.kind = reaction::MP_KIND_STOP, .label = "止める", .strength = PatchStrength::None},
            PatchKind{.kind = reaction::MP_KIND_CATALYST,
                      .label = "触媒(活性化エネルギーを下げる)",
                      .strength = PatchStrength::JoulesPerMol}};

        constexpr size_t MAX_LISTED = 24;  // 一覧に出す置いたパッチ(新しい順)
        constexpr int MAX_DURATION_TICKS = 1000000;
        constexpr float LAB_SLICE_PIXELS = 192.0f;  // 実験室の断面の幅の目安
        constexpr float LAB_CELL_MAX_PIXELS = 14.0f;

        // --- --auto-patch の実験(実験室。--auto-lab と同じ空気と木)---
        constexpr uint32_t AUTO_IGNITE_TICK = 3;
        constexpr uint32_t AUTO_PATCH_TICKS = 40;
        constexpr uint32_t AUTO_IGNITE_MILLIKELVIN = 1500000;
        constexpr uint32_t AUTO_ROOM_MILLIKELVIN = 300000;
        constexpr uint64_t AUTO_OXYGEN_PASCAL = 60000;
        constexpr uint64_t AUTO_NITROGEN_PASCAL = 140000;
        constexpr sim::LabCellRange AUTO_WOOD_RANGE = {.low = {.x = 3, .y = 3, .z = 3},
                                                       .high = {.x = 4, .y = 4, .z = 4}};

        // --- --auto-patch の仮の世界(木箱の壁のまわり。--auto-ignite の火と同じ場所)---
        constexpr uint64_t AUTO_WORLD_FRAME = 4;
        constexpr std::array<int32_t, 3> AUTO_WORLD_CENTER = {28, 32, 32};
        constexpr int32_t AUTO_WORLD_HALF_CELLS = 3;
        constexpr uint32_t AUTO_WORLD_TICKS = 40;
        constexpr uint32_t AUTO_WORLD_CATALYST_JOULES_PER_MOL = 20000;

        // common/magic_patch.hlsli の List の約束(パネルが写した列)
        struct WordList {
            std::span<uint32_t> words;

            [[nodiscard]] uint32_t PatchWord(uint32_t word) const { return words[word]; }
            void SetPatchWord(uint32_t word, uint32_t value) const { words[word] = value; }
        };

        struct ConstWordList {
            std::span<const uint32_t> words;

            [[nodiscard]] uint32_t PatchWord(uint32_t word) const { return words[word]; }
        };

        // 表の規則 rule の反応物に物質 species がある
        bool RuleConsumes(const reaction::RxRule& rule, uint32_t species) {
            for (uint32_t i = 0; i < rule.reactantCount; ++i) {
                if (rule.reactants[i] == species)
                    return true;
            }

            return false;
        }

        // 物質 species を使う規則の ID(表の順)
        std::vector<uint32_t> RulesConsuming(const sim::BakedReactionTable& table, uint32_t species) {
            std::vector<uint32_t> rules;
            for (uint32_t rule = 0; rule < table.rules.size(); ++rule) {
                if (species != 0 && RuleConsumes(table.rules[rule], species))
                    rules.push_back(rule);
            }

            return rules;
        }

        // 箱全体の物質 species の合計(µmol)
        uint64_t BoxTotalOf(const sim::MultiresNest& nest, lab::LabBoxShape shape, uint32_t species) {
            const sim::LabCellRange whole = sim::LabWholeBox(shape);
            uint64_t total = 0;
            for (uint32_t z = 0; z <= whole.high.z; ++z) {
                for (uint32_t y = 0; y <= whole.high.y; ++y) {
                    for (uint32_t x = 0; x <= whole.high.x; ++x) {
                        const reaction::RxCell cell = sim::LabBoxCell(nest, lab::LabCellIndex(x, y, z), shape);
                        for (uint32_t i = 0; i < cell.speciesCount; ++i)
                            total += cell.species[i] == species ? cell.amounts[i] : 0;
                    }
                }
            }

            return total;
        }

        std::string DescribeRange(const std::array<int32_t, 3>& low, const std::array<int32_t, 3>& high) {
            return std::format("({}, {}, {})〜({}, {}, {})", low[0], low[1], low[2], high[0], high[1], high[2]);
        }

    }  // namespace

    // --- 効き方 ---

    std::span<const PatchKind> PatchKinds() {
        return PATCH_KINDS;
    }

    uint32_t MakePatchEffect(uint32_t kind, int64_t strength) {
        if (kind == reaction::MP_KIND_SCALE) {
            const auto shift = std::clamp<int64_t>(strength, -reaction::MP_MAX_SHIFT, reaction::MP_MAX_SHIFT);
            return reaction::MpMakeScaleEffect(static_cast<int32_t>(shift));
        }

        if (kind == reaction::MP_KIND_STOP)
            return reaction::MpMakeStopEffect();

        if (kind == reaction::MP_KIND_CATALYST) {
            const auto lowered = std::clamp<int64_t>(strength, 1, reaction::MP_MAX_CATALYST_JOULES_PER_MOL);
            return reaction::MpMakeCatalystEffect(static_cast<uint32_t>(lowered));
        }

        return kind;  // 知らない種類(MpEffectValid が断る)
    }

    std::string DescribePatchEffect(uint32_t effect) {
        const uint32_t kind = reaction::MpEffectKind(effect);
        const auto found = std::ranges::find(PATCH_KINDS, kind, &PatchKind::kind);
        if (found == PATCH_KINDS.end())
            return std::format("種類 {}", kind);

        if (found->strength == PatchStrength::Shift)
            return std::format("2^{} 倍", reaction::MpEffectShift(effect));

        if (found->strength == PatchStrength::JoulesPerMol)
            return std::format("触媒 −{} J/mol", reaction::MpEffectCatalystJoulesPerMol(effect));

        return std::string(found->label);
    }

    // --- 範囲の線(描画側)---

    BrushPreview BuildPatchRangePreview(const std::array<int32_t, 3>& low, const std::array<int32_t, 3>& high,
                                        uint32_t sliceAxis, uint32_t slicePlane, uint32_t gridSize) {
        BrushPreview preview;
        if (gridSize == 0 || sliceAxis > 2)
            return preview;

        // --- 格子の中に切る([begin, end) のセルの面)---
        std::array<int64_t, 3> begin{};
        std::array<int64_t, 3> end{};
        for (size_t axis = 0; axis < 3; ++axis) {
            begin[axis] = std::max<int64_t>(low[axis], 0);
            end[axis] = std::min<int64_t>(int64_t{high[axis]} + 1, gridSize);
            if (begin[axis] >= end[axis])
                return preview;
        }

        const auto corner = [&](uint32_t bits) {
            return render::Vector3(static_cast<float>((bits & 1u) != 0 ? end[0] : begin[0]),
                                   static_cast<float>((bits & 2u) != 0 ? end[1] : begin[1]),
                                   static_cast<float>((bits & 4u) != 0 ? end[2] : begin[2]));
        };
        for (uint32_t bits = 0; bits < 8; ++bits) {
            for (uint32_t axisBit = 1; axisBit < 8; axisBit <<= 1) {
                if ((bits & axisBit) == 0)
                    preview.segments.push_back({.from = corner(bits), .to = corner(bits | axisBit), .onSlice = false});
            }
        }

        // --- 断面で切った四角形(面の真ん中 plane + 0.5)---
        if (slicePlane < begin[sliceAxis] || slicePlane >= end[sliceAxis])
            return preview;

        const uint32_t axisU = sliceAxis == 0 ? 1 : 0;
        const uint32_t axisV = sliceAxis == 2 ? 1 : 2;
        preview.sliceCells = static_cast<uint32_t>((end[axisU] - begin[axisU]) * (end[axisV] - begin[axisV]));

        const float along = static_cast<float>(slicePlane) + 0.5f;
        const auto point = [&](int64_t u, int64_t v) {
            std::array<float, 3> xyz{};
            xyz[sliceAxis] = along;
            xyz[axisU] = static_cast<float>(u);
            xyz[axisV] = static_cast<float>(v);
            return render::Vector3(xyz[0], xyz[1], xyz[2]);
        };
        const std::array<render::Vector3, 4> square = {point(begin[axisU], begin[axisV]),
                                                       point(end[axisU], begin[axisV]), point(end[axisU], end[axisV]),
                                                       point(begin[axisU], end[axisV])};
        for (size_t i = 0; i < square.size(); ++i)
            preview.segments.push_back({.from = square[i], .to = square[(i + 1) % square.size()], .onSlice = true});

        return preview;
    }

    // --- 表 ---

    void PatchPanel::UseTable(std::shared_ptr<const sim::BakedReactionTable> table, uint64_t version) {
        if (table == nullptr || (m_table != nullptr && version == m_tableVersion))
            return;

        m_table = std::move(table);
        m_tableVersion = version;
        m_rule = std::clamp(m_rule, 0, std::max(static_cast<int>(m_table->rules.size()) - 1, 0));
    }

    // --- パネル ---

    void PatchPanel::Build(LabPanel& lab) {
        ImGui::SetNextWindowPos({860.0f, 420.0f}, ImGuiCond_FirstUseEver);
        ImGui::Begin("魔法のパッチ(試す道具)", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        if (m_table == nullptr) {
            ImGui::TextUnformatted("反応表がまだ無い");
            ImGui::End();
            return;
        }

        if (m_autoPending) {
            m_autoPending = false;
            RunAutoLab(lab);
        }

        ImGui::TextDisabled("値は試験用の仮のもの(D-006)。置く操作はコマンドとして記録・再生される");
        ImGui::SeparatorText("置く先");
        if (ImGui::RadioButton("仮の世界", m_target == Target::World))
            m_target = Target::World;

        ImGui::SameLine();
        if (ImGui::RadioButton("実験室の箱", m_target == Target::Lab))
            m_target = Target::Lab;

        BuildRule();
        BuildEffect();
        BuildRange();

        ImGui::SeparatorText("期間");
        ImGui::InputInt("刻み", &m_durationTicks);
        m_durationTicks = std::clamp(m_durationTicks, 1, MAX_DURATION_TICKS);

        Composed composed;
        const bool composable = Compose(composed);
        ImGui::BeginDisabled(!composable);
        if (ImGui::Button("置く")) {
            if (m_target == Target::World)
                PlaceInWorld(composed);
            else
                PlaceInLab(lab, composed);
        }

        ImGui::EndDisabled();
        if (!composable)
            ImGui::TextDisabled("置けない(範囲の角の順・実験室は箱の座標が 0 以上)");

        if (!m_message.empty())
            ImGui::TextWrapped("%s", m_message.c_str());

        BuildList(lab);
        if (m_target == Target::Lab)
            BuildLabSlice(lab);

        ImGui::End();
    }

    // 規則を名前で選ぶ(名前の一部で絞る)
    void PatchPanel::BuildRule() {
        ImGui::SeparatorText("規則");
        std::array<char, 64> filter{};
        std::copy_n(m_filter.begin(), std::min(m_filter.size(), filter.size() - 1), filter.begin());
        if (ImGui::InputText("名前で絞る", filter.data(), filter.size()))
            m_filter = filter.data();

        const std::vector<std::string>& names = m_table->ruleNames;
        const char* preview = static_cast<size_t>(m_rule) < names.size() ? names[m_rule].c_str() : "(無し)";
        if (!ImGui::BeginCombo("規則", preview))
            return;

        for (size_t rule = 0; rule < names.size(); ++rule) {
            if (!m_filter.empty() && !names[rule].contains(m_filter))
                continue;

            if (ImGui::Selectable(names[rule].c_str(), static_cast<int>(rule) == m_rule))
                m_rule = static_cast<int>(rule);
        }

        ImGui::EndCombo();
    }

    // 効き方(MP_KIND_* の一覧)と強さ
    void PatchPanel::BuildEffect() {
        ImGui::SeparatorText("効き方");
        if (ImGui::BeginCombo("種類", PATCH_KINDS.at(m_kind).label.data())) {
            for (size_t i = 0; i < PATCH_KINDS.size(); ++i) {
                if (ImGui::Selectable(PATCH_KINDS[i].label.data(), i == m_kind))
                    m_kind = i;
            }

            ImGui::EndCombo();
        }

        const PatchStrength strength = PATCH_KINDS.at(m_kind).strength;
        if (strength == PatchStrength::Shift) {
            ImGui::SliderInt("shift", &m_shift, -reaction::MP_MAX_SHIFT, reaction::MP_MAX_SHIFT);
            ImGui::TextDisabled("どの温度でも同じ倍率(負なら弱める)");
        }

        if (strength == PatchStrength::JoulesPerMol) {
            ImGui::InputInt("下げる量 (J/mol)", &m_joulesPerMol, 1000, 10000);
            m_joulesPerMol = std::clamp(m_joulesPerMol, 1, static_cast<int>(reaction::MP_MAX_CATALYST_JOULES_PER_MOL));
            if (static_cast<size_t>(m_rule) < m_table->rules.size())
                ImGui::TextDisabled("この規則の Ea = %u J/mol(下げる量の和はこれで止まる)。冷たいほど大きく効く",
                                    m_table->rules[m_rule].activationEnergy);
        }
    }

    // 範囲: 中心 + 半幅(筆の直方体と同じ選び方)か両端の角。レベル 0 のセルの座標
    void PatchPanel::BuildRange() {
        ImGui::SeparatorText("範囲");
        if (ImGui::RadioButton("中心 + 半幅", !m_corners))
            m_corners = false;

        ImGui::SameLine();
        if (ImGui::RadioButton("両端の角", m_corners))
            m_corners = true;

        if (m_corners) {
            ImGui::InputInt3("小さい角", m_low.data());
            ImGui::InputInt3("大きい角", m_high.data());
        } else {
            if (m_target == Target::World)
                ImGui::Checkbox("左クリックのセルを中心に(つつきの代わり)", &m_picking);

            ImGui::InputInt3("中心", m_center.data());
            ImGui::InputInt3("半幅", m_halfCells.data());
            for (int& cells : m_halfCells)
                cells = std::max(cells, 0);
        }

        if (m_target == Target::World)
            ImGui::TextDisabled("仮の世界のセル(0〜%u。格子の外にはみ出してよい)", sim::PROBE_GRID_SIZE - 1);
        else
            ImGui::TextDisabled("実験室の箱のセル(細かくしたセルは、それを含む箱のセルで効く)");
    }

    bool PatchPanel::Compose(Composed& composed) const {
        if (m_table == nullptr || static_cast<size_t>(m_rule) >= m_table->rules.size())
            return false;

        const PatchKind& kind = PATCH_KINDS.at(m_kind);
        const int64_t strength = kind.strength == PatchStrength::Shift ? m_shift : m_joulesPerMol;
        composed.rule = static_cast<uint32_t>(m_rule);
        composed.effect = MakePatchEffect(kind.kind, strength);
        composed.durationTicks = static_cast<uint32_t>(m_durationTicks);
        for (size_t axis = 0; axis < 3; ++axis) {
            composed.low[axis] = m_corners ? m_low[axis] : m_center[axis] - m_halfCells[axis];
            composed.high[axis] = m_corners ? m_high[axis] : m_center[axis] + m_halfCells[axis];
            if (composed.low[axis] > composed.high[axis])
                return false;

            if (m_target == Target::Lab && composed.low[axis] < 0)
                return false;
        }

        return reaction::MpEffectValid(composed.effect);
    }

    PatchPanel::Placed PatchPanel::MakePlaced(Target target, const Composed& composed) {
        return {.target = target,
                .order = m_nextOrder++,
                .rule = m_table->ruleNames.at(composed.rule),
                .effect = composed.effect,
                .durationTicks = composed.durationTicks,
                .low = composed.low,
                .high = composed.high};
    }

    // --- 置く ---

    // 仮の世界: パッチのコマンド(次の TakeWorldCommands で返す)。足せたかは GPU のイベントで分かる
    bool PatchPanel::PlaceInWorld(const Composed& composed) {
        const sim::ProbePatchRange range = {.low = composed.low, .high = composed.high};
        m_worldQueue.push_back(sim::MakePatchCommand(0, 0, m_table->rules.at(composed.rule).key, composed.effect,
                                                     composed.durationTicks, range));
        const Placed placed = MakePlaced(Target::World, composed);
        m_message = std::format("仮の世界に投げた: {}・{}・{}・{} 刻み", placed.rule,
                                DescribePatchEffect(placed.effect), DescribeRange(placed.low, placed.high),
                                placed.durationTicks);
        m_placed.push_back(placed);
        Log(Channel::Sim, Level::Info, "パッチ: {}", m_message);

        return true;
    }

    // 実験室: 次の刻みのコマンド(LabSession::ApplyPatch)。断られるかは、箱の CPU リファレンスの列の写しに
    // 刻みと同じ関数(MpAddPatch)で足して決める(同じ刻みに置いたほかのパッチも写しに足してある)
    bool PatchPanel::PlaceInLab(LabPanel& lab, const Composed& composed) {
        if (!lab.EnsureSession()) {
            m_message = "実験室の箱を作れない";
            return false;
        }

        sim::LabSession& session = *lab.Session();
        const std::string& name = m_table->ruleNames.at(composed.rule);
        const auto found = std::ranges::find(session.Table().ruleNames, name);
        if (found == session.Table().ruleNames.end()) {
            m_message = std::format("箱の表に規則「{}」が無い", name);
            return false;
        }

        const auto boxRule = static_cast<uint32_t>(std::distance(session.Table().ruleNames.begin(), found));
        const sim::LabCellRange range = {.low = {.x = static_cast<uint32_t>(composed.low[0]),
                                                 .y = static_cast<uint32_t>(composed.low[1]),
                                                 .z = static_cast<uint32_t>(composed.low[2])},
                                         .high = {.x = static_cast<uint32_t>(composed.high[0]),
                                                  .y = static_cast<uint32_t>(composed.high[1]),
                                                  .z = static_cast<uint32_t>(composed.high[2])}};
        if (!session.ApplyPatch(boxRule, composed.effect, composed.durationTicks, range)) {
            m_message = "置けない(1 刻みのコマンドが一杯)";
            return false;
        }

        // --- 断られるか(刻みの初めの列の写しに足す)---
        const uint64_t tick = session.NextTick();
        if (m_labPredictTick != tick || session.PendingCommands() <= 1) {
            m_labPredictList = session.Cpu().magicPatches;
            m_labPredictTick = tick;
        }

        const reaction::RxRule& rule = session.Table().rules[boxRule];
        const reaction::MpPatch patch = reaction::MpPatchFromCommand(
            rule.key, composed.effect, composed.durationTicks, composed.low[0], composed.low[1], composed.low[2],
            composed.high[0], composed.high[1], composed.high[2], tick);
        const bool accepted = reaction::MpAddPatch(WordList{.words = m_labPredictList}, session.Table().View(), patch,
                                                   tick, static_cast<uint32_t>(session.Table().rules.size()));

        Placed placed = MakePlaced(Target::Lab, composed);
        placed.startTick = tick;
        placed.state = accepted ? State::Applied : State::Refused;
        m_message = std::format("実験室の刻み {} に置いた: {}・{}・{}・{} 刻み{}", tick, placed.rule,
                                DescribePatchEffect(placed.effect), DescribeRange(placed.low, placed.high),
                                placed.durationTicks, accepted ? "" : "(列が一杯なので断られる)");
        m_placed.push_back(placed);

        return true;
    }

    // --- 仮の世界 ---

    std::vector<sim::ProbeCommand> PatchPanel::TakeWorldCommands(uint64_t frame, uint32_t& sequence) {
        std::vector<sim::ProbeCommand> commands;
        if (m_autoLabDone && !m_autoWorldQueued && frame >= AUTO_WORLD_FRAME)
            QueueAutoWorld();

        for (sim::ProbeCommand command : m_worldQueue) {
            command.sequence = sequence++;
            commands.push_back(command);
        }

        m_worldQueue.clear();

        return commands;
    }

    void PatchPanel::PickCenter(const render::CellCoordinate& cell) {
        m_center = {static_cast<int>(cell.x), static_cast<int>(cell.y), static_cast<int>(cell.z)};
        m_corners = false;
    }

    void PatchPanel::NoteApplied(const sim::ProbeEvent& event) {
        Placed* oldest = nullptr;
        for (Placed& placed : m_placed) {
            const bool waiting = placed.target == Target::World && placed.state == State::Waiting;
            if (waiting && (oldest == nullptr || placed.order < oldest->order))
                oldest = &placed;
        }

        const bool refused = event.place == sim::PROBE_PATCH_REFUSED;
        if (oldest == nullptr) {
            Log(Channel::Sim, Level::Info, "パッチ: 刻み {} に{}(このパネルが投げたものでない)", event.tick,
                refused ? "断られた" : "足せた");
            return;
        }

        oldest->startTick = event.tick;
        oldest->state = refused ? State::Refused : State::Applied;
        Log(Channel::Sim, Level::Info, "パッチ: 刻み {} に {}・{} が{}", event.tick, oldest->rule,
            DescribePatchEffect(oldest->effect), refused ? "断られた" : std::format("列の {} 番に入った", event.place));
    }

    void PatchPanel::NoteRewound(uint64_t tick) {
        for (Placed& placed : m_placed) {
            if (placed.target == Target::World && placed.state != State::Waiting && placed.startTick >= tick) {
                placed.state = State::Waiting;
                placed.startTick = 0;
            }
        }
    }

    std::vector<BrushPreviewSegment> PatchPanel::WorldRangeSegments(uint32_t sliceAxis, uint32_t slicePlane) const {
        std::vector<BrushPreviewSegment> segments;
        const auto append = [&](const std::array<int32_t, 3>& low, const std::array<int32_t, 3>& high) {
            const BrushPreview preview = BuildPatchRangePreview(low, high, sliceAxis, slicePlane, sim::PROBE_GRID_SIZE);
            rng::copy(preview.segments, std::back_inserter(segments));
        };

        for (const Placed& placed : m_placed) {
            const bool live = placed.state == State::Waiting ||
                              (placed.state == State::Applied && placed.startTick + placed.durationTicks > m_worldTick);
            if (placed.target == Target::World && live)
                append(placed.low, placed.high);
        }

        Composed composed;
        if (m_target == Target::World && Compose(composed))
            append(composed.low, composed.high);

        return segments;
    }

    // 範囲の線: 断面で切った四角形は濃く、直方体の辺は薄く(筆の形と違う色)
    void PatchPanel::DrawRanges(std::span<const BrushPreviewLine> lines) {
        constexpr ImU32 SLICE_COLOR = IM_COL32(224, 96, 255, 255);
        constexpr ImU32 BOX_COLOR = IM_COL32(224, 96, 255, 110);
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        for (const BrushPreviewLine& line : lines) {
            drawList->AddLine({line.from[0], line.from[1]}, {line.to[0], line.to[1]},
                              line.onSlice ? SLICE_COLOR : BOX_COLOR, line.onSlice ? 2.0f : 1.0f);
        }
    }

    // --- 一覧 ---

    void PatchPanel::BuildList(LabPanel& lab) {
        ImGui::SeparatorText("置いたパッチ");
        const sim::LabSession* session = lab.Session();
        const uint64_t labTick = session != nullptr ? session->NextTick() : 0;
        const auto refused = [&](Target target) {
            return rng::count_if(m_placed,
                                 [&](const Placed& p) { return p.target == target && p.state == State::Refused; });
        };
        ImGui::Text("仮の世界: 刻み %llu・断られた %zu", static_cast<unsigned long long>(m_worldTick),
                    static_cast<size_t>(refused(Target::World)));
        if (session != nullptr)
            ImGui::Text("実験室: 刻み %llu・列 %u / %u・断られた %zu", static_cast<unsigned long long>(labTick),
                        reaction::MpPatchCount(ConstWordList{.words = session->Cpu().magicPatches}),
                        reaction::MP_MAX_PATCHES, static_cast<size_t>(refused(Target::Lab)));

        if (m_placed.empty())
            return;

        if (ImGui::Button("一覧を空にする"))
            m_placed.clear();

        if (!ImGui::BeginTable("patches", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
            return;

        ImGui::TableSetupColumn("先");
        ImGui::TableSetupColumn("規則");
        ImGui::TableSetupColumn("効き方");
        ImGui::TableSetupColumn("範囲");
        ImGui::TableSetupColumn("残り");
        ImGui::TableHeadersRow();

        const size_t first = m_placed.size() > MAX_LISTED ? m_placed.size() - MAX_LISTED : 0;
        for (size_t i = m_placed.size(); i-- > first;) {
            const Placed& placed = m_placed[i];
            const uint64_t now = placed.target == Target::World ? m_worldTick : labTick;
            const uint64_t end = placed.startTick + placed.durationTicks;
            std::string remaining;
            if (placed.state == State::Waiting)
                remaining = "待ち";
            else if (placed.state == State::Refused)
                remaining = "断られた";
            else if (now < placed.startTick)
                remaining = "(戻した)";
            else
                remaining = end > now ? std::format("{} 刻み", end - now) : "終わった";

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(placed.target == Target::World ? "世界" : "実験室");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(placed.rule.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(DescribePatchEffect(placed.effect).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(DescribeRange(placed.low, placed.high).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(remaining.c_str());
        }

        ImGui::EndTable();
    }

    // 実験室の断面(z = m_labSliceZ): 今効いているパッチが覆うセルを塗り、今選んでいる範囲を枠で(描画側だけ)
    void PatchPanel::BuildLabSlice(LabPanel& lab) {
        const sim::LabSession* session = lab.Session();
        if (session == nullptr) {
            ImGui::TextDisabled("実験室の箱はまだ無い");
            return;
        }

        const lab::LabBoxShape shape = session->Shape();
        const sim::LabCellRange whole = sim::LabWholeBox(shape);
        const auto edgeX = static_cast<int>(whole.high.x + 1);
        const auto edgeY = static_cast<int>(whole.high.y + 1);
        ImGui::SeparatorText("実験室の断面(紫 = 効いているパッチ・黄 = 選んでいる範囲)");
        ImGui::SliderInt("z", &m_labSliceZ, 0, static_cast<int>(whole.high.z));

        const ConstWordList list{.words = session->Cpu().magicPatches};
        const uint32_t count = reaction::MpPatchCount(list);
        const uint64_t tick = session->NextTick();
        const float pixels = std::min(LAB_CELL_MAX_PIXELS,
                                      LAB_SLICE_PIXELS / static_cast<float>(std::max(edgeX, edgeY)));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        Composed composed;
        const bool selected = m_target == Target::Lab && Compose(composed);
        for (int y = 0; y < edgeY; ++y) {
            for (int x = 0; x < edgeX; ++x) {
                uint32_t covering = 0;
                for (uint32_t i = 0; i < count; ++i) {
                    const reaction::MpPatch patch = reaction::MpLoadPatch(list, i);
                    const bool inside = x >= patch.lowX && x <= patch.highX && y >= patch.lowY && y <= patch.highY &&
                                        m_labSliceZ >= patch.lowZ && m_labSliceZ <= patch.highZ;
                    covering += inside && reaction::MpActiveAt(patch, tick) ? 1 : 0;
                }

                // 断面の上が y の大きい方(箱の上)
                const ImVec2 low = {origin.x + (static_cast<float>(x) * pixels),
                                    origin.y + (static_cast<float>(edgeY - 1 - y) * pixels)};
                const ImVec2 high = {low.x + pixels - 1.0f, low.y + pixels - 1.0f};
                const int shade = std::min(80 + (static_cast<int>(covering) * 60), 255);
                drawList->AddRectFilled(low, high,
                                        covering > 0 ? IM_COL32(shade, 64, shade, 255) : IM_COL32(40, 40, 48, 255));

                const bool inSelection = selected && x >= composed.low[0] && x <= composed.high[0] &&
                                         y >= composed.low[1] && y <= composed.high[1] &&
                                         m_labSliceZ >= composed.low[2] && m_labSliceZ <= composed.high[2];
                if (inSelection)
                    drawList->AddRect(low, high, IM_COL32(255, 220, 64, 255));
            }
        }

        ImGui::Dummy({static_cast<float>(edgeX) * pixels, static_cast<float>(edgeY) * pixels});
    }

    // --- 人がいない確認(--auto-patch)---

    // 実験室: 木を燃やす実験を、止めない・セルロースを使う規則を全部止める、の 2 回。止めた間は箱のセルロースが減らない
    void PatchPanel::RunAutoLab(LabPanel& lab) {
        m_autoLabDone = true;
        uint64_t plainLeft = 0;
        uint64_t stoppedLeft = 0;
        std::string failure = RunAutoLabExperiment(lab, false, plainLeft);
        if (failure.empty())
            failure = RunAutoLabExperiment(lab, true, stoppedLeft);

        const auto material = rng::find_if(lab.Materials(), [](const sim::LabMaterial& m) { return m.name == "木"; });
        const uint32_t cellulose = m_table->SpeciesId("cellulose");
        uint64_t placed = 0;
        if (material != lab.Materials().end()) {
            for (const sim::SpeciesAmount& amount : material->contents)
                placed += amount.species == cellulose ? amount.amount * 8 : 0;
        }

        if (failure.empty() && stoppedLeft != placed)
            failure = std::format("止めたのにセルロースが変わった(置いた {} µmol・残り {} µmol)", placed, stoppedLeft);

        if (failure.empty() && plainLeft >= placed)
            failure = std::format("止めない実験で木が燃えなかった(置いた {} µmol・残り {} µmol)", placed, plainLeft);

        if (!failure.empty()) {
            m_autoLabSummary = failure;
            Log(Channel::Sim, Level::Error, "--auto-patch: 実験室で失敗({})", failure);
            return;
        }

        m_autoLabPassed = true;
        m_autoLabSummary = std::format(
            "実験室: セルロースを使う規則を止めた {} 刻みで箱のセルロースは {} µmol のまま(止めない実験は {} µmol "
            "に減った)・"
            "列が一杯のパッチは断られた・記録から再生しても同じ",
            AUTO_PATCH_TICKS, stoppedLeft, plainLeft);
        Log(Channel::Sim, Level::Info, "--auto-patch: {}", m_autoLabSummary);
    }

    std::string PatchPanel::RunAutoLabExperiment(LabPanel& lab, bool stop, uint64_t& celluloseLeft) {
        if (!lab.EnsureSession())
            return "箱を作れない";

        sim::LabSession& session = *lab.Session();
        if (auto reset = session.Reset(); !reset)
            return reset.error();

        const sim::BakedReactionTable& table = session.Table();
        const uint32_t cellulose = table.SpeciesId("cellulose");
        const auto material = rng::find_if(lab.Materials(), [](const sim::LabMaterial& m) { return m.name == "木"; });
        const std::vector<uint32_t> rules = RulesConsuming(*m_table,
                                                           m_table->SpeciesId("cellulose"));  // パネルの表の ID
        if (material == lab.Materials().end() || rules.empty() || rules.size() >= reaction::MP_MAX_PATCHES)
            return std::format("表に木かセルロースを使う規則が無い(規則 {} 個)", rules.size());

        // --- 箱全体を酸素の多い 200 kPa の空気に・真ん中に木を 8 セル(--auto-lab と同じ)---
        const std::array<sim::SpeciesAmount, 2> atmosphere = {
            sim::SpeciesAmount{.species = table.SpeciesId("oxygen"),
                               .amount = sim::LabGasMicromoles(AUTO_OXYGEN_PASCAL, AUTO_ROOM_MILLIKELVIN)},
            sim::SpeciesAmount{.species = table.SpeciesId("nitrogen"),
                               .amount = sim::LabGasMicromoles(AUTO_NITROGEN_PASCAL, AUTO_ROOM_MILLIKELVIN)}};
        const std::vector<sim::SpeciesAmount> wood = material->contents;
        const sim::LabCellRange whole = sim::LabWholeBox(session.Shape());
        const bool placed = session.PlaceRegion(whole, atmosphere, AUTO_ROOM_MILLIKELVIN) &&
                            session.PlaceRegion(AUTO_WOOD_RANGE, wood, AUTO_ROOM_MILLIKELVIN);
        if (!placed)
            return "木を置けない";

        if (auto stepped = session.Step(AUTO_IGNITE_TICK); !stepped)
            return stepped.error();

        // --- 火を付けた刻みに、パネルと同じ道でパッチを置く(箱全体で止める。列を一杯にしてから 1 つ多く = 断られる)---
        static_cast<void>(session.SetTemperatureRegion(AUTO_WOOD_RANGE, AUTO_IGNITE_MILLIKELVIN));
        if (stop) {
            if (std::string failure = PlaceAutoLabPatches(lab, rules, whole); !failure.empty())
                return failure;
        }

        if (auto stepped = session.Step(AUTO_PATCH_TICKS); !stepped)
            return stepped.error();

        if (session.Mismatch())
            return session.Mismatch()->what;

        const uint32_t count = reaction::MpPatchCount(ConstWordList{.words = session.Cpu().magicPatches});
        if (stop && count != reaction::MP_MAX_PATCHES)
            return std::format("箱の列のパッチが {} 個(一杯の {} 個のはず)", count, reaction::MP_MAX_PATCHES);

        celluloseLeft = BoxTotalOf(session.Gpu(), session.Shape(), cellulose);

        // --- 記録から流し直しても同じ(パッチのコマンドも記録に入る)---
        const sim::LabRecording recording = session.Recording();
        if (auto replayed = session.Replay(recording); !replayed)
            return replayed.error();

        if (session.Mismatch() || session.ReplayDivergence() || session.Recording().hashes != recording.hashes)
            return "記録から流し直すと違う";

        return {};
    }

    // セルロースを使う規則を箱全体で止め、列を一杯にしてから 1 つ多く置く(最後の 1 つだけ断られるはず)。パネルの「置く」と同じ道
    std::string PatchPanel::PlaceAutoLabPatches(LabPanel& lab, std::span<const uint32_t> rules,
                                                sim::LabCellRange whole) {
        const size_t before = m_placed.size();
        const auto put = [&](uint32_t rule, uint32_t effect) {
            Composed composed{.rule = rule,
                              .effect = effect,
                              .durationTicks = AUTO_PATCH_TICKS + 1,
                              .low = {0, 0, 0},
                              .high = {static_cast<int32_t>(whole.high.x), static_cast<int32_t>(whole.high.y),
                                       static_cast<int32_t>(whole.high.z)}};
            return PlaceInLab(lab, composed);
        };

        bool queued = true;
        for (const uint32_t rule : rules)
            queued = queued && put(rule, MakePatchEffect(reaction::MP_KIND_STOP, 0));

        for (size_t i = rules.size(); i <= reaction::MP_MAX_PATCHES; ++i)
            queued = queued && put(rules.front(), MakePatchEffect(reaction::MP_KIND_SCALE, 0));

        if (!queued)
            return std::format("パッチを置けない({})", m_message);

        const bool lastRefused = m_placed.back().state == State::Refused;
        const bool othersApplied = std::all_of(m_placed.begin() + static_cast<std::ptrdiff_t>(before),
                                               m_placed.end() - 1,
                                               [](const Placed& p) { return p.state == State::Applied; });
        if (!lastRefused || !othersApplied)
            return "列が一杯のパッチだけが断られる、にならない";

        return {};
    }

    // 仮の世界: 木箱の壁のまわりでセルロースを使う規則を止める・触媒(格子の外にはみ出す)・期間 0(断られる)
    void PatchPanel::QueueAutoWorld() {
        m_autoWorldQueued = true;
        const std::vector<uint32_t> rules = RulesConsuming(*m_table, m_table->SpeciesId("cellulose"));
        if (rules.empty()) {
            Log(Channel::Sim, Level::Error, "--auto-patch: 表にセルロースを使う規則が無い");
            return;
        }

        Composed composed{.rule = rules.front(),
                          .effect = MakePatchEffect(reaction::MP_KIND_STOP, 0),
                          .durationTicks = AUTO_WORLD_TICKS};
        for (size_t axis = 0; axis < 3; ++axis) {
            composed.low[axis] = AUTO_WORLD_CENTER[axis] - AUTO_WORLD_HALF_CELLS;
            composed.high[axis] = AUTO_WORLD_CENTER[axis] + AUTO_WORLD_HALF_CELLS;
        }

        const auto put = [&](const Composed& patch, State expected) {
            m_autoWorldExpected.emplace_back(m_nextOrder, expected);
            PlaceInWorld(patch);
        };

        put(composed, State::Applied);

        Composed catalyst = composed;
        catalyst.effect = MakePatchEffect(reaction::MP_KIND_CATALYST, AUTO_WORLD_CATALYST_JOULES_PER_MOL);
        catalyst.low = {-4, -4, -4};
        catalyst.high = {3, 3, 3};
        put(catalyst, State::Applied);

        Composed empty = composed;
        empty.durationTicks = 0;
        put(empty, State::Refused);
    }

    bool PatchPanel::AutoPassed() const {
        if (!m_autoLabPassed || m_autoWorldExpected.empty())
            return false;

        return rng::all_of(m_autoWorldExpected, [&](const std::pair<uint32_t, State>& expected) {
            const auto placed = rng::find(m_placed, expected.first, &Placed::order);
            return placed != m_placed.end() && placed->state == expected.second;
        });
    }

    std::string PatchPanel::AutoSummary() const {
        const auto delivered = rng::count_if(m_autoWorldExpected, [&](const std::pair<uint32_t, State>& expected) {
            const auto placed = rng::find(m_placed, expected.first, &Placed::order);
            return placed != m_placed.end() && placed->state == expected.second;
        });

        return std::format(
            "{}。仮の世界: 投げた {} 個のうち {} 個が思った結果で戻った(止める・格子の外にはみ出す触媒は足せた・"
            "期間 0 は断られた)",
            m_autoLabSummary.empty() ? "実験室: まだ" : m_autoLabSummary, m_autoWorldExpected.size(),
            static_cast<size_t>(delivered));
    }

}  // namespace bicameral::editor
