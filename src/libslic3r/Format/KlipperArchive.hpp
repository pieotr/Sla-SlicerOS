// Native Klipper MSLA ZIP schema, based on local UVtools KlipperFile.cs.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
class KlipperArchive : public SLAArchiveWriter {
    SLAPrinterConfig m_cfg;
    bool m_rgb;
protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;
public:
    explicit KlipperArchive(const SLAPrinterConfig &cfg, bool rgb = false) : m_cfg(cfg), m_rgb(rgb) {}
    static void validate_configuration(const SLAPrint &print);
    void export_print(const std::string filename, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};
}
