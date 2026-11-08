// Native FDG v2 port from local UVtools FDGFile.cs (AGPLv3-or-later).
#ifndef SLIC3R_FORMAT_FDG_ARCHIVE_HPP
#define SLIC3R_FORMAT_FDG_ARCHIVE_HPP
#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"
namespace Slic3r {
class FDGArchive : public SLAArchiveWriter {
    SLAPrinterConfig m_cfg;
    uint32_t m_seed=0;
protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;
public:
    explicit FDGArchive(const SLAPrinterConfig &cfg,uint32_t seed=0):m_cfg(cfg),m_seed(seed){}
    explicit FDGArchive(SLAPrinterConfig &&cfg,uint32_t seed=0):m_cfg(std::move(cfg)),m_seed(seed){}
    static void crypt_mask(std::vector<uint8_t> &bytes,uint32_t seed,uint32_t layer_index);
    static void validate_configuration(const SLAPrint &print);
    void export_print(const std::string fname,const SLAPrint &print,const ThumbnailsList &thumbnails,
                      const std::string &projectname="") override;
};
}
#endif
