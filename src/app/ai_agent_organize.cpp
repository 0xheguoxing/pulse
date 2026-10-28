// ai_agent_organize.cpp - Pulse 助手's organize_files: the plan card, the confirm and the moves.
// Split from ai_assistant_agent.cpp; the shared parts are in ai_agent_internal.h.
#include "ai_agent_internal.h"

namespace pulse {
namespace ai_agent {

// organize_files after validation.
struct OrganizePlan {
    std::wstring folder;
    std::wstring label;   // FolderLabel(folder)
    struct Group {
        std::wstring name;
        std::wstring path;
        bool exists = false;
        std::vector<std::wstring> items;   // names
    };
    std::vector<Group> groups;
    size_t total = 0;
    size_t created = 0;   // groups whose folder is new
    uint64_t undo_group = 0;
    std::vector<std::wstring> sources;   // every moved item's full path
};

bool ValidName(const std::wstring& name) {
    if (name.empty() || name.size() > 80 || name == L"." || name == L"..") return false;
    for (wchar_t ch : name)
        if (ch < 32 || wcschr(L"\\/:*?\"<>|", ch)) return false;
    if (name.back() == L'.' || name.back() == L' ') return false;
    static const wchar_t* kReserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4",
                                         L"LPT1", L"LPT2", L"LPT3"};
    std::wstring stem = name.substr(0, name.find(L'.'));
    for (const wchar_t* r : kReserved)
        if (_wcsicmp(stem.c_str(), r) == 0) return false;
    return true;
}

std::wstring Trim(std::wstring v) {
    while (!v.empty() && std::iswspace(v.back())) v.pop_back();
    size_t i = 0;
    while (i < v.size() && std::iswspace(v[i])) ++i;
    return v.substr(i);
}

bool PlanOrganize(const json::Value& args, OrganizePlan& plan, ToolOutcome& error) {
    if (!app::ai::CheckPathArg(args.find("folder") ? *args.find("folder") : json::Value(), "folder", plan.folder, error))
        return false;
    if (!IsDirectory(plan.folder)) {
        error = ToolOutcome::Fail("not_found", "folder does not exist or is not a folder");
        return false;
    }
    if (app::ai::IsSystemFolder(plan.folder)) {
        error = ToolOutcome::Fail("forbidden", "Pulse does not reorganize system folders");
        return false;
    }
    const json::Value* groups = args.find("groups");
    if (!groups || !groups->is_array() || groups->size() == 0 || groups->size() > 30) {
        error = ToolOutcome::Fail("invalid_args", "groups must list 1-30 destination folders");
        return false;
    }
    std::vector<std::wstring> seen;   // lower-cased item names already planned
    for (const json::Value& g : groups->items()) {
        OrganizePlan::Group group;
        group.name = Trim(json::Wide(g.get_string("name")));
        if (!ValidName(group.name)) {
            error = ToolOutcome::Fail("invalid_args", "invalid folder name: " + Utf8(group.name));
            return false;
        }
        for (const OrganizePlan::Group& other : plan.groups)
            if (_wcsicmp(other.name.c_str(), group.name.c_str()) == 0) {
                error = ToolOutcome::Fail("invalid_args", "two groups share the name " + Utf8(group.name));
                return false;
            }
        group.path = JoinPath(plan.folder, group.name);
        group.exists = IsDirectory(group.path);
        if (!group.exists && Exists(group.path)) {
            error = ToolOutcome::Fail("conflict", "a file named " + Utf8(group.name) + " already exists in the folder");
            return false;
        }
        const json::Value* items = g.find("items");
        if (!items || !items->is_array()) {
            error = ToolOutcome::Fail("invalid_args", "each group needs items");
            return false;
        }
        for (const json::Value& it : items->items()) {
            std::wstring name = Trim(json::Wide(it.as_string()));
            // A full path inside the folder is fine too.
            if (name.size() > plan.folder.size() && _wcsnicmp(name.c_str(), plan.folder.c_str(), plan.folder.size()) == 0) {
                std::wstring rest = name.substr(plan.folder.size());
                if (!rest.empty() && (rest[0] == L'\\' || rest[0] == L'/')) rest.erase(0, 1);
                name = rest;
            }
            if (name.empty() || name.find_first_of(L"\\/") != std::wstring::npos || name == L"." || name == L"..") {
                error = ToolOutcome::Fail("invalid_args", "items must be names directly inside folder: " + Utf8(name));
                return false;
            }
            if (_wcsicmp(name.c_str(), group.name.c_str()) == 0) continue;   // the folder itself
            bool is_target = false;
            for (const json::Value& g2 : groups->items())
                if (_wcsicmp(Trim(json::Wide(g2.get_string("name"))).c_str(), name.c_str()) == 0) is_target = true;
            if (is_target) continue;   // a destination folder never moves
            std::wstring lower = name;
            for (wchar_t& ch : lower) ch = static_cast<wchar_t>(std::towlower(ch));
            if (std::find(seen.begin(), seen.end(), lower) != seen.end()) continue;
            if (!Exists(JoinPath(plan.folder, name))) {
                error = ToolOutcome::Fail("not_found", "no item named " + Utf8(name) + " in the folder");
                return false;
            }
            seen.push_back(lower);
            group.items.push_back(name);
        }
        if (group.items.empty()) continue;
        plan.total += group.items.size();
        if (!group.exists) ++plan.created;
        plan.groups.push_back(std::move(group));
    }
    if (plan.groups.empty()) {
        error = ToolOutcome::Fail("invalid_args", "nothing to move");
        return false;
    }
    if (plan.total > kMaxOrganize) {
        error = ToolOutcome::Fail("too_many", "at most 2000 items per call");
        return false;
    }
    plan.label = FolderLabel(plan.folder);
    for (const auto& g : plan.groups)
        for (const auto& name : g.items) plan.sources.push_back(JoinPath(plan.folder, name));
    return true;
}

std::wstring GroupsText(const OrganizePlan& plan, I all_new, I any) {
    return Fill(Fill(Text(plan.created == plan.groups.size() ? all_new : any), L"{n}", Num(plan.total)), L"{g}",
                Num(plan.groups.size()));
}

void SettleOrganize(AppState& s, std::shared_ptr<OrganizePlan> plan, const std::string& call_id, size_t moved);

// One request of the sort: a new folder, one item, or a whole group.
struct OrganizeAct {
    enum Kind { Create, MoveOne, MoveGroup } kind = Create;
    size_t group = 0;
    size_t item = 0;
    uint32_t delay = 0;   // ms after the previous one
};

void SubmitAct(AppState& s, const OrganizePlan& plan, const OrganizeAct& act, const std::wstring& undo_label,
               ui::AiListOverlay* ov) {
    const OrganizePlan::Group& g = plan.groups[act.group];
    const uint64_t now = Now();
    ops::OpRequest r;
    r.undo_group = plan.undo_group;   // one undo step for the whole sort, like 合并到文件夹
    r.undo_label = undo_label;
    if (act.kind == OrganizeAct::Create) {
        r.type = ops::OpType::CreateFolder;
        r.sources.push_back(g.path);
        if (ov) ov->new_folders[ui::AiListLower(g.name)] = now;
    } else {
        r.type = ops::OpType::Move;
        r.dest_dir = g.path;
        r.collision_policy = ops::CollisionPolicy::KeepBoth;
        const size_t first = act.kind == OrganizeAct::MoveOne ? act.item : 0;
        const size_t last = act.kind == OrganizeAct::MoveOne ? act.item + 1 : g.items.size();
        for (size_t i = first; i < last; ++i) {
            r.sources.push_back(JoinPath(plan.folder, g.items[i]));
            if (ov) ov->leaving[ui::AiListLower(g.items[i])] = now;
        }
        // The folder bumps as the card lands (organizeAnim: +470 ms).
        if (ov) ov->bumps[ui::AiListLower(g.name)].push_back(now + 470);
    }
    s.ops.Submit(std::move(r));
}

void RunOrganize(AppState& s, std::shared_ptr<OrganizePlan> plan, const std::string& call_id) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    a.organized = true;
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    plan->undo_group = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    const std::wstring undo_label = Text(I::AiAsstUndoLabel);
    // On screen the sort plays like the design (scene 5): the new folders
    // open 120 ms apart, then the items slide into them one by one; a big
    // sort, or one off screen, moves a group at a time.
    std::shared_ptr<ui::AiListOverlay> ov;
    if (Shown(s, plan->folder)) {
        ListOverlay(plan->folder);
        ov = g_list;
        ov->new_badge = Text(I::AiAsstNewBadge);
        ov->new_folders.clear();
        ov->leaving.clear();
        ov->target.clear();
        ov->targets.clear();
        ov->bumps.clear();
        for (const auto& g : plan->groups) {
            const std::wstring folder = ui::AiListLower(g.name);
            ov->targets.insert(folder);
            for (const auto& name : g.items) ov->target[ui::AiListLower(name)] = folder;
        }
    }
    auto acts = std::make_shared<std::vector<OrganizeAct>>();
    for (size_t gi = 0; gi < plan->groups.size(); ++gi)
        if (!plan->groups[gi].exists)
            acts->push_back({OrganizeAct::Create, gi, 0, ov ? (acts->empty() ? 300u : 120u) : 0u});
    const uint32_t first_move = ov ? (acts->empty() ? 300u : 120u + 420u) : 0u;
    if (ov && plan->total <= 60) {
        // Items in list order (the snapshot's), each its own request.
        std::vector<std::pair<size_t, size_t>> order;
        for (size_t gi = 0; gi < plan->groups.size(); ++gi)
            for (size_t ii = 0; ii < plan->groups[gi].items.size(); ++ii) order.push_back({gi, ii});
        if (app::Tab* tab = ActiveTab(s); tab && tab->EntryCount() <= 20000) {
            std::unordered_map<std::wstring, size_t> at;
            for (size_t i = 0; i < tab->EntryCount(); ++i) at.emplace(ui::AiListLower(tab->EntryAt(i).name), i);
            auto rank = [&](const std::pair<size_t, size_t>& p) {
                auto it = at.find(ui::AiListLower(plan->groups[p.first].items[p.second]));
                return it == at.end() ? SIZE_MAX : it->second;
            };
            std::stable_sort(order.begin(), order.end(),
                             [&](const auto& l, const auto& r) { return rank(l) < rank(r); });
        }
        const uint32_t step = (std::max)(20u, (std::min)(190u, static_cast<uint32_t>(3000 / (std::max<size_t>)(plan->total, 1))));
        for (size_t k = 0; k < order.size(); ++k)
            acts->push_back({OrganizeAct::MoveOne, order[k].first, order[k].second, k == 0 ? first_move : step});
    } else {
        for (size_t gi = 0; gi < plan->groups.size(); ++gi)
            acts->push_back({OrganizeAct::MoveGroup, gi, 0, gi == 0 ? first_move : (ov ? 190u : 0u)});
    }
    // The progress step.
    FoldSteps(s, a);
    ui::AiStepView& st = AddStep(s, a, ui::AiIcon::Move, Text(I::AiAsstStepMove), plan->label,
                                 Fill(Text(I::AiAsstStepMoveRun), L"{g}", Num(plan->created)));
    st.progress = 0.0f;
    AiChatSetChip(c, ui::StatusAiState::Busy,
                  Fill(Fill(Text(I::AiAsstChipOrganizing), L"{k}", L"0"), L"{n}", Num(plan->total)), 0.0f);
    struct Watch {
        size_t moved = 0;
        uint64_t last_change = 0;
        uint64_t started = 0;
        uint64_t polled = 0;
        size_t next = 0;        // the next act
        uint64_t next_at = 0;
    };
    auto w = std::make_shared<Watch>();
    w->started = w->last_change = Now();
    w->next_at = Now() + (acts->empty() ? 0 : acts->front().delay);
    const uint64_t gen = a.generation;
    g_flush_sort = [plan, w, acts, undo_label](AppState& st2) {
        while (w->next < acts->size()) SubmitAct(st2, *plan, (*acts)[w->next++], undo_label, nullptr);
    };
    AiChatPush(s, 0, [plan, call_id, w, gen, acts, ov, undo_label](AppState& st2) {
        AiAgent& ag = Agent(st2);
        if (ag.generation != gen) {
            while (w->next < acts->size()) SubmitAct(st2, *plan, (*acts)[w->next++], undo_label, nullptr);
            return -1;
        }
        const uint64_t now = Now();
        // Submit what is due.
        while (w->next < acts->size() && now >= w->next_at) {
            SubmitAct(st2, *plan, (*acts)[w->next], undo_label, ov.get());
            ++w->next;
            if (w->next < acts->size()) w->next_at = now + (*acts)[w->next].delay;
            w->last_change = now;
        }
        const bool fed = w->next >= acts->size();
        if (fed) g_flush_sort = nullptr;
        if (now - w->polled >= 150 || fed) {
            w->polled = now;
            size_t gone = 0;
            for (const auto& src : plan->sources)
                if (!Exists(src)) ++gone;
            if (gone != w->moved) {
                w->moved = gone;
                w->last_change = now;
            }
            const float p = plan->total ? static_cast<float>(gone) / static_cast<float>(plan->total) : 1.0f;
            if (ui::AiStepView* step = LastStep(st2, ag)) {
                step->progress = p;
                if (gone) step->sub = Fill(Fill(Text(I::AiAsstStepMoving), L"{k}", Num(gone)), L"{n}", Num(plan->total));
            }
            AiChatSetChip(Chat(st2), ui::StatusAiState::Busy,
                          Fill(Fill(Text(I::AiAsstChipOrganizing), L"{k}", Num(gone)), L"{n}", Num(plan->total)), p);
            const bool idle = !st2.ops.Status().active;
            const bool all = gone >= plan->total;
            // Everything moved; or the queue went quiet with no progress (a
            // failure or a skipped item); or ten minutes passed.
            if (fed && (all || (idle && now - w->last_change > 2500) || now - w->started > 600000)) {
                // Let the last card land and its folder bump (organizeAnim: +650 ms).
                if (ov && all && now - w->last_change < 650) return 650 - static_cast<int>(now - w->last_change);
                SettleOrganize(st2, plan, call_id, gone);
                return -1;
            }
        }
        uint64_t wait = 150;
        if (!fed) wait = (std::min)(wait, w->next_at > now ? w->next_at - now : 1);
        return static_cast<int>(wait);
    });
}

