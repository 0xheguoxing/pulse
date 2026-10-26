// ai_voice_service.cpp - Speech recognition over WinHTTP (ai_voice_service.h).
#include "ai_voice_service.h"

#include "../common/json_value.h"

#include <winhttp.h>

#include <algorithm>
#include <cwctype>

namespace pulse::app::ai {
namespace {

struct Handle {
    HINTERNET value = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET h) : value(h) {}
    ~Handle() { if (value) WinHttpCloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct Url {
    std::wstring host;
    std::wstring path;   // without a trailing slash
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
};

bool Crack(const std::wstring& base, Url& url) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[1024]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(base.c_str(), static_cast<DWORD>(base.size()), 0, &parts)) return false;
    url.host.assign(host, parts.dwHostNameLength);
    url.path.assign(path, parts.dwUrlPathLength);
    while (!url.path.empty() && url.path.back() == L'/') url.path.pop_back();
    url.port = parts.nPort;
    url.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return !url.host.empty();
}

void Wipe(std::wstring& text) {
    if (!text.empty()) SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
    text.clear();
}

// The service's {"error": {"message", "type", "code"}} body (also a bare
// {"message": ...}, which some servers send).
void ReadError(const std::string& body, std::string& type, std::string& message) {
    json::Value v;
    if (!json::Parse(body, v) || !v.is_object()) return;
    if (const json::Value* err = v.find("error"); err && err->is_object()) {
        message = err->get_string("message");
        type = err->get_string("type");
        if (type.empty()) type = err->get_string("code");
        return;
    }
    if (const json::Value* err = v.find("error"); err && err->is_string()) message = err->as_string();
    if (message.empty()) message = v.get_string("message");
}

// One request; `extra_headers` end with \r\n. The key is wiped on the way out.
VoiceAnswer Call(VoiceEndpoint& endpoint, const std::wstring& suffix, const wchar_t* verb,
                 const std::wstring& extra_headers, const std::string& body, DWORD receive_ms, std::string& reply) {
    VoiceAnswer answer;
    const ULONGLONG started = GetTickCount64();
    struct KeyWipe {
        std::wstring& key;
        ~KeyWipe() { Wipe(key); }
    } key_wipe{endpoint.key};
    Url url;
    if (!Crack(endpoint.base_url, url)) {
        answer.win32_error = ERROR_INVALID_PARAMETER;
        answer.error_type = "bad_url";
        return answer;
    }
    Handle session(WinHttpOpen(L"Pulse", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value)   // Windows before 8.1: no automatic proxy
        session.value = WinHttpOpen(L"Pulse", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.value) {
        answer.win32_error = GetLastError();
        return answer;
    }
    WinHttpSetTimeouts(session.value, 8000, 10000, 20000, static_cast<int>(receive_ms));
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    Handle connection(WinHttpConnect(session.value, url.host.c_str(), url.port, 0));
    const std::wstring path = url.path + suffix;
    Handle request(connection.value ? WinHttpOpenRequest(connection.value, verb, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                         WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0)
                                    : nullptr);
    if (!request.value) {
        answer.win32_error = GetLastError();
        return answer;
    }
    std::wstring headers = L"Accept: application/json\r\n" + extra_headers;
    if (!endpoint.key.empty()) headers += L"Authorization: Bearer " + endpoint.key + L"\r\n";
    const BOOL sent = WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(headers.size()),
                                         body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                         static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    Wipe(headers);
    if (!sent || !WinHttpReceiveResponse(request.value, nullptr)) {
        answer.win32_error = GetLastError();
        answer.ms = static_cast<uint32_t>(GetTickCount64() - started);
        return answer;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    std::vector<char> buffer(8192);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read) || read == 0) break;
        if (reply.size() < 512 * 1024) reply.append(buffer.data(), read);
    }
    answer.http_status = static_cast<int>(status);
    answer.ms = static_cast<uint32_t>(GetTickCount64() - started);
    if (status != 200) ReadError(reply, answer.error_type, answer.error_message);
    return answer;
}

std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& ch : out)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return out;
}

bool LooksLikeSpeech(std::string_view id) {
    const std::string low = Lower(id);
    for (const char* word : {"whisper", "transcribe", "asr", "sensevoice", "speech", "stt", "paraformer", "voxtral"})
        if (low.find(word) != std::string::npos) return true;
    return false;
}

