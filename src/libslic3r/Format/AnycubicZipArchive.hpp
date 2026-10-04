// Native implementation of the Anycubic ZIP family (AGPL-3.0-or-later).
// Layout reference: UVtools.Core/FileFormats/AnycubicZipFile.cs, UVtools 6.0.1.
#ifndef SLIC3R_ANYCUBIC_ZIP_ARCHIVE_HPP
#define SLIC3R_ANYCUBIC_ZIP_ARCHIVE_HPP
#include "UVToolsCompatArchive.hpp"
namespace Slic3r {
class AnycubicZipArchive final : public UVToolsZIPArchiveBase {
    std::string m_extension;
public:
    AnycubicZipArchive(const SLAPrinterConfig &config, std::string extension)
        : UVToolsZIPArchiveBase(config), m_extension(std::move(extension)) {}
    void export_print(const std::string filename, const SLAPrint &print,
                      const ThumbnailsList &thumbnails, const std::string &projectname = "") override;
};
}
#endif