// The result card's 撤销: one group undo through the queue, then the card
// turns grey once the items are back.
std::function<void(AppState&)> MakeUndo(std::shared_ptr<OrganizePlan> plan) {
    return [plan](AppState& st) {
        AiChat& c2 = Chat(st);
        const auto outcome = st.ops.UndoGroup(plan->undo_group);
        if (outcome != ops::OpsManager::GroupUndo::Started) {
            AiBlockView& note = c2.Add(AiBlockKind::Note, Now());
            const bool pending = outcome == ops::OpsManager::GroupUndo::Pending;
            note.text = Text(pending ? I::AiAsstUndoBusy : I::AiAsstUndoGone);
            if (pending) c2.on_undo = MakeUndo(plan);   // the button works again later
            Redraw(st);
            return;
        }
        AiChatSetChip(c2, ui::StatusAiState::Busy, Text(I::AiAsstChipUndoing), 0.5f);
        c2.peek_undo = false;
        AiChatHidePeek(st);
        if (ui::AiListOverlay* o = OverlayOf(plan->folder)) {
            // undoAnim: the folders the sort made slide out; the items come
            // back with the list's own appear motion.
            const uint64_t now = Now();
            o->dests.clear();
            o->new_folders.clear();
            o->target.clear();
            o->targets.clear();
            o->bumps.clear();
            o->leaving.clear();
            for (const auto& g : plan->groups)
                if (!g.exists) o->leaving[ui::AiListLower(g.name)] = now;
            o->end_ms = now + 4000;
        }
        auto started = std::make_shared<uint64_t>(Now());
        AiChatPush(st, 300, [plan, started](AppState& s3) {
            size_t back = 0;
            for (const auto& src : plan->sources)
                if (Exists(src)) ++back;
            if (back < plan->total && !(Now() - *started > 3000 && !s3.ops.Status().active) &&
                Now() - *started < 600000)
                return 150;
            AiChat& c3 = Chat(s3);
            if (AiBlockView* rr = c3.Find(c3.result_id)) {
                rr->done = true;
                rr->title = Text(I::AiAsstUndone);
                rr->sub = plan->created
                              ? Fill(Fill(Fill(Text(I::AiAsstUndoneSub), L"{n}", Num(back)), L"{folder}", plan->label),
                                     L"{g}", Num(plan->created))
                              : Fill(Fill(Text(I::AiAsstUndoneSubAny), L"{n}", Num(back)), L"{folder}", plan->label);
            }
            AiBlockView& note = c3.Add(AiBlockKind::Note, Now());
            note.text = Text(I::AiAsstUndoNote);
            AiChatSetChip(c3, ui::StatusAiState::Done, Text(I::AiAsstUndone));
            if (c3.agent) c3.agent->pending_note = L"(The user clicked Undo in Pulse: the last organize_files was reversed and every item is back where it was.)";
            Redraw(s3);
            return -1;
        });
    };
}

