// ai_agent_space.cpp - Pulse 助手's freeing space (D): the disk card, the scan and the cleanup.
// Split from ai_assistant_agent.cpp; the shared parts are in ai_agent_internal.h.
#include "ai_agent_internal.h"

namespace pulse {
namespace ai_agent {

double Gb(uint64_t bytes) { return static_cast<double>(bytes) / kGiB; }

std::wstring Gb1(uint64_t bytes) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f", Gb(bytes));
    // A few megabytes must not read as nothing (0.0 GB).
    if (bytes > 0 && wcscmp(buf, L"0.0") == 0) return L"<0.1";
    return buf;
}

// Fills "{gb} GB"; under 1 GB the whole "{gb} GB" becomes MB / KB, so a small
// batch (duplicates) reads 13.5 MB instead of <0.1 GB.
std::wstring FillGb(std::wstring t, uint64_t bytes) {
    if (bytes >= static_cast<uint64_t>(kGiB)) return Fill(t, L"{gb}", Gb1(bytes));
    wchar_t buf[32];
    if (bytes >= 1024ull * 1024) swprintf_s(buf, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else swprintf_s(buf, L"%llu KB", static_cast<unsigned long long>((bytes + 1023) / 1024));
    const size_t at = t.find(L"{gb} GB");
    if (at != std::wstring::npos) return t.replace(at, 7, buf);
    return Fill(t, L"{gb}", Gb1(bytes));
}

// 221.0 -> 221 (the big used figure).
std::wstring GbTrim(uint64_t bytes) {
    std::wstring v = Gb1(bytes);
    if (v.size() > 2 && v.compare(v.size() - 2, 2, L".0") == 0) v.resize(v.size() - 2);
    return v;
}

// 1,337 (zh / en) - the design's INT().
std::wstring Int(uint64_t n) {
    std::wstring d = std::to_wstring(n), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out += L',';
        out += d[i];
    }
    return out;
}

double Round1(uint64_t bytes) { return std::round(Gb(bytes) * 10.0) / 10.0; }

wchar_t DriveArg(const json::Value& args) {
    const std::string d = args.get_string("drive");
    wchar_t c = d.empty() ? L'C' : static_cast<wchar_t>(std::towupper(static_cast<wchar_t>(static_cast<unsigned char>(d[0]))));
    return c >= L'A' && c <= L'Z' ? c : 0;
}

struct SpaceDisk {
    app::ai::DiskUsage du;
    uint32_t card_id = 0;
};

std::shared_ptr<SpaceDisk> g_space_disk;     // the latest disk card
std::shared_ptr<SpaceClean> g_space_clean;   // the latest cleanup

ui::AiIcon CleanIcon(app::ai::CleanKind k) {
    using K = app::ai::CleanKind;
    switch (k) {
    case K::Installer: return ui::AiIcon::Install;
    case K::Extracted: return ui::AiIcon::Package;
    case K::Copy: return ui::AiIcon::Copy;
    case K::Temp: return ui::AiIcon::Temp;
    default: return ui::AiIcon::Video;
    }
}

ui::AiTint CleanTint(app::ai::CleanKind k) {
    using K = app::ai::CleanKind;
    switch (k) {
    case K::Installer: return ui::AiTint::App;
    case K::Extracted: return ui::AiTint::Archive;
    case K::Copy: return ui::AiTint::Document;
    case K::Temp: return ui::AiTint::Image;
    default: return ui::AiTint::Video;
    }
}

I CleanTitleId(app::ai::CleanKind k) {
    using K = app::ai::CleanKind;
    switch (k) {
    case K::Installer: return I::AiClCatInstaller;
    case K::Extracted: return I::AiClCatExtracted;
    case K::Copy: return I::AiClCatCopy;
    case K::Temp: return I::AiClCatTemp;
    default: return I::AiClCatVideo;
    }
}

const char* CleanKey(app::ai::CleanKind k) {
    using K = app::ai::CleanKind;
    switch (k) {
    case K::Installer: return "installers";
    case K::Extracted: return "extracted_archives";
    case K::Copy: return "duplicate_downloads";
    case K::Temp: return "temp_files";
    default: return "old_videos";
    }
}

// One file's reason (the list tab's 原因 column).
std::wstring CleanReason(app::ai::CleanKind k, const app::ai::CleanItem& it) {
    using K = app::ai::CleanKind;
    switch (k) {
    case K::Installer: return Fill(Text(I::AiClWhyInstaller), L"{app}", it.arg);
    case K::Extracted: return Fill(Text(I::AiClWhyExtracted), L"{folder}", Leaf(it.arg));
    case K::Copy: return Fill(Text(I::AiClWhyCopy), L"{name}", Leaf(it.arg));
    case K::Temp: return Fill(Text(I::AiClWhyTemp), L"{n}", Num(it.days));
    default: return Fill(Text(I::AiClWhyVideo), L"{n}", Num(it.days));
    }
}

// Where a category's files are: 下载, or 下载 · 桌面.
std::wstring CleanWhere(const app::ai::CleanCategory& c) {
    std::vector<std::pair<std::wstring, size_t>> where;
    for (const auto& it : c.items) {
        const std::wstring l = FolderLabel(app::ai::ParentFolder(it.path));
        auto f = std::find_if(where.begin(), where.end(), [&](const auto& w) { return w.first == l; });
        if (f == where.end()) where.push_back({l, 1});
        else ++f->second;
    }
    std::stable_sort(where.begin(), where.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
    std::wstring out;
    for (size_t i = 0; i < where.size() && i < 2; ++i) out += (i ? L" \u00B7 " : L"") + where[i].first;
    return out;
}

void DiskSegments(ui::AiDiskView& d, const app::ai::DiskUsage& du, uint64_t in_bin) {
    const double t = du.total ? static_cast<double>(du.total) : 1.0;
    const uint64_t users = du.users > in_bin ? du.users - in_bin : 0;
    d.seg[0] = static_cast<float>(du.system / t);
    d.seg[1] = static_cast<float>(du.programs / t);
    d.seg[2] = static_cast<float>(users / t);
    d.seg[3] = static_cast<float>(du.other / t);
    d.seg[4] = static_cast<float>(in_bin / t);
    d.legend_recycle = Fill(Text(I::AiDkLegRecycle), L"{gb}", Gb1(in_bin));
}

void FillDiskCard(AiBlockView& b, const app::ai::DiskUsage& du) {
    b.label = Fill(Text(I::AiDkTitle), L"{d}", std::wstring(1, du.drive));
    b.meta = du.indexed ? Fill(Text(I::AiDkMetaIndex), L"{t}", Secs(du.ms)) : Text(I::AiDkMetaNoIndex);
    b.hint_icon = du.indexed ? ui::AiIcon::Bolt : ui::AiIcon::Count;
    ui::AiDiskView& d = b.disk;
    const uint64_t used = du.total - (std::min)(du.free, du.total);
    d.used_gb = d.used_from = static_cast<float>(Gb(used));
    d.free_gb = d.free_from = static_cast<float>(Gb(du.free));
    d.used_of = Fill(Text(I::AiDkUsedOf), L"{total}", GbTrim(du.total));
    d.free_unit = Text(I::AiDkFreeUnit);
    d.legend[0] = Fill(Text(I::AiDkLegSys), L"{gb}", Gb1(du.system));
    d.legend[1] = Fill(Text(I::AiDkLegProg), L"{gb}", Gb1(du.programs));
    std::wstring user = Fill(Fill(Text(I::AiDkLegUser), L"{name}", du.user_name), L"{gb}", Gb1(du.users));
    for (size_t at; (at = user.find(L"  ")) != std::wstring::npos;) user.erase(at, 1);
    d.legend[2] = user;
    d.legend[3] = Fill(Text(I::AiDkLegOther), L"{gb}", Gb1(du.other));
    d.legend_free = Text(I::AiDkLegFree);
    DiskSegments(d, du, 0);
    d.top_label = Text(I::AiDkTop);
    const uint64_t biggest = du.top.empty() ? 1 : (std::max<uint64_t>)(du.top.front().bytes, 1);
    for (const app::ai::DiskTop& t : du.top) {
        ui::AiDiskRowView r;
        r.file = t.is_file;
        r.name = Leaf(t.path);
        using P = app::ai::DiskPlace;
        switch (t.place) {
        case P::Profile:
            r.name = FolderLabel(t.path);
            r.sub = Fill(Text(I::AiDkSubUser), L"{name}", du.user_name);
            break;
        case P::AppData: {
            r.name = L"AppData \u203A " + Leaf(t.path);
            for (const auto& [name, bytes] : t.parts)
                r.sub += (r.sub.empty() ? L"" : L" \u00B7 ") + name + L" " + Gb1(bytes);
            break;
        }
        case P::System: r.sys = true; r.sub = Text(I::AiDkSubSystem); break;
        case P::Programs: r.sys = true; r.sub = Text(I::AiDkSubPrograms); break;
        case P::Hibernate: r.sys = true; r.sub = Text(I::AiDkSubHiber); break;
        case P::Pagefile: r.sys = true; r.sub = Text(I::AiDkSubPage); break;
        default: {
            // Where it sits, as a trail like 用户 › SS: 本地磁盘 (C:) › Games
            // rather than a bare C:\ for things at the root.
            const std::wstring parent = app::ai::ParentFolder(path::StripExtendedPathPrefix(t.path));
            if (parent.size() >= 3 && parent[1] == L':' && parent[2] == L'\\') {
                r.sub = Fill(Text(I::AiDkTitle), L"{d}", std::wstring(1, static_cast<wchar_t>(std::towupper(parent[0]))));
                for (size_t at = 3; at < parent.size();) {
                    const size_t next = (std::min)(parent.find(L'\\', at), parent.size());
                    if (next > at) r.sub += L" \u203A " + parent.substr(at, next - at);
                    at = next + 1;
                }
            } else {
                r.sub = parent;
            }
            break;
        }
        }
        r.value = Gb1(t.bytes);
        r.frac = static_cast<float>(static_cast<double>(t.bytes) / static_cast<double>(biggest));
        d.rows.push_back(std::move(r));
    }
    d.grow_ms = Now() + 60;
}

json::Value DiskJson(const app::ai::DiskUsage& du) {
    json::Value o = json::Value::Object();
    o.set("drive", Utf8(std::wstring(1, du.drive) + L":"));
    o.set("total_gb", Round1(du.total));
    o.set("free_gb", Round1(du.free));
    o.set("used_gb", Round1(du.total - (std::min)(du.free, du.total)));
    o.set("system_gb", Round1(du.system));
    o.set("programs_gb", Round1(du.programs));
    o.set("users_gb", Round1(du.users));
    o.set("other_gb", Round1(du.other));
    o.set("recycle_bin_gb", Round1(du.recycle_bytes));
    o.set("recycle_bin_items", du.recycle_items);
    json::Value top = json::Value::Array();
    for (const app::ai::DiskTop& t : du.top) {
        json::Value e = json::Value::Object();
        e.set("path", Utf8(t.path));
        e.set("gb", Round1(t.bytes));
        using P = app::ai::DiskPlace;
        const bool sys = t.place == P::System || t.place == P::Programs || t.place == P::Hibernate || t.place == P::Pagefile;
        e.set("kind", t.place == P::Hibernate ? "hibernation_file" : t.place == P::Pagefile ? "page_file"
                      : t.place == P::System ? "windows" : t.place == P::Programs ? "programs"
                      : t.place == P::AppData ? "app_data" : t.place == P::Profile ? "user_folder" : "other");
        if (sys) e.set("note", "system: do not suggest deleting");
        if (!t.parts.empty()) {
            json::Value parts = json::Value::Array();
            for (const auto& [name, bytes] : t.parts) {
                json::Value p = json::Value::Object();
                p.set("name", Utf8(name));
                p.set("gb", Round1(bytes));
                parts.push(std::move(p));
            }
            e.set("biggest", std::move(parts));
        }
        top.push(std::move(e));
    }
    o.set("biggest", std::move(top));
    if (!du.indexed)
        o.set("note", "Pulse's index has no folder sizes for this drive (文件夹大小 is off or not ready), so only the totals are known.");
    o.set("shown", "Pulse shows this as a card: do not repeat the numbers as a list; one or two sentences on what stands out.");
    return o;
}

void ToolDiskUsage(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    const wchar_t drive = DriveArg(args);
    if (!drive) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", "drive must be a letter such as C"));
        ++a.call_index;
        NextTool(s);
        return;
    }
    AddStep(s, a, ui::AiIcon::Hdd, Text(I::AiDkStep), std::wstring(1, drive) + L":", Text(I::AiDkStepRun));
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiDkChipMeasuring), 0.3f);
    auto disk = std::make_shared<SpaceDisk>();
    const std::string call_id = call.id;
    const uint64_t gen = a.generation;
    RunJob(s, [disk, drive](const std::atomic<bool>& cancel) {
               if (!app::ai::MeasureDisk(drive, disk->du, cancel))
                   return ToolOutcome::Fail("not_found", "That drive does not exist or is not a local disk.");
               ToolOutcome out;
               out.data = DiskJson(disk->du);
               return out;
           },
           [disk, call_id, gen](AppState& st, const ToolOutcome& out) {
               AiAgent& ag = Agent(st);
               if (ag.generation != gen) return;
               AiChat& c = Chat(st);
               if (!out.ok) {
                   EndStep(st, ag, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(out.error.message)));
               } else {
                   const app::ai::DiskUsage& du = disk->du;
                   const uint64_t used = du.total - (std::min)(du.free, du.total);
                   EndStep(st, ag, true,
                           Fill(Fill(Fill(Text(du.indexed ? I::AiDkStepDone : I::AiDkStepDonePlain), L"{used}", GbTrim(used)),
                                     L"{total}", GbTrim(du.total)),
                                L"{t}", Secs(du.ms)));
                   AiBlockView& b = c.Add(AiBlockKind::Disk, Now());
                   FillDiskCard(b, du);
                   disk->card_id = b.id;
                   g_space_disk = disk;
                   AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiDkChipDone));
               }
               AddToolResult(ag, call_id, out);
               ++ag.call_index;
               AiChatPush(st, 350, [](AppState& s2) { NextTool(s2); return -1; });
           });
}

