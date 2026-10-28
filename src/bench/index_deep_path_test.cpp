#include "../index/index_engine.h"
#include "../index/index_path_builder.h"
#include <cstdio>
namespace pulse::index {
struct EngineTestAccess {
    static int32_t Fill(Engine& e, int depth, const std::wstring& name, const std::wstring& root = L"C:") {
        int32_t parent = e.AddNodeLocked(e.live_, -1, root, Engine::kFlagDir);
        for (int i = 0; i < depth; ++i) parent = e.AddNodeLocked(e.live_, parent, name, Engine::kFlagDir);
        const auto leaf = e.AddNodeLocked(e.live_, parent, L"leaf.txt", 0);
        e.ready_ = true;
        return leaf;
    }
    static std::wstring Path(Engine& e, int32_t id, bool query) {
        (void)query;  // queries and events share BuildPathLocked since a719a89
        return e.BuildPathLocked(id);
    }
    static void Parent(Engine& e, int32_t id, int32_t parent) { e.live_.nodes[static_cast<size_t>(id)].parent = parent; }
};
}
int main() {
    using namespace pulse::index;
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
    for (int depth : {0, 70, 257, 1000}) {
        Engine engine;
        const auto id = EngineTestAccess::Fill(engine, depth, L"d");
        std::wstring expected = L"C:\\";
        for (int i = 0; i < depth; ++i) expected += L"d\\";
        expected += L"leaf.txt";
        check(EngineTestAccess::Path(engine, id, false) == expected, "event/attribute path keeps complete ancestry");
        check(EngineTestAccess::Path(engine, id, true) == expected, "query path equals complete event path");
        Query q; q.needle = L"leaf"; q.rank = false;
        const auto result = engine.Search(q);
        check(result.total == 1 && result.hits.size() == 1 && result.hits[0].path == expected, "search returns complete deep path");
        const auto feed = engine.ReadFeed(false, L"C:\\", 0, 0);
        check(!feed.gap && feed.records.size() == 1 && feed.records[0].path == expected, "feed equals search path");
    }
    for (bool query : {false, true}) {
        Engine loop;
        const auto id = EngineTestAccess::Fill(loop, 3, L"d");
        EngineTestAccess::Parent(loop, 1, id);
        check(EngineTestAccess::Path(loop, id, query).empty(), "cyclic parent rejected");
        EngineTestAccess::Parent(loop, 1, 999999);
        check(EngineTestAccess::Path(loop, id, query).empty(), "out of range parent rejected");
        EngineTestAccess::Parent(loop, 1, -2);
        check(EngineTestAccess::Path(loop, id, query).empty(), "invalid negative parent rejected");
        check(EngineTestAccess::Path(loop, -1, query).empty(), "invalid start rejected");
        Engine oversized;
        const auto huge = EngineTestAccess::Fill(oversized, 140, std::wstring(255, L'd'));
        check(EngineTestAccess::Path(oversized, huge, query).empty(), "oversized full path rejected rather than truncated");
        Engine relative;
        const auto rel = EngineTestAccess::Fill(relative, 1, L"d", L"relative");
        check(EngineTestAccess::Path(relative, rel, query).empty(), "relative root rejected");
        Engine unc;
        const auto network = EngineTestAccess::Fill(unc, 70, L"d", L"\\\\server\\share");
        const auto path = EngineTestAccess::Path(unc, network, query);
        check(path.starts_with(L"\\\\server\\share\\d\\") && path.ends_with(L"\\leaf.txt"), "deep UNC path keeps server and share");
    }
    return failures ? 1 : 0;
}
