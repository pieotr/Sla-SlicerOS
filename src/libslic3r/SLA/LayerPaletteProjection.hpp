// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerDocument.hpp"
#include "libslic3r/Exception.hpp"
#include <cmath>

namespace Slic3r::sla {
struct LayerPaletteLoss {
    uint64_t changed_pixels=0;
    unsigned maximum_change=0;
};
// Analysis never changes masks, metadata, dirty state or Undo history.
inline LayerPaletteLoss layer_palette_loss(const LayerDocument &source,LayerMaskPrecision precision,
                                          const LayerProgress &progress={})
{
    if(precision!=LayerMaskPrecision::Gray8 && precision!=LayerMaskPrecision::NativeGray7 &&
       precision!=LayerMaskPrecision::NativeGray4 && !mask_bitplanes(precision))
        throw RuntimeError("Unknown export mask precision");
    LayerPaletteLoss result;
    for(size_t i=0;i<source.layer_count();++i) {
        check_layer_progress(progress,i,source.layer_count());
        for(uint8_t value:source.image(i).buf) {
            const auto projected=project_mask_value(value,precision);
            if(value!=projected)++result.changed_pixels;
            result.maximum_change=std::max(result.maximum_change,unsigned(std::abs(int(value)-int(projected))));
        }
    }
    check_layer_progress(progress,source.layer_count(),source.layer_count());
    return result;
}
// An explicitly approved export copy. Source rows, physical geometry and
// profile intentions survive; the original document and its history stay intact.
inline LayerDocument project_layer_palette_copy(const LayerDocument &source,LayerMaskPrecision precision,
                                                const LayerProgress &progress={})
{
    if(!source.fresh_job() || source.mask_plane_count()!=1)
        throw RuntimeError("Palette export copy requires an owned single-plane document");
    auto job=*source.fresh_job();job.mask_precision=precision;
    job.validate(source.layer_count());
    std::vector<EncodedRaster> masks;masks.reserve(source.layer_count());
    for(size_t i=0;i<source.layer_count();++i) {
        check_layer_progress(progress,i,source.layer_count());
        auto image=source.image(i);
        for(auto &value:image.buf)value=project_mask_value(value,precision);
        masks.push_back(PNGRasterEncoder{}(image.buf.data(),image.cols,image.rows,1));
    }
    check_layer_progress(progress,source.layer_count(),source.layer_count());
    return LayerDocument::from_fresh_rasters(std::move(masks),std::move(job));
}
} // namespace Slic3r::sla
