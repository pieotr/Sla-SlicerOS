/// Native port of UVtools.Core/FileFormats/LGSFile.cs (AGPLv3).
#ifndef SLIC3R_FORMAT_LONGER_ORANGE_ARCHIVE_HPP
#define SLIC3R_FORMAT_LONGER_ORANGE_ARCHIVE_HPP
#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
class SLAPrint;
// One container/codec, with model-specific headers and preview/rotation rules.
class LongerOrangeArchive : public SLAArchiveWriter {
    SLAPrinterConfig m_cfg;
    uint32_t m_model;
protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;
public:
    LongerOrangeArchive(const SLAPrinterConfig &cfg, uint32_t model) : m_cfg(cfg), m_model(model) {}
    static sla::EncodedRaster encode_mask(const void *, size_t, size_t, size_t, bool rotate_clockwise);
    static void validate_configuration(const SLAPrint &, uint32_t model);
    static uint32_t model_for_format(const std::string &format, const std::string &printer_model);
    void export_print(const std::string, const SLAPrint &, const ThumbnailsList &, const std::string & = "") override;
};
class LongerOrange10Archive : public LongerOrangeArchive {
public: explicit LongerOrange10Archive(const SLAPrinterConfig &cfg) : LongerOrangeArchive(cfg,10) {}
};
class LongerOrange30Archive : public LongerOrangeArchive {
public: explicit LongerOrange30Archive(const SLAPrinterConfig &cfg) : LongerOrangeArchive(cfg,30) {}
};
class LongerOrange120Archive : public LongerOrangeArchive {
public: explicit LongerOrange120Archive(const SLAPrinterConfig &cfg) : LongerOrangeArchive(cfg,120) {}
};
class LongerOrange4KArchive : public LongerOrangeArchive {
public: explicit LongerOrange4KArchive(const SLAPrinterConfig &cfg) : LongerOrangeArchive(cfg,cfg.printer_model.value=="4000"?4000:4500) {}
};
}
#endif
