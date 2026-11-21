// Native NanoDLP layer encoding; wire layout follows NanoDLPFile.cs (AGPLv3).
#ifndef SLIC3R_FORMAT_NANODLP_ENCODER_HPP
#define SLIC3R_FORMAT_NANODLP_ENCODER_HPP
#include <cstdint>
#include <vector>
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/Exception.hpp"
namespace Slic3r::nanodlp {
enum class ImageMode { Mono, RGB };
inline sla::EncodedRaster encode_layer(const void *ptr, size_t width, size_t height,
                                      size_t components, ImageMode mode = ImageMode::Mono)
{
    if (!ptr || components != 1 || width == 0 || height == 0 ||
        width > 32768 || height > 32768 || uint64_t(width) * height > 128ULL * 1024 * 1024)
        throw ExportError("Invalid NanoDLP grayscale raster dimensions or components");
    if (mode == ImageMode::Mono) {
        // Packed RGB is a byte reshape, NOT a luminance or color conversion.
        // Each RGB pixel represents three consecutive 8-bit grayscale pixels.
        // For other widths, ordinary PNG8 is also supported by NanoDLP/UVtools.
        if (width % 3 == 0)
            return sla::PNGRasterEncoder{}(ptr, width / 3, height, 3);
        return sla::PNGRasterEncoder{}(ptr, width, height, 1);
    }
    // RGB NanoDLP uses full-size color pixels. LayerDocument currently supplies
    // grayscale; replicate channels without claiming a true-color layer editor.
    const auto *source = static_cast<const uint8_t *>(ptr);
    std::vector<uint8_t> rgb(width * height * 3);
    for (size_t i = 0; i < width * height; ++i)
        rgb[3*i] = rgb[3*i+1] = rgb[3*i+2] = source[i];
    return sla::PNGRasterEncoder{}(rgb.data(), width, height, 3);
}
}
#endif