void SettleOrganize(AppState& s, std::shared_ptr<OrganizePlan> plan, const std::string& call_id, size_t moved) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    const bool all = moved >= plan->total;
    EndStep(s, a, moved > 0,
            all ? GroupsText(*plan, I::AiAsstStepMoveDone, I::AiAsstStepMoveDoneAny)
                : Fill(Fill(Text(I::AiAsstMovedSome), L"{k}", Num(moved)), L"{n}", Num(plan->total)));
    if (AiBlockView* b = Block(s, a.steps_id)) {
        b->label = GroupsText(*plan, I::AiAsstStepsMove, I::AiAsstStepsMoveAny);
        b->collapsed = true;
        b->open = false;
    }
    a.steps_id = 0;
    // The result card.
    AiBlockView& r = c.Add(AiBlockKind::Result, Now());
    r.title = Fill(Text(I::AiAsstResultTitle), L"{n}", Num(moved));
    r.sub = Fill(Fill(Text(plan->created == plan->groups.size() ? I::AiAsstResultSub : I::AiAsstResultSubAny),
                      L"{folder}", plan->label),
                 L"{g}", Num(plan->groups.size()));
    for (const auto& g : plan->groups) {
        ui::AiIcon icon = ui::AiIcon::Folder;
        ui::AiTint tint = ui::AiTint::Neutral;
        AiChatGroupLook(g.items, icon, tint);
        r.tags.push_back({tint, g.name + L" " + Num(g.items.size())});
    }
    r.deny = Text(I::AiAsstUndo);
    r.accept = Text(I::AiAsstShowInPulse);
    c.result_id = r.id;
    c.on_show = [plan](AppState& st) { NavigateTo(st, plan->folder); };
    c.on_undo = MakeUndo(plan);
    // The peek bubble's line (shown if the panel is closed when the turn ends).
    std::wstring names;
    for (const auto& g : plan->groups) {
        if (!names.empty()) names += L"、";
        names += g.name;
    }
    c.peek_text = Fill(Fill(Fill(Text(plan->created == plan->groups.size() ? I::AiPeekSorted : I::AiPeekSortedAny),
                                 L"{n}", Num(moved)),
                            L"{g}", Num(plan->groups.size())),
                       L"{names}", names);
    c.peek_undo = moved > 0;
    if (ui::AiListOverlay* o = OverlayOf(plan->folder)) o->end_ms = Now() + 6000;
    // Back to the model.
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("ok", moved > 0);
    out.data.set("moved", static_cast<uint64_t>(moved));
    out.data.set("total", static_cast<uint64_t>(plan->total));
    json::Value folders = json::Value::Array();
    for (const auto& g : plan->groups) {
        json::Value f = json::Value::Object();
        f.set("name", Utf8(g.name));
        f.set("path", Utf8(g.path));
        f.set("created", !g.exists);
        f.set("items", static_cast<uint64_t>(g.items.size()));
        folders.push(std::move(f));
    }
    out.data.set("folders", std::move(folders));
    out.data.set("undo", "The user can undo the whole sort with the Undo button on the result card in Pulse.");
    if (!all) out.data.set("note", "Some items did not move (locked, skipped or failed).");
    AddToolResult(a, call_id, out);
    ++a.call_index;
    AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
    AiChatPush(s, 350, [](AppState& st) { NextTool(st); return -1; });
}

