// SPDX-License-Identifier: GPL-3.0-or-later
// A PNG file from 8-bit RGB pixels, with no compression library: the image data goes into "stored" deflate blocks
// (RFC 1951 BTYPE 00), so the file is about the size of the pixels, and every viewer opens it. Used for screenshots
// (nvn_debug.cpp); tests/png_write_test.py checks the output with Python's own zlib.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace nx {

namespace png_detail {

inline uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
    static const auto table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

inline void put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24)), out.push_back(uint8_t(v >> 16)), out.push_back(uint8_t(v >> 8)),
        out.push_back(uint8_t(v));
}

inline void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32(out, uint32_t(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(out.data() + start, out.size() - start));
}

}  // namespace png_detail

// `rgb` is width * height * 3 bytes, rows top to bottom. Empty on a zero size.
inline std::vector<uint8_t> encode_png_rgb(const uint8_t* rgb, uint32_t width, uint32_t height) {
    using namespace png_detail;
    std::vector<uint8_t> out;
    if (!rgb || !width || !height) return out;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    out.insert(out.end(), signature, signature + 8);
    std::vector<uint8_t> ihdr;
    put32(ihdr, width), put32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8 bits, truecolour, deflate, adaptive filtering, no interlace
    chunk(out, "IHDR", ihdr);

    // The filtered scanlines: filter type 0 (none) before each row.
    const size_t row = size_t(width) * 3;
    std::vector<uint8_t> raw;
    raw.reserve((row + 1) * height);
    for (uint32_t y = 0; y < height; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb + y * row, rgb + (y + 1) * row);
    }
    // zlib stream: header 0x78 0x01, stored blocks of at most 65535 bytes, Adler-32 of the raw data.
    std::vector<uint8_t> z;
    z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    z.push_back(0x78), z.push_back(0x01);
    size_t at = 0;
    do {
        const size_t n = raw.size() - at < 65535 ? raw.size() - at : 65535;
        const bool last = at + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)), z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + ptrdiff_t(at), raw.begin() + ptrdiff_t(at + n));
        at += n;
    } while (at < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) a = (a + v) % 65521, b = (b + a) % 65521;
    put32(z, (b << 16) | a);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

}  // namespace nx
