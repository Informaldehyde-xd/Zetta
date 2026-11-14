#include "zb/texture_decode.h"

#include <algorithm>
#include <cmath>

namespace zb {

namespace {

constexpr std::uint32_t kDxt1Rgb = 0x83F0;
constexpr std::uint32_t kDxt1Rgba = 0x83F1;
constexpr std::uint32_t kDxt3 = 0x83F2;
constexpr std::uint32_t kDxt5 = 0x83F3;
constexpr std::uint32_t kSrgbDxt1 = 0x8C4C;
constexpr std::uint32_t kSrgbAlphaDxt1 = 0x8C4D;
constexpr std::uint32_t kSrgbAlphaDxt3 = 0x8C4E;
constexpr std::uint32_t kSrgbAlphaDxt5 = 0x8C4F;
constexpr std::uint32_t kRedRgtc1 = 0x8DBB;
constexpr std::uint32_t kRgRgtc2 = 0x8DBD;
constexpr std::uint32_t kLatc1 = 0x8C70;
constexpr std::uint32_t kLatc2 = 0x8C72;
constexpr std::uint32_t k3dcX = 0x87F9;
constexpr std::uint32_t k3dcXy = 0x87FA;
constexpr std::uint32_t kAtcRgb = 0x8C92;
constexpr std::uint32_t kAtcExplicit = 0x8C93;
constexpr std::uint32_t kAtcInterpolated = 0x87EE;

struct Rgb {
    int r, g, b;
};

Rgb expand565(std::uint32_t c) {
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}

Rgb expand555(std::uint32_t c) {
    const int r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
    return {(r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)};
}

std::uint32_t read16(const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8); }
std::uint32_t read32(const std::uint8_t* p) { return read16(p) | (read16(p + 2) << 16); }

// 8-byte block of 16 values: two end points and 3-bit indices (DXT5 alpha, BC4, BC5 channels).
void decode_value_block(const std::uint8_t* b, std::uint8_t out[16]) {
    const int v0 = b[0], v1 = b[1];
    int palette[8];
    palette[0] = v0;
    palette[1] = v1;
    if (v0 > v1) {
        for (int i = 1; i <= 6; ++i) palette[1 + i] = ((7 - i) * v0 + i * v1) / 7;
    } else {
        for (int i = 1; i <= 4; ++i) palette[1 + i] = ((5 - i) * v0 + i * v1) / 5;
        palette[6] = 0;
        palette[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) out[i] = static_cast<std::uint8_t>(palette[(bits >> (3 * i)) & 7]);
}

// 8-byte DXT colour block. `four_colors` = always use the 4 colour mode (DXT3 / DXT5); otherwise the
// DXT1 rule applies: c0 <= c1 gives 3 colours plus transparent black.
void decode_dxt_colors(const std::uint8_t* b, bool four_colors, bool dxt1_alpha, std::uint8_t out[16][4]) {
    const std::uint32_t c0 = read16(b), c1 = read16(b + 2);
    const Rgb a = expand565(c0), z = expand565(c1);
    int pr[4] = {a.r, z.r, 0, 0}, pg[4] = {a.g, z.g, 0, 0}, pb[4] = {a.b, z.b, 0, 0}, pa[4] = {255, 255, 255, 255};
    if (four_colors || c0 > c1) {
        pr[2] = (2 * a.r + z.r) / 3; pg[2] = (2 * a.g + z.g) / 3; pb[2] = (2 * a.b + z.b) / 3;
        pr[3] = (a.r + 2 * z.r) / 3; pg[3] = (a.g + 2 * z.g) / 3; pb[3] = (a.b + 2 * z.b) / 3;
    } else {
        pr[2] = (a.r + z.r) / 2; pg[2] = (a.g + z.g) / 2; pb[2] = (a.b + z.b) / 2;
        pr[3] = pg[3] = pb[3] = 0;
        pa[3] = dxt1_alpha ? 0 : 255;
    }
    const std::uint32_t bits = read32(b + 4);
    for (int i = 0; i < 16; ++i) {
        const int k = (bits >> (2 * i)) & 3;
        out[i][0] = static_cast<std::uint8_t>(pr[k]);
        out[i][1] = static_cast<std::uint8_t>(pg[k]);
        out[i][2] = static_cast<std::uint8_t>(pb[k]);
        out[i][3] = static_cast<std::uint8_t>(pa[k]);
    }
}

int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// 8-byte ATC colour block (Qualcomm / AMD).
void decode_atc_colors(const std::uint8_t* b, std::uint8_t out[16][4]) {
    const std::uint32_t c0 = read16(b), c1 = read16(b + 2);
    int pr[4], pg[4], pb[4];
    const Rgb z = expand565(c1);
    if ((c0 & 0x8000) == 0) {
        const Rgb a = expand555(c0);
        pr[0] = a.r; pg[0] = a.g; pb[0] = a.b;
        pr[3] = z.r; pg[3] = z.g; pb[3] = z.b;
        pr[1] = (5 * a.r + 3 * z.r) / 8; pg[1] = (5 * a.g + 3 * z.g) / 8; pb[1] = (5 * a.b + 3 * z.b) / 8;
        pr[2] = (3 * a.r + 5 * z.r) / 8; pg[2] = (3 * a.g + 5 * z.g) / 8; pb[2] = (3 * a.b + 5 * z.b) / 8;
    } else {
        const Rgb a = expand555(c0 & 0x7FFF);
        pr[0] = pg[0] = pb[0] = 0;
        pr[1] = clamp255(a.r - (z.r >> 2)); pg[1] = clamp255(a.g - (z.g >> 2)); pb[1] = clamp255(a.b - (z.b >> 2));
        pr[2] = a.r; pg[2] = a.g; pb[2] = a.b;
        pr[3] = z.r; pg[3] = z.g; pb[3] = z.b;
    }
    const std::uint32_t bits = read32(b + 4);
    for (int i = 0; i < 16; ++i) {
        const int k = (bits >> (2 * i)) & 3;
        out[i][0] = static_cast<std::uint8_t>(pr[k]);
        out[i][1] = static_cast<std::uint8_t>(pg[k]);
        out[i][2] = static_cast<std::uint8_t>(pb[k]);
        out[i][3] = 255;
    }
}

// 8-byte block of 16 four-bit alphas (DXT3, ATC explicit).
void decode_explicit_alpha(const std::uint8_t* b, std::uint8_t out[16]) {
    for (int i = 0; i < 16; ++i) {
        const int nibble = (b[i >> 1] >> ((i & 1) * 4)) & 15;
        out[i] = static_cast<std::uint8_t>(nibble * 17);
    }
}

int block_bytes(std::uint32_t format) {
    switch (format) {
        case kDxt1Rgb: case kDxt1Rgba: case kSrgbDxt1: case kSrgbAlphaDxt1:
        case kRedRgtc1: case kLatc1: case k3dcX: case kAtcRgb:
            return 8;
        case kDxt3: case kDxt5: case kSrgbAlphaDxt3: case kSrgbAlphaDxt5:
        case kRgRgtc2: case kLatc2: case k3dcXy: case kAtcExplicit: case kAtcInterpolated:
            return 16;
        default:
            return 0;
    }
}

}  // namespace

TextureFamily texture_format_family(std::uint32_t f) {
    switch (f) {
        case kDxt1Rgb: case kDxt1Rgba: case kDxt3: case kDxt5:
        case kSrgbDxt1: case kSrgbAlphaDxt1: case kSrgbAlphaDxt3: case kSrgbAlphaDxt5:
            return TextureFamily::S3tc;
        case kRedRgtc1: case kRgRgtc2: case kLatc1: case kLatc2: case k3dcX: case k3dcXy:
            return TextureFamily::Rgtc;
        case kAtcRgb: case kAtcExplicit: case kAtcInterpolated:
            return TextureFamily::Atc;
        default:
            return TextureFamily::None;
    }
}

std::size_t compressed_texture_size(std::uint32_t format, int width, int height) {
    const int bytes = block_bytes(format);
    if (bytes == 0 || width <= 0 || height <= 0) return 0;
    return static_cast<std::size_t>((width + 3) / 4) * static_cast<std::size_t>((height + 3) / 4) * bytes;
}

bool decode_compressed_texture(std::uint32_t format, int width, int height, const std::uint8_t* data,
                               std::size_t size, std::uint8_t* rgba) {
    const int bytes = block_bytes(format);
    if (bytes == 0 || data == nullptr || rgba == nullptr || width <= 0 || height <= 0) return false;
    if (size < compressed_texture_size(format, width, height)) return false;

    const int blocks_w = (width + 3) / 4;
    const int blocks_h = (height + 3) / 4;
    for (int by = 0; by < blocks_h; ++by) {
        for (int bx = 0; bx < blocks_w; ++bx) {
            const std::uint8_t* block = data + (static_cast<std::size_t>(by) * blocks_w + bx) * bytes;
            std::uint8_t px[16][4];
            switch (format) {
                case kDxt1Rgb: case kSrgbDxt1:
                    decode_dxt_colors(block, false, false, px);
                    break;
                case kDxt1Rgba: case kSrgbAlphaDxt1:
                    decode_dxt_colors(block, false, true, px);
                    break;
                case kDxt3: case kSrgbAlphaDxt3: {
                    std::uint8_t alpha[16];
                    decode_explicit_alpha(block, alpha);
                    decode_dxt_colors(block + 8, true, false, px);
                    for (int i = 0; i < 16; ++i) px[i][3] = alpha[i];
                    break;
                }
                case kDxt5: case kSrgbAlphaDxt5: {
                    std::uint8_t alpha[16];
                    decode_value_block(block, alpha);
                    decode_dxt_colors(block + 8, true, false, px);
                    for (int i = 0; i < 16; ++i) px[i][3] = alpha[i];
                    break;
                }
                case kRedRgtc1: {
                    std::uint8_t r[16];
                    decode_value_block(block, r);
                    for (int i = 0; i < 16; ++i) { px[i][0] = r[i]; px[i][1] = 0; px[i][2] = 0; px[i][3] = 255; }
                    break;
                }
                case kLatc1: case k3dcX: {
                    std::uint8_t l[16];
                    decode_value_block(block, l);
                    for (int i = 0; i < 16; ++i) { px[i][0] = px[i][1] = px[i][2] = l[i]; px[i][3] = 255; }
                    break;
                }
                case kRgRgtc2: case k3dcXy: {
                    std::uint8_t r[16], g[16];
                    decode_value_block(block, r);
                    decode_value_block(block + 8, g);
                    for (int i = 0; i < 16; ++i) { px[i][0] = r[i]; px[i][1] = g[i]; px[i][2] = 0; px[i][3] = 255; }
                    break;
                }
                case kLatc2: {
                    std::uint8_t l[16], a[16];
                    decode_value_block(block, l);
                    decode_value_block(block + 8, a);
                    for (int i = 0; i < 16; ++i) { px[i][0] = px[i][1] = px[i][2] = l[i]; px[i][3] = a[i]; }
                    break;
                }
                case kAtcRgb:
                    decode_atc_colors(block, px);
                    break;
                case kAtcExplicit: {
                    std::uint8_t alpha[16];
                    decode_explicit_alpha(block, alpha);
                    decode_atc_colors(block + 8, px);
                    for (int i = 0; i < 16; ++i) px[i][3] = alpha[i];
                    break;
                }
                case kAtcInterpolated: {
                    std::uint8_t alpha[16];
                    decode_value_block(block, alpha);
                    decode_atc_colors(block + 8, px);
                    for (int i = 0; i < 16; ++i) px[i][3] = alpha[i];
                    break;
                }
                default:
                    return false;
            }
            for (int py = 0; py < 4; ++py) {
                const int y = by * 4 + py;
                if (y >= height) break;
                for (int pxl = 0; pxl < 4; ++pxl) {
                    const int x = bx * 4 + pxl;
                    if (x >= width) break;
                    std::uint8_t* out = rgba + (static_cast<std::size_t>(y) * width + x) * 4;
                    const std::uint8_t* in = px[py * 4 + pxl];
                    out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; out[3] = in[3];
                }
            }
        }
    }
    return true;
}

void swap_red_blue(std::uint8_t* pixels, int width, int height, int bytes_per_pixel, std::size_t row_stride) {
    if (pixels == nullptr || bytes_per_pixel < 3) return;
    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = pixels + static_cast<std::size_t>(y) * row_stride;
        for (int x = 0; x < width; ++x) std::swap(row[x * bytes_per_pixel], row[x * bytes_per_pixel + 2]);
    }
}

}  // namespace zb
