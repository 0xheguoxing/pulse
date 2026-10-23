// Quick jump P1: the double-Ctrl tap logic and the 跳转 folder ranking.
#include "../app/double_ctrl.h"
#include "../app/jump_match.h"
#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
// Feeds "C+ C- C+ C-" style events: each step is {ctrl, down, at_ms}.
struct Step { bool ctrl, down; uint64_t at; };
int Fires(const std::vector<Step>& steps, uint32_t gap = 300) {
    pulse::DoubleCtrlDetector d;
    d.SetGap(gap);
    int fired = 0;
    for (const auto& s : steps) fired += d.Key(s.ctrl, s.down, s.at) ? 1 : 0;
    return fired;
}
}

int main() {
    using pulse::app::JumpMatchRank;
    // Double Ctrl
    Check(Fires({{true, true, 0}, {true, false, 80}, {true, true, 200}, {true, false, 270}}) == 1, "two quick taps fire");
    Check(Fires({{true, true, 0}, {true, false, 80}, {true, true, 450}, {true, false, 520}}) == 0, "slow second tap does not fire");
    Check(Fires({{true, true, 0}, {true, false, 80}, {true, true, 330}, {true, false, 400}}, 400) == 1, "400 ms setting accepts a slower tap");
    Check(Fires({{true, true, 0}, {true, false, 500}, {true, true, 600}, {true, false, 650}}) == 0, "long hold is not a tap");
    Check(Fires({{true, true, 0}, {false, true, 40}, {false, false, 60}, {true, false, 80}, {true, true, 200}, {true, false, 260}}) == 0, "Ctrl+C then Ctrl does not fire");
    Check(Fires({{true, true, 0}, {true, false, 80}, {false, true, 120}, {true, true, 200}, {true, false, 260}}) == 0, "key between taps does not fire");
    Check(Fires({{true, true, 0}, {true, true, 30}, {true, true, 60}, {true, false, 90}, {true, true, 200}, {true, true, 230}, {true, false, 260}}) == 1, "auto-repeat inside a tap is fine");
    Check(Fires({{true, true, 0}, {true, false, 60}, {true, true, 150}, {true, false, 210}, {true, true, 300}, {true, false, 360}}) == 1, "triple tap fires once");
    Check(Fires({{true, true, 0}, {true, false, 60}, {true, true, 150}, {true, false, 210}, {true, true, 300}, {true, false, 360}, {true, true, 450}, {true, false, 510}}) == 2, "four taps fire twice (toggle back)");
    {
        pulse::DoubleCtrlDetector d; d.SetGap(300);
        d.Key(true, true, 0); d.Interrupt(); d.Key(true, false, 60);
        const bool fired = d.Key(true, true, 150) | d.Key(true, false, 200);
        Check(!fired, "Ctrl+click spoils the first tap");
        Check(!d.Pending(1000), "nothing pending after the gesture");
    }
    {
        pulse::DoubleCtrlDetector d; d.SetGap(300);
        d.Key(true, true, 0); d.Key(true, false, 60);
        Check(d.Pending(200) && !d.Pending(400), "pending only within the gap");
    }
    // Jump ranking
    Check(JumpMatchRank(L"D:\\Work\\Pulse", L"pulse", true) == 0, "exact name");
    Check(JumpMatchRank(L"D:\\Work\\PulseBuild", L"pul", true) == 1, "name prefix");
    Check(JumpMatchRank(L"C:\\Program Files", L"files", true) == 2, "word prefix");
    Check(JumpMatchRank(L"D:\\Work\\MyProject", L"proj", true) == 2, "camelCase word prefix");
    Check(JumpMatchRank(L"D:\\Work\\backup", L"cku", true) == 3, "substring");
    Check(JumpMatchRank(L"D:\\资料\\项目归档", L"xmgd", true) == 4, "pinyin initials");
    Check(JumpMatchRank(L"D:\\资料\\项目归档", L"xiangmu", true) == 4, "full pinyin");
    Check(JumpMatchRank(L"D:\\资料\\我的项目", L"xm", true) == 5, "pinyin inside the name");
    Check(JumpMatchRank(L"D:\\资料\\项目归档", L"xmgd", false) == -1, "pinyin off");
    Check(JumpMatchRank(L"C:\\Program Files", L"pf", true) == 6, "word initials");
    Check(JumpMatchRank(L"D:\\Work\\Pulse", L"zz", true) == -1, "no match");
    Check(JumpMatchRank(L"D:\\项目\\接口", L"项目\\jk", true) == 4, "head in parent path");
    Check(JumpMatchRank(L"E:\\项目\\接口", L"d:\\jk", true) == -1, "head must be in parent path");
    Check(JumpMatchRank(L"D:\\资料\\项目\\接口", L"xm\\jk", true) == 4, "pinyin head");
    Check(JumpMatchRank(L"D:\\Work\\Pulse Build", L"pulse bu", true) == 2, "every word must match");
    Check(JumpMatchRank(L"D:\\Work\\Pulse Build", L"pulse zz", true) == -1, "a missing word fails");
    Check(JumpMatchRank(L"C:\\Users\\me\\Downloads", L"xz", true, L"下载") == 4, "shown name matches by pinyin");
    Check(JumpMatchRank(L"C:\\Users\\me\\Downloads", L"down", true, L"下载") == 1, "disk name still matches");
    {
        const std::vector<pulse::app::JumpCandidate> paths{{L"D:\\a\\xyz pulse", {}}, {L"D:\\b\\Pulse", {}}, {L"D:\\c\\PulseX", {}}, {L"D:\\d\\other", {}}};
        const auto order = pulse::app::RankJumpFolders(paths, L"pulse", true, 4);
        Check(order.size() == 3 && order[0] == 1 && order[1] == 2 && order[2] == 0, "ranked order");
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
