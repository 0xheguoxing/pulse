#include "jpeg_dc_preview.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>

namespace pulse::preview {
namespace {

constexpr uint64_t kMaxBlocksPerComponent = 32ull * 1024ull * 1024ull;  // ~2 GP pictures

struct Huffman {
    bool defined = false;
    // 9-bit lookup: (length << 8) | symbol, 0 when the code is longer.
    std::array<uint16_t, 512> fast{};
    int32_t maxcode[18]{};
    int32_t valptr[17]{};
    int32_t mincode[17]{};
    uint8_t values[256]{};
};

bool BuildHuffman(const uint8_t counts[16], const uint8_t* symbols, size_t total, Huffman& h) {
    h = Huffman{};
    if (total > 256) return false;
    std::memcpy(h.values, symbols, total);
    int32_t code = 0;
    size_t k = 0;
    for (int len = 1; len <= 16; ++len) {
        const int n = counts[len - 1];
        h.valptr[len] = static_cast<int32_t>(k);
        h.mincode[len] = code;
        for (int i = 0; i < n; ++i, ++k, ++code) {
            if (len <= 9) {
                const int shift = 9 - len;
                const int first = code << shift;
                for (int j = 0; j < (1 << shift); ++j)
                    h.fast[static_cast<size_t>(first + j)] =
                        static_cast<uint16_t>((len << 8) | h.values[k]);
            }
        }
        h.maxcode[len] = n ? code - 1 : -1;
        if (code > (1 << len)) return false;  // over-subscribed table
        code <<= 1;
    }
    h.maxcode[17] = INT32_MAX;
    h.defined = true;
    return true;
}

class BitReader {
public:
    BitReader(const uint8_t* p, const uint8_t* end) : p_(p), end_(end) {}
    // Bits past a marker read as zeros (the standard's padding); bits past
    // the end of the buffer set ran_out().
    void Fill() {
        while (n_ <= 56) {
            uint8_t byte = 0;
            if (!marker_ && p_ < end_) {
                byte = *p_++;
                if (byte == 0xFF) {
                    if (p_ >= end_) { ran_out_ = true; --p_; byte = 0; marker_ = true; }
                    else if (*p_ == 0x00) ++p_;
                    else { marker_ = true; --p_; byte = 0; }
                }
            } else if (!marker_) {
                ran_out_ = true;
            }
            acc_ |= static_cast<uint64_t>(byte) << (56 - n_);
            n_ += 8;
            if (marker_ || ran_out_) { n_ = 64; break; }  // remaining bits are zero
        }
    }
    int Decode(const Huffman& h) {
        if (n_ < 16) Fill();
        const uint16_t fast = h.fast[static_cast<size_t>(acc_ >> 55)];
        if (fast) {
            Consume(fast >> 8);
            return fast & 0xFF;
        }
        for (int len = 10; len <= 16; ++len) {
            const int32_t code = static_cast<int32_t>(acc_ >> (64 - len));
            if (code <= h.maxcode[len]) {
                const int32_t index = h.valptr[len] + code - h.mincode[len];
                if (index < 0 || index > 255) return -1;
                Consume(len);
                return h.values[index];
            }
        }
        return -1;
    }
    int32_t Receive(int s) {
        if (s == 0) return 0;
        if (n_ < s) Fill();
        int32_t v = static_cast<int32_t>(acc_ >> (64 - s));
        Consume(s);
        if (v < (1 << (s - 1))) v -= (1 << s) - 1;
        return v;
    }
    // After a restart interval: drop the padding bits and step over RSTn.
    bool Restart() {
        acc_ = 0; n_ = 0;
        if (!marker_) {
            while (p_ + 1 < end_ && !(p_[0] == 0xFF && p_[1] >= 0xD0 && p_[1] <= 0xD7)) ++p_;
        }
        marker_ = false;
        if (p_ + 1 >= end_) { ran_out_ = true; return false; }
        if (p_[0] != 0xFF || p_[1] < 0xD0 || p_[1] > 0xD7) return false;
        p_ += 2;
        return true;
    }
    bool ran_out() const { return ran_out_; }
    // Position of the marker that ended the scan (or the scan data still unread).
    const uint8_t* Position() const { return p_; }
private:
    void Consume(int bits) { acc_ <<= bits; n_ -= bits; }
    const uint8_t* p_;
    const uint8_t* end_;
    uint64_t acc_ = 0;
    int n_ = 0;
    bool marker_ = false;
    bool ran_out_ = false;
};

struct Component {
    uint8_t id = 0, h = 1, v = 1, tq = 0;
    uint32_t grid_w = 0, grid_h = 0;  // padded to whole MCUs
    uint32_t coded_w = 0, coded_h = 0;  // blocks a single-component scan codes
    std::vector<uint8_t> dc;  // one sample per block (the block's mean)
    bool done = false;
};

uint16_t Read16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

inline uint8_t Clamp(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

} // namespace

JpegDcStatus DecodeProgressiveJpegDc(const uint8_t* data, size_t size, uint32_t min_edge,
                                     JpegDcPicture& out) {
    out = JpegDcPicture{};
    if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8) return JpegDcStatus::Unsupported;
    const uint8_t* const end = data + size;
    const uint8_t* p = data + 2;
    uint16_t quant[4][64]{};
    bool quant_defined[4]{};
    Huffman dc_tables[4];
    std::vector<Component> comps;
    uint32_t width = 0, height = 0, restart_interval = 0;
    int hmax = 1, vmax = 1;
    uint32_t mcux = 0, mcuy = 0;
    bool frame = false, adobe = false;
    int adobe_transform = -1;
    size_t comps_done = 0;

