// lan_discovery.cpp - see lan_discovery.h. The property keys of Function
// Discovery are defined in this translation unit only (initguid).
#include "lan_discovery.h"

#include <windows.h>
#include <initguid.h>
#include <functiondiscovery.h>
#include <functiondiscoverykeys.h>
#include <netlistmgr.h>
#include <windns.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace pulse::app {
namespace {
using Microsoft::WRL::ComPtr;

class ComScope {
public:
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned_ = SUCCEEDED(hr);  // RPC_E_CHANGED_MODE: the thread already has an STA
    }
    ~ComScope() { if (owned_) CoUninitialize(); }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool owned_ = false;
};

std::wstring Lower(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(towlower(c));
    return text;
}

std::wstring TrimDot(std::wstring text) {
    while (!text.empty() && text.back() == L'.') text.pop_back();
    return text;
}

std::wstring Ipv4Text(DWORD network_order) {
    const BYTE* b = reinterpret_cast<const BYTE*>(&network_order);
    wchar_t text[16]{};
    swprintf_s(text, L"%u.%u.%u.%u", static_cast<unsigned>(b[0]), static_cast<unsigned>(b[1]),
               static_cast<unsigned>(b[2]), static_cast<unsigned>(b[3]));
    return text;
}

// ---------------------------------------------------------------------------
// mDNS. DnsServiceBrowse is looked up at run time: the import would keep
// Pulse from starting on Windows 10 builds before 1809. The request layout
// mirrors windns.h (DNS_SERVICE_BROWSE_REQUEST, version 1), which the SDK
// only declares for newer NTDDI targets.
using BrowseCallback = VOID(WINAPI*)(DWORD status, PVOID context, PDNS_RECORD records);
struct BrowseRequest {
    ULONG Version;          // DNS_QUERY_REQUEST_VERSION1
    ULONG InterfaceIndex;   // 0 = all
    PCWSTR QueryName;
    BrowseCallback pBrowseCallback;
    PVOID pQueryContext;
};
struct BrowseCancel {
    PVOID reserved;
};
using DnsServiceBrowseFn = DNS_STATUS(WINAPI*)(BrowseRequest*, BrowseCancel*);
using DnsServiceBrowseCancelFn = DNS_STATUS(WINAPI*)(BrowseCancel*);
using DnsRecordListFreeFn = VOID(WINAPI*)(PDNS_RECORD, DNS_FREE_TYPE);
constexpr DNS_STATUS kRequestPending = 9506;  // DNS_REQUEST_PENDING

struct DnsApi {
    DnsServiceBrowseFn browse = nullptr;
    DnsServiceBrowseCancelFn cancel = nullptr;
    DnsRecordListFreeFn free_list = nullptr;
    bool Ready() const { return browse && cancel && free_list; }
};

