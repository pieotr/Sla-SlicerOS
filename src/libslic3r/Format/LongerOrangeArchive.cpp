///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "LongerOrangeArchive.hpp"
#include "libslic3r/SLAPrint.hpp"

namespace Slic3r {

// LongerOrange10Archive implementation
std::unique_ptr<sla::RasterBase> LongerOrange10Archive::create_raster() const
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

sla::RasterEncoder LongerOrange10Archive::get_encoder() const { return sla::RasterEncoder{}; }

void LongerOrange10Archive::export_print(const std::string fname, const SLAPrint &print,
                                          const ThumbnailsList &thumbnails,
                                          const std::string    &projectname)
{
    // TODO: Implement LGS-10 format
}

// LongerOrange30Archive implementation
std::unique_ptr<sla::RasterBase> LongerOrange30Archive::create_raster() const
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

sla::RasterEncoder LongerOrange30Archive::get_encoder() const { return sla::RasterEncoder{}; }

void LongerOrange30Archive::export_print(const std::string fname, const SLAPrint &print,
                                          const ThumbnailsList &thumbnails,
                                          const std::string    &projectname)
{
    // TODO: Implement LGS-30 format
}

// LongerOrange120Archive implementation
std::unique_ptr<sla::RasterBase> LongerOrange120Archive::create_raster() const
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

sla::RasterEncoder LongerOrange120Archive::get_encoder() const { return sla::RasterEncoder{}; }

void LongerOrange120Archive::export_print(const std::string fname, const SLAPrint &print,
                                           const ThumbnailsList &thumbnails,
                                           const std::string    &projectname)
{
    // TODO: Implement LGS-120 format
}

// LongerOrange4KArchive implementation
std::unique_ptr<sla::RasterBase> LongerOrange4KArchive::create_raster() const
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

sla::RasterEncoder LongerOrange4KArchive::get_encoder() const { return sla::RasterEncoder{}; }

void LongerOrange4KArchive::export_print(const std::string fname, const SLAPrint &print,
                                          const ThumbnailsList &thumbnails,
                                          const std::string    &projectname)
{
    // TODO: Implement LGS-4K format
}

}
