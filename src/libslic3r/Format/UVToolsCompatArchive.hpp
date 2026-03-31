///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLIC3R_FORMAT_UVTOOLS_COMPAT_ARCHIVE_HPP
#define SLIC3R_FORMAT_UVTOOLS_COMPAT_ARCHIVE_HPP

#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

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
public:
    explicit UVToolsCWSArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    explicit UVToolsCWSArchive(SLAPrinterConfig &&cfg) : UVToolsZIPArchiveBase(std::move(cfg)) {}
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// GenericZIP - ZIP with manifest.uvtools JSON
class UVToolsGenericZIPArchive : public UVToolsZIPArchiveBase {
public:
    explicit UVToolsGenericZIPArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    explicit UVToolsGenericZIPArchive(SLAPrinterConfig &&cfg) : UVToolsZIPArchiveBase(std::move(cfg)) {}
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// ZCode - ZIP with XML task/profile structure
class UVToolsZCodeArchive : public UVToolsZIPArchiveBase {
public:
    explicit UVToolsZCodeArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    explicit UVToolsZCodeArchive(SLAPrinterConfig &&cfg) : UVToolsZIPArchiveBase(std::move(cfg)) {}
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// ZCodeX - ZCode variant with JSON metadata  
class UVToolsZCodeXArchive : public UVToolsZIPArchiveBase {
public:
    explicit UVToolsZCodeXArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    explicit UVToolsZCodeXArchive(SLAPrinterConfig &&cfg) : UVToolsZIPArchiveBase(std::move(cfg)) {}
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

// JXS (GigaKnightSung) - ZIP with INI/JSON config
class UVToolsJXSArchive : public UVToolsZIPArchiveBase {
public:
    explicit UVToolsJXSArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    explicit UVToolsJXSArchive(SLAPrinterConfig &&cfg) : UVToolsZIPArchiveBase(std::move(cfg)) {}
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

// VDA (Structo) - ZIP with XML root structure
class UVToolsVDAArchive : public UVToolsZIPArchiveBase {
public:
    explicit UVToolsVDAArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    explicit UVToolsVDAArchive(SLAPrinterConfig &&cfg) : UVToolsZIPArchiveBase(std::move(cfg)) {}
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// VDT - ZIP with manifest.json + 10 preview variants
class UVToolsVDTArchive : public UVToolsZIPArchiveBase {
public:
    explicit UVToolsVDTArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    explicit UVToolsVDTArchive(SLAPrinterConfig &&cfg) : UVToolsZIPArchiveBase(std::move(cfg)) {}
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

// FlashForgeSVGX - ZIP with SVG vector graphics per layer
class UVToolsFlashForgeSVGXArchive : public UVToolsZIPArchiveBase {
public:
    explicit UVToolsFlashForgeSVGXArchive(const SLAPrinterConfig &cfg) : UVToolsZIPArchiveBase(cfg) {}
    explicit UVToolsFlashForgeSVGXArchive(SLAPrinterConfig &&cfg) : UVToolsZIPArchiveBase(std::move(cfg)) {}
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

// FDG (Voxelab) - Binary RLE with 54-field header
class UVToolsFDGArchive : public UVToolsRLEBinaryBase {
public:
    explicit UVToolsFDGArchive(const SLAPrinterConfig &cfg) : UVToolsRLEBinaryBase(cfg) {}
    explicit UVToolsFDGArchive(SLAPrinterConfig &&cfg) : UVToolsRLEBinaryBase(std::move(cfg)) {}
    sla::RasterEncoder get_encoder() const override;
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// Chitubox CTB/CBDDLP - Binary RLE with preview previews
class UVToolsChituboxArchive : public UVToolsRLEBinaryBase {
    uint32_t m_magic;
public:
    explicit UVToolsChituboxArchive(const SLAPrinterConfig &cfg, uint32_t magic = 0x12FD0086)
        : UVToolsRLEBinaryBase(cfg), m_magic(magic) {}
    explicit UVToolsChituboxArchive(SLAPrinterConfig &&cfg, uint32_t magic = 0x12FD0086)
        : UVToolsRLEBinaryBase(std::move(cfg)), m_magic(magic) {}
    sla::RasterEncoder get_encoder() const override;
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// PHZ (Phrozen) - Nearly identical to CTB with different magic
class UVToolsPHZArchive : public UVToolsChituboxArchive {
public:
    explicit UVToolsPHZArchive(const SLAPrinterConfig &cfg) : UVToolsChituboxArchive(cfg, 0x9FDA83AE) {}
    explicit UVToolsPHZArchive(SLAPrinterConfig &&cfg) : UVToolsChituboxArchive(std::move(cfg), 0x9FDA83AE) {}
};

// CTB Encrypted - Chitubox with XOR encryption
class UVToolsCTBEncryptedArchive : public UVToolsChituboxArchive {
public:
    explicit UVToolsCTBEncryptedArchive(const SLAPrinterConfig &cfg) : UVToolsChituboxArchive(cfg, 0x12FD0107) {}
    explicit UVToolsCTBEncryptedArchive(SLAPrinterConfig &&cfg) : UVToolsChituboxArchive(std::move(cfg), 0x12FD0107) {}
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

// GR1 (Makerbase) - Big-endian RGB565 packed format with vector lines
class UVToolsGR1Archive : public UVToolsPackedPixelBase {
public:
    explicit UVToolsGR1Archive(const SLAPrinterConfig &cfg) : UVToolsPackedPixelBase(cfg) {}
    explicit UVToolsGR1Archive(SLAPrinterConfig &&cfg) : UVToolsPackedPixelBase(std::move(cfg)) {}
    sla::RasterEncoder get_encoder() const override;
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// MDLP (Makerbase) - Nearly identical to GR1
class UVToolsMDLPArchive : public UVToolsGR1Archive {
public:
    explicit UVToolsMDLPArchive(const SLAPrinterConfig &cfg) : UVToolsGR1Archive(cfg) {}
    explicit UVToolsMDLPArchive(SLAPrinterConfig &&cfg) : UVToolsGR1Archive(std::move(cfg)) {}
};

// LGS (Longer Orange) - Packed pixels with 200+ byte header
class UVToolsLGSArchive : public UVToolsPackedPixelBase {
public:
    explicit UVToolsLGSArchive(const SLAPrinterConfig &cfg) : UVToolsPackedPixelBase(cfg) {}
    explicit UVToolsLGSArchive(SLAPrinterConfig &&cfg) : UVToolsPackedPixelBase(std::move(cfg)) {}
    sla::RasterEncoder get_encoder() const override;
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

// OSF (Vlare) - Big-endian UInt24 packed pixels
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

// QDT (Emake3D) - Text-based vector line format
class UVToolsQDTArchive : public SLAArchiveWriter {
    SLAPrinterConfig m_cfg;
public:
    explicit UVToolsQDTArchive(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}
    explicit UVToolsQDTArchive(SLAPrinterConfig &&cfg) : m_cfg(std::move(cfg)) {}
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;
    void export_print(const std::string fname, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};

} // namespace Slic3r

#endif // SLIC3R_FORMAT_UVTOOLS_COMPAT_ARCHIVE_HPP
