// action_policy.cpp - The assistant's gate for each action (action_policy.h).
#include "action_policy.h"

#include <cwctype>
#include <iterator>

namespace pulse::app::actions {

namespace {

bool SameFolded(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    return true;
}

} // namespace

bool IsProgramPath(std::wstring_view path) noexcept {
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring_view name = slash == std::wstring_view::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring_view::npos || dot + 1 >= name.size()) return false;
    const std::wstring_view ext = name.substr(dot + 1);
    static constexpr std::wstring_view kPrograms[] = {
        L"exe", L"com", L"scr", L"pif", L"bat", L"cmd", L"ps1", L"psm1", L"vbs", L"vbe", L"js", L"jse",
        L"wsf", L"wsh", L"hta", L"msi", L"msp", L"msix", L"msixbundle", L"appx", L"appxbundle", L"cpl",
        L"reg", L"jar", L"lnk", L"url", L"application", L"appref-ms",
    };
    for (const std::wstring_view p : kPrograms)
        if (SameFolded(ext, p)) return true;
    return false;
}

Policy AiPolicy(std::string_view id, std::wstring_view target) noexcept {
    const ActionEntry* e = FindAction(id);
    if (!e) return {Gate::Locked, ActionRisk::UserOnly};
    ActionRisk risk = e->risk;
    if (id == "settings.set") return {Gate::Direct, risk};
    if (id == "nav.open" && IsProgramPath(target)) risk = ActionRisk::External;
    switch (risk) {
    case ActionRisk::Read:
    case ActionRisk::View:
    case ActionRisk::Organize:
    case ActionRisk::Create: return {Gate::Direct, risk};
    case ActionRisk::System:
    case ActionRisk::External: return {Gate::Confirm, risk};
    case ActionRisk::Move:
    case ActionRisk::Delete:
    case ActionRisk::PermanentDelete: return {Gate::Later, risk};
    case ActionRisk::UserOnly: break;
    }
    return {Gate::Locked, risk};
}

const char* RiskName(ActionRisk risk) noexcept {
    static constexpr const char* kNames[] = {"read", "view", "organize", "create", "move", "system",
                                             "external", "delete", "permanent_delete", "user_only"};
    const size_t i = static_cast<size_t>(risk);
    return i < std::size(kNames) ? kNames[i] : "user_only";
}

} // namespace pulse::app::actions