// upto: only the first categories (the rows landed so far).
CleanPick Picked(const SpaceClean& sc, size_t upto) {
    CleanPick p;
    p.cat_files.assign(sc.scan.cats.size(), 0);
    p.cat_bytes.assign(sc.scan.cats.size(), 0);
    for (size_t c = 0; c < sc.scan.cats.size() && c < upto; ++c)
        for (size_t i = 0; i < sc.scan.cats[c].items.size(); ++i)
            if (sc.on[c][i]) {
                const app::ai::CleanItem& it = sc.scan.cats[c].items[i];
                p.cat_files[c] += it.files;
                p.cat_bytes[c] += it.bytes;
                p.files += it.files;
                p.bytes += it.bytes;
                ++p.items;
            }
    return p;
}

AiBlockView* CleanCard(AppState& s, const SpaceClean& sc) { return Chat(s).Find(sc.card_id); }

// The card's rows, sum and the confirmation follow the checkboxes.
// A row shows what is ticked in it; an unticked one its whole size, grey.
void SyncClean(AppState& s, SpaceClean& sc, bool bump, size_t upto = SIZE_MAX) {
    const CleanPick all = Picked(sc);
    const CleanPick p = Picked(sc, upto);
    const uint64_t now = Now();
    if (AiBlockView* b = CleanCard(s, sc)) {
        for (size_t c = 0; c < sc.scan.cats.size() && c < b->clean.rows.size(); ++c) {
            ui::AiCleanRowView& r = b->clean.rows[c];
            const bool any = all.cat_files[c] > 0;
            const std::wstring n = Int(any ? all.cat_files[c] : sc.scan.cats[c].files);
            const std::wstring g = Gb1(any ? all.cat_bytes[c] : sc.scan.cats[c].bytes);
            if (bump && (n != r.count || g != r.size)) r.bump_ms = now;
            r.count = n;
            r.size = g;
            r.on = any;
            r.tag = sc.scan.cats[c].default_on || r.on ? std::wstring() : Text(I::AiClTagLook);
            for (size_t f = 0; f < r.files.size(); ++f) r.files[f].on = sc.on[c][f];
        }
        const std::wstring n = Int(p.files), g = Gb1(p.bytes);
        if (bump && (n != b->clean.sum_n || g != b->clean.sum_gb)) b->clean.sum_bump_ms = now;
        b->clean.sum_n = n;
        b->clean.sum_gb = g;
    }
    AiChat& ch = Chat(s);
    if (AiBlockView* cf = ch.Find(sc.confirm_id); cf && !cf->done && upto == SIZE_MAX) {
        cf->title = Fill(Text(I::AiClConfirmTitle), L"{n}", Int(p.files));
        std::wstring what;
        for (size_t c = 0; c < sc.scan.cats.size(); ++c)
            if (p.cat_files[c]) what += (what.empty() ? L"" : Text(I::AiTkListSep)) + Text(CleanTitleId(sc.scan.cats[c].kind));
        cf->sub = Fill(Fill(Text(I::AiClConfirmSub), L"{gb}", Gb1(p.bytes)), L"{what}", what);
        const app::ai::BinState& bin = sc.bin;
        const bool fits = bin.available && bin.used_bytes + p.bytes <= bin.max_bytes;
        cf->hint2_warn = !fits;
        cf->hint2 = !bin.available ? Text(I::AiClBinOff)
                  : Fill(Fill(Text(fits ? I::AiClBinOk : I::AiClBinFull), L"{max}", Gb1(bin.max_bytes)), L"{used}",
                         Gb1(bin.used_bytes));
        cf->accept_off = !fits || p.items == 0;
    }
    // An open 查看 tab keeps its selection in step.
    if (app::Tab* tab = ActiveTab(s)) {
        std::wstring kind, rest;
        if (app::ParsePulsePath(tab->current_path, &kind, &rest) && kind == L"ai-list") {
            const uint32_t id = static_cast<uint32_t>(wcstoul(rest.c_str(), nullptr, 10));
            if (id >= sc.list_base && id < sc.list_base + sc.scan.cats.size() && tab->snapshot) {
                const size_t c = id - sc.list_base;
                // By path: the tab skips files that are gone.
                const auto& items = sc.scan.cats[c].items;
                std::vector<int> picked;
                for (size_t e = 0; e < tab->EntryCount(); ++e) {
                    const fs::DirEntry entry = tab->EntryAt(e);
                    const std::wstring& path = entry.full_path;
                    for (size_t i = 0; i < items.size() && i < sc.on[c].size(); ++i) {
                        if (_wcsicmp(items[i].path.c_str(), path.c_str()) != 0) continue;
                        if (sc.on[c][i]) picked.push_back(static_cast<int>(e));
                        break;
                    }
                }
                tab->ClearSelection();
                if (!picked.empty()) tab->SelectIndices(picked);
            }
        }
    }
}

