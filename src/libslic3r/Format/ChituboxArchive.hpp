///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLIC3R_FORMAT_CHITUBOX_ARCHIVE_HPP
#define SLIC3R_FORMAT_CHITUBOX_ARCHIVE_HPP

#include <memory>
#include <string>

#include "SLAArchiveWriter.hpp"
#include "UVToolsCompatArchive.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class SLAPrint;

// CBDDLP/Photon v1 are binary; v2 uses 1–16 RLE exposure planes.
class ChituboxCBDDLPArchive : public UVToolsChituboxArchive {
public:
    explicit ChituboxCBDDLPArchive(const SLAPrinterConfig &cfg, unsigned version=2) : UVToolsChituboxArchive(cfg, 0x12fd0019, version) {}
    explicit ChituboxCBDDLPArchive(SLAPrinterConfig &&cfg, unsigned version=2) : UVToolsChituboxArchive(std::move(cfg), 0x12fd0019, version) {}
    static void validate_configuration(const SLAPrint &print, unsigned version=2);
};

// CTB v1 has the grayscale codec but no v2 print-parameters block.
// Its extension is .ctb, not the historical registry typo .cbt.
class ChituboxCTBv1Archive : public UVToolsChituboxArchive {
public:
    explicit ChituboxCTBv1Archive(const SLAPrinterConfig &cfg) : UVToolsChituboxArchive(cfg, 0x12fd0086, 1) {}
    explicit ChituboxCTBv1Archive(SLAPrinterConfig &&cfg) : UVToolsChituboxArchive(std::move(cfg), 0x12fd0086, 1) {}
    static void validate_configuration(const SLAPrint &print);
};

// Ordinary CTB v3-v5 use an 84-byte record before each grayscale payload.
// This schema is distinct from CTBEncryptedFile's AES settings/signature.
class ChituboxCTBModernArchive : public UVToolsRLEBinaryBase {
    unsigned m_version;
    bool m_gktwo;
    uint32_t m_seed;
public:
    explicit ChituboxCTBModernArchive(const SLAPrinterConfig &cfg, unsigned version,
                                     bool gktwo = false, uint32_t seed = 0xEFBEADDE)
        : UVToolsRLEBinaryBase(cfg), m_version(version), m_gktwo(gktwo), m_seed(seed) {}
    sla::RasterEncoder get_encoder() const override;
    void export_print(const std::string filename, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
    static void validate_configuration(const SLAPrint &print, unsigned version);
};

class ChituboxPhotonArchive : public ChituboxCBDDLPArchive {
public:
    using ChituboxCBDDLPArchive::ChituboxCBDDLPArchive;
};

// Native AnycubicPhotonSFile schema (binary masks, fixed Photon S geometry).
class ChituboxPhotonSArchive : public SLAArchiveWriter {
    SLAPrinterConfig m_cfg;

protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;

public:
    static void validate_configuration(const SLAPrint &print);
    static sla::EncodedRaster encode_mask(const void *pixels, size_t width, size_t height, size_t components);

    explicit ChituboxPhotonSArchive(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}
    explicit ChituboxPhotonSArchive(SLAPrinterConfig &&cfg) : m_cfg(std::move(cfg)) {}

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

} // namespace Slic3r

#endif // SLIC3R_FORMAT_CHITUBOX_ARCHIVE_HPP
