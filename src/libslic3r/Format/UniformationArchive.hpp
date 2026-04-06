///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLIC3R_FORMAT_UNIFORMATION_ARCHIVE_HPP
#define SLIC3R_FORMAT_UNIFORMATION_ARCHIVE_HPP

#include <memory>
#include <string>

#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class SLAPrint;

// Uniformation GKone JXS format
class UniformationGKoneArchive : public SLAArchiveWriter {
    SLAPrinterConfig m_cfg;

protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;

public:
    explicit UniformationGKoneArchive(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}
    explicit UniformationGKoneArchive(SLAPrinterConfig &&cfg) : m_cfg(std::move(cfg)) {}

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

} // namespace Slic3r

#endif // SLIC3R_FORMAT_UNIFORMATION_ARCHIVE_HPP
