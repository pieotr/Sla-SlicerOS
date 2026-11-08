///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLIC3R_FORMAT_UVTOOLS_COMPAT_ARCHIVE_HPP
#define SLIC3R_FORMAT_UVTOOLS_COMPAT_ARCHIVE_HPP

#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace sla { struct CWSFreshExportPlan; }
namespace sla { struct CBDDLPFreshExportPlan; }

// ============================================================================
// ZIP-BASED FORMATS (SL1, CWS, GenericZIP, ZCode, OSLA, JXS, NanoDLP, etc.)
// ============================================================================

class UVToolsZIPArchiveBase : public SLAArchiveWriter {
protected:
    SLAPrinterConfig m_cfg;
    UVToolsZIPArchiveBase(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}
    UVToolsZIPArchiveBase(SLAPrinterConfig &&cfg) : m_cfg(std::move(cfg)) {}
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder               get_encoder() const override;
};

// CWS (Wanhao) - ZIP with XML manifest
class UVToolsCWSArchive : public UVToolsZIPArchiveBase {
    void export_print_impl(const std::string &fname, const SLAPrint &print,
                           const sla::CWSFreshExportPlan *fresh);
public:
    explicit UVToolsCWSArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    explicit UVToolsCWSArchive(SLAPrinterConfig &&cfg) : UVToolsZIPArchiveBase(std::move(cfg)) {}
    void export_fresh_print(const std::string &fname, const SLAPrint &print,
                            const sla::CWSFreshExportPlan &fresh);
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// Chitubox ZIP: run.gcode is the container's SLA exposure/motion sequence.
class UVToolsChituboxZIPArchive : public UVToolsZIPArchiveBase {
public:
    explicit UVToolsChituboxZIPArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// GenericZIP - ZIP with manifest.uvtools XML
class UVToolsGenericZIPArchive : public UVToolsZIPArchiveBase {
public:
    explicit UVToolsGenericZIPArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    explicit UVToolsGenericZIPArchive(SLAPrinterConfig &&cfg) : UVToolsZIPArchiveBase(std::move(cfg)) {}
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// OSLA (Open SLA) - ZIP with binary header + PNG layers
class UVToolsOSLAArchive : public UVToolsZIPArchiveBase {
public:
    explicit UVToolsOSLAArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    explicit UVToolsOSLAArchive(SLAPrinterConfig &&cfg) : UVToolsZIPArchiveBase(std::move(cfg)) {}
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// NanoDLP - ZIP with multi-manifest JSON system
class UVToolsNanoDLPArchive : public UVToolsZIPArchiveBase {
public:
    explicit UVToolsNanoDLPArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    explicit UVToolsNanoDLPArchive(SLAPrinterConfig &&cfg) : UVToolsZIPArchiveBase(std::move(cfg)) {}
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// UVJ - ZIP with config.json + /slice images
class UVToolsUVJArchive : public UVToolsZIPArchiveBase {
public:
    explicit UVToolsUVJArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    explicit UVToolsUVJArchive(SLAPrinterConfig &&cfg) : UVToolsZIPArchiveBase(std::move(cfg)) {}
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// Voxeldance Additive ZIP with UVtools VDARoot XML.
class UVToolsVDAArchive : public UVToolsZIPArchiveBase {
public:
    explicit UVToolsVDAArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    explicit UVToolsVDAArchive(SLAPrinterConfig &&cfg) : UVToolsZIPArchiveBase(std::move(cfg)) {}
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// Klipper - ZIP with GCode + PNG layers
class UVToolsKlipperArchive : public UVToolsZIPArchiveBase {
public:
    explicit UVToolsKlipperArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    explicit UVToolsKlipperArchive(SLAPrinterConfig &&cfg) : UVToolsZIPArchiveBase(std::move(cfg)) {}
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// ============================================================================
// BINARY RLE FORMATS (Chitubox, PHZ, FDG, Anycubic, CXDLP variants)
// ============================================================================

class UVToolsRLEBinaryBase : public SLAArchiveWriter {
protected:
    SLAPrinterConfig m_cfg;
    UVToolsRLEBinaryBase(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}
    UVToolsRLEBinaryBase(SLAPrinterConfig &&cfg) : m_cfg(std::move(cfg)) {}
    std::unique_ptr<sla::RasterBase> create_raster() const override;
};

// Chitubox CTB/CBDDLP - Binary RLE with preview previews
class UVToolsChituboxArchive : public UVToolsRLEBinaryBase {
    uint32_t m_magic;
    uint32_t m_version;
    unsigned cbddlp_plane_count() const;
    sla::RasterEncoder get_document_encoder(const sla::LayerDocument &document) const override;
    void export_print_impl(const std::string &filename,const SLAPrint &print,
                           const ThumbnailsList &thumbnails,const sla::CBDDLPFreshExportPlan *fresh);
public:
    void export_fresh_print(const std::string &filename,const SLAPrint &print,
                            const ThumbnailsList &thumbnails,const sla::CBDDLPFreshExportPlan &fresh);
    explicit UVToolsChituboxArchive(const SLAPrinterConfig &cfg, uint32_t magic = 0x12FD0086, uint32_t version = 2)
        : UVToolsRLEBinaryBase(cfg), m_magic(magic), m_version(version) {}
    explicit UVToolsChituboxArchive(SLAPrinterConfig &&cfg, uint32_t magic = 0x12FD0086, uint32_t version = 2)
        : UVToolsRLEBinaryBase(std::move(cfg)), m_magic(magic), m_version(version) {}
    sla::RasterEncoder get_encoder() const override;
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// CXDLP (Creality hybrid binary) - Special line-based format
class UVToolsCXDLPArchive : public SLAArchiveWriter {
    SLAPrinterConfig m_cfg;
public:
    explicit UVToolsCXDLPArchive(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}
    explicit UVToolsCXDLPArchive(SLAPrinterConfig &&cfg) : m_cfg(std::move(cfg)) {}
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// ============================================================================
// PACKED-PIXEL FORMATS (GR1, MDLP, LGS, OSF)
// ============================================================================

class UVToolsPackedPixelBase : public SLAArchiveWriter {
protected:
    SLAPrinterConfig m_cfg;
    UVToolsPackedPixelBase(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}
    UVToolsPackedPixelBase(SLAPrinterConfig &&cfg) : m_cfg(std::move(cfg)) {}
    std::unique_ptr<sla::RasterBase> create_raster() const override;
};

// OSF (Vlare) v4 - big-endian settings and seven-bit grayscale RLE
class UVToolsOSFArchive : public UVToolsPackedPixelBase {
public:
    explicit UVToolsOSFArchive(const SLAPrinterConfig &cfg) : UVToolsPackedPixelBase(cfg) {}
    explicit UVToolsOSFArchive(SLAPrinterConfig &&cfg) : UVToolsPackedPixelBase(std::move(cfg)) {}
    sla::RasterEncoder get_encoder() const override;
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// GOO (Elegoo) - RGB565 packed with comprehensive ASCII header
class UVToolsGOOArchive : public UVToolsPackedPixelBase {
public:
    explicit UVToolsGOOArchive(const SLAPrinterConfig &cfg) : UVToolsPackedPixelBase(cfg) {}
    explicit UVToolsGOOArchive(SLAPrinterConfig &&cfg) : UVToolsPackedPixelBase(std::move(cfg)) {}
    sla::RasterEncoder get_encoder() const override;
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// ============================================================================
// OTHER/SPECIAL FORMATS (Anet, QDT, etc.)
// ============================================================================

// Anet N4/N7 - Binary with UTF-16BE strings and BMP preview
class UVToolsAnetArchive : public SLAArchiveWriter {
    SLAPrinterConfig m_cfg;
public:
    explicit UVToolsAnetArchive(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}
    explicit UVToolsAnetArchive(SLAPrinterConfig &&cfg) : m_cfg(std::move(cfg)) {}
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

} // namespace Slic3r

#endif // SLIC3R_FORMAT_UVTOOLS_COMPAT_ARCHIVE_HPP