// SenseVoice can leave its tags in (<|zh|><|NEUTRAL|><|Speech|>); drop them
// and the whitespace around the words.
std::string Clean(std::string text) {
    for (size_t at; (at = text.find("<|")) != std::string::npos;) {
        const size_t end = text.find("|>", at + 2);
        if (end == std::string::npos) break;
        text.erase(at, end + 2 - at);
    }
    auto space = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
    size_t a = 0, b = text.size();
    while (a < b && space(text[a])) ++a;
    while (b > a && space(text[b - 1])) --b;
    return text.substr(a, b - a);
}

constexpr VoicePreset kPresets[] = {
    {"siliconflow", L"https://api.siliconflow.cn/v1", "FunAudioLLM/SenseVoiceSmall"},
    {"openai", L"https://api.openai.com/v1", "whisper-1"},
    {"custom", L"", ""},
};

} // namespace

VoiceAnswer ListVoiceModels(VoiceEndpoint endpoint) {
    std::wstring host_low = endpoint.base_url;
    for (wchar_t& ch : host_low) ch = static_cast<wchar_t>(std::towlower(ch));
    const bool silicon = host_low.find(L"siliconflow") != std::wstring::npos;
    std::string reply;
    VoiceAnswer answer = Call(endpoint, silicon ? L"/models?sub_type=speech-to-text" : L"/models", L"GET", {}, {}, 15000, reply);
    if (!answer.Ok()) return answer;
    std::vector<std::string> all;
    json::Value v;
    if (json::Parse(reply, v) && v.is_object())
        if (const json::Value* data = v.find("data"); data && data->is_array())
            for (const json::Value& m : data->items())
                if (std::string id = m.get_string("id"); !id.empty()) all.push_back(std::move(id));
    if (silicon) {
        answer.models = std::move(all);
    } else {
        for (const std::string& id : all)
            if (LooksLikeSpeech(id)) answer.models.push_back(id);
        if (answer.models.empty()) answer.models = std::move(all);
    }
    return answer;
}

VoiceAnswer TranscribeVoice(VoiceEndpoint endpoint, const std::string& wav) {
    // A boundary that cannot occur in the parts: random hex.
    LARGE_INTEGER seed{};
    QueryPerformanceCounter(&seed);
    char hex[17]{};
    uint64_t x = static_cast<uint64_t>(seed.QuadPart) * 0x9E3779B97F4A7C15ull ^ GetTickCount64();
    for (int i = 0; i < 16; ++i, x >>= 4) hex[i] = "0123456789abcdef"[x & 15u];
    const std::string boundary = std::string("----PulseVoice") + hex;
    std::string body;
    body.reserve(wav.size() + 512);
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + endpoint.model + "\r\n";
    body += "--" + boundary +
            "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    body += wav;
    body += "\r\n--" + boundary + "--\r\n";
    const std::wstring headers = L"Content-Type: multipart/form-data; boundary=" + std::wstring(boundary.begin(), boundary.end()) + L"\r\n";
    std::string reply;
    VoiceAnswer answer = Call(endpoint, L"/audio/transcriptions", L"POST", headers, body, 60000, reply);
    SecureZeroMemory(body.data(), body.size());   // the recording: not kept, not even in freed memory
    if (!answer.Ok()) return answer;
    json::Value v;
    if (json::Parse(reply, v) && v.is_object()) answer.text = Clean(v.get_string("text"));
    else answer.text = Clean(reply);   // response_format=text servers
    return answer;
}

const VoicePreset* FindVoicePreset(std::string_view id) {
    for (const VoicePreset& p : kPresets)
        if (id == p.id) return &p;
    return nullptr;
}

const VoicePreset* VoicePresets(size_t& count) {
    count = std::size(kPresets);
    return kPresets;
}

std::string PickVoiceModel(std::string_view service, const std::vector<std::string>& models) {
    if (models.empty()) return {};
    if (const VoicePreset* p = FindVoicePreset(service); p && *p->model)
        for (const std::string& id : models)
            if (id == p->model) return id;
    return models.front();
}

std::wstring ShortVoiceModel(const std::string& model) {
    const size_t slash = model.find_last_of('/');
    return json::Wide(slash == std::string::npos ? model : model.substr(slash + 1));
}

} // namespace pulse::app::ai
