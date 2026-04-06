///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ChituboxArchive.hpp"
#include "libslic3r/SLAPrint.hpp"

namespace Slic3r {

// ChituboxCBDDLPv1Archive
std::unique_ptr<sla::RasterBase> ChituboxCBDDLPv1Archive::create_raster() const
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

sla::RasterEncoder ChituboxCBDDLPv1Archive::get_encoder() const { return sla::RasterEncoder{}; }

void ChituboxCBDDLPv1Archive::export_print(const std::string fname, const SLAPrint &print,
                                             const ThumbnailsList &thumbnails,
                                             const std::string    &projectname)
{
    // TODO: Implement Chitubox CBDDLP v1 format
}

// ChituboxCTBv1Archive
std::unique_ptr<sla::RasterBase> ChituboxCTBv1Archive::create_raster() const
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

sla::RasterEncoder ChituboxCTBv1Archive::get_encoder() const { return sla::RasterEncoder{}; }

void ChituboxCTBv1Archive::export_print(const std::string fname, const SLAPrint &print,
                                         const ThumbnailsList &thumbnails,
                                         const std::string    &projectname)
{
    // TODO: Implement Chitubox CTB v1 format
}

// ChituboxPhotonArchive
std::unique_ptr<sla::RasterBase> ChituboxPhotonArchive::create_raster() const
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

sla::RasterEncoder ChituboxPhotonArchive::get_encoder() const { return sla::RasterEncoder{}; }

void ChituboxPhotonArchive::export_print(const std::string fname, const SLAPrint &print,
                                          const ThumbnailsList &thumbnails,
                                          const std::string    &projectname)
{
    // TODO: Implement Chitubox Photon format
}

// ChituboxPhotonSArchive
std::unique_ptr<sla::RasterBase> ChituboxPhotonSArchive::create_raster() const
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

sla::RasterEncoder ChituboxPhotonSArchive::get_encoder() const { return sla::RasterEncoder{}; }

void ChituboxPhotonSArchive::export_print(const std::string fname, const SLAPrint &print,
                                           const ThumbnailsList &thumbnails,
                                           const std::string    &projectname)
{
    // TODO: Implement Chitubox PhotonS format
}

}
