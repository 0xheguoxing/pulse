#include "text_task_reader.h"
#include "../fs/cloud_state.h"
#include <algorithm>
#include <array>
#include <cwctype>
#include <cwchar>
#include <limits>

namespace pulse::index {
namespace {
constexpr size_t kChunk = 64 * 1024;
constexpr size_t kContext = 160;
struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
uint32_t Lines(std::wstring_view text) {
    return static_cast<uint32_t>(std::count(text.begin(), text.end(), L'\n'));
}
struct LiteralPattern {
    std::wstring needle;
    bool case_sensitive;
    std::vector<wint_t> folded_needle;
    std::array<wint_t, 128> folded_ascii{};
    void Prepare() {
        if (case_sensitive || !folded_needle.empty()) return;
        folded_needle.reserve(needle.size());
        for (wchar_t c : needle) folded_needle.push_back(towlower(c));
        for (size_t i = 0; i < folded_ascii.size(); ++i) folded_ascii[i] = towlower(static_cast<wchar_t>(i));
    }
};
struct LiteralWindow {
    const LiteralPattern& pattern;
    std::wstring tail, body;
    uint32_t tail_lines = 0, skipped_lines = 0;
    void Reset() {
        tail.clear(); body.clear();
        tail_lines = skipped_lines = 0;
    }
    void Release() {
        Reset();
        std::wstring().swap(tail);
        std::wstring().swap(body);
    }
    void Append(std::wstring_view text) {
        if (!body.empty()) return;
        const auto& needle = pattern.needle;
        tail.append(text);
        auto found = tail.end();
        if (pattern.case_sensitive) found = std::search(tail.begin(), tail.end(), needle.begin(), needle.end());
        else {
            found = std::search(tail.begin(), tail.end(), pattern.folded_needle.begin(), pattern.folded_needle.end(),
                [&](wchar_t a, wint_t b) {
                    return (static_cast<unsigned>(a) < pattern.folded_ascii.size() ? pattern.folded_ascii[a] : towlower(a)) == b;
                });
        }
        if (found != tail.end()) {
            const size_t at = static_cast<size_t>(found - tail.begin());
            size_t begin = at > kContext ? at - kContext : 0;
            if (begin && tail[begin] >= 0xdc00 && tail[begin] <= 0xdfff &&
                tail[begin - 1] >= 0xd800 && tail[begin - 1] <= 0xdbff) --begin;
            size_t end = (std::min)(tail.size(), at + needle.size() + kContext);
            if (end > at + needle.size() && tail[end - 1] >= 0xd800 && tail[end - 1] <= 0xdbff) --end;
            skipped_lines = tail_lines + Lines(std::wstring_view(tail).substr(0, begin));
            body.assign(tail, begin, end - begin);
            tail.clear();
            return;
        }
        const size_t keep = needle.size() + kContext;
        if (tail.size() > keep) {
            const size_t drop = tail.size() - keep;
            tail_lines += Lines(std::wstring_view(tail).substr(0, drop));
            tail.erase(0, drop);
        }
    }
};
// Only the trailing incomplete character is carried. Windows remains the
// authority for strict validation and replacement of malformed input.
size_t CompleteBytes(const std::vector<uint8_t>& bytes, UINT page, bool final) {
    if (final || bytes.empty()) return bytes.size();
    if (page == CP_UTF8) {
        size_t begin = bytes.size() - 1;
        while (begin && (bytes[begin] & 0xc0) == 0x80) --begin;
        const uint8_t lead = bytes[begin];
        const size_t length = lead >= 0xc2 && lead <= 0xdf ? 2 :
                              lead >= 0xe0 && lead <= 0xef ? 3 :
                              lead >= 0xf0 && lead <= 0xf4 ? 4 : 1;
        return bytes.size() - begin < length ? begin : bytes.size();
    }
    size_t at = 0;
    while (at < bytes.size()) {
        if (bytes[at] < 0x80) { ++at; continue; }
        const bool lead = page == 54936 ? bytes[at] >= 0x81 && bytes[at] <= 0xfe :
                                         IsDBCSLeadByteEx(page, bytes[at]) != FALSE;
        size_t length = lead ? 2 : 1;
        if (page == 54936 && lead && at + 1 < bytes.size() && bytes[at + 1] >= 0x30 && bytes[at + 1] <= 0x39)
            length = 4;
        if (at + length > bytes.size()) return at;
        at += length;
    }
    return at;
}
struct Decoder {
    UINT page = CP_UTF8;
    DWORD flags = 0;
    bool valid = true;
    std::vector<uint8_t> pending;
    std::wstring decoded;
    LiteralWindow match;
    explicit Decoder(const LiteralPattern& pattern) : match{pattern} {}
    void Reset(UINT next_page, DWORD next_flags) {
        page = next_page; flags = next_flags; valid = true;
        pending.clear(); decoded.clear(); match.Reset();
    }
    void Release() {
        std::vector<uint8_t>().swap(pending);
        std::wstring().swap(decoded);
        match.Release();
    }
    void Append(const uint8_t* bytes, size_t size, bool final) {
        if (!valid) return;
        pending.insert(pending.end(), bytes, bytes + size);
        const size_t complete = CompleteBytes(pending, page, final);
        if (!complete) return;
        const int count = static_cast<int>(complete);
        const auto* raw = reinterpret_cast<const char*>(pending.data());
        // These code pages emit at most one UTF16 code unit per input byte.
        // One bounded conversion therefore validates and decodes the block.
        decoded.resize(complete);
        const int chars = MultiByteToWideChar(page, flags, raw, count, decoded.data(), count);
        if (chars <= 0) { valid = false; return; }
        decoded.resize(static_cast<size_t>(chars));
        match.Append(decoded);
        pending.erase(pending.begin(), pending.begin() + complete);
    }
};
bool Stable(HANDLE file, const std::wstring& path, const BY_HANDLE_FILE_INFORMATION& before,
            const FILE_BASIC_INFO& before_basic) {
    BY_HANDLE_FILE_INFORMATION after{}, current{};
    FILE_BASIC_INFO after_basic{}, current_basic{};
    if (!GetFileInformationByHandle(file, &after) || before.nFileSizeHigh != after.nFileSizeHigh ||
        before.nFileSizeLow != after.nFileSizeLow || CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) ||
        !GetFileInformationByHandleEx(file, FileBasicInfo, &after_basic, sizeof(after_basic)) ||
        before_basic.ChangeTime.QuadPart != after_basic.ChangeTime.QuadPart)
        return false;
    File path_file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    return path_file.handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(path_file.handle, &current) &&
        GetFileInformationByHandleEx(path_file.handle, FileBasicInfo, &current_basic, sizeof(current_basic)) &&
        current.nFileIndexHigh == before.nFileIndexHigh && current.nFileIndexLow == before.nFileIndexLow &&
        current.dwVolumeSerialNumber == before.dwVolumeSerialNumber &&
        current_basic.ChangeTime.QuadPart == before_basic.ChangeTime.QuadPart;
}
}