void OpenCleanList(AppState& s, SpaceClean& sc, size_t c) {
    const app::ai::CleanCategory& cat = sc.scan.cats[c];
    std::vector<app::ai::AiListRow> rows;
    for (size_t i = 0; i < cat.items.size(); ++i) {
        const app::ai::CleanItem& it = cat.items[i];
        app::ai::AiListRow r;
        r.path = it.path;
        r.reason = CleanReason(cat.kind, it);
        r.bytes = it.bytes;
        r.mtime = it.mtime;
        r.dir = it.dir;
        r.on = sc.on[c][i];
        rows.push_back(std::move(r));
    }
    const uint32_t id = sc.list_base + static_cast<uint32_t>(c);
    app::ai::PutAiList(id, Fill(Text(I::AiClListTab), L"{cat}", Text(CleanTitleId(cat.kind))), std::move(rows));
    NavigateTo(s, L"pulse:ai-list:" + std::to_wstring(id));
}

void CleanClick(AppState& s, ui::AiPanelPart part, int row, int file) {
    std::shared_ptr<SpaceClean> sc = g_space_clean;
    if (!sc || row < 0 || static_cast<size_t>(row) >= sc->scan.cats.size()) return;
    const size_t c = static_cast<size_t>(row);
    std::vector<bool>& on = sc->on[c];
    if (part == ui::AiPanelPart::CleanCheck) {
        const bool any = std::find(on.begin(), on.end(), true) != on.end();
        std::fill(on.begin(), on.end(), !any);
    } else if (part == ui::AiPanelPart::CleanFile) {
        if (file >= 0 && static_cast<size_t>(file) < on.size()) on[static_cast<size_t>(file)] = !on[static_cast<size_t>(file)];
    } else if (part == ui::AiPanelPart::CleanLink) {
        if (AiBlockView* b = CleanCard(s, *sc); b && c < b->clean.rows.size()) {
            ui::AiCleanRowView& r = b->clean.rows[c];
            const bool open = !r.open;
            for (auto& other : b->clean.rows) other.open = false;
            r.open = open && !r.files.empty();
            if (open) OpenCleanList(s, *sc, c);
        }
        return;
    }
    SyncClean(s, *sc, true);
}

