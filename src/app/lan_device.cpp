#include "lan_device.h"

#include <cwctype>

namespace pulse::app {
namespace {

std::wstring Key(std::wstring_view text) {
    std::wstring out(text);
    for (auto& c : out) c = static_cast<wchar_t>(towlower(c));
    if (out.size() > 6 && out.ends_with(L".local")) out.resize(out.size() - 6);
    while (!out.empty() && out.back() == L'.') out.pop_back();
    return out;
}

bool SameDevice(const LanDevice& a, const LanDevice& b) {
    if (!a.ip.empty() && !b.ip.empty()) return a.ip == b.ip;
    const std::wstring ah = Key(a.host.empty() ? a.name : a.host);
    const std::wstring bh = Key(b.host.empty() ? b.name : b.host);
    if (!ah.empty() && ah == bh) return true;
    return !a.name.empty() && Key(a.name) == Key(b.name);
}

bool Fill(std::wstring& field, const std::wstring& value) {
    if (!field.empty() || value.empty()) return false;
    field = value;
    return true;
}

}  // namespace

std::wstring MdnsInstanceName(std::wstring_view target) {
    // The instance label ends at the first unescaped '.'.
    std::wstring out;
    for (size_t i = 0; i < target.size(); ++i) {
        const wchar_t c = target[i];
        if (c == L'.') break;
        if (c == L'\\' && i + 1 < target.size()) {
            if (i + 3 < target.size() && iswdigit(target[i + 1]) && iswdigit(target[i + 2]) && iswdigit(target[i + 3])) {
                const int value = (target[i + 1] - L'0') * 100 + (target[i + 2] - L'0') * 10 + (target[i + 3] - L'0');
                out += static_cast<wchar_t>(value);
                i += 3;
            } else {
                out += target[++i];
            }
            continue;
        }
        out += c;
    }
    return out;
}

std::wstring TxtValue(const std::vector<std::wstring>& txt, std::wstring_view key) {
    for (const auto& entry : txt) {
        const size_t eq = entry.find(L'=');
        if (eq != key.size()) continue;
        bool match = true;
        for (size_t i = 0; i < eq && match; ++i) match = towlower(entry[i]) == towlower(key[i]);
        if (match) return entry.substr(eq + 1);
    }
    return {};
}

bool MergeLanDevice(std::vector<LanDevice>& list, const LanDevice& device) {
    if (device.name.empty() && device.host.empty() && device.ip.empty()) return false;
    for (auto& known : list) {
        if (!SameDevice(known, device)) continue;
        bool changed = false;
        changed |= Fill(known.name, device.name);
        changed |= Fill(known.host, device.host);
        changed |= Fill(known.ip, device.ip);
        changed |= Fill(known.model, device.model);
        if ((known.sources | device.sources) != known.sources) {
            known.sources |= device.sources;
            changed = true;
        }
        return changed;
    }
    list.push_back(device);
    if (list.back().name.empty()) list.back().name = !device.host.empty() ? Key(device.host) : device.ip;
    return true;
}

std::wstring LanDeviceUnc(const LanDevice& device) {
    if (!device.host.empty()) return L"\\\\" + device.host;
    if (!device.ip.empty()) return L"\\\\" + device.ip;
    return {};
}

namespace {
std::wstring Lower(std::wstring_view text) {
    std::wstring out(text);
    for (auto& c : out) c = static_cast<wchar_t>(towlower(c));
    return out;
}

// "ds920+" / "rs1221" / "ts-453d": letters, then a digit (or '-' for QNAP).
bool ModelPrefix(const std::wstring& model, std::wstring_view prefix, bool dash) {
    if (model.size() <= prefix.size() || !model.starts_with(prefix)) return false;
    const wchar_t next = model[prefix.size()];
    return dash ? next == L'-' : iswdigit(next) != 0;
}
}  // namespace

uint8_t LanDeviceKind(const LanDevice& device) {
    const std::wstring model = Lower(device.model);
    for (std::wstring_view mac : {L"macmini", L"macbook", L"macpro", L"macstudio", L"imac", L"xserve", L"mac"})
        if (model.starts_with(mac)) return kLanKindMac;
    if (ModelPrefix(model, L"ds", false) || ModelPrefix(model, L"rs", false) || ModelPrefix(model, L"dva", false) ||
        ModelPrefix(model, L"ts", true) || ModelPrefix(model, L"tvs", true) || ModelPrefix(model, L"tbs", true))
        return kLanKindNas;
    static constexpr std::wstring_view kNasWords[] = {
        L"synology", L"diskstation", L"rackstation", L"qnap", L"truenas", L"freenas", L"unraid",
        L"asustor", L"terramaster", L"readynas", L"nas",
    };
    for (const std::wstring& text : {model, Lower(device.name), Lower(device.host)})
        for (std::wstring_view word : kNasWords)
            if (text.find(word) != std::wstring::npos) return kLanKindNas;
    return kLanKindPc;
}

} // namespace pulse::app
