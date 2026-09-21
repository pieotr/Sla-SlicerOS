///|/ Copyright (c) Prusa Research 2022 - 2023 Tomáš Mészáros @tamasmeszaros
///|/ Copyright (c) 2023 Mimoja @Mimoja
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "SLAArchiveWriter.hpp"

#include "SLAArchiveFormatRegistry.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Exception.hpp"

namespace Slic3r {

namespace {
// Keep unsupported presets loadable. Report the problem during slicing/export,
// where the application's job error handling can present it to the user.
class UnavailableArchive final : public SLAArchiveWriter {
    std::string m_reason;
    std::unique_ptr<sla::RasterBase> create_raster() const override { throw RuntimeError(m_reason); }
    sla::RasterEncoder get_encoder() const override { throw RuntimeError(m_reason); }
public:
    explicit UnavailableArchive(std::string reason) : m_reason(std::move(reason)) {}
    void export_print(const std::string, const SLAPrint &, const ThumbnailsList &,
                      const std::string &) override { throw RuntimeError(m_reason); }
};
}

std::unique_ptr<SLAArchiveWriter>
SLAArchiveWriter::create(const std::string &archtype, const SLAPrinterConfig &cfg)
{
    std::unique_ptr<SLAArchiveWriter> ret;
    if (get_archive_entry(archtype.c_str())) {
        const auto reason = archive_export_unavailable_reason(archtype.c_str());
        if (!reason.empty())
            return std::make_unique<UnavailableArchive>(reason);
    }
    auto factory = get_writer_factory(archtype.c_str());

    if (factory)
        ret = factory(cfg);

    return ret;
}

} // namespace Slic3r
