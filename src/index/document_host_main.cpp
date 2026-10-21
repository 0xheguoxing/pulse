#include "document_protocol.h"
#include "../fs/cloud_state.h"
#include "document_filter.h"
#include "office_ooxml.h"
#include "pdfium_text.h"
#include "document_memory_trace.h"
#include "document_file_version.h"
#include <windows.h>
#include <objbase.h>
#include <filterr.h>
#include <algorithm>
#include <cwctype>
#include <new>
#include <string>
#include <thread>

namespace {
bool ReadAll(HANDLE pipe, void* output, DWORD size) {
    auto* bytes = static_cast<uint8_t*>(output);
    while (size) {
        DWORD n = 0;
        if (!ReadFile(pipe, bytes, size, &n, nullptr) || !n) return false;
        bytes += n; size -= n;
    }
    return true;
}
bool WriteAll(HANDLE pipe, HANDLE ready, const void* input, DWORD size) {
    const auto* bytes = static_cast<const uint8_t*>(input);
    while (size) {
        DWORD n = 0;
        if (!WriteFile(pipe, bytes, (std::min)(size, DWORD{64 * 1024}), &n, nullptr) || !n) return false;
        SetEvent(ready);
        bytes += n; size -= n;
    }
    return true;
}
DWORD Extract(const std::wstring& path, uint64_t maximum_bytes, std::wstring& body, uint64_t& size,
              std::wstring_view stop_needle, bool case_sensitive, pulse::index::DocumentReadMetrics& metrics,
              const pulse::index::document::Request& request, pulse::index::document::Begin& begin,
              bool& began, HANDLE output, HANDLE ready) {
    // Keep the checked file stable while OPC/IFilter reopen it by name. File
    // access itself is inside the parent's cancellation and timeout boundary.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    struct File { HANDLE value; ~File() { CloseHandle(value); } } owner{file};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) return GetLastError();
    if (pulse::index::document::QueryFileVersion(file, info, begin.version)) begin.flags = 1;
    if (!pulse::index::document::FallbackSourceMatches(request, begin)) return ERROR_FILE_INVALID;
    constexpr DWORD unavailable = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
        ::pulse::fs::kCloudPlaceholderAttrs;
    if (info.dwFileAttributes & unavailable) return ERROR_FILE_OFFLINE;
    const uint64_t file_bytes = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (file_bytes > (std::min)(maximum_bytes, pulse::index::document::kMaximumFileBytes)) return ERROR_FILE_TOO_LARGE;
    began = true;
    if (!WriteAll(output, ready, &begin, sizeof(begin))) return ERROR_BROKEN_PIPE;
    const auto dot = path.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (ext == L".docx" || ext == L".xlsx" || ext == L".pptx")
        hr = pulse::index::ExtractOfficeOpenXml(path, body, stop_needle, case_sensitive, &metrics);
    else if (ext == L".doc" || ext == L".xls" || ext == L".ppt" || ext == L".rtf") {
        pulse::index::DocumentMetricScope timing(metrics.filter_us);
        hr = pulse::index::ExtractOfficeFilter(path, body, stop_needle, case_sensitive);
    } else if (ext == L".pdf") {
        const bool filter_only = begin.engine == 2;
        if (!filter_only) {
            try { hr = pulse::index::ExtractPdfiumText(file, file_bytes, body, stop_needle, case_sensitive, metrics); }
            catch (const std::bad_alloc&) { hr = E_OUTOFMEMORY; }
        }
        // Retry compatibility failures, not ordinary nonmatches or image-only documents.
        const bool fallback = !filter_only && begin.config_mode != 1 &&
            (hr == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) ||
             hr == HRESULT_FROM_WIN32(ERROR_BAD_FORMAT) || hr == HRESULT_FROM_WIN32(ERROR_READ_FAULT));
        if (filter_only || fallback) {
            if (fallback || (request.flags & 2)) ++metrics.pdf_fallbacks;
            // The stock PDF filter requires MTA; keep legacy Office on its STA.
            std::thread pdf([&] {
                hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (FAILED(hr)) return;
                try { pulse::index::DocumentMetricScope timing(metrics.filter_us); hr = pulse::index::ExtractOfficeFilter(path, body, stop_needle, case_sensitive); }
                catch (const std::bad_alloc&) { hr = E_OUTOFMEMORY; }
                catch (...) { hr = E_FAIL; }
                CoUninitialize();
            });
            pdf.join();
        }
        // Empty extraction is not evidence that a scanned PDF was searched.
        if (SUCCEEDED(hr) && body.find_first_not_of(L" \t\r\n\0", 0, 5) == std::wstring::npos)
            hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    if (SUCCEEDED(hr)) { size = file_bytes; return ERROR_SUCCESS; }
    if (hr == FILTER_E_PASSWORD || hr == FILTER_E_ACCESS || hr == E_ACCESSDENIED) return ERROR_ACCESS_DENIED;
    if (hr == FILTER_E_UNKNOWNFORMAT || hr == REGDB_E_CLASSNOTREG || hr == E_NOINTERFACE) return ERROR_NOT_SUPPORTED;
    if (hr == E_OUTOFMEMORY) return ERROR_NOT_ENOUGH_MEMORY;
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) return HRESULT_CODE(hr);
    return ERROR_BAD_FORMAT;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 5) return 1;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const auto input = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[1], nullptr, 10)));
    const auto output = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[2], nullptr, 10)));
    const auto request_ready = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[3], nullptr, 10)));
    const auto response_ready = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[4], nullptr, 10)));
    if (!input || !output || !request_ready || !response_ready) return 1;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    using namespace pulse::index::document;
    pulse::index::DocumentMemoryTrace memory_trace;
    uint64_t document_sequence = 0;
    for (;;) {
        if (WaitForSingleObject(request_ready, kIdleMs) != WAIT_OBJECT_0) return 0;
        Request request;
        if (!ReadAll(input, &request, sizeof(request))) return 0;
        if (request.magic != kMagic || !request.path_chars || request.path_chars > 32767 ||
            request.maximum_bytes > kMaximumFileBytes || request.stop_needle_chars > 4096 || request.flags > 3 ||
            request.expected_config > 3 || ((request.flags & 2) ?
                (request.expected_config != 0 || request.override_engine != 2) : request.override_engine != 0)) return 3;
        std::wstring path(request.path_chars, L'\0');
        if (!ReadAll(input, path.data(), request.path_chars * sizeof(wchar_t))) return 0;
        if (path.find(L'\0') != std::wstring::npos) return 3;
        std::wstring stop_needle(request.stop_needle_chars, L'\0');
        if (!ReadAll(input, stop_needle.data(), request.stop_needle_chars * sizeof(wchar_t))) return 0;
        ++document_sequence;
        memory_trace.Sample(document_sequence, "before_extract", path);
        DWORD document_error = 0;
        {
            std::wstring body;
            Response response;
            Begin begin;
            begin.config_mode = PdfEngineMode();
            const auto dot = path.find_last_of(L'.');
            const bool pdf = dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".pdf") == 0;
            begin.engine = pdf ? ((request.override_engine == 2 || begin.config_mode == 2) ? 2u : 1u) : 0u;
            bool began = false;
            const auto cpu_start = pulse::index::DocumentCpuMicros();
            try { response.error = Extract(path, request.maximum_bytes, body, response.file_bytes, stop_needle,
                (request.flags & 1) != 0, response.metrics, request, begin, began, output, response_ready); }
            catch (const std::bad_alloc&) { response.error = ERROR_NOT_ENOUGH_MEMORY; }
            catch (...) { response.error = ERROR_BAD_FORMAT; }
            response.metrics.cpu_us = pulse::index::DocumentCpuMicros() - cpu_start;
            if (!began) {
                begin.error = response.error;
                if (!WriteAll(output, response_ready, &begin, sizeof(begin))) return 0;
            }
            memory_trace.Sample(document_sequence, "after_extract", path, response.error);
            if (body.size() > kMaximumTextChars) response.error = ERROR_FILE_TOO_LARGE;
            if (response.error) { body.clear(); response.file_bytes = 0; }
            response.text_chars = static_cast<uint32_t>(body.size());
            if (!WriteAll(output, response_ready, &response, sizeof(response)) ||
                !WriteAll(output, response_ready, body.data(), response.text_chars * sizeof(wchar_t))) return 0;
            document_error = response.error;
        }
        memory_trace.Sample(document_sequence, "after_release", path, document_error);
        Completion completion;
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
            completion.flags = 1;
            completion.private_bytes = memory.PrivateUsage;
            if (completion.private_bytes >= kRetirePrivateBytes) completion.flags |= 2;
        }
        if (!WriteAll(output, response_ready, &completion, sizeof(completion))) return 0;
    }
}
