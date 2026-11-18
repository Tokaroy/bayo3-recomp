// Writes PNGs with runtime/src/png_write.h for tests/png_write_test.py to check with Python's zlib.
#include <cstdio>
#include <vector>

#include "png_write.h"

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    // 300 x 250: rows of 900 bytes, 225250 bytes of scanlines, so the data spans four stored blocks.
    const uint32_t w = 300, h = 250;
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* p = &rgb[(size_t(y) * w + x) * 3];
            p[0] = uint8_t(x), p[1] = uint8_t(y), p[2] = uint8_t(x ^ y);
        }
    const std::vector<uint8_t> png = nx::encode_png_rgb(rgb.data(), w, h);
    if (nx::encode_png_rgb(rgb.data(), 0, h).size() != 0) return 3;
    FILE* f = std::fopen(argv[1], "wb");
    if (!f) return 4;
    std::fwrite(png.data(), 1, png.size(), f);
    return std::fclose(f) == 0 ? 0 : 5;
}