json::Value CleanJson(const SpaceClean& sc) {
    json::Value o = json::Value::Object();
    json::Value cats = json::Value::Array();
    for (const app::ai::CleanCategory& c : sc.scan.cats) {
        json::Value e = json::Value::Object();
        e.set("kind", CleanKey(c.kind));
        e.set("files", c.files);
        e.set("gb", Round1(c.bytes));
        e.set("checked_by_default", c.default_on);
        json::Value names = json::Value::Array();
        for (size_t i = 0; i < c.items.size() && i < 3; ++i) names.push(Utf8(Leaf(c.items[i].path)));
        e.set("examples", std::move(names));
        cats.push(std::move(e));
    }
    o.set("categories", std::move(cats));
    return o;
}

ui::AiMiniDiskView MiniDisk(wchar_t drive, uint64_t total, uint64_t free, uint64_t in_bin) {
    ui::AiMiniDiskView m;
    m.on = true;
    m.label = Fill(Text(I::AiClMiniFree), L"{d}", std::wstring(1, drive));
    m.value = m.value_from = static_cast<float>(Gb(free));
    m.unit = L"GB";
    const double t = total ? static_cast<double>(total) : 1.0;
    const uint64_t used = total - (std::min)(free, total);
    m.used = static_cast<float>((used > in_bin ? used - in_bin : 0) / t);
    m.recycle = m.recycle_from = static_cast<float>(in_bin / t);
    return m;
}

bool DriveSpace(wchar_t drive, uint64_t& total, uint64_t& free) {
    const wchar_t root[] = {drive, L':', L'\\', 0};
    ULARGE_INTEGER avail{}, tot{}, fr{};
    if (!GetDiskFreeSpaceExW(root, &avail, &tot, &fr)) return false;
    total = tot.QuadPart;
    free = fr.QuadPart;
    return true;
}

void SettleRecycle(AppState& s, std::shared_ptr<SpaceClean> sc);

// 撤销: one group undo, then the card turns grey once the files are back.
// Files behind one moved path: a temp folder counts what is inside it.
// Both sides normalised: Recycle Bin entries spell folders their own way.
uint64_t CleanFilesOf(const SpaceClean& sc, const std::wstring& path) {
    const std::wstring want = fs::NormalizePath(path);
    for (const app::ai::CleanCategory& cat : sc.scan.cats)
        for (const app::ai::CleanItem& it : cat.items)
            if (_wcsicmp(fs::NormalizePath(it.path).c_str(), want.c_str()) == 0)
                return it.files ? static_cast<uint64_t>(it.files) : 1;
    return 1;
}

// A 查看 tab lists files by path; reload it once they moved or came back.
void RefreshCleanLists(AppState& s) { AiAssistantRefreshLists(s); }

std::function<void(AppState&)> MakeCleanUndo(std::shared_ptr<SpaceClean> sc) {
    return [sc](AppState& st) {
        AiChat& c2 = Chat(st);
        const auto outcome = st.ops.UndoGroup(sc->undo_group);
        if (outcome != ops::OpsManager::GroupUndo::Started) {
            AiBlockView& note = c2.Add(AiBlockKind::Note, Now());
            const bool pending = outcome == ops::OpsManager::GroupUndo::Pending;
            note.text = Text(pending ? I::AiAsstUndoBusy : I::AiAsstUndoGone);
            if (pending) c2.on_undo = MakeCleanUndo(sc);
            Redraw(st);
            return;
        }
        c2.on_purge = nullptr;
        AiChatSetChip(c2, ui::StatusAiState::Busy, Text(I::AiAsstChipUndoing), 0.5f);
        c2.peek_undo = false;
        AiChatHidePeek(st);
        auto started = std::make_shared<uint64_t>(Now());
        AiChatPush(st, 300, [sc, started](AppState& s3) {
            size_t back = 0;
            uint64_t back_files = 0;   // a temp folder counts its files, as the result card did
            for (size_t i = 0; i < sc->sources.size(); ++i) {
                if (!sc->moved[i] || !Exists(sc->sources[i])) continue;
                ++back;
                uint64_t n = 1;
                for (const auto& it : sc->scan.cats[sc->source_cat[i]].items)
                    if (it.path == sc->sources[i]) { n = it.files; break; }
                back_files += n;
            }
            if (back < sc->moved_n && !(Now() - *started > 3000 && !s3.ops.Status().active) && Now() - *started < 600000)
                return 150;
            AiChat& c3 = Chat(s3);
            if (AiBlockView* rr = c3.Find(sc->result_id)) {
                rr->done = true;
                rr->title = Text(I::AiAsstUndone);
                rr->sub = Fill(Text(sc->dup ? I::AiDupUndoneSub : I::AiClUndoneSub), L"{n}", Int(back_files));
                rr->mini.on = false;
            }
            if (g_space_disk && g_space_disk->du.drive == sc->drive)
                if (AiBlockView* db = c3.Find(g_space_disk->card_id)) DiskSegments(db->disk, g_space_disk->du, 0);
            AiBlockView& note = c3.Add(AiBlockKind::Note, Now());
            note.text = Text(I::AiClUndoNote);
            AiChatSetChip(c3, ui::StatusAiState::Done, Text(I::AiAsstUndone));
            RefreshCleanLists(s3);
            if (c3.agent)
                c3.agent->pending_note = sc->dup
                    ? L"(The user clicked Undo in Pulse: the duplicate copies find_duplicates moved to the Recycle Bin are back where they were.)"
                    : L"(The user clicked Undo in Pulse: the files find_cleanup moved to the Recycle Bin are back where they were.)";
            Redraw(s3);
            return -1;
        });
    };
}

