///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "MakerbaseArchive.hpp"
#include "libslic3r/SLAPrint.hpp"

namespace Slic3r {

// MakerbaseGR1Archive
std::unique_ptr<sla::RasterBase> MakerbaseGR1Archive::create_raster() const
{
    double w = m_cfg.display_width.getFloat();
    double h = m_cfg.display_height.getFloat();
    size_t pw = m_cfg.display_pixels_x.getInt();
    size_t ph = m_cfg.display_pixels_y.getInt();

    sla::Resolution res{pw, ph};
    sla::PixelDim pxdim{w / pw, h / ph};
    
    auto ro = m_cfg.display_orientation.getInt();
    sla::RasterBase::Orientation orientation = 
        ro == sla::RasterBase::roPortrait ? sla::RasterBase::roPortrait : sla::RasterBase::roLandscape;
    sla::RasterBase::Trafo tr{orientation, {false, false}};
    double gamma = m_cfg.gamma_correction.getFloat();
    return sla::create_raster_grayscale_aa(res, pxdim, gamma, tr);
}

sla::RasterEncoder MakerbaseGR1Archive::get_encoder() const { return sla::RasterEncoder{}; }

void MakerbaseGR1Archive::export_print(const std::string fname, const SLAPrint &print,
                                        const ThumbnailsList &thumbnails,
                                        const std::string    &projectname)
{
    // TODO: Implement GR1 format (big-endian RGB565 packed pixels)
}

// MakerbaseMDLPArchive
std::unique_ptr<sla::RasterBase> MakerbaseMDLPArchive::create_raster() const
{
    double w = m_cfg.display_width.getFloat();
    double h = m_cfg.display_height.getFloat();
    size_t pw = m_cfg.display_pixels_x.getInt();
    size_t ph = m_cfg.display_pixels_y.getInt();

    sla::Resolution res{pw, ph};
    sla::PixelDim pxdim{w / pw, h / ph};
    
    auto ro = m_cfg.display_orientation.getInt();
    sla::RasterBase::Orientation orientation = 
        ro == sla::RasterBase::roPortrait ? sla::RasterBase::roPortrait : sla::RasterBase::roLandscape;
    sla::RasterBase::Trafo tr{orientation, {false, false}};
    double gamma = m_cfg.gamma_correction.getFloat();
    return sla::create_raster_grayscale_aa(res, pxdim, gamma, tr);
}

sla::RasterEncoder MakerbaseMDLPArchive::get_encoder() const { return sla::RasterEncoder{}; }

void MakerbaseMDLPArchive::export_print(const std::string fname, const SLAPrint &print,
                                         const ThumbnailsList &thumbnails,
                                         const std::string    &projectname)
{
    // TODO: Implement MDLP format (nearly identical to GR1)
}

}
