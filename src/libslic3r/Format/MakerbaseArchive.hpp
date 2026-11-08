/// Native GR1File/MDLPFile port from local UVtools (AGPLv3).
#ifndef SLIC3R_FORMAT_MAKERBASE_ARCHIVE_HPP
#define SLIC3R_FORMAT_MAKERBASE_ARCHIVE_HPP
#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"
namespace Slic3r {
class SLAPrint;
class MakerbaseArchive : public SLAArchiveWriter {
    SLAPrinterConfig m_cfg;
    bool m_mdlp;
protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;
public:
    MakerbaseArchive(const SLAPrinterConfig &cfg,bool mdlp) : m_cfg(cfg),m_mdlp(mdlp) {}
    static sla::EncodedRaster encode_mask(const void *,size_t,size_t,size_t);
    static void validate_configuration(const SLAPrint &,bool mdlp);
    void export_print(const std::string,const SLAPrint &,const ThumbnailsList &,const std::string & = "") override;
};
}
#endif