    while (true) {
        // Next marker (fill bytes 0xFF allowed).
        while (p < end && *p != 0xFF) ++p;  // tolerate junk between segments
        while (p < end && *p == 0xFF) ++p;
        if (p >= end) return JpegDcStatus::NeedMoreData;
        const uint8_t marker = *p++;
        // 0x00: a stuffed byte met while stepping past scan data.
        if (marker == 0x00 || marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) continue;
        if (marker == 0xD9) return JpegDcStatus::Corrupt;  // EOI before every DC
        if (end - p < 2) return JpegDcStatus::NeedMoreData;
        const uint16_t length = Read16(p);
        if (length < 2) return JpegDcStatus::Corrupt;
        if (static_cast<size_t>(end - p) < length) return JpegDcStatus::NeedMoreData;
        const uint8_t* seg = p + 2;
        const uint8_t* seg_end = p + length;
        p = seg_end;

        if (marker == 0xC2) {  // SOF2: progressive Huffman
            if (frame || seg_end - seg < 6) return JpegDcStatus::Corrupt;
            if (seg[0] != 8) return JpegDcStatus::Unsupported;
            height = Read16(seg + 1);
            width = Read16(seg + 3);
            const int n = seg[5];
            if (!width || !height) return JpegDcStatus::Unsupported;  // DNL height
            if (n != 1 && n != 3) return JpegDcStatus::Unsupported;
            if (seg_end - seg < 6 + 3 * n) return JpegDcStatus::Corrupt;
            comps.resize(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) {
                const uint8_t* c = seg + 6 + 3 * i;
                comps[i].id = c[0];
                comps[i].h = static_cast<uint8_t>(c[1] >> 4);
                comps[i].v = static_cast<uint8_t>(c[1] & 15);
                comps[i].tq = c[2];
                if (comps[i].h < 1 || comps[i].h > 4 || comps[i].v < 1 || comps[i].v > 4 ||
                    comps[i].tq > 3) return JpegDcStatus::Corrupt;
                hmax = (std::max)(hmax, static_cast<int>(comps[i].h));
                vmax = (std::max)(vmax, static_cast<int>(comps[i].v));
            }
            const uint32_t out_w = (width + 7) / 8, out_h = (height + 7) / 8;
            if ((std::max)(out_w, out_h) < min_edge) return JpegDcStatus::Unsupported;
            mcux = (width + 8u * hmax - 1) / (8u * hmax);
            mcuy = (height + 8u * vmax - 1) / (8u * vmax);
            for (auto& c : comps) {
                c.grid_w = mcux * c.h;
                c.grid_h = mcuy * c.v;
                if (static_cast<uint64_t>(c.grid_w) * c.grid_h > kMaxBlocksPerComponent)
                    return JpegDcStatus::Unsupported;
                const uint32_t cw = (width * c.h + hmax - 1) / hmax;
                const uint32_t ch = (height * c.v + vmax - 1) / vmax;
                c.coded_w = (cw + 7) / 8;
                c.coded_h = (ch + 7) / 8;
                c.dc.assign(static_cast<size_t>(c.grid_w) * c.grid_h, 128);
            }
            frame = true;
        } else if (marker == 0xC0 || marker == 0xC1 || marker == 0xC3 ||
                   (marker >= 0xC5 && marker <= 0xCF && marker != 0xC8 && marker != 0xCC)) {
            return JpegDcStatus::Unsupported;  // baseline / lossless / arithmetic
        } else if (marker == 0xDB) {  // DQT
            const uint8_t* q = seg;
            while (q < seg_end) {
                const int precision = *q >> 4, id = *q & 15;
                ++q;
                if (id > 3 || precision > 1) return JpegDcStatus::Corrupt;
                const size_t bytes = precision ? 128 : 64;
                if (static_cast<size_t>(seg_end - q) < bytes) return JpegDcStatus::Corrupt;
                for (int i = 0; i < 64; ++i)
                    quant[id][i] = precision ? Read16(q + 2 * i) : q[i];
                quant_defined[id] = true;
                q += bytes;
            }
        } else if (marker == 0xC4) {  // DHT
            const uint8_t* q = seg;
            while (q < seg_end) {
                if (seg_end - q < 17) return JpegDcStatus::Corrupt;
                const int cls = *q >> 4, id = *q & 15;
                const uint8_t* counts = q + 1;
                size_t total = 0;
                for (int i = 0; i < 16; ++i) total += counts[i];
                q += 17;
                if (cls > 1 || id > 3 || static_cast<size_t>(seg_end - q) < total)
                    return JpegDcStatus::Corrupt;
                if (cls == 0 && !BuildHuffman(counts, q, total, dc_tables[id]))
                    return JpegDcStatus::Corrupt;
                q += total;
            }
        } else if (marker == 0xDD) {  // DRI
            if (seg_end - seg < 2) return JpegDcStatus::Corrupt;
            restart_interval = Read16(seg);
        } else if (marker == 0xEE) {  // APP14 Adobe: colour transform flag
            if (seg_end - seg >= 12 && std::memcmp(seg, "Adobe", 5) == 0) {
                adobe = true;
                adobe_transform = seg[11];
            }
        } else if (marker == 0xDA) {  // SOS
            if (!frame || seg_end - seg < 1) return JpegDcStatus::Corrupt;
            const int ns = seg[0];
            if (ns < 1 || ns > 4 || seg_end - seg < 1 + 2 * ns + 3) return JpegDcStatus::Corrupt;
            std::vector<Component*> scan;
            std::vector<const Huffman*> tables;
            for (int i = 0; i < ns; ++i) {
                const uint8_t id = seg[1 + 2 * i];
                const int td = seg[2 + 2 * i] >> 4;
                Component* found = nullptr;
                for (auto& c : comps) if (c.id == id) found = &c;
                if (!found || td > 3) return JpegDcStatus::Corrupt;
                scan.push_back(found);
                tables.push_back(&dc_tables[td]);
            }
            const uint8_t* tail = seg + 1 + 2 * ns;
            const int ss = tail[0], se = tail[1], ah = tail[2] >> 4, al = tail[2] & 15;
            const bool dc_first = ss == 0 && se == 0 && ah == 0;
            bool wanted = dc_first;
            for (auto* c : scan) if (c->done) wanted = false;
            if (!wanted) {
                // AC or refinement scan: skip its entropy-coded data.
                while (p + 1 < end && !(p[0] == 0xFF && p[1] != 0x00 && !(p[1] >= 0xD0 && p[1] <= 0xD7))) ++p;
                if (p + 1 >= end) return JpegDcStatus::NeedMoreData;
                continue;
            }
            if (al > 13) return JpegDcStatus::Corrupt;
            for (size_t i = 0; i < scan.size(); ++i) {
                if (!tables[i]->defined || !quant_defined[scan[i]->tq]) return JpegDcStatus::Corrupt;
            }
            BitReader bits(p, end);
            int32_t pred[4]{};
            uint32_t until_restart = restart_interval;
            auto decode_block = [&](size_t i, uint8_t* sample) -> bool {
                const int s = bits.Decode(*tables[i]);
                if (s < 0 || s > 15) return false;
                // Valid 8-bit data stays within +-2^11; the clamp only keeps
                // corrupt input from overflowing.
                pred[i] = (std::clamp)(pred[i] + bits.Receive(s), -(1 << 20), 1 << 20);
                const int64_t coefficient = static_cast<int64_t>(pred[i]) * (int64_t{1} << al);
                // A DC-only block is flat at DC * Q / 8 around the level shift.
                const int64_t level = ((coefficient * quant[scan[i]->tq][0]) >> 3) + 128;
                *sample = static_cast<uint8_t>(level < 0 ? 0 : level > 255 ? 255 : level);
                return true;
            };
            auto restart = [&]() -> bool {
                if (!restart_interval) return true;
                if (until_restart == 0) {
                    if (!bits.Restart()) return false;
                    std::memset(pred, 0, sizeof(pred));
                    until_restart = restart_interval;
                }
                --until_restart;
                return true;
            };
            bool ok = true;
            if (ns == 1) {
                Component& c = *scan[0];
                for (uint32_t by = 0; ok && by < c.coded_h; ++by)
                    for (uint32_t bx = 0; ok && bx < c.coded_w; ++bx)
                        ok = restart() && decode_block(0, &c.dc[static_cast<size_t>(by) * c.grid_w + bx]);
            } else {
                for (uint32_t my = 0; ok && my < mcuy; ++my) {
                    for (uint32_t mx = 0; ok && mx < mcux; ++mx) {
                        ok = restart();
                        for (size_t i = 0; ok && i < scan.size(); ++i) {
                            Component& c = *scan[i];
                            for (uint32_t v = 0; ok && v < c.v; ++v)
                                for (uint32_t h = 0; ok && h < c.h; ++h)
                                    ok = decode_block(i, &c.dc[static_cast<size_t>(my * c.v + v) * c.grid_w +
                                                               mx * c.h + h]);
                        }
                    }
                }
            }
            if (bits.ran_out()) return JpegDcStatus::NeedMoreData;
            if (!ok) return JpegDcStatus::Corrupt;
            for (auto* c : scan) { c->done = true; ++comps_done; }
            p = bits.Position();
            if (comps_done == comps.size()) break;
        }
        // Other segments (APPn, COM, ...) are skipped by length.
    }

