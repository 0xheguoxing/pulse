#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {
// How well a folder Pulse already knows (open tabs, Quick access, recent
// folders) answers what was typed into quick search. Lower is better; -1 is
// no match. Every space-separated word has to match; the worst word counts.
//   0 exact name   1 name prefix   2 word prefix   3 substring
//   4 pinyin from the first character   5 pinyin elsewhere   6 word initials
// A word with a backslash ("项目\jg", "d:\jg") also needs its head in the
// parent path.
// `label` is the name Pulse shows when it differs from the folder name on
// disk ("下载" for Downloads); either may match.
int JumpMatchRank(std::wstring_view path, std::wstring_view query, bool pinyin, std::wstring_view label = {});

struct JumpCandidate {
    std::wstring path, label;
};
// Best matches first, ties in list order; at most `limit` indexes.
std::vector<size_t> RankJumpFolders(const std::vector<JumpCandidate>& folders, std::wstring_view query,
                                    bool pinyin, size_t limit);
}