void RunRecycle(AppState& s, std::shared_ptr<SpaceClean> sc) {
    AiAgent& a = Agent(s);
    const CleanPick p = Picked(*sc);
    for (size_t c = 0; c < sc->scan.cats.size(); ++c)
        for (size_t i = 0; i < sc->scan.cats[c].items.size(); ++i)
            if (sc->on[c][i]) {
                sc->sources.push_back(sc->scan.cats[c].items[i].path);
                sc->source_bytes.push_back(sc->scan.cats[c].items[i].bytes);
                sc->source_cat.push_back(c);
            }
    sc->moved.assign(sc->sources.size(), false);
    DriveSpace(sc->drive, sc->total, sc->free_before);
    sc->bin_items_before = sc->bin.items;
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    sc->undo_group = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    // $I records carry the deletion time; allow for a coarse clock.
    const uint64_t since = sc->undo_group - 20000000ull;
    sc->since.dwLowDateTime = static_cast<DWORD>(since);
    sc->since.dwHighDateTime = static_cast<DWORD>(since >> 32);
    a.organized = true;
    AddStep(s, a, ui::AiIcon::Trash, Text(I::AiClRecStep),
            Fill(Fill(Text(I::AiClRecArg), L"{n}", Int(p.files)), L"{gb}", Gb1(p.bytes)), {});
    if (AiBlockView* sb = Block(s, a.steps_id)) {
        sb->label = Fill(Text(I::AiClRecSteps), L"{n}", Int(p.files));
        sb->open = true;
    }
    ops::OpRequest r;
    r.type = ops::OpType::RecycleDelete;
    r.sources = sc->sources;
    r.undo_group = sc->undo_group;
    r.undo_label = Text(I::AiClUndoLabel);
    s.ops.Submit(std::move(r));
    struct Watch { uint64_t started = 0, last_change = 0; size_t gone = 0; };
    auto w = std::make_shared<Watch>();
    w->started = w->last_change = Now();
    AiChatPush(s, 150, [sc, w](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != sc->gen) return -1;
        const uint64_t now = Now();
        size_t gone = 0;
        std::wstring next;
        for (size_t i = 0; i < sc->sources.size(); ++i) {
            if (!sc->moved[i] && !Exists(sc->sources[i])) sc->moved[i] = true;
            if (sc->moved[i]) ++gone;
            else if (next.empty()) {
                next = Leaf(sc->sources[i]);
            }
        }
        if (gone != w->gone) {
            w->gone = gone;
            w->last_change = now;
        }
        const float prog = sc->sources.empty() ? 1.0f : static_cast<float>(gone) / static_cast<float>(sc->sources.size());
        if (ui::AiStepView* step = LastStep(st, ag)) {
            step->progress = prog;
            step->sub = Fill(Fill(Fill(Text(I::AiClRecSub), L"{k}", Int(gone)), L"{n}", Int(sc->sources.size())), L"{name}",
                             next);
        }
        AiChatSetChip(Chat(st), ui::StatusAiState::Busy,
                      Fill(Text(I::AiClChipRec), L"{p}", Num(static_cast<size_t>(prog * 100.0f + 0.5f))), prog);
        const bool idle = !st.ops.Status().active;
        if (gone >= sc->sources.size() || (idle && now - w->last_change > 2500) || now - w->started > 600000) {
            SettleRecycle(st, sc);
            return -1;
        }
        return 150;
    });
}