    out.source_width = width;
    out.source_height = height;
    out.width = (width + 7) / 8;
    out.height = (height + 7) / 8;
    if (comps.size() == 1) {
        out.channels = 1;
        out.stride = (out.width + 3) & ~3u;
        out.pixels.assign(static_cast<size_t>(out.stride) * out.height, 0);
        const Component& c = comps[0];
        for (uint32_t y = 0; y < out.height; ++y)
            std::memcpy(&out.pixels[static_cast<size_t>(y) * out.stride],
                        &c.dc[static_cast<size_t>(y) * c.grid_w], out.width);
        return JpegDcStatus::Made;
    }
    // Three components: YCbCr unless an Adobe marker says untransformed, or
    // the component ids spell R G B (libjpeg's convention).
    const bool rgb = (adobe && adobe_transform == 0) ||
                     (!adobe && comps[0].id == 'R' && comps[1].id == 'G' && comps[2].id == 'B');
    out.channels = 3;
    out.stride = (out.width * 3 + 3) & ~3u;
    out.pixels.assign(static_cast<size_t>(out.stride) * out.height, 0);
    std::vector<uint32_t> cols[3];
    for (int k = 0; k < 3; ++k) {
        cols[k].resize(out.width);
        for (uint32_t x = 0; x < out.width; ++x)
            cols[k][x] = (std::min)(x * comps[k].h / hmax, comps[k].grid_w - 1);
    }
    for (uint32_t y = 0; y < out.height; ++y) {
        const uint8_t* rows[3];
        for (int k = 0; k < 3; ++k) {
            const uint32_t by = (std::min)(y * comps[k].v / vmax, comps[k].grid_h - 1);
            rows[k] = &comps[k].dc[static_cast<size_t>(by) * comps[k].grid_w];
        }
        uint8_t* dst = &out.pixels[static_cast<size_t>(y) * out.stride];
        for (uint32_t x = 0; x < out.width; ++x, dst += 3) {
            const int c0 = rows[0][cols[0][x]], c1 = rows[1][cols[1][x]], c2 = rows[2][cols[2][x]];
            if (rgb) {
                dst[0] = static_cast<uint8_t>(c2); dst[1] = static_cast<uint8_t>(c1); dst[2] = static_cast<uint8_t>(c0);
                continue;
            }
            // JFIF YCbCr -> RGB, 16.16 fixed point.
            const int cb = c1 - 128, cr = c2 - 128;
            const int y16 = c0 << 16;
            dst[2] = Clamp((y16 + 91881 * cr + 32768) >> 16);
            dst[1] = Clamp((y16 - 22554 * cb - 46802 * cr + 32768) >> 16);
            dst[0] = Clamp((y16 + 116130 * cb + 32768) >> 16);
        }
    }
    return JpegDcStatus::Made;
}

bool DecodeProgressiveJpegDcFile(const std::wstring& path, uint32_t min_edge, JpegDcPicture& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool made = false;
    // 64 KB settles baseline files (SOF0) at once; the DC scans of a 100 MP
    // photo take 1-2 MB, so the read grows 8x while the decoder asks for more.
    constexpr uint64_t kMaxRead = 512ull * 1024ull * 1024ull;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 4 &&
        static_cast<uint64_t>(size.QuadPart) <= kMaxRead) {
        const size_t total = static_cast<size_t>(size.QuadPart);
        std::vector<uint8_t> bytes;
        size_t want = (std::min)(total, static_cast<size_t>(64u * 1024u));
        while (true) {
            const size_t have = bytes.size();
            bytes.resize(want);
            bool read_ok = true;
            for (size_t done = have; read_ok && done < want;) {
                DWORD n = 0;
                const DWORD chunk = static_cast<DWORD>((std::min)(want - done, static_cast<size_t>(1u << 24)));
                read_ok = ReadFile(file, bytes.data() + done, chunk, &n, nullptr) && n > 0;
                done += n;
            }
            if (!read_ok) break;
            const JpegDcStatus status = DecodeProgressiveJpegDc(bytes.data(), bytes.size(), min_edge, out);
            if (status == JpegDcStatus::Made) { made = true; break; }
            if (status != JpegDcStatus::NeedMoreData || want == total) break;
            want = (std::min)(total, want * 8);
        }
    }
    CloseHandle(file);
    if (!made) out = JpegDcPicture{};
    return made;
}

} // namespace pulse::preview
