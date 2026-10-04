///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLIC3R_FORMAT_CREALITY_CXDLPV4_HPP
#define SLIC3R_FORMAT_CREALITY_CXDLPV4_HPP

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class SLAPrint;

class CrealityCXDLPv4Archive : public SLAArchiveWriter {
    SLAPrinterConfig m_cfg;

protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;

public:
    // Shared native codec for model export and standalone layer editing.
    static sla::EncodedRaster encode_mask(const void *pixels, size_t width, size_t height, size_t components);
    static std::vector<uint8_t> decode_mask(const uint8_t *data, size_t length, size_t width, size_t height);
    static void crypt_mask(std::vector<uint8_t> &data, uint32_t seed, uint32_t layer_index);
    explicit CrealityCXDLPv4Archive(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}
    explicit CrealityCXDLPv4Archive(SLAPrinterConfig &&cfg) : m_cfg(std::move(cfg)) {}

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

} // namespace Slic3r

#endif // SLIC3R_FORMAT_CREALITY_CXDLPV4_HPP