void SettleRecycle(AppState& s, std::shared_ptr<SpaceClean> sc) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    RefreshCleanLists(s);
    std::vector<uint64_t> cat_files(sc->scan.cats.size(), 0), cat_bytes(sc->scan.cats.size(), 0), cat_left(sc->scan.cats.size(), 0);
    uint64_t files = 0;
    size_t left = 0;
    for (size_t i = 0; i < sc->sources.size(); ++i) {
        const size_t ci = sc->source_cat[i];
        uint64_t n = 1;
        for (const auto& it : sc->scan.cats[ci].items)
            if (it.path == sc->sources[i]) { n = it.files; break; }
        if (sc->moved[i]) {
            ++sc->moved_n;
            sc->moved_bytes += sc->source_bytes[i];
            cat_files[ci] += n;
            cat_bytes[ci] += sc->source_bytes[i];
            files += n;
        } else {
            ++left;
            ++cat_left[ci];
        }
    }
    EndStep(s, a, sc->moved_n > 0,
            Fill(Fill(Text(I::AiClRecDone), L"{n}", Int(files)), L"{gb}", Gb1(sc->moved_bytes)) +
                (left ? Fill(Text(I::AiClRecSkip), L"{k}", Int(left)) : std::wstring()));
    if (AiBlockView* sb = Block(s, a.steps_id)) {
        sb->collapsed = true;
        sb->open = false;
    }
    a.steps_id = 0;
    // The disk card: the user part gives the batch to the hatched bin part.
    if (g_space_disk && g_space_disk->du.drive == sc->drive)
        if (AiBlockView* db = c.Find(g_space_disk->card_id)) DiskSegments(db->disk, g_space_disk->du, sc->moved_bytes);
    if (AiBlockView* cb = CleanCard(s, *sc)) {
        for (auto& r : cb->clean.rows) r.open = false;
    }
    if (sc->moved_n) {
        AiBlockView& r = c.Add(AiBlockKind::Result, Now());
        r.title = Fill(Text(I::AiClResTitle), L"{n}", Int(files));
        r.sub = Fill(Text(I::AiClResSub), L"{gb}", Gb1(sc->moved_bytes));
        for (size_t ci = 0; ci < sc->scan.cats.size(); ++ci) {
            if (!cat_files[ci]) continue;
            ui::AiResultLine ln;
            ln.before = Text(CleanTitleId(sc->scan.cats[ci].kind)) + L" ";
            ln.bold = Int(cat_files[ci]);
            ln.after = Fill(Text(I::AiClResLineAfter), L"{gb}", Gb1(cat_bytes[ci]));
            r.lines.push_back(std::move(ln));
        }
        if (left) {
            ui::AiResultLine ln;
            ln.warn = true;
            ln.before = Fill(Text(I::AiClResSkipped), L"{k}", Int(left));
            r.lines.push_back(std::move(ln));
        }
        {
            ui::AiResultLine ln;
            ln.warn = true;
            ln.before = Fill(Text(I::AiClResStill), L"{d}", std::wstring(1, sc->drive));
            r.lines.push_back(std::move(ln));
        }
        uint64_t total = sc->total, free = sc->free_before;
        DriveSpace(sc->drive, total, free);
        r.mini = MiniDisk(sc->drive, total, free, sc->moved_bytes);
        r.mini.recycle_from = 0.0f;
        r.mini.count_ms = Now() + 200;
        r.mini.leg_used = Text(I::AiClMiniUsed);
        r.mini.leg_recycle = Fill(Text(I::AiDkLegRecycle), L"{gb}", Gb1(sc->moved_bytes)) + L" GB";
        r.mini.leg_free = Text(I::AiDkLegFree);
        r.deny = Text(I::AiAsstUndo);
        r.purge = Text(I::AiClPurge);
        sc->result_id = r.id;
        c.result_id = r.id;
        c.on_show = nullptr;
        c.on_undo = MakeCleanUndo(sc);
        c.on_purge = [sc](AppState& st) { AskPurge(st, sc); };
        c.peek_text = Fill(Fill(Text(I::AiClPeek), L"{n}", Int(files)), L"{gb}", Gb1(sc->moved_bytes));
        c.peek_undo = true;
    }
    AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("ok", sc->moved_n > 0);
    out.data.set("moved_files", files);
    out.data.set("moved_gb", Round1(sc->moved_bytes));
    out.data.set("not_moved", static_cast<uint64_t>(left));
    json::Value cats = json::Value::Array();
    for (size_t ci = 0; ci < sc->scan.cats.size(); ++ci) {
        if (!cat_files[ci]) continue;
        json::Value e = json::Value::Object();
        e.set("kind", CleanKey(sc->scan.cats[ci].kind));
        e.set("files", cat_files[ci]);
        e.set("gb", Round1(cat_bytes[ci]));
        cats.push(std::move(e));
    }
    out.data.set("moved", std::move(cats));
    out.data.set("recycle_bin_items_before", sc->bin_items_before);
    out.data.set("note", "The files are in the Recycle Bin, so they still take space on the drive and free space has not "
                         "changed yet. The user can undo with the card's Undo button, or free the space with its "
                         "\xE6\xB0\xB8\xE4\xB9\x85\xE5\x88\xA0\xE9\x99\xA4\xE8\xBF\x99\xE4\xBA\x9B button, which deletes only "
                         "this batch and leaves the rest of the Recycle Bin alone. Say so briefly.");
    if (left) out.data.set("not_moved_note", "Some files did not move (in use or no permission).");
    AddToolResult(a, sc->call_id, out);
    ++a.call_index;
    AiChatPush(s, 350, [](AppState& s2) { NextTool(s2); return -1; });
    Redraw(s);
}

