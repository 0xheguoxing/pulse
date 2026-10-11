// window_registry_test.cpp - Multi-window M3a: the per-process window registry
// keeps most-recently-activated order, survives unknown and repeated calls,
// and empties when the last window leaves.
#include "../app/window_registry.h"

#include <cstdio>
#include <vector>

namespace {
struct FakeWindow { int id = 0; };
int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}
HWND H(int n) { return reinterpret_cast<HWND>(static_cast<INT_PTR>(0x1000 + n)); }
std::vector<int> Order(const pulse::app::BasicWindowRegistry<FakeWindow>& r) {
    std::vector<int> ids;
    r.ForEach([&](HWND, FakeWindow& w) { ids.push_back(w.id); });
    return ids;
}
}  // namespace

int main() {
    pulse::app::BasicWindowRegistry<FakeWindow> registry;
    FakeWindow a{1}, b{2}, c{3};
    Check(registry.Empty() && !registry.MostRecent() && !registry.MostRecentHwnd(), "a new registry is empty");

    registry.Add(H(1), &a);
    Check(registry.Count() == 1 && registry.MostRecent() == &a && registry.MostRecentHwnd() == H(1),
          "the first window is the most recent one");
    registry.Add(H(2), &b);
    registry.Add(H(3), &c);
    Check((Order(registry) == std::vector<int>{3, 2, 1}), "new windows are most recent first");

    registry.Touch(H(1));
    Check((Order(registry) == std::vector<int>{1, 3, 2}), "activation moves a window to the front");
    registry.Touch(H(1));
    Check((Order(registry) == std::vector<int>{1, 3, 2}), "activating the front window changes nothing");
    registry.Touch(H(99));
    Check((Order(registry) == std::vector<int>{1, 3, 2}) && registry.Count() == 3,
          "activating an unregistered window is ignored");

    registry.Add(H(2), &b);
    Check((Order(registry) == std::vector<int>{2, 1, 3}) && registry.Count() == 3,
          "registering again does not duplicate and makes it most recent");

    Check(registry.Lookup(H(3)) == &c && registry.Lookup(H(42)) == nullptr, "lookup by window handle");
    Check(!registry.Remove(H(42)), "removing an unknown window reports false");

    Check(registry.Remove(H(2)) && registry.MostRecent() == &a,
          "closing the most recent window hands routing to the next one");
    registry.Add(nullptr, &b);
    registry.Add(H(5), nullptr);
    Check(registry.Count() == 2, "null handles or states are not registered");

    Check(registry.Remove(H(1)) && registry.Remove(H(3)) && registry.Empty() && !registry.MostRecent(),
          "the registry is empty after the last window leaves");

    std::printf("window_registry_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
