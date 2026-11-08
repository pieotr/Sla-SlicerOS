// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "RasterBase.hpp"
#include "CancellablePNGEncoder.hpp"
#include "LayerMaskPrecision.hpp"
#include <functional>
#include <array>
#include "libslic3r/Exception.hpp"
namespace Slic3r::sla {
// Project an immutable grayscale raster before its first PNG encoding.
// A cancellation exception leaves the input unchanged and publishes no PNG.
struct ProjectedPNGMaskEncoder {
    LayerMaskPrecision precision = LayerMaskPrecision::Gray8;
    std::function<void()> check_cancel;
    // Match editor Invert followed by canonicalization: Q(255 - Q(raw)).
    // Complement alone leaves some grayscale/bitplane source alphabets.
    bool invert = false;
    EncodedRaster operator()(const void *pixels, size_t width, size_t height, size_t components) const {
        if (precision != LayerMaskPrecision::Gray8 && precision != LayerMaskPrecision::NativeGray7 &&
            precision != LayerMaskPrecision::NativeGray4 && !mask_bitplanes(precision))
            throw RuntimeError("Unknown generated mask precision");
        const auto check = [&] { if (check_cancel) check_cancel(); };
        check();
        if (precision == LayerMaskPrecision::Gray8 && !invert) {
            auto result=encode_png_raster(pixels,width,height,components,check_cancel);
            check();return result;
        }
        if (!pixels || !width || !height || width>32768 || height>32768 || components!=1 ||
            uint64_t(width)*height > (uint64_t(512)<<20))
            throw RuntimeError("Projected masks require a bounded grayscale raster");
        std::array<uint8_t,256> palette{};
        for (size_t i=0;i<palette.size();++i) {
            const auto value = project_mask_value(uint8_t(i), precision);
            palette[i] = invert ? project_mask_value(uint8_t(255 - value), precision) : value;
        }
        const auto *input=static_cast<const uint8_t *>(pixels);
        std::vector<uint8_t> projected(width*height);
        constexpr size_t chunk=size_t(1)<<20;
        for (size_t first=0;first<projected.size();first+=chunk) {
            check();
            const auto last=std::min(projected.size(),first+chunk);
            for (size_t i=first;i<last;++i) projected[i]=palette[input[i]];
        }
        check();
        auto result=encode_png_raster(projected.data(),width,height,1,check_cancel);
        check();return result;
    }
};
}