DnsApi LoadDnsApi() {
    DnsApi api;
    HMODULE module = GetModuleHandleW(L"dnsapi.dll");
    if (!module) module = LoadLibraryExW(L"dnsapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return api;
    api.browse = reinterpret_cast<DnsServiceBrowseFn>(GetProcAddress(module, "DnsServiceBrowse"));
    api.cancel = reinterpret_cast<DnsServiceBrowseCancelFn>(GetProcAddress(module, "DnsServiceBrowseCancel"));
    api.free_list = reinterpret_cast<DnsRecordListFreeFn>(GetProcAddress(module, "DnsRecordListFree"));
    return api;
}

struct MdnsAnswers;
struct BrowseContext {
    MdnsAnswers* answers = nullptr;
    bool device_info = false;
};

// What the answers said, keyed for joining at the end. Shared with the
// browse callbacks (thread pool) through a heap block that outlives a slow
// cancellation (see Finish); the callback contexts live in it too.
struct MdnsAnswers {
    BrowseContext contexts[2];
    std::mutex mutex;
    std::condition_variable done_cv;
    int finished = 0;                           // browses that reported their end
    std::map<std::wstring, std::wstring> instance_host;   // "ds920" -> "ds920.local" (SRV)
    std::map<std::wstring, std::wstring> instance_name;   // "ds920" -> "DS920" (display)
    std::map<std::wstring, std::wstring> instance_model;  // _device-info TXT model=
    std::map<std::wstring, std::wstring> host_ip;         // "ds920.local" -> IPv4
    std::vector<std::wstring> smb_instances;              // lower-case keys, in arrival order
    DnsRecordListFreeFn free_list = nullptr;
};

void Record(MdnsAnswers& a, const BrowseContext& ctx, const DNS_RECORDW* r) {
    if (!r->pName) return;
    const std::wstring owner = TrimDot(r->pName);
    switch (r->wType) {
    case DNS_TYPE_PTR: {
        if (!r->Data.PTR.pNameHost) break;
        const std::wstring target = TrimDot(r->Data.PTR.pNameHost);
        const std::wstring name = MdnsInstanceName(target);
        if (name.empty()) break;
        const std::wstring key = Lower(name);
        a.instance_name.emplace(key, name);
        if (!ctx.device_info && std::find(a.smb_instances.begin(), a.smb_instances.end(), key) == a.smb_instances.end())
            a.smb_instances.push_back(key);
        break;
    }
    case DNS_TYPE_SRV:
        if (r->Data.SRV.pNameTarget)
            a.instance_host[Lower(MdnsInstanceName(owner))] = TrimDot(r->Data.SRV.pNameTarget);
        break;
    case DNS_TYPE_A:
        a.host_ip[Lower(owner)] = Ipv4Text(r->Data.A.IpAddress);
        break;
    case DNS_TYPE_TEXT: {
        if (!ctx.device_info) break;
        std::vector<std::wstring> txt;
        for (DWORD i = 0; i < r->Data.TXT.dwStringCount; ++i)
            if (r->Data.TXT.pStringArray[i]) txt.emplace_back(r->Data.TXT.pStringArray[i]);
        const std::wstring model = TxtValue(txt, L"model");
        if (!model.empty()) a.instance_model[Lower(MdnsInstanceName(owner))] = model;
        break;
    }
    default:
        break;
    }
}

VOID WINAPI OnBrowse(DWORD status, PVOID context, PDNS_RECORD records) {
    auto* ctx = static_cast<BrowseContext*>(context);
    if (!ctx || !ctx->answers) return;
    MdnsAnswers& a = *ctx->answers;
    {
        std::lock_guard lock(a.mutex);
        for (auto* r = reinterpret_cast<const DNS_RECORDW*>(records); r; r = r->pNext) Record(a, *ctx, r);
        if (status != ERROR_SUCCESS) ++a.finished;  // ERROR_CANCELLED or a failure ends this browse
    }
    if (records && a.free_list) a.free_list(records, DnsFreeRecordList);
    if (status != ERROR_SUCCESS) a.done_cv.notify_all();
}

struct MdnsBrowse {
    DnsApi api;
    // Heap blocks owned here; leaked on purpose when the system never reports
    // the end of a cancelled browse, so a late callback has memory to touch.
    MdnsAnswers* answers = nullptr;
    BrowseCancel cancels[2]{};
    int started = 0;

    bool Start() {
        api = LoadDnsApi();
        if (!api.Ready()) return false;
        answers = new MdnsAnswers;
        answers->free_list = api.free_list;
        static const wchar_t* const kQueries[2] = {L"_smb._tcp.local", L"_device-info._tcp.local"};
        for (int i = 0; i < 2; ++i) {
            answers->contexts[i] = BrowseContext{answers, i == 1};
            BrowseRequest request{1, 0, kQueries[i], &OnBrowse, &answers->contexts[i]};
            if (api.browse(&request, &cancels[i]) == kRequestPending) started |= 1 << i;
        }
        return started != 0;
    }

    // Stops the browses and copies the joined devices into `out`.
    void Finish(std::vector<LanDevice>& out) {
        if (!answers) return;
        int expected = 0;
        for (int i = 0; i < 2; ++i) {
            if (!(started & (1 << i))) continue;
            ++expected;
            api.cancel(&cancels[i]);
        }
        bool settled = false;
        {
            std::unique_lock lock(answers->mutex);
            settled = answers->done_cv.wait_for(lock, std::chrono::milliseconds(1500),
                                                [&] { return answers->finished >= expected; });
            for (const auto& key : answers->smb_instances) {
                LanDevice device;
                device.sources = kLanSourceMdns;
                if (auto it = answers->instance_name.find(key); it != answers->instance_name.end()) device.name = it->second;
                if (auto it = answers->instance_host.find(key); it != answers->instance_host.end()) device.host = it->second;
                if (auto it = answers->instance_model.find(key); it != answers->instance_model.end()) device.model = it->second;
                if (!device.host.empty())
                    if (auto it = answers->host_ip.find(Lower(device.host)); it != answers->host_ip.end()) device.ip = it->second;
                if (device.host.empty() && device.ip.empty()) continue;  // nothing to open
                MergeLanDevice(out, device);
            }
        }
        if (settled) delete answers;
        answers = nullptr;
    }
};

// ---------------------------------------------------------------------------
// WS-Discovery through Function Discovery. Windows PCs (Function Discovery
// Resource Publication) and NAS units running wsdd answer as pub:Computer.
std::wstring PropString(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring out;
    if (SUCCEEDED(store->GetValue(key, &value))) {
        if (value.vt == VT_LPWSTR && value.pwszVal) out = value.pwszVal;
        else if (value.vt == (VT_VECTOR | VT_LPWSTR) && value.calpwstr.cElems && value.calpwstr.pElems[0])
            out = value.calpwstr.pElems[0];
    }
    PropVariantClear(&value);
    return out;
}

std::vector<std::wstring> PropStrings(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT value;
    PropVariantInit(&value);
    std::vector<std::wstring> out;
    if (SUCCEEDED(store->GetValue(key, &value))) {
        if (value.vt == VT_LPWSTR && value.pwszVal) out.emplace_back(value.pwszVal);
        else if (value.vt == (VT_VECTOR | VT_LPWSTR))
            for (ULONG i = 0; i < value.calpwstr.cElems; ++i)
                if (value.calpwstr.pElems[i]) out.emplace_back(value.calpwstr.pElems[i]);
    }
    PropVariantClear(&value);
    return out;
}

bool IsIpv4Text(const std::wstring& text) {
    int dots = 0;
    for (wchar_t c : text) {
        if (c == L'.') ++dots;
        else if (c < L'0' || c > L'9') return false;
    }
    return dots == 3;
}

bool LooksLikeHostName(const std::wstring& text) {
    if (text.empty() || text.size() > 63) return false;
    for (wchar_t c : text) if (iswspace(c) || c == L'/' || c == L'\\' || c == L':') return false;
    return true;
}

// pub:Computer values look like "NAS/Workgroup:WORKGROUP" or "PC/Domain:x".
std::wstring ComputerFromTypes(const std::vector<std::wstring>& values) {
    for (const auto& v : values) {
        const size_t slash = v.find(L'/');
        if (slash != std::wstring::npos && slash > 0 && v.find(L':', slash) != std::wstring::npos)
            return v.substr(0, slash);
    }
    return {};
}

bool DeviceFromInstance(IFunctionInstance* instance, LanDevice& device) {
    ComPtr<IPropertyStore> store;
    if (!instance || FAILED(instance->OpenPropertyStore(STGM_READ, &store)) || !store) return false;
    bool computer = false;
    for (const auto& type : PropStrings(store.Get(), PKEY_PNPX_Types))
        computer |= Lower(type).find(L"computer") != std::wstring::npos;
    for (const auto& category : PropStrings(store.Get(), PKEY_PNPX_DeviceCategory)) {
        const std::wstring c = Lower(category);
        computer |= c.find(L"computers") != std::wstring::npos || c.find(L"storage") != std::wstring::npos;
    }
    if (!computer) return false;  // printers, scanners, media renderers
    device.sources = kLanSourceWsd;
    device.name = PropString(store.Get(), PKEY_PNPX_FriendlyName);
    device.model = PropString(store.Get(), PKEY_PNPX_ModelName);
    std::wstring computer_name = ComputerFromTypes(PropStrings(store.Get(), PKEY_PNPX_Types));
    if (computer_name.empty() && LooksLikeHostName(device.name)) computer_name = device.name;
    device.host = computer_name;
    for (const auto& ip : PropStrings(store.Get(), PKEY_PNPX_IpAddress)) {
        if (IsIpv4Text(ip)) { device.ip = ip; break; }
    }
    return !device.host.empty() || !device.ip.empty();
}

class WsdNotify : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                      IFunctionDiscoveryNotification> {
public:
    STDMETHODIMP OnUpdate(QueryUpdateAction action, FDQUERYCONTEXT, IFunctionInstance* instance) override {
        if (action != QUA_ADD && action != QUA_CHANGE) return S_OK;
        LanDevice device;
        if (!DeviceFromInstance(instance, device)) return S_OK;
        std::lock_guard lock(mutex_);
        MergeLanDevice(devices_, device);
        return S_OK;
    }
    STDMETHODIMP OnError(HRESULT, FDQUERYCONTEXT, const WCHAR*) override { return S_OK; }
    STDMETHODIMP OnEvent(DWORD, FDQUERYCONTEXT, const WCHAR*) override { return S_OK; }

    void Add(IFunctionInstance* instance) { OnUpdate(QUA_ADD, 0, instance); }
    std::vector<LanDevice> Devices() {
        std::lock_guard lock(mutex_);
        return devices_;
    }

private:
    std::mutex mutex_;
    std::vector<LanDevice> devices_;
};

struct WsdBrowse {
    ComPtr<WsdNotify> notify;
    ComPtr<IFunctionDiscovery> discovery;
    ComPtr<IFunctionInstanceCollectionQuery> query;

    bool Start() {
        notify = Microsoft::WRL::Make<WsdNotify>();
        if (!notify) return false;
        if (FAILED(CoCreateInstance(__uuidof(FunctionDiscovery), nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&discovery))))
            return false;
        if (FAILED(discovery->CreateInstanceCollectionQuery(FCTN_CATEGORY_WSDISCOVERY, nullptr, FALSE,
                                                            notify.Get(), nullptr, &query)) || !query)
            return false;
        ComPtr<IFunctionInstanceCollection> collection;
        const HRESULT hr = query->Execute(&collection);
        if (FAILED(hr) && hr != E_PENDING) return false;
        DWORD count = 0;
        if (collection && SUCCEEDED(collection->GetCount(&count))) {
            for (DWORD i = 0; i < count; ++i) {
                ComPtr<IFunctionInstance> instance;
                if (SUCCEEDED(collection->Item(i, &instance))) notify->Add(instance.Get());
            }
        }
        return true;
    }

    void Finish(std::vector<LanDevice>& out) {
        query.Reset();  // releasing the query ends it
        discovery.Reset();
        if (!notify) return;
        for (const auto& device : notify->Devices()) MergeLanDevice(out, device);
    }
};

