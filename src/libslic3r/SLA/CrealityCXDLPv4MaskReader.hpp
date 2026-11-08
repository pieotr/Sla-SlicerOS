// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "libslic3r/Format/CrealityCXDLPv4.hpp"
#include "libslic3r/Format/CrealityCXDLPv4Layout.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include <functional>

namespace Slic3r::sla {
// The layout validates the complete native container, including CRC and
// section bounds. Its immutable bytes must outlive this call. The returned
// pixels own their storage; no editable document or PNG round-trip is needed.
inline png::ImageGreyscale cxdlpv4_layer_image(const CrealityCXDLPv4Layout &layout,
    size_t index, const std::function<void()> &checkpoint = {})
{
    if (checkpoint) checkpoint();
    const auto &layer=layout.layers.at(index);
    const auto *payload=layout.bytes.data()+layer.data+44;
    const size_t length=layer.length-44;
    std::vector<uint8_t> decrypted;
    if(layout.seed) {
        decrypted.assign(payload,payload+length);
        CrealityCXDLPv4Archive::crypt_mask(decrypted,layout.seed,uint32_t(index));
        payload=decrypted.data();
    }
    png::ImageGreyscale image;
    image.cols=layout.columns;image.rows=layout.rows;
    image.buf=CrealityCXDLPv4Archive::decode_mask(payload,length,image.cols,image.rows);
    if(checkpoint)checkpoint();
    return image;
}
} // namespace Slic3r::sla
