// zip_compress.h - 压缩: the selection into one .zip next to it.
//
// Written through the system's libarchive (archiveint.dll, Windows 10 1803+,
// loaded from System32 only), deflate, UTF-8 names with the language flag so
// any unzip shows Chinese names right. Extraction is left to the archiver the
// user has installed.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::app {

struct ZipCompressResult {
    bool ok = false;
    bool cancelled = false;
    std::wstring error;      // libarchive's or the system's message
    uint64_t files = 0;
    uint64_t bytes = 0;      // uncompressed bytes read
};

// True when archiveint.dll and its writer exports are there.
bool ZipCompressAvailable();

// The archive's path for `sources` (full paths, one parent): a single item
// gives "<stem>.zip", several give "<folder name>.zip"; " (2)", " (3)" ...
// keep it from replacing anything in `dir`.
std::wstring ZipPathFor(const std::vector<std::wstring>& sources, const std::wstring& dir);

// Blocking: run it off the UI thread. Writes a hidden temporary next to
// `zip_path` and renames it into place at the end, so a listing never shows a
// half-written archive and a failure leaves nothing behind. Folders recurse;
// entry names are relative to the sources' parent. `progress` (optional) gets
// bytes read so far and the total, from the worker thread.
// `names` (optional, parallel to `sources`): each source's entry name in the
// archive instead of its leaf, "/" between folders ("2026-01/a.pdf").
ZipCompressResult CompressToZip(const std::vector<std::wstring>& sources, const std::wstring& zip_path,
                                const std::atomic<bool>* cancel,
                                const std::function<void(uint64_t, uint64_t)>& progress,
                                const std::vector<std::wstring>* names = nullptr);

} // namespace pulse::app
