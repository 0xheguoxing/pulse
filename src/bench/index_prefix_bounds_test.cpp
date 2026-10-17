#include "../index/index_engine.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>

namespace pulse::index {
struct EngineTestAccess {
    static bool Write(Engine& e, const std::wstring& path) {
        Engine::Store s;
        e.AddNodeLocked(s, -1, L"C:", Engine::kFlagDir);
        e.AddNodeLocked(s, 0, L"paper.pdf", 0);
        e.AddNodeLocked(s, 0, L"alpha.txt", 0);
        std::vector<Engine::VolState> volumes;
        return e.WriteIndexFile(path, s, volumes, 123456) &&
            MoveFileExW((path + L".tmp").c_str(), path.c_str(), 0);
    }
    static bool Load(Engine& e, const std::wstring& path) {
        std::unique_ptr<Engine::MappedFile> mapped;
        if (!e.MapIndexFile(path, mapped)) return false;
        e.AdoptMappedLocked(std::move(mapped));
        return true;
    }
};
}
int main() {
    using namespace pulse::index;
    namespace fs = std::filesystem;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    const auto root = fs::absolute(fs::path(L"bench_data") /
        (L"prefix_bounds_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    if (!fs::create_directory(root)) return 2;
    const auto path = root / L"snapshot.bin";
    Engine writer;
    if (!EngineTestAccess::Write(writer, path.wstring())) return 2;
    std::vector<char> original;
    {
        std::ifstream in(path, std::ios::binary);
        original.assign(std::istreambuf_iterator<char>(in), {});
    }
    DiskHeader header{};
    if (original.size() < sizeof(header)) return 2;
    std::memcpy(&header, original.data(), sizeof(header));
    auto verify = [&](const std::vector<char>& data, bool valid, const char* label) {
        { std::ofstream out(path, std::ios::binary | std::ios::trunc); out.write(data.data(), data.size()); }
        Engine engine;
        const bool loaded = EngineTestAccess::Load(engine, path.wstring());
        check(loaded == valid, label);
        if (loaded && valid) {
            for (const auto* term : {L"a", L"pa", L"\"paper.pdf\""}) {
                Query q; q.needle = term; q.rank = false;
                const auto result = engine.Search(q);
                check(result.total == (term[0] == L'a' ? 2u : 1u), "valid mapped prefix search");
            }
            // Name terms combined with filters are driven by the name postings
            // and must still apply every filter (and the pinyin-free literal).
            const struct { const wchar_t* needle; uint32_t total; } filtered[] = {
                {L"pa ext:pdf", 1}, {L"a ext:pdf", 1}, {L"a ext:txt", 1}, {L"pa ext:txt", 0},
                {L"a !alpha", 1}, {L"alpha !ext:txt", 0}, {L"ext:pdf pa", 1},
            };
            for (const auto& c : filtered) {
                Query q; q.needle = c.needle; q.rank = false;
                check(engine.Search(q).total == c.total, "mapped name search with filter");
            }
        }
    };
    verify(original, true, "healthy snapshot accepted");
    for (uint64_t raw : {header.prefix1_off, header.prefix2_off}) {
        const size_t off = static_cast<size_t>(raw & ~(1ull << 63));
        uint32_t count = 0;
        std::memcpy(&count, original.data() + off + 65536 * 4, 4);
        if (!off || count == 0) return 2;
        auto mutate = [&](size_t position, uint32_t value, const char* label) {
            auto corrupt = original;
            std::memcpy(corrupt.data() + position, &value, 4);
            verify(corrupt, false, label);
        };
        for (uint32_t bucket : {0u, 1u, 97u, 98u, 32768u, 65535u})
            mutate(off + bucket * 4, count + 100000, "interior bucket exceeds posting count rejected");
        mutate(off, 1, "nonzero initial bucket rejected");
        mutate(off + 65535 * 4, 0, "descending bucket rejected");
        mutate(off + 65536 * 4, 0, "tail smaller than interior rejected");
        mutate(off + 65536 * 4, UINT32_MAX, "tail outside file rejected");
        mutate(off + 65537 * 4, UINT32_MAX, "negative posting rejected");
        mutate(off + 65537 * 4, header.node_count, "posting at node limit rejected");
        mutate(off + 65537 * 4, INT32_MAX, "oversized posting rejected");
    }
    verify(original, true, "healthy snapshot reload after rejected files");
    std::error_code error;
    fs::remove_all(root, error);
    check(!error && !fs::exists(root), "owned fixture cleaned up");
    return failures ? 1 : 0;
}
