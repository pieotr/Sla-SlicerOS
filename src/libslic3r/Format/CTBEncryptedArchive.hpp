// Native CTBEncryptedFile v4/v5 port, AGPL-3.0-or-later.
#ifndef SLIC3R_CTB_ENCRYPTED_ARCHIVE_HPP
#define SLIC3R_CTB_ENCRYPTED_ARCHIVE_HPP
#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"
namespace Slic3r {
class CTBEncryptedArchive : public SLAArchiveWriter {
    SLAPrinterConfig m_cfg;
    uint32_t m_version;
protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;
public:
    explicit CTBEncryptedArchive(const SLAPrinterConfig &cfg,uint32_t version=5):m_cfg(cfg),m_version(version){}
    static void validate_configuration(const SLAPrint &print);
    void export_print(const std::string fname,const SLAPrint &print,const ThumbnailsList &thumbnails,
                      const std::string &projectname="") override;
};
}
#endif
