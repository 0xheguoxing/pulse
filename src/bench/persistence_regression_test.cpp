#include "../app/app_prefs.h"
#include "../app/places.h"
#include "../app/session.h"
#include "../app/app_model.h"
#include "../app/io_task_queue.h"
#include "../app/tag_ads_sync.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <cstdio>
#include <string_view>
#include <deque>
namespace pulse::app { static std::wstring fixture; std::wstring GetPulseDataDir() { return fixture; } }
static int failures;
static void Check(bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; }
static int TagAdsOnly() {
    using namespace pulse::app;
    const auto dir = std::filesystem::absolute(L"bench_data/tag-ads-diagnostic-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(dir)) return 2;
    fixture = dir.wstring();
    const auto file = fixture + L"\\private.txt";
    Check(pulse::WriteUtf8FileAtomic(file, L"private"), "create exclusive private fixture");
    Check(WriteTagAdsV2(file, {}), "deleting absent ADS is idempotent");
    Check(WriteTagAdsV2(file, {{L"t", L"label", 0x123456}}), "create private ADS");
    HANDLE held = CreateFileW((file + L":Pulse.Tag").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Check(held != INVALID_HANDLE_VALUE, "hold private ADS against deletion");
    TagAdsUpdate removal; removal.path = file;
    std::vector<TagAdsFailure> diagnostics;
    Check(!SyncTagAdsUpdates({removal}, &diagnostics).empty(), "locked stream retains pending failure");
    Check(diagnostics.size() == 1 && diagnostics[0].stage == TagAdsFailureStage::StreamWrite &&
        diagnostics[0].error == ERROR_SHARING_VIOLATION && !diagnostics[0].unsupported,
        "sharing violation preserves exact stage and error without unsupported claim");
    if (!diagnostics.empty()) Check(TagAdsFailureMessage(diagnostics[0]).find(L"stream-write") != std::wstring::npos,
        "retry message preserves actionable failure stage");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    Check(SyncTagAdsUpdates({}, &diagnostics).empty() && diagnostics.empty(), "release permits deletion retry");
    Check(WriteTagAdsV2(file, {}) && WriteTagAdsV2(file, {}), "repeated deletion of removed ADS succeeds");
    const auto journal = fixture + L"\\tag_ads_pending.dat";
    held = CreateFileW(journal.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(held != INVALID_HANDLE_VALUE, "lock private journal replacement");
    Check(!SyncTagAdsUpdates({removal}, &diagnostics).empty() && HasPendingTagAds(file),
        "journal write failure retains deletion intent");
    Check(diagnostics.size() == 1 && diagnostics[0].stage == TagAdsFailureStage::JournalWrite &&
        diagnostics[0].error != ERROR_SUCCESS && !diagnostics[0].unsupported,
        "journal write failure cannot claim volume unsupported");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    Check(SyncTagAdsUpdates({}, &diagnostics).empty(), "journal recovery retries idempotent deletion");
    std::vector<TagAdsUpdate> batch;
    bool created = true;
    for (int i = 0; i < 65; ++i) {
        wchar_t name[32]{}; swprintf_s(name, L"\\batch-%03d.txt", i);
        TagAdsUpdate update; update.path = fixture + name;
        created = pulse::WriteUtf8FileAtomic(update.path, L"private batch") && created;
        batch.push_back(std::move(update));
    }
    Check(created, "create 65 exclusive private batch files");
    const auto deferred = SyncTagAdsUpdates(batch, &diagnostics);
    Check(deferred.size() == 1 && deferred.front() == batch.back().path &&
        HasPendingTagAds(batch.back().path) && !HasPendingTagAds(batch.front().path),
        "first batch completes 64 and retains exact final pending path");
    Check(diagnostics.size() == 1 && diagnostics[0].stage == TagAdsFailureStage::Pending &&
        diagnostics[0].error == ERROR_IO_PENDING && !diagnostics[0].unsupported,
        "unattempted item explicitly reports pending without fabricating stream failure");
    TagAdsNoticeState mixed_notice;
    std::wstring mixed_title, mixed_message;
    const TagAdsFailure real_failure{file, TagAdsFailureStage::StreamWrite, ERROR_SHARING_VIOLATION};
    auto mixed = diagnostics; mixed.insert(mixed.begin(), real_failure);
    ApplyTagAdsNotice(mixed_notice, mixed, mixed_title, mixed_message, true);
    Check(mixed_message == TagAdsFailureMessage(real_failure), "deferred work cannot hide a real failure diagnostic");
    TagAdsNoticeState batch_notice;
    std::wstring batch_title, batch_message;
    ApplyTagAdsNotice(batch_notice, {{file, TagAdsFailureStage::StreamWrite, ERROR_SHARING_VIOLATION}},
        batch_title, batch_message, true);
    ApplyTagAdsNotice(batch_notice, diagnostics, batch_title, batch_message, false);
    Check(!diagnostics.empty() && !batch_message.empty() && batch_message == TagAdsFailureMessage(diagnostics.front()),
        "first batch cannot clear banner while one item remains pending");
    Check(SyncTagAdsUpdates({}, &diagnostics).empty() && diagnostics.empty() &&
        !HasPendingTagAds(batch.back().path), "second batch completes final item");
    ApplyTagAdsNotice(batch_notice, diagnostics, batch_title, batch_message, false);
    Check(batch_title.empty() && batch_message.empty(), "only fully completed batch clears banner");
    std::wstring journal_text;
    Check(pulse::ReadUtf8File(journal, journal_text) && journal_text == L"PULSE_ADS_PENDING_1\n",
        "final persisted journal contains no pending items");
    std::deque<std::function<void()>> jobs;
    std::vector<TagAdsFailure> last_result;
    auto enqueue = [&](std::function<void()> task) { jobs.push_back(std::move(task)); };
    auto publish = [&](std::vector<TagAdsFailure> result) { last_result = std::move(result); };
    auto pump = [&] {
        size_t count = 0;
        while (!jobs.empty() && count < 10) {
            auto job = std::move(jobs.front()); jobs.pop_front(); job(); ++count;
        }
        return count;
    };
    QueueTagAdsDrain(batch, enqueue, publish);
    Check(pump() == 2 && jobs.empty() && last_result.empty(), "65 items automatically drain across two serial jobs");
    for (int i = 65; i < 130; ++i) {
        wchar_t name[32]{}; swprintf_s(name, L"\\batch-%03d.txt", i);
        TagAdsUpdate update; update.path = fixture + name;
        created = pulse::WriteUtf8FileAtomic(update.path, L"private batch") && created;
        batch.push_back(std::move(update));
    }
    Check(created, "create additional exclusive batch files");
    QueueTagAdsDrain(batch, enqueue, publish);
    Check(pump() == 3 && jobs.empty() && last_result.empty(), "130 items automatically drain across three jobs");
    Check(WriteTagAdsV2(batch.front().path, {{L"t", L"locked", 0}}), "prepare failed head ADS");
    held = CreateFileW((batch.front().path + L":Pulse.Tag").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Check(held != INVALID_HANDLE_VALUE, "lock failed head ADS");
    QueueTagAdsDrain(batch, enqueue, publish);
    Check(pump() == 3 && jobs.empty() && last_result.size() == 1 &&
        last_result[0].path == batch.front().path && last_result[0].error == ERROR_SHARING_VIOLATION &&
        !HasPendingTagAds(batch.back().path), "failed first entry cannot starve later batches");
    Check(pump() == 0 && HasPendingTagAds(batch.front().path), "persistent failure stops without spinning and retains intent");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    QueueTagAdsDrain({}, enqueue, publish);
    Check(pump() == 1 && last_result.empty(), "next trigger retries retained failure after release");
    QueueTagAdsDrain(batch, enqueue, publish);
    { auto job = std::move(jobs.front()); jobs.pop_front(); job(); }
    Check(!jobs.empty() && !last_result.empty(), "batch schedules continuation with durable pending work");
    jobs.clear(); // WorkerPool shutdown rejects/destroys queued serial continuations.
    Check(HasPendingTagAds(batch.back().path), "cancelled continuation preserves pending journal");
    QueueTagAdsDrain({}, enqueue, publish);
    Check(pump() == 2 && last_result.empty() && pulse::ReadUtf8File(journal, journal_text) &&
        journal_text == L"PULSE_ADS_PENDING_1\n", "startup trigger resumes cancelled drain to durable completion");
    fixture = (dir / L"bad-profile").wstring(); std::filesystem::create_directory(fixture);
    Check(pulse::WriteUtf8FileAtomic(fixture + L"\\tag_ads_pending.dat", L"invalid"), "create malformed private journal");
    Check(!SyncTagAdsUpdates({removal}, &diagnostics).empty() && diagnostics.size() == 1 &&
        diagnostics[0].stage == TagAdsFailureStage::JournalRead && !diagnostics[0].unsupported,
        "malformed journal reports read stage without unsupported claim");
    TagAdsNoticeState notice;
    std::wstring title, message;
    const TagAdsFailure first{file, TagAdsFailureStage::StreamWrite, ERROR_SHARING_VIOLATION};
    const TagAdsFailure second{file + L"-other", TagAdsFailureStage::StreamWrite, ERROR_ACCESS_DENIED};
    ApplyTagAdsNotice(notice, {first}, title, message, true);
    Check(!message.empty() && message == TagAdsFailureMessage(first), "failure installs owned sync banner");
    ApplyTagAdsNotice(notice, {second}, title, message, false);
    Check(message == TagAdsFailureMessage(second), "remaining pending failure survives resolution of first even on background page");
    ApplyTagAdsNotice(notice, {}, title, message, false);
    Check(title.empty() && message.empty(), "successful sync clears resolved owned banner on background page");
    ApplyTagAdsNotice(notice, {first}, title, message, true);
    title = L"Unrelated notification"; message = L"Keep this warning";
    ApplyTagAdsNotice(notice, {}, title, message, true);
    Check(title == L"Unrelated notification" && message == L"Keep this warning",
        "successful sync cannot clear unrelated replacement banner");
    ApplyTagAdsNotice(notice, {first}, title, message, true);
    Check(message == L"Keep this warning", "sync retry cannot overwrite unrelated banner");
    uint64_t revision = 0;
    Check(AcceptTagAdsResult(revision, 2) && !AcceptTagAdsResult(revision, 1) &&
        !AcceptTagAdsResult(revision, 2) && AcceptTagAdsResult(revision, 3),
        "late and duplicate snapshots cannot override newer sync result");
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    Check(!error, "remove only exclusive private fixture");
    return failures ? 1 : 0;
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--tag-ads") return TagAdsOnly();
    using namespace pulse; using namespace pulse::app;
    auto dir = std::filesystem::absolute(L"bench_data/persistence-regression-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir); fixture = dir.wstring();
    const auto config = fixture + L"\\app.json";
    AppPrefs original; original.background_image = L"original.png";
    Check(original.Save(), "fixture saved");
    std::wstring before; ReadUtf8File(config, before);
    HANDLE locked = CreateFileW(config.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    AppPrefs prefs; prefs.persist = false;
    Check(!prefs.Load() && prefs.load_failed, "PERSIST-01 existing locked configuration records failure");
    CloseHandle(locked); prefs.persist = true;
    Check(!prefs.Save(), "PERSIST-01 unlock alone cannot overwrite with defaults");
    std::wstring after; ReadUtf8File(config, after);
    Check(before==after, "PERSIST-01 original bytes retained");
    prefs.persist=false;
    Check(prefs.Load() && !prefs.load_failed && prefs.background_image==L"original.png", "PERSIST-01 successful reread restores settings");
    WriteUtf8FileAtomic(config, L"{\"background_image\":");
    Check(!prefs.Load() && prefs.load_failed, "PERSIST-01 malformed configuration rejected");
    prefs.persist=true; Check(!prefs.Save(), "PERSIST-01 malformed config protected");
    WriteUtf8FileAtomic(config,before); prefs.persist=false; prefs.Load(); prefs.persist=true;
    LayoutTabSnapshot saved; saved.layout=1; saved.focused=1; saved.target=0;
    PaneFolderSnapshot pc; PaneFolderSnapshot disk; disk.path=L"C:\\";
    saved.panes={pc,disk}; LayoutTab tab;
    RestoreLayoutTab(tab,saved,[](Tab& view,const std::wstring& path){view.current_path=path;});
    Check(tab.panes.size()==2 && tab.panes[0]->ActiveTab()->current_path.empty() && tab.panes[1]->ActiveTab()->current_path==L"C:\\" && tab.focused_index==1 && tab.target_index==0, "PERSIST-02 This PC preserves pane order focus and target");
    const std::wstring places_file=fixture+L"\\places.json";
    const std::wstring json=LR"({"workspaces":[{"name":"a}b\"c","root":"C:\\project}2026","layout":0,"panes":["C:\\project}2026"]},{"name":"next","root":"C:\\next"}],"tags":[{"id":"t","name":"tag}x","rgb":0xABCDEF,"paths":[]}],"networks":[{"name":"n}x","unc":"\\\\host\\share}"}]})";
    WriteUtf8FileAtomic(places_file,json);
    {
        PlacesCatalog places; places.persist=false;
        Check(places.Load() && places.workspaces.size()==2 && places.workspaces[0].name==L"a}b\"c" && places.tags[0].name==L"tag}x" && places.networks[0].name==L"n}x", "PERSIST-03 quoted braces escapes and following records load");
        places.persist=true; Check(places.Save(), "PERSIST-03 round trip saves"); places.persist=false;
        Check(places.Load() && places.workspaces.size()==2 && places.workspaces[0].name==L"a}b\"c", "PERSIST-03 round trip retains records");
        WriteUtf8FileAtomic(places_file,L"{broken");
        Check(!places.Load() && places.load_failed && places.workspaces.size()==2, "PERSIST-03 corrupt reload retains live catalog");
        places.persist=true; Check(!places.Save(), "PERSIST-03 corrupt file cannot be overwritten");
        places.persist=false;
    }
    IoTaskQueue queue; std::wstring order;
    queue.Push([&]{order+=L"add";},true);queue.Push([&]{order+=L"remove";},true);
    auto first=queue.Pop();Check(!queue.Ready(), "PERSIST-04 second ADS update cannot overtake active update");
    queue.Push([&]{order+=L"other";},false);auto parallel=queue.Pop();parallel.task();queue.Complete(parallel);
    first.task();queue.Complete(first);auto second=queue.Pop();second.task();queue.Complete(second);
    Check(order==L"otheraddremove", "PERSIST-04 serial FIFO keeps unrelated IO available");
    const auto file=fixture+L"\\tagged.txt";WriteUtf8FileAtomic(file,L"fixture");
    Check(WriteTagAdsV2(file,{{L"t",L"label",0xABCDEF}}), "PERSIST-04 create ADS");
    locked=CreateFileW((file+L":Pulse.Tag").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    Check(!WriteTagAdsV2(file,{}), "PERSIST-04 deletion failure reported");
    TagAdsUpdate removal; removal.path=file;
    Check(!SyncTagAdsUpdates({removal}).empty() && HasPendingTagAds(file), "PERSIST-04 failed delete retains authoritative pending intent");
    const auto fixture_saved=fixture;fixture+=L"\\other";HasPendingTagAds(file);fixture=fixture_saved;
    Check(HasPendingTagAds(file), "PERSIST-04 pending intent survives journal reload");
    { PlacesCatalog catalog;catalog.persist=false;
      catalog.MergeAdsRecords(file,{{L"old",L"stale",0}},{});
      Check(catalog.tags.empty(), "PERSIST-04 pending path rejects stale ADS reimport"); }
    CloseHandle(locked);
    Check(SyncTagAdsUpdates({}).empty() && !HasPendingTagAds(file), "PERSIST-04 retry clears pending intent after success");
    Check(WriteTagAdsV2(file,{}) && ReadTagAdsV2(file).empty(), "PERSIST-04 delete retry removes ADS");
    const auto old=fixture+L"\\wallpaper.png"; WriteUtf8FileAtomic(old,L"old-image");prefs.background_image=old;prefs.Save();
    Check(!prefs.StoreBackgroundImage(fixture+L"\\missing.png") && prefs.background_image==old && std::filesystem::exists(old), "PERSIST-05 missing source preserves previous image");
    const auto source=fixture+L"\\new.png";WriteUtf8FileAtomic(source,L"new-image");
    locked=CreateFileW(config.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
    Check(!prefs.StoreBackgroundImage(source) && prefs.background_image==old && std::filesystem::exists(old), "PERSIST-05 configuration commit failure preserves previous image");CloseHandle(locked);
    Check(prefs.StoreBackgroundImage(source) && std::filesystem::exists(prefs.background_image) && !std::filesystem::exists(old), "PERSIST-05 successful commit precedes old image removal");
    AppPrefs reloaded;reloaded.persist=false;Check(reloaded.Load() && reloaded.background_image==prefs.background_image, "PERSIST-05 new cache path survives reload");
    {
        using Tools = std::vector<std::wstring>;
        AppPrefs legacy; legacy.persist=false;
        Check(legacy.FromJson(LR"({"toolbar_pins":5})") && legacy.toolbar_tools==Tools{L"new_text",L"terminal"}, "TOOLBAR-01 legacy pin mask becomes the tool list");
        legacy.toolbar_tools={L"hidden",L"copy_path",L"bogus",L"hidden",L"theme"};
        const std::wstring saved_json=legacy.ToJson();
        AppPrefs again; again.persist=false;
        Check(again.FromJson(saved_json) && again.toolbar_tools==Tools{L"hidden",L"copy_path",L"theme"}, "TOOLBAR-02 tool list round trips; unknown and repeated ids dropped");
        Check(saved_json.find(L"\"toolbar_pins\":2")!=std::wstring::npos, "TOOLBAR-03 an older Pulse still reads its pins");
        AppPrefs cleared; cleared.persist=false;
        Check(cleared.FromJson(LR"({"toolbar_tools":[],"toolbar_pins":31})") && cleared.toolbar_tools.empty(), "TOOLBAR-04 an emptied list is not refilled from the old mask");
        Check(cleared.toolbar_selection==pulse::app::DefaultToolbarSelection(), "TOOLBAR-05 prefs without toolbar_selection show every chip action");
        AppPrefs chip; chip.persist=false;
        chip.toolbar_selection={L"tags",L"bogus",L"compare",L"tags"};
        AppPrefs chip_again; chip_again.persist=false;
        Check(chip_again.FromJson(chip.ToJson()) && chip_again.toolbar_selection==Tools{L"tags",L"compare"}, "TOOLBAR-06 chip action list round trips; unknown and repeated ids dropped");
        AppPrefs chip_none; chip_none.persist=false;
        Check(chip_none.FromJson(LR"({"toolbar_selection":[]})") && chip_none.toolbar_selection.empty(), "TOOLBAR-07 an emptied chip list stays empty");
    }
    std::filesystem::remove_all(dir);
    return failures ? 1 : 0;
}
