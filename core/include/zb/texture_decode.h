#pragma once

#include <cstddef>
#include <cstdint>

namespace zb {

// CPU decoders for the legacy compressed texture formats that phone GPUs often lack. They turn a
// block-compressed image into plain RGBA8888 (rows top to bottom, 4 bytes per pixel), which every GPU
// accepts. Used by the GL bridge for glCompressedTexImage2D / glCompressedTexSubImage2D.
//
//   S3TC / DXT : DXT1 (RGB and RGBA), DXT3, DXT5 and their sRGB variants (0x83F0-0x83F3, 0x8C4C-0x8C4F)
//   RGTC / BC4 / BC5, LATC, 3DC : 0x8DBB 0x8DBD, 0x8C70 0x8C72, 0x87F9 0x87FA (unsigned variants)
//   ATC (Qualcomm / AMD) : 0x8C92 (RGB), 0x8C93 (explicit alpha), 0x87EE (interpolated alpha)
//
// KTX and TGA are file formats: a game reads those files itself and hands the pixels to GL. A KTX file
// with a compressed format ends up here, a TGA ends up in glTexImage2D (see swap_red_blue).

enum class TextureFamily { None, S3tc, Rgtc, Atc };

TextureFamily texture_format_family(std::uint32_t gl_format);
inline bool texture_format_decodable(std::uint32_t gl_format) {
    return texture_format_family(gl_format) != TextureFamily::None;
}

// Bytes of compressed data a width x height image needs in this format (0 if the format is not known).
std::size_t compressed_texture_size(std::uint32_t gl_format, int width, int height);

// Decodes into rgba (width * height * 4 bytes). False if the format is unknown or `size` is too small.
bool decode_compressed_texture(std::uint32_t gl_format, int width, int height, const std::uint8_t* data,
                               std::size_t size, std::uint8_t* rgba);

// Swaps bytes 0 and 2 of every pixel (BGR(A) <-> RGB(A)) in a buffer with the given row stride.
void swap_red_blue(std::uint8_t* pixels, int width, int height, int bytes_per_pixel, std::size_t row_stride);

}  // namespace zb
