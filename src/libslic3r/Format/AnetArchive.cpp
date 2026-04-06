///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "AnetArchive.hpp"
#include "libslic3r/SLAPrint.hpp"

namespace Slic3r {

// AnetN4Archive implementation
std::unique_ptr<sla::RasterBase> AnetN4Archive::create_raster() const
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

sla::RasterEncoder AnetN4Archive::get_encoder() const { return sla::RasterEncoder{}; }

void AnetN4Archive::export_print(const std::string fname, const SLAPrint &print,
                                  const ThumbnailsList &thumbnails,
                                  const std::string    &projectname)
{
    // TODO: Implement Anet N4 format (binary with UTF-16BE strings and BMP preview)
}

// AnetN7Archive implementation  
std::unique_ptr<sla::RasterBase> AnetN7Archive::create_raster() const
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

sla::RasterEncoder AnetN7Archive::get_encoder() const { return sla::RasterEncoder{}; }

void AnetN7Archive::export_print(const std::string fname, const SLAPrint &print,
                                  const ThumbnailsList &thumbnails,
                                  const std::string    &projectname)
{
    // TODO: Implement Anet N7 format
}

}
