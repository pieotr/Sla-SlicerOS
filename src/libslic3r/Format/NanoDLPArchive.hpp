///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLIC3R_FORMAT_NANODLP_ARCHIVE_HPP
#define SLIC3R_FORMAT_NANODLP_ARCHIVE_HPP

#include <memory>
#include <string>
#include <nlohmann/json_fwd.hpp>

#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class SLAPrint;

class NanoDLPArchive : public SLAArchiveWriter {
    SLAPrinterConfig m_cfg;
    bool m_rgb=false;
    unsigned m_version=2;

protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;

public:
    explicit NanoDLPArchive(const SLAPrinterConfig &cfg, bool rgb=false, unsigned version=2) : m_cfg(cfg), m_rgb(rgb), m_version(version) {}
    explicit NanoDLPArchive(SLAPrinterConfig &&cfg, bool rgb=false, unsigned version=2) : m_cfg(std::move(cfg)), m_rgb(rgb), m_version(version) {}

    static nlohmann::json parse_device_profile(const std::string &json);
    static void validate_configuration(const SLAPrint &print);

    // Metadata preparation shares geometry and cached contour statistics.
    nlohmann::json prepare_metadata(const SLAPrint &print, const std::string &projectname) const;

    // Explicit device-profile API. Motion templates belong to the target
    // controller and cannot be inferred from generic lift/retract settings.
    // The application never executes these strings or invokes a converter.
    // Normal export reads this profile from the printer preset.
    void export_with_profile(const std::string &filename, const SLAPrint &print,
                             const ThumbnailsList &thumbnails,
                             const nlohmann::json &device_profile,
                             unsigned version = 2, const std::string &projectname = "") const;

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

} // namespace Slic3r

#endif // SLIC3R_FORMAT_NANODLP_ARCHIVE_HPP