std::wstring GuidText(const GUID& id) {
    wchar_t text[64]{};
    StringFromGUID2(id, text, ARRAYSIZE(text));
    return text;
}

}  // namespace

void ReadLanNetworkIdentity(LanDiscoveryResult& out) {
    ComScope com;
    ComPtr<INetworkListManager> manager;
    if (FAILED(CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager)))) return;
    ComPtr<IEnumNetworks> networks;
    if (FAILED(manager->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &networks)) || !networks) return;
    bool any = false, any_private = false;
    for (;;) {
        ComPtr<INetwork> network;
        ULONG fetched = 0;
        if (networks->Next(1, &network, &fetched) != S_OK || !fetched || !network) break;
        any = true;
        NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PUBLIC;
        network->GetCategory(&category);
        if (category == NLM_NETWORK_CATEGORY_PUBLIC) {
            if (out.network_category == 0) {
                out.network_category = 3;
                BSTR name = nullptr;
                if (SUCCEEDED(network->GetName(&name)) && name) out.network_name = name;
                SysFreeString(name);
            }
            continue;
        }
        if (!any_private) {
            GUID id{};
            if (SUCCEEDED(network->GetNetworkId(&id))) out.network_key = GuidText(id);
            out.network_category = category == NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED ? 2 : 1;
            out.network_name.clear();
            BSTR name = nullptr;
            if (SUCCEEDED(network->GetName(&name)) && name) out.network_name = name;
            SysFreeString(name);
        }
        any_private = true;
    }
    out.offline = !any;
    out.public_network = any && !any_private;
}

LanDiscoveryResult RunLanDiscovery(const std::function<bool()>& cancel, DWORD budget_ms) {
    ComScope com;
    LanDiscoveryResult result;
    ReadLanNetworkIdentity(result);
    if (result.offline || result.public_network) return result;
    const auto cancelled = [&] { return cancel && cancel(); };
    if (cancelled()) { result.cancelled = true; return result; }

    MdnsBrowse mdns;
    result.mdns_unavailable = !mdns.Start();
    WsdBrowse wsd;
    wsd.Start();
    const ULONGLONG until = GetTickCount64() + budget_ms;
    while (GetTickCount64() < until) {
        if (cancelled()) { result.cancelled = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    mdns.Finish(result.devices);
    wsd.Finish(result.devices);
    std::stable_sort(result.devices.begin(), result.devices.end(), [](const LanDevice& a, const LanDevice& b) {
        return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return result;
}

} // namespace pulse::app
