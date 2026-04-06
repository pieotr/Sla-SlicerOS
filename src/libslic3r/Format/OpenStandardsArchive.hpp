///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLIC3R_FORMAT_OPENSTANDARDS_ARCHIVE_HPP
#define SLIC3R_FORMAT_OPENSTANDARDS_ARCHIVE_HPP

#include <memory>
#include <string>

#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class SLAPrint;

// Open SLA (OSLA) format - ZIP with binary header + PNG layers
class OpenSLAArchive : public SLAArchiveWriter {
    SLAPrinterConfig m_cfg;

protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;

public:
    explicit OpenSLAArchive(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}
    explicit OpenSLAArchive(SLAPrinterConfig &&cfg) : m_cfg(std::move(cfg)) {}

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

// UVJ Format - ZIP with config.json + /slice images
class UVJArchive : public SLAArchiveWriter {
    SLAPrinterConfig m_cfg;

protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;

public:
    explicit UVJArchive(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}
    explicit UVJArchive(SLAPrinterConfig &&cfg) : m_cfg(std::move(cfg)) {}

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

} // namespace Slic3r

#endif // SLIC3R_FORMAT_OPENSTANDARDS_ARCHIVE_HPP
