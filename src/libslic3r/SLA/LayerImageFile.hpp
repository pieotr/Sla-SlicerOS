#pragma once
#include "libslic3r/PNGReadWrite.hpp"
#include <array>
#include <string>
#include <vector>

namespace Slic3r::sla {
// ImageFile formats are single masks, not printer-job containers. A caller
// exporting a document must explicitly choose a layer or iterate a range.
struct LayerImageFormat {
    const char *id;
    const char *description;
    bool lossy;
    bool binary;
};
const std::array<LayerImageFormat, 11> &layer_image_formats();
const LayerImageFormat *find_layer_image_format(const std::string &extension);
std::vector<uint8_t> encode_layer_image(const png::ImageGreyscale &image, const std::string &format);
png::ImageGreyscale decode_layer_image(png::ReadBuf bytes, const std::string &format);
}
