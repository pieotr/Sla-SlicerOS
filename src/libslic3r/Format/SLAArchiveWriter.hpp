///|/ Copyright (c) Prusa Research 2022 - 2023 Tomáš Mészáros @tamasmeszaros
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLAARCHIVE_HPP
#define SLAARCHIVE_HPP

#include <stddef.h>
#include <vector>
#include <memory>
#include <string>
#include <cstddef>
#include <stdexcept>
#include <functional>

#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/SLA/LayerMaskValidation.hpp"
#include "libslic3r/Execution/ExecutionTBB.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Execution/Execution.hpp"

namespace Slic3r {

class SLAPrint;
class SLAPrinterConfig;
namespace sla { class LayerDocument; }

class SLAArchiveWriter {
    // Native cache transfer is engine-private. Public callers cannot install
    // arbitrary buffers or bypass document validation through the writer.
    friend class SLAPrint;
protected:
    std::vector<sla::EncodedRaster> m_layers;

    virtual std::unique_ptr<sla::RasterBase> create_raster() const = 0;
    virtual sla::RasterEncoder get_encoder() const = 0;
    // A canonical native palette may require encoding plane counts rather
    // than applying the raw renderer's thresholds a second time.
    virtual sla::RasterEncoder get_document_encoder(const sla::LayerDocument &) const { return get_encoder(); }

public:
    virtual ~SLAArchiveWriter() = default;
    // Masks already use printer raster coordinates; never transform them twice.
    // The optional check may throw; it must be thread safe. Publish the new
    // encoding cache only after every layer and the final check succeed.
    void encode_layers(const sla::LayerDocument &document, const std::function<void()> &check_cancel = {});

    // Fn have to be thread safe: void(sla::RasterBase& raster, size_t lyrid);
    template<class Fn, class CancelFn, class EP = ExecutionTBB>
    void draw_layers(
        size_t     layer_num,
        Fn &&      drawfn,
        CancelFn cancelfn = []() { return false; },
        const EP & ep       = {},
        std::vector<sla::EncodedRaster> *canonical = nullptr,
        const std::vector<size_t> *representatives = nullptr,
        const sla::RasterEncoder &canonical_encoder = sla::PNGRasterEncoder{},
        std::vector<size_t> *lit_counts = nullptr)
    {
        // The caller establishes exact equality of scene-space geometry.
        // Representatives must precede copies, and may not themselves be copies.
        if (representatives) {
            if (representatives->size() != layer_num)
                throw std::invalid_argument("SLA raster representative count mismatch");
            for (size_t i=0;i<layer_num;++i) {
                const size_t source=(*representatives)[i];
                if (source>i || (*representatives)[source]!=source)
                    throw std::invalid_argument("Invalid SLA raster representative");
            }
        }
        if (cancelfn()) return;
        std::vector<sla::EncodedRaster> layers(layer_num);
        std::vector<sla::EncodedRaster> captured;
        if (canonical) captured.resize(layer_num);
        // Count lit pixels while the drawn buffer is still in memory, but only
        // when the canonical PNG is a verbatim Gray8 copy of that buffer. A
        // projected palette encoder changes pixel values, so its caller counts
        // after projection instead.
        // Only archives whose own encoder is also PNG (SL1) consume the ledger;
        // for every other format the extra full-raster pass is pure overhead.
        const bool count_lit = lit_counts && canonical && canonical_encoder.target<sla::PNGRasterEncoder>() &&
                               get_encoder().template target<sla::PNGRasterEncoder>();
        // SIZE_MAX marks a layer whose count was not taken (never trusted later).
        std::vector<size_t> lit(count_lit ? layer_num : 0, SIZE_MAX);
        execution::for_each(
            ep, size_t(0), layers.size(),
            [this, &drawfn, &cancelfn, canonical, representatives, &canonical_encoder, &layers, &captured, &lit, count_lit](size_t idx) {
                if (cancelfn() || (representatives && (*representatives)[idx]!=idx)) return;

                sla::EncodedRaster &enc = layers[idx];
                auto                rst = create_raster();
                drawfn(*rst, idx);
                const auto encoder = get_encoder();
                if (canonical) {
                    // Capture the canonical mask from the same drawn raster.
                    // PNG formats reuse their encoding instead of compressing twice.
                    enc = rst->encode([&](const void *ptr, size_t w, size_t h, size_t n) {
                        if (count_lit && n == 1 && ptr)
                            lit[idx] = sla::validate_and_count_mask_chunk(static_cast<const uint8_t *>(ptr), w * h,
                                sla::LayerMaskPrecision::Gray8).nonzero_pixels;
                        auto mask = canonical_encoder(ptr, w, h, n);
                        const bool same_png = encoder.target<sla::PNGRasterEncoder>() &&
                                              canonical_encoder.target<sla::PNGRasterEncoder>();
                        auto native = same_png ? mask : encoder(ptr, w, h, n);
                        captured[idx] = std::move(mask);
                        return native;
                    });
                } else enc = rst->encode(encoder);
            },
            execution::max_concurrency(ep));
        // Complete the parallel work before reading any representative buffer.
        // Layer positions and exposure settings remain independent of masks.
        if (representatives && !cancelfn())
            for(size_t i=0;i<layer_num;++i) {
                if(cancelfn()) return;
                const size_t source=(*representatives)[i];
                if(source!=i) {
                    layers[i]=layers[source];
                    if(canonical) captured[i]=captured[source];
                    if(count_lit) lit[i]=lit[source];
                }
            }
        if (cancelfn()) return;
        // All encoders and representative copies succeeded. Neither cache is
        // replaced on a cancellation or exception during the parallel work.
        m_layers.swap(layers);
        if (canonical) canonical->swap(captured);
        if (count_lit) lit_counts->swap(lit);
    }

    // Export the print into an archive using the provided filename.
    virtual void export_print(const std::string     fname,
                              const SLAPrint       &print,
                              const ThumbnailsList &thumbnails,
                              const std::string    &projectname = "") = 0;

    // Factory method to create an archiver instance
    static std::unique_ptr<SLAArchiveWriter> create(
        const std::string &archtype, const SLAPrinterConfig &);
};

} // namespace Slic3r
#endif // SLAARCHIVE_HPP