void ToolOrganize(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    auto plan = std::make_shared<OrganizePlan>();
    ToolOutcome error;
    if (!PlanOrganize(args, *plan, error)) {
        AddToolResult(a, call.id, error);
        ++a.call_index;
        NextTool(s);
        return;
    }
    FoldSteps(s, a);
    // The plan card, its groups sliding in.
    AiBlockView& b = c.Add(AiBlockKind::Plan, Now());
    b.label = Text(I::AiAsstPlanTitle);
    b.meta = Fill(Fill(Text(plan->created == plan->groups.size() ? I::AiAsstPlanMeta : I::AiAsstPlanMetaAny), L"{g}",
                       Num(plan->groups.size())),
                  L"{n}", Num(plan->total));
    for (const auto& g : plan->groups) {
        ui::AiGroupView v;
        AiChatGroupLook(g.items, v.icon, v.tint);
        v.name = g.name;
        v.count = Fill(Text(I::AiAsstCount), L"{n}", Num(g.items.size()));
        for (size_t i = 0; i < g.items.size() && i < 40; ++i) {
            if (!v.list.empty()) v.list += L"\u3001";
            v.list += g.items[i];
        }
        if (!g.exists) v.badge = Text(I::AiAsstNewBadge);
        b.groups.push_back(std::move(v));
    }
    const uint32_t plan_id = b.id;
    const uint64_t gen = a.generation;
    auto gi = std::make_shared<size_t>(0);
    AiChatPush(s, 0, [plan_id, gi, gen](AppState& st) {
        if (Agent(st).generation != gen) return -1;
        AiBlockView* pb = Chat(st).Find(plan_id);
        if (!pb || *gi >= pb->groups.size()) return -1;
        pb->groups[(*gi)++].in_ms = Now();
        return *gi < pb->groups.size() ? 170 : -1;
    });
    // The amber card; the turn waits here for the user.
    const std::string call_id = call.id;
    AiChatPush(s, 400, [plan, plan_id, call_id, gen](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != gen) return -1;
        AiChat& ch = Chat(st);
        AiChatSetChip(ch, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
        if (!st.aiAssistant.open) ch.unread = true;
        AiBlockView& cf = ch.Add(AiBlockKind::Confirm, Now());
        cf.title = GroupsText(*plan, I::AiAsstConfirmTitle, I::AiAsstConfirmTitleAny);
        cf.sub = Fill(Text(I::AiAsstConfirmSub), L"{folder}", plan->label);
        cf.deny = Text(I::AiAsstDeny);
        cf.accept = Text(I::AiAsstOrganize);
        ch.confirm_id = cf.id;
        // Each item shows its folder in the list before you decide (scene 4).
        if (Shown(st, plan->folder)) {
            ui::AiListOverlay& o = ListOverlay(plan->folder);
            o.dests.clear();
            for (const auto& g : plan->groups) {
                ui::AiIcon icon = ui::AiIcon::Folder;
                ui::AiTint tint = ui::AiTint::Neutral;
                AiChatGroupLook(g.items, icon, tint);
                for (const auto& name : g.items)
                    o.dests[ui::AiListLower(name)] = {g.name, static_cast<uint8_t>(tint)};
            }
            o.dest_start = Now();
            o.dest_end = 0;
        }
        ch.on_confirm = [plan, plan_id, call_id, gen](AppState& s2, bool accept) {
            AiChat& c2 = Chat(s2);
            AiAgent& a2 = Agent(s2);
            AiBlockView* card = c2.Find(c2.confirm_id);
            if (card) {
                const uint64_t now = Now();
                if (now > card->born_ms) c2.turn_start += now - card->born_ms;   // waiting is not model time
                card->done = true;
                card->title = Fill(Text(accept ? I::AiAsstConfirmed : I::AiAsstDenied), L"{n}", Num(plan->total));
            }
            if (AiBlockView* pb = c2.Find(plan_id)) pb->collapsed = true;
            c2.confirm_id = 0;
            c2.on_confirm = nullptr;
            if (a2.generation != gen) return;
            if (!accept) {
                if (ui::AiListOverlay* o = OverlayOf(plan->folder)) {
                    o->dest_end = Now();
                    o->end_ms = Now() + 500;
                }
                AddToolResult(a2, call_id,
                              ToolOutcome::Fail("user_denied",
                                                "The user declined this plan in Pulse; nothing moved. Do not propose "
                                                "the same plan again; ask what they would like instead."));
                ++a2.call_index;
                AiChatSetChip(c2, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                NextTool(s2);
                Redraw(s2);
                return;
            }
            RunOrganize(s2, plan, call_id);
            Redraw(s2);
        };
        return -1;
    });
}
} // namespace ai_agent

} // namespace pulse
