// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared native grayscale codec used by CTB and CTBEncryptedFile.
#ifndef SLIC3R_CHITUBOX_RASTER_ENCODER_HPP
#define SLIC3R_CHITUBOX_RASTER_ENCODER_HPP
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/Exception.hpp"
#include <functional>
#include <algorithm>
namespace Slic3r::chitubox {
// CTB stores 7-bit grayscale followed, for repeated pixels, by a variable-length
// big-endian count. Runs cross scanlines; this is distinct from CBDDLP bitplanes.
struct Gray7RasterEncoder {
    sla::EncodedRaster operator()(const void *ptr, size_t w, size_t h, size_t num_components) const
    {
        if (!ptr || !w || !h || num_components != 1 || w > 32768 || h > 32768 || uint64_t(w)*h > (uint64_t(128)<<20))
            throw ExportError("CTB requires a nonempty single-channel mask up to 32768 pixels per axis");
        const auto *pixels = static_cast<const uint8_t *>(ptr);
        std::vector<uint8_t> out;
        auto append_run = [&out](uint8_t gray, uint32_t count) {
            out.push_back(uint8_t(gray | (count > 1 ? 0x80 : 0)));
            if (count == 1) return;
            if (count <= 0x7f) out.push_back(uint8_t(count));
            else if (count <= 0x3fff) {
                out.push_back(uint8_t(0x80 | (count >> 8))); out.push_back(uint8_t(count));
            } else if (count <= 0x1fffff) {
                out.push_back(uint8_t(0xc0 | (count >> 16)));
                out.push_back(uint8_t(count >> 8)); out.push_back(uint8_t(count));
            } else {
                out.push_back(uint8_t(0xe0 | (count >> 24)));
                out.push_back(uint8_t(count >> 16)); out.push_back(uint8_t(count >> 8));
                out.push_back(uint8_t(count));
            }
        };
        // The count field has 28 bits. Split only at that boundary, including
        // black and white runs; 255 is a valid input pixel, never a sentinel.
        const size_t length = w * h;
        for (size_t begin = 0; begin < length;) {
            const uint8_t gray = pixels[begin] >> 1;
            size_t end = begin + 1;
            while (end < length && end - begin < 0x0fffffff && (pixels[end] >> 1) == gray)
                ++end;
            append_run(gray, uint32_t(end - begin));
            begin = end;
        }
        return sla::EncodedRaster(std::move(out), "ctb");
    }
};


// Decode the common Gray7 wire mask after its format-specific crypto layer.
inline std::vector<uint8_t> decode_gray7(const std::vector<uint8_t> &payload, size_t length,
                                       const std::function<void()> &check = {})
{
    std::vector<uint8_t> pixels(length,0);
    size_t cursor=0,pixel=0,next_check=0;
    while (cursor<payload.size()) {
        if (check && cursor>=next_check) { check();next_check=cursor+65536; }
        const uint8_t token=payload[cursor++];uint32_t run=1;
        if (token&128) {
            if (cursor==payload.size()) throw RuntimeError("Truncated CTB Gray7 run");
            const auto first=payload[cursor++];unsigned extra=0;
            if (first<128) run=first;
            else if (first<192) { run=first&63;extra=1; }
            else if (first<224) { run=first&31;extra=2; }
            else if (first<240) { run=first&15;extra=3; }
            else throw RuntimeError("Invalid CTB Gray7 run prefix");
            if (extra>payload.size()-cursor) throw RuntimeError("Truncated CTB Gray7 run");
            while (extra--) run=(run<<8)|payload[cursor++];
        }
        if (!run || run>length-pixel) throw RuntimeError("CTB Gray7 run exceeds raster dimensions");
        const uint8_t value=(token&127)?uint8_t((token&127)*2+1):0;
        for (size_t filled=0;filled<run;) {
            if (check && (filled || run>=(size_t(1)<<20))) check();
            const auto chunk=std::min(size_t(1)<<20,size_t(run)-filled);
            std::fill_n(pixels.begin()+pixel+filled,chunk,value);filled+=chunk;
        }
        pixel+=run;
    }
    if (pixel!=length) throw RuntimeError("Incomplete CTB Gray7 mask");
    if (check) check();
    return pixels;
}

// Count the actual 7-bit exported mask, not the pre-quantization source PNG.
// In particular, input gray 1 disappears from CTB and must not add resin volume.
inline uint64_t exposed_pixels(const sla::EncodedRaster &raster, uint64_t expected_pixels)
{
    const auto *bytes = static_cast<const uint8_t *>(raster.data());
    uint64_t total = 0, exposed = 0;
    for (size_t offset = 0; offset < raster.size();) {
        const uint8_t token = bytes[offset++];
        uint32_t count = 1;
        if (token & 0x80) {
            if (offset == raster.size()) throw ExportError("Incomplete CTB raster count");
            const uint8_t first = bytes[offset++];
            unsigned extra = 0;
            if (!(first & 0x80)) count = first;
            else if ((first & 0xc0) == 0x80) { count = first & 0x3f; extra = 1; }
            else if ((first & 0xe0) == 0xc0) { count = first & 0x1f; extra = 2; }
            else if ((first & 0xf0) == 0xe0) { count = first & 0x0f; extra = 3; }
            else throw ExportError("Invalid CTB raster count prefix");
            if (extra > raster.size() - offset) throw ExportError("Incomplete CTB raster count");
            while (extra--) count = (count << 8) | bytes[offset++];
        }
        if (!count || count > expected_pixels - total)
            throw ExportError("CTB raster dimensions do not match printer resolution");
        total += count;
        if (token & 0x7f) exposed += count;
    }
    if (total != expected_pixels) throw ExportError("Incomplete CTB raster mask");
    return exposed;
}
}
#endif