struct TaskTextLiteralReader::Impl {
    LiteralPattern pattern;
    std::vector<uint8_t> bytes;
    Decoder primary, fallback;
    LiteralWindow utf16;
    std::wstring wide;
    Impl(std::wstring_view needle, bool case_sensitive)
        : pattern{std::wstring(needle), case_sensitive}, primary(pattern), fallback(pattern), utf16{pattern} {}
    void Release() {
        std::vector<uint8_t>().swap(bytes);
        std::vector<wint_t>().swap(pattern.folded_needle);
        std::wstring().swap(wide);
        primary.Release(); fallback.Release(); utf16.Release();
    }
};
TaskTextLiteralReader::TaskTextLiteralReader(std::wstring_view needle, bool case_sensitive)
    : impl_(std::make_unique<Impl>(needle, case_sensitive)) {}
TaskTextLiteralReader::~TaskTextLiteralReader() = default;

bool TaskTextLiteralReader::Read(const std::wstring& path, uint64_t maximum_bytes,
                               TaskTextLiteralResult& result, DWORD* error,
                               text::Encoding encoding, const std::function<bool()>& cancelled,
                               const std::function<bool(const TaskTextMetadata&)>& preflight) {
    result = {};
    auto fail = [&](DWORD code) {
        result.body.clear(); result.version.clear(); result.skipped_lines = 0;
        if (code == ERROR_CANCELLED) impl_->Release();
        if (error) *error = code;
        return false;
    };
    auto stopped = [&] { return cancelled && cancelled(); };
    if (stopped()) return fail(ERROR_CANCELLED);
    const auto& needle = impl_->pattern.needle;
    if (needle.empty() || needle.size() > (std::numeric_limits<size_t>::max)() - kContext)
        return fail(ERROR_INVALID_PARAMETER);
    File file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (file.handle == INVALID_HANDLE_VALUE) return fail(GetLastError());
    BY_HANDLE_FILE_INFORMATION before{};
    FILE_BASIC_INFO before_basic{};
    if (!GetFileInformationByHandle(file.handle, &before) ||
        !GetFileInformationByHandleEx(file.handle, FileBasicInfo, &before_basic, sizeof(before_basic)))
        return fail(GetLastError());
    const uint64_t size = (uint64_t(before.nFileSizeHigh) << 32) | before.nFileSizeLow;
    constexpr DWORD unsafe = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
        FILE_ATTRIBUTE_DEVICE | ::pulse::fs::kCloudPlaceholderAttrs;
    if (before.dwFileAttributes & unsafe) return fail(ERROR_ACCESS_DENIED);
    if (size > maximum_bytes) return fail(ERROR_FILE_TOO_LARGE);
    wchar_t version[64]{};
    swprintf_s(version, L"%lu:%lu:%lu:%lld", before.dwVolumeSerialNumber, before.nFileIndexHigh,
        before.nFileIndexLow, static_cast<long long>(before_basic.ChangeTime.QuadPart));
    if (preflight) {
        const TaskTextMetadata metadata{size,
            (uint64_t(before.ftLastWriteTime.dwHighDateTime) << 32) | before.ftLastWriteTime.dwLowDateTime,
            before.dwFileAttributes, version};
        const bool selected = preflight(metadata);
        if (stopped()) return fail(ERROR_CANCELLED);
        if (!selected) {
            result.skipped = true;
            if (error) *error = ERROR_SUCCESS;
            return true;
        }
    }
    auto& bytes = impl_->bytes;
    bytes.resize(static_cast<size_t>((std::min)(uint64_t(kChunk), size)));
    const UINT page = encoding == text::Encoding::System ? GetACP() :
                      encoding == text::Encoding::Gb18030 ? 54936 : CP_UTF8;
    impl_->pattern.Prepare();
    auto& primary = impl_->primary;
    auto& fallback = impl_->fallback;
    auto& utf16 = impl_->utf16;
    primary.Reset(page, MB_ERR_INVALID_CHARS);
    fallback.Reset(GetACP(), 0);
    utf16.Reset();
    bool first = true, little = false, big = false, auto_fallback = encoding == text::Encoding::Auto;
    const LiteralWindow* selected = nullptr;
    while (result.bytes_read < size) {
        if (stopped()) return fail(ERROR_CANCELLED);
        const DWORD wanted = static_cast<DWORD>((std::min)(uint64_t(kChunk), size - result.bytes_read));
        DWORD read = 0;
        if (!::ReadFile(file.handle, bytes.data(), wanted, &read, nullptr)) return fail(GetLastError());
        if (read != wanted) return fail(ERROR_RETRY);
        result.bytes_read += read;
        size_t offset = 0;
        if (first) {
            bytes.resize(read);
            if (text::LooksBinary(bytes)) return fail(ERROR_BAD_FORMAT);
            little = read >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe;
            big = read >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff;
            if (little || big) {
                if (size % 2) return fail(ERROR_BAD_FORMAT);
                offset = 2;
            } else if (read >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) {
                offset = 3; primary.page = CP_UTF8; auto_fallback = false;
            }
            first = false;
        }
        const bool final = result.bytes_read == size;
        if (little || big) {
            auto& decoded = impl_->wide;
            decoded.clear();
            decoded.reserve((read - offset) / 2);
            for (size_t at = offset; at + 1 < read; at += 2)
                decoded.push_back(static_cast<wchar_t>(little ? bytes[at] | (bytes[at + 1] << 8) :
                                                                         (bytes[at] << 8) | bytes[at + 1]));
            utf16.Append(decoded);
            if (!utf16.body.empty() || final) { selected = &utf16; break; }
        } else {
            primary.Append(bytes.data() + offset, read - offset, final);
            // At EOF a valid primary decoding is authoritative. The Auto
            // fallback cannot change this result, so do not decode it again.
            if (final && primary.valid) { selected = &primary.match; break; }
            if (auto_fallback) fallback.Append(bytes.data() + offset, read - offset, final);
            if (!primary.valid && !auto_fallback) return fail(ERROR_BAD_FORMAT);
            // Auto's encoding decision depends on the unread suffix. An early
            // result is safe only when both possible decodings show the same hit.
            if (auto_fallback && fallback.valid && !fallback.match.body.empty() &&
                (!primary.valid || (primary.match.body == fallback.match.body &&
                                    primary.match.skipped_lines == fallback.match.skipped_lines))) {
                selected = &fallback.match; break;
            }
            if (final) {
                if (primary.valid) selected = &primary.match;
                else if (auto_fallback && fallback.valid) selected = &fallback.match;
                else return fail(ERROR_BAD_FORMAT);
            }
        }
    }
    if (stopped()) return fail(ERROR_CANCELLED);
    if (!Stable(file.handle, path, before, before_basic)) return fail(ERROR_RETRY);
    if (selected) { result.body = selected->body; result.skipped_lines = selected->skipped_lines; }
    result.version.assign(version);
    if (error) *error = ERROR_SUCCESS;
    return true;
}
bool ReadTaskTextLiteral(const std::wstring& path, uint64_t maximum_bytes,
                         TaskTextLiteralResult& result, DWORD* error,
                         text::Encoding encoding, const std::function<bool()>& cancelled,
                         std::wstring_view needle, bool case_sensitive) {
    TaskTextLiteralReader reader(needle, case_sensitive);
    return reader.Read(path, maximum_bytes, result, error, encoding, cancelled);
}
}
