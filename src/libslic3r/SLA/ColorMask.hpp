// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "RasterBase.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Exception.hpp"

namespace Slic3r::sla {
// RGB.CWS groups three consecutive grayscale samples as B/G/R. PNG stores
// them in RGB order; its pixel width is one third of the LCD width.
EncodedRaster encode_bgr_packed_mask(const png::ImageGreyscale &image);
bool decode_bgr_packed_mask(const png::ReadBuf &input, size_t columns, size_t rows,
                            png::ImageGreyscale &output);
// XML.CWS requires PNG32: preserve canonical grayscale in all RGB channels,
// with opaque alpha. This is a container conversion, not another LCD transform.
inline EncodedRaster encode_rgba_mask(const png::ImageGreyscale &image) {
    if (!image.cols || !image.rows || image.cols>32768 || image.rows>32768 ||
        image.cols*image.rows>128ULL*1024*1024 || image.buf.size()!=image.cols*image.rows)
        throw RuntimeError("Invalid XML.CWS mask dimensions");
    std::vector<uint8_t> rgba(4*image.buf.size());
    for (size_t i=0;i<image.buf.size();++i) {
        rgba[4*i]=rgba[4*i+1]=rgba[4*i+2]=image.buf[i];rgba[4*i+3]=255;
    }
    auto encoded=PNGRasterEncoder{}(rgba.data(),image.cols,image.rows,4);
    if (!encoded.size()) throw RuntimeError("Cannot encode XML.CWS RGBA mask");
    return encoded;
}
} // namespace Slic3r::sla
