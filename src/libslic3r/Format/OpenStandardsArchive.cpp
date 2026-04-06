///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "OpenStandardsArchive.hpp"
#include "libslic3r/SLAPrint.hpp"

namespace Slic3r {

// OpenSLAArchive
std::unique_ptr<sla::RasterBase> OpenSLAArchive::create_raster() const
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

sla::RasterEncoder OpenSLAArchive::get_encoder() const { return sla::RasterEncoder{}; }

void OpenSLAArchive::export_print(const std::string fname, const SLAPrint &print,
                                   const ThumbnailsList &thumbnails,
                                   const std::string    &projectname)
{
    // TODO: Implement Open SLA (OSLA) format (ZIP with binary header + PNG layers)
}

// UVJArchive
std::unique_ptr<sla::RasterBase> UVJArchive::create_raster() const
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

sla::RasterEncoder UVJArchive::get_encoder() const { return sla::RasterEncoder{}; }

void UVJArchive::export_print(const std::string fname, const SLAPrint &print,
                               const ThumbnailsList &thumbnails,
                               const std::string    &projectname)
{
    // TODO: Implement UVJ format (ZIP with config.json)
}

}