// 永久删除这些…: a red confirmation, then only this batch leaves the bin.
void AskPurge(AppState& s, std::shared_ptr<SpaceClean> sc) {
    AiChat& ch = Chat(s);
    if (ch.busy || ch.confirm_id) return;
    const std::vector<app::ai::BinHit> hits = [&] {
        std::vector<std::wstring> moved;
        for (size_t i = 0; i < sc->sources.size(); ++i)
            if (sc->moved[i]) moved.push_back(sc->sources[i]);
        return app::ai::FindInBin(moved, sc->since);
    }();
    if (hits.empty()) {
        AiBlockView& note = ch.Add(AiBlockKind::Note, Now());
        note.text = Text(I::AiClPurgeMissing);
        Redraw(s);
        return;
    }
    uint64_t bytes = 0, hit_files = 0;
    for (const auto& h : hits) {
        bytes += h.bytes;
        hit_files += CleanFilesOf(*sc, h.original);
    }
    const app::ai::BinState bin = app::ai::ReadBin(sc->drive);
    const uint64_t others = bin.items > hits.size() ? bin.items - hits.size() : 0;
    AiChatSetChip(ch, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
    AiBlockView& cf = ch.Add(AiBlockKind::Confirm, Now());
    cf.danger = true;
    cf.title = Fill(Text(I::AiClPurgeTitle), L"{n}", Int(hit_files));
    cf.sub = Fill(FillGb(Text(I::AiClPurgeSub), bytes), L"{k}", Int(others));
    cf.hint = Text(I::AiClPurgeHint);
    cf.hint_icon = ui::AiIcon::Warn;
    cf.deny = Text(I::AiAsstDeny);
    cf.accept = Text(I::AiClPurgeAccept);
    ch.confirm_id = cf.id;
    auto shared_hits = std::make_shared<std::vector<app::ai::BinHit>>(hits);
    ch.on_confirm = [sc, shared_hits, others, hit_files](AppState& s3, bool accept) {
        AiChat& c3 = Chat(s3);
        if (AiBlockView* card = c3.Find(c3.confirm_id)) {
            card->done = true;
            card->title = accept ? Fill(Text(I::AiClPurgeConfirmed), L"{n}", Int(hit_files)) : Text(I::AiClPurgeDenied);
        }
        c3.confirm_id = 0;
        c3.on_confirm = nullptr;
        if (!accept) {
            AiChatSetChip(c3, ui::StatusAiState::Done, Text(I::AiClChipInBin));
            Redraw(s3);
            return;
        }
        AiChatSetChip(c3, ui::StatusAiState::Busy, Text(I::AiClChipPurge), 0.6f);
        ops::OpRequest r;
        r.type = ops::OpType::RealDelete;
        for (const auto& h : *shared_hits) {
            r.sources.push_back(h.payload);
            if (!h.index.empty()) r.sources.push_back(h.index);
        }
        s3.ops.Submit(std::move(r));
        auto started = std::make_shared<uint64_t>(Now());
        AiChatPush(s3, 200, [sc, shared_hits, others, started](AppState& s4) {
            size_t left = 0;
            uint64_t freed = 0, freed_files = 0;
            for (const auto& h : *shared_hits) {
                if (Exists(h.payload)) {
                    ++left;
                    continue;
                }
                freed += h.bytes;
                freed_files += CleanFilesOf(*sc, h.original);
            }
            if (left && !(Now() - *started > 2500 && !s4.ops.Status().active) && Now() - *started < 600000) return 150;
            recycle_empty::NotifyShell();
            AiChat& c4 = Chat(s4);
            uint64_t total = sc->total, free = sc->free_before;
            DriveSpace(sc->drive, total, free);
            // The earlier card can no longer undo.
            if (AiBlockView* old = c4.Find(sc->result_id)) {
                old->gone = true;
                old->sub = Text(I::AiClGoneSub);
            }
            if (c4.result_id == sc->result_id) {
                c4.on_undo = nullptr;
                c4.on_purge = nullptr;
            }
            AiBlockView& r2 = c4.Add(AiBlockKind::Result, Now());
            r2.freed = true;
            r2.title = FillGb(Text(I::AiClFreedTitle), freed);
            r2.sub = Fill(Fill(Text(I::AiClFreedSub), L"{n}", Int(freed_files)), L"{k}", Int(others));
            const uint64_t free_was = free > freed ? free - freed : 0;
            r2.mini = MiniDisk(sc->drive, total, free, 0);
            r2.mini.value_from = static_cast<float>(Gb(free_was));
            r2.mini.from = Fill(Text(I::AiClMiniFrom), L"{gb}", Gb1(free_was));
            r2.mini.recycle_from = static_cast<float>(Gb(freed) / (std::max)(Gb(total), 0.001));
            r2.mini.recycle = 0.0f;
            r2.mini.count_ms = Now() + 120;
            r2.mini.leg_used = Fill(Text(I::AiClMiniUsedN), L"{gb}", Gb1(total - (std::min)(free, total)));
            r2.mini.leg_free = Fill(Text(I::AiClMiniFreeN), L"{gb}", Gb1(free));
            sc->result_id = 0;
            // The disk card: the hatch goes, the figures count to the new ones.
            if (g_space_disk && g_space_disk->du.drive == sc->drive)
                if (AiBlockView* db = c4.Find(g_space_disk->card_id)) {
                    app::ai::DiskUsage& du = g_space_disk->du;
                    ui::AiDiskView& d = db->disk;
                    d.used_from = d.used_gb;
                    d.free_from = d.free_gb;
                    du.users = du.users > freed ? du.users - freed : 0;
                    du.free = free;
                    DiskSegments(d, du, 0);
                    d.used_gb = static_cast<float>(Gb(total - (std::min)(free, total)));
                    d.free_gb = static_cast<float>(Gb(free));
                    d.free_ok = true;
                    d.count_ms = Now() + 120;
                }
            AiChatSetChip(c4, ui::StatusAiState::Done,
                          Fill(Fill(Text(I::AiClChipFreed), L"{d}", std::wstring(1, sc->drive)), L"{gb}", Gb1(free)));
            if (c4.agent)
                c4.agent->pending_note = L"(In Pulse the user permanently deleted the " + Int(freed_files) +
                                         (sc->dup ? L" duplicate copies that find_duplicates" : L" files that find_cleanup") +
                                         L" had moved to the Recycle Bin; " +
                                         std::wstring(1, sc->drive) + L": now has " + Gb1(free) + L" GB free.)";
            Redraw(s4);
            return -1;
        });
        Redraw(s3);
    };
    Redraw(s);
}

void ToolFindCleanup(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    const wchar_t drive = DriveArg(args);
    if (!drive) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", "drive must be a letter such as C"));
        ++a.call_index;
        NextTool(s);
        return;
    }
    const app::ai::SpaceRoots roots = app::ai::CurrentSpaceRoots();
    AddStep(s, a, ui::AiIcon::Search, Text(I::AiClStep), FolderLabel(roots.profile), Text(I::AiClStage0));
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiClChipFinding), 0.5f);
    auto sc = std::make_shared<SpaceClean>();
    sc->drive = drive;
    sc->call_id = call.id;
    sc->gen = a.generation;
    auto progress = std::make_shared<app::ai::CleanProgress>();
    auto job = std::make_shared<AiAgent::Job>();
    a.job = job;
    std::thread([job, sc, progress, drive] {
        app::ai::FindCleanup(drive, sc->scan, *progress, job->cancelled);
        job->done.store(true);
    }).detach();
    const uint64_t started = Now();
    AiChatPush(s, 40, [sc, job, progress, started](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != sc->gen) {
            job->cancelled.store(true);
            return -1;
        }
        if (!job->done.load() || Now() - started < 900) {
            if (ui::AiStepView* step = LastStep(st, ag)) {
                const int stage = std::clamp(progress->stage.load(), 0, 4);
                static constexpr I kStage[] = {I::AiClStage0, I::AiClStage1, I::AiClStage2, I::AiClStage3, I::AiClStage4};
                step->sub = Text(kStage[stage]);
                step->progress = std::clamp((static_cast<float>(stage) + progress->fraction.load()) / 5.0f, 0.02f, 1.0f);
            }
            return 60;
        }
        ag.job.reset();
        AiChat& c = Chat(st);
        app::ai::CleanupScan& scan = sc->scan;
        for (auto& cat : scan.cats)
            std::stable_sort(cat.items.begin(), cat.items.end(),
                             [](const app::ai::CleanItem& x, const app::ai::CleanItem& y) { return x.bytes > y.bytes; });
        uint64_t files = 0, bytes = 0;
        for (const auto& cat : scan.cats) {
            files += cat.files;
            bytes += cat.bytes;
        }
        if (scan.cats.empty()) {
            EndStep(st, ag, true, Text(I::AiClStepNone));
            ToolOutcome out;
            out.data = CleanJson(*sc);
            out.data.set("note", "Nothing in the user's Downloads, Temp or Videos is safe to clean. Say so; do not suggest "
                                 "deleting system or program files.");
            AddToolResult(ag, sc->call_id, out);
            ++ag.call_index;
            AiChatPush(st, 350, [](AppState& s2) { NextTool(s2); return -1; });
            return -1;
        }
        EndStep(st, ag, true,
                Fill(Fill(Fill(Text(I::AiClStepDone), L"{k}", Num(scan.cats.size())), L"{n}", Int(files)), L"{gb}",
                     Gb1(bytes)));
        if (AiBlockView* sb = Block(st, ag.steps_id)) {
            sb->label = Fill(Text(I::AiClStepsLabel), L"{k}", Num(scan.cats.size()));
            sb->collapsed = true;
            sb->open = false;
        }
        ag.steps_id = 0;
        // 清理建议.
        sc->on.clear();
        for (const auto& cat : scan.cats) sc->on.emplace_back(cat.items.size(), cat.default_on);
        AiBlockView& b = c.Add(AiBlockKind::Clean, Now());
        sc->card_id = b.id;
        sc->list_base = b.id * 8;
        b.label = Text(I::AiClTitle);
        b.meta = Text(I::AiClMeta);
        ui::AiCleanView& cv = b.clean;
        cv.unit_n = Text(I::AiClUnitN);
        cv.unit_gb = L"GB";
        cv.sum_pre = Text(I::AiClSumPre);
        cv.sum_mid = Text(I::AiClSumMid);
        cv.sum_unit = L"GB";
        cv.safe = Text(I::AiClSafe);
        cv.keep = Text(I::AiClKeep);
        cv.note = Text(I::AiClNote);
        if (g_space_disk && g_space_disk->du.drive == sc->drive)
            for (const auto& t : g_space_disk->du.top)
                if (t.place == app::ai::DiskPlace::Hibernate && t.bytes > 2 * kGiB)
                    cv.note += Fill(Text(I::AiClNoteHiber), L"{gb}", Gb1(t.bytes));
        const uint64_t now = Now();
        for (size_t ci = 0; ci < scan.cats.size(); ++ci) {
            const app::ai::CleanCategory& cat = scan.cats[ci];
            ui::AiCleanRowView r;
            r.icon = CleanIcon(cat.kind);
            r.tint = CleanTint(cat.kind);
            r.title = Text(CleanTitleId(cat.kind));
            r.link = Text(I::AiClView);
            const std::wstring where = CleanWhere(cat);
            switch (cat.kind) {
            case app::ai::CleanKind::Installer: r.desc = Fill(Text(I::AiClDescInstaller), L"{where}", where); break;
            case app::ai::CleanKind::Extracted: r.desc = Fill(Text(I::AiClDescExtracted), L"{where}", where); break;
            case app::ai::CleanKind::Copy: r.desc = Fill(Text(I::AiClDescCopy), L"{where}", where); break;
            case app::ai::CleanKind::Temp: r.desc = Text(I::AiClDescTemp); break;
            default: r.desc = Fill(Text(I::AiClDescVideo), L"{where}", where); break;
            }
            for (size_t i = 0; i < cat.items.size() && i < 5; ++i) {
                const app::ai::CleanItem& it = cat.items[i];
                ui::AiCleanFileView f;
                f.name = Leaf(it.path);
                f.size = it.bytes >= 1024ull * 1024 * 1024 ? Gb1(it.bytes) + L" GB"
                                                           : std::to_wstring((it.bytes + 512 * 1024) / (1024 * 1024)) + L" MB";
                if (it.dir) {
                    f.icon = ui::AiIcon::FolderFilled;
                    f.tint = ui::AiTint::Neutral;
                } else {
                    AiChatGroupLook({f.name}, f.icon, f.tint);
                }
                r.files.push_back(std::move(f));
            }
            if (cat.items.size() > 5) r.more = Fill(Text(I::AiClMore), L"{n}", Int(cat.items.size() - 5));
            r.in_ms = now + 120 + 200 * ci;
            cv.rows.push_back(std::move(r));
        }
        sc->bin = app::ai::ReadBin(sc->drive);
        g_space_clean = sc;
        c.clean_id = b.id;
        c.on_clean = [](AppState& s2, ui::AiPanelPart part, int row, int file) { CleanClick(s2, part, row, file); };
        // The rows land one by one and the sum counts up with them.
        SyncClean(st, *sc, false, 0);
        for (size_t ci = 0; ci < scan.cats.size(); ++ci) {
            AiChatPush(st, ci == 0 ? 120 : 200, [sc, ci](AppState& s2) {
                if (Agent(s2).generation != sc->gen) return -1;
                SyncClean(s2, *sc, true, ci + 1);
                Redraw(s2);
                return -1;
            });
        }
        // The confirmation; the turn waits here while the user ticks.
        AiChatPush(st, 700, [sc](AppState& s4) {
            AiAgent& a4 = Agent(s4);
            if (a4.generation != sc->gen) return -1;
            AiChat& ch = Chat(s4);
            const CleanPick p = Picked(*sc);
            AiChatSetChip(ch, ui::StatusAiState::Waiting,
                          Fill(Text(I::AiClChipReady), L"{gb}", Gb1(p.bytes)));
            if (!s4.aiAssistant.open) ch.unread = true;
            AiBlockView& cf = ch.Add(AiBlockKind::Confirm, Now());
            cf.tag = Text(I::AiClTagDelete);
            cf.hint = Text(I::AiClConfirmHint);
            cf.hint_icon = ui::AiIcon::Undo;
            cf.deny = Text(I::AiAsstDeny);
            cf.accept = Text(I::AiClAccept);
            sc->confirm_id = cf.id;
            ch.confirm_id = cf.id;
            SyncClean(s4, *sc, false);
            ch.on_confirm = [sc](AppState& s3, bool accept) {
                AiChat& c3 = Chat(s3);
                AiAgent& a3 = Agent(s3);
                const CleanPick pk = Picked(*sc);
                if (AiBlockView* card = c3.Find(c3.confirm_id)) {
                    const uint64_t now2 = Now();
                    if (now2 > card->born_ms) c3.turn_start += now2 - card->born_ms;
                    card->done = true;
                    card->tag.clear();
                    card->hint2.clear();
                    card->title = accept ? Fill(Text(I::AiClConfirmed), L"{n}", Int(pk.files)) : Text(I::AiClDenied);
                }
                c3.confirm_id = 0;
                c3.on_confirm = nullptr;
                // The card's checkboxes are settled now.
                c3.clean_id = 0;
                c3.on_clean = nullptr;
                if (a3.generation != sc->gen) return;
                if (!accept) {
                    if (AiBlockView* cb = CleanCard(s3, *sc)) {
                        cb->collapsed = true;
                        for (auto& r : cb->clean.rows) r.open = false;
                    }
                    AddToolResult(a3, sc->call_id,
                                  ToolOutcome::Fail("user_denied", "The user declined the cleanup in Pulse; nothing was "
                                                                   "moved. Do not offer the same cleanup again; ask what "
                                                                   "they would like instead."));
                    ++a3.call_index;
                    AiChatSetChip(c3, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                    NextTool(s3);
                    Redraw(s3);
                    return;
                }
                RunRecycle(s3, sc);
                Redraw(s3);
            };
            Redraw(s4);
            return -1;
        });
        Redraw(st);
        return -1;
    });
}
} // namespace ai_agent

} // namespace pulse
