// SPDX-License-Identifier: AGPL-3.0-or-later
// Cheap IHDR inspection shared by the ZIP layer readers. The editor decodes a
// PNG only after its declared size passed the raster budget, so a forged header
// cannot make libpng allocate gigabytes.
#ifndef SLIC3R_PNG_HEADER_PROBE_HPP
#define SLIC3R_PNG_HEADER_PROBE_HPP

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "libslic3r/Exception.hpp"
#include "libslic3r/PNGReadWrite.hpp"

namespace Slic3r {

struct PngHeaderInfo {
    size_t width = 0, height = 0;
    unsigned bit_depth = 0, color_type = 0; // PNG color types: 0 gray, 2 RGB, 4 gray+alpha, 6 RGBA
};

// `where` names the layer in the error message, e.g. "Klipper ZIP layer 3".
inline PngHeaderInfo read_png_header(const std::vector<uint8_t> &png, const std::string &where)
{
    if (png.size() < 33 || !png::is_png({png.data(), png.size()}) || std::memcmp(png.data() + 12, "IHDR", 4) != 0)
        throw RuntimeError(where + " is not a PNG image");
    auto word = [&](size_t at) {
        return (uint32_t(png[at]) << 24) | (uint32_t(png[at + 1]) << 16) | (uint32_t(png[at + 2]) << 8) | png[at + 3];
    };
    PngHeaderInfo info;
    info.width = word(16); info.height = word(20); info.bit_depth = png[24]; info.color_type = png[25];
    if (!info.width || !info.height || info.width > 32768 || info.height > 32768 ||
        uint64_t(info.width) * info.height > 128ULL * 1024 * 1024)
        throw RuntimeError(where + " exceeds the editor raster limit");
    return info;
}

} // namespace Slic3r

#endif
