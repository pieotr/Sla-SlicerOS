///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLIC3R_FORMAT_PHROZEN_ARCHIVE_HPP
#define SLIC3R_FORMAT_PHROZEN_ARCHIVE_HPP

#include <memory>
#include <string>

#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class SLAPrint;

// Native UVtools PHZ v2 container, with per-layer exposure/Z and optional XOR.
class PhrozenPHZArchive : public SLAArchiveWriter {
    SLAPrinterConfig m_cfg;

protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;

public:
    static sla::EncodedRaster encode_mask(const void *pixels, size_t width, size_t height, size_t components);
    static void crypt_mask(std::vector<uint8_t> &bytes, uint32_t seed, uint32_t layer_index);
    static uint32_t parse_encryption_key(const std::string &text);
    static void validate_configuration(const SLAPrint &print);

    explicit PhrozenPHZArchive(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}
    explicit PhrozenPHZArchive(SLAPrinterConfig &&cfg) : m_cfg(std::move(cfg)) {}

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

} // namespace Slic3r

#endif // SLIC3R_FORMAT_PHROZEN_ARCHIVE_HPP
