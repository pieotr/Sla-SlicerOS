///|/ Copyright (c) Prusa Research 2024
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLA_ARCHIVE_WRITER_FACTORY_HPP
#define SLA_ARCHIVE_WRITER_FACTORY_HPP

#include "SLAArchiveWriter.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include <memory>
#include <string>

namespace Slic3r {

class SLAPrint;
class SLAPrinterConfig;

// ============================================================================
// Base class for polymorphic format writers.
// Each format (Anycubic, Chitubox, Creality, etc.) inherits from this.
// ============================================================================
class SLAFormatWriter : public SLAArchiveWriter {
protected:
    const SLAPrinterConfig *m_printer_config = nullptr;

    // Subclasses may override these to customize raster generation
    virtual std::unique_ptr<sla::RasterBase> create_raster() const = 0;
    virtual sla::RasterEncoder get_encoder() const = 0;

public:
    explicit SLAFormatWriter(const SLAPrinterConfig &cfg)
        : m_printer_config(&cfg) {}

    virtual ~SLAFormatWriter() = default;

    // Subclasses implement this to write their specific format
    virtual void export_print(const std::string     fname,
                              const SLAPrint       &print,
                              const ThumbnailsList &thumbnails,
                              const std::string    &projectname = "") = 0;
};

// ============================================================================
// Specialized writers for major format families
// ============================================================================

// Anycubic/Photon formats (PWS, PWMO, PWMX, etc.)
class AnycubicFormatWriter : public SLAFormatWriter {
    uint16_t m_version;

protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;

public:
    explicit AnycubicFormatWriter(const SLAPrinterConfig &cfg, uint16_t version = 1)
        : SLAFormatWriter(cfg), m_version(version) {}

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

// Chitubox formats (CTB, CBDDLP, PHOTON, PHZ)
// These use similar RLE binary format as Anycubic, but with different header structure
class ChituboxFormatWriter : public SLAFormatWriter {
    std::string m_format_id; // "ctb", "cbddlp", "photon", "phz"

protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;

public:
    explicit ChituboxFormatWriter(const SLAPrinterConfig &cfg, const std::string &format_id)
        : SLAFormatWriter(cfg), m_format_id(format_id) {}

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

// Creality CXDLP formats (CXDLP, CXDLPv4)
class CrealityFormatWriter : public SLAFormatWriter {
    std::string m_format_id;

protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;

public:
    explicit CrealityFormatWriter(const SLAPrinterConfig &cfg, const std::string &format_id)
        : SLAFormatWriter(cfg), m_format_id(format_id) {}

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

// ZIP-based formats (NanoDLP, Longer Orange, Voxeldance, etc.)
// These store layers as PNG images in a ZIP archive
class ZipArchiveFormatWriter : public SLAFormatWriter {
    std::string m_format_id;

protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;

public:
    explicit ZipArchiveFormatWriter(const SLAPrinterConfig &cfg, const std::string &format_id)
        : SLAFormatWriter(cfg), m_format_id(format_id) {}

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

// Generic fallback writer (currently maps to Anycubic for compatibility)
class GenericBinaryFormatWriter : public AnycubicFormatWriter {
    std::string m_format_id;

public:
    explicit GenericBinaryFormatWriter(const SLAPrinterConfig &cfg, const std::string &format_id)
        : AnycubicFormatWriter(cfg, 1), m_format_id(format_id) {}
};

} // namespace Slic3r

#endif // SLA_ARCHIVE_WRITER_FACTORY_HPP
