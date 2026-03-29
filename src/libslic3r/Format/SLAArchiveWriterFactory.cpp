///|/ Copyright (c) Prusa Research 2024
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "SLAArchiveWriterFactory.hpp"
#include "AnycubicSLA.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/RasterBase.hpp"

namespace Slic3r {

namespace {

void anycubic_like_get_pixel_span(const std::uint8_t *ptr,
                                  const std::uint8_t *end,
                                  std::uint8_t       &pixel,
                                  size_t             &span_len)
{
    span_len = 0;
    pixel = (*ptr) & 0xF0;
    const size_t max_len = (pixel == 0 || pixel == 0xF0) ? 0xFFF : 0xF;

    while (ptr < end && span_len < max_len && ((*ptr) & 0xF0) == pixel) {
        ++span_len;
        ++ptr;
    }
}

struct AnycubicFallbackRasterEncoder
{
    sla::EncodedRaster operator()(const void *ptr,
                                  size_t      w,
                                  size_t      h,
                                  size_t      num_components) const
    {
        std::vector<std::uint8_t> dst;
        const size_t size = w * h * num_components;
        dst.reserve(size);

        const auto *src = reinterpret_cast<const std::uint8_t *>(ptr);
        const auto *src_end = src + size;

        while (src < src_end) {
            size_t span_len = 0;
            std::uint8_t pixel = 0;
            anycubic_like_get_pixel_span(src, src_end, pixel, span_len);
            src += span_len;

            if (pixel == 0 || pixel == 0xF0) {
                const std::uint8_t b0 = static_cast<std::uint8_t>(pixel | (span_len >> 8));
                const std::uint8_t b1 = static_cast<std::uint8_t>(span_len & 0xFF);
                dst.push_back(b0);
                dst.push_back(b1);
            } else {
                const std::uint8_t b0 = static_cast<std::uint8_t>(pixel | span_len);
                dst.push_back(b0);
            }
        }

        return sla::EncodedRaster(std::move(dst), "pwimg");
    }
};

} // namespace

// ============================================================================
// AnycubicFormatWriter - Implementation
// ============================================================================

std::unique_ptr<sla::RasterBase> AnycubicFormatWriter::create_raster() const
{
    sla::Resolution     res;
    sla::PixelDim       pxdim;
    std::array<bool, 2> mirror;

    double w  = m_printer_config->display_width.getFloat();
    double h  = m_printer_config->display_height.getFloat();
    auto   pw = size_t(m_printer_config->display_pixels_x.getInt());
    auto   ph = size_t(m_printer_config->display_pixels_y.getInt());

    mirror[X] = m_printer_config->display_mirror_x.getBool();
    mirror[Y] = m_printer_config->display_mirror_y.getBool();

    auto                         ro = m_printer_config->display_orientation.getInt();
    sla::RasterBase::Orientation orientation =
        ro == sla::RasterBase::roPortrait ? sla::RasterBase::roPortrait :
                                            sla::RasterBase::roLandscape;

    if (orientation == sla::RasterBase::roPortrait) {
        std::swap(w, h);
        std::swap(pw, ph);
    }

    res   = sla::Resolution{pw, ph};
    pxdim = sla::PixelDim{w / pw, h / ph};
    sla::RasterBase::Trafo tr{orientation, mirror};

    double gamma = m_printer_config->gamma_correction.getFloat();

    return sla::create_raster_grayscale_aa(res, pxdim, gamma, tr);
}

sla::RasterEncoder AnycubicFormatWriter::get_encoder() const
{
    return AnycubicFallbackRasterEncoder{};
}

void AnycubicFormatWriter::export_print(const std::string     fname,
                                         const SLAPrint       &print,
                                         const ThumbnailsList &thumbnails,
                                         const std::string    &projectname)
{
    // Delegate directly to AnycubicSLAArchive with the configured version
    AnycubicSLAArchive archiver(*m_printer_config, m_version);
    archiver.export_print(fname, print, thumbnails, projectname);
}

// ============================================================================
// ChituboxFormatWriter - Implementation
// ============================================================================

std::unique_ptr<sla::RasterBase> ChituboxFormatWriter::create_raster() const
{
    // Chitubox uses same display configuration as Anycubic
    sla::Resolution     res;
    sla::PixelDim       pxdim;
    std::array<bool, 2> mirror;

    double w  = m_printer_config->display_width.getFloat();
    double h  = m_printer_config->display_height.getFloat();
    auto   pw = size_t(m_printer_config->display_pixels_x.getInt());
    auto   ph = size_t(m_printer_config->display_pixels_y.getInt());

    mirror[X] = m_printer_config->display_mirror_x.getBool();
    mirror[Y] = m_printer_config->display_mirror_y.getBool();

    auto                         ro = m_printer_config->display_orientation.getInt();
    sla::RasterBase::Orientation orientation =
        ro == sla::RasterBase::roPortrait ? sla::RasterBase::roPortrait :
                                            sla::RasterBase::roLandscape;

    if (orientation == sla::RasterBase::roPortrait) {
        std::swap(w, h);
        std::swap(pw, ph);
    }

    res   = sla::Resolution{pw, ph};
    pxdim = sla::PixelDim{w / pw, h / ph};
    sla::RasterBase::Trafo tr{orientation, mirror};

    double gamma = m_printer_config->gamma_correction.getFloat();

    return sla::create_raster_grayscale_aa(res, pxdim, gamma, tr);
}

sla::RasterEncoder ChituboxFormatWriter::get_encoder() const
{
    // Temporary compatibility path until native Chitubox encoder lands.
    return AnycubicFallbackRasterEncoder{};
}

void ChituboxFormatWriter::export_print(const std::string     fname,
                                         const SLAPrint       &print,
                                         const ThumbnailsList &thumbnails,
                                         const std::string    &projectname)
{
    // Chitubox formats are compatible with Anycubic binary layout
    // For now, use Anycubic writer with format-specific header handling
    AnycubicSLAArchive archiver(*m_printer_config, ANYCUBIC_SLA_FORMAT_VERSION_1);
    archiver.export_print(fname, print, thumbnails, projectname);
}

// ============================================================================
// CrealityFormatWriter - Implementation
// ============================================================================

std::unique_ptr<sla::RasterBase> CrealityFormatWriter::create_raster() const
{
    // Creality CXDLP uses same display configuration
    sla::Resolution     res;
    sla::PixelDim       pxdim;
    std::array<bool, 2> mirror;

    double w  = m_printer_config->display_width.getFloat();
    double h  = m_printer_config->display_height.getFloat();
    auto   pw = size_t(m_printer_config->display_pixels_x.getInt());
    auto   ph = size_t(m_printer_config->display_pixels_y.getInt());

    mirror[X] = m_printer_config->display_mirror_x.getBool();
    mirror[Y] = m_printer_config->display_mirror_y.getBool();

    auto                         ro = m_printer_config->display_orientation.getInt();
    sla::RasterBase::Orientation orientation =
        ro == sla::RasterBase::roPortrait ? sla::RasterBase::roPortrait :
                                            sla::RasterBase::roLandscape;

    if (orientation == sla::RasterBase::roPortrait) {
        std::swap(w, h);
        std::swap(pw, ph);
    }

    res   = sla::Resolution{pw, ph};
    pxdim = sla::PixelDim{w / pw, h / ph};
    sla::RasterBase::Trafo tr{orientation, mirror};

    double gamma = m_printer_config->gamma_correction.getFloat();

    return sla::create_raster_grayscale_aa(res, pxdim, gamma, tr);
}

sla::RasterEncoder CrealityFormatWriter::get_encoder() const
{
    // Temporary compatibility path until native CXDLP encoder lands.
    return AnycubicFallbackRasterEncoder{};
}

void CrealityFormatWriter::export_print(const std::string     fname,
                                         const SLAPrint       &print,
                                         const ThumbnailsList &thumbnails,
                                         const std::string    &projectname)
{
    // Creality CXDLP is compatible with Anycubic format structure
    AnycubicSLAArchive archiver(*m_printer_config, ANYCUBIC_SLA_FORMAT_VERSION_1);
    archiver.export_print(fname, print, thumbnails, projectname);
}

// ============================================================================
// ZipArchiveFormatWriter - Implementation
// ============================================================================

std::unique_ptr<sla::RasterBase> ZipArchiveFormatWriter::create_raster() const
{
    // ZIP-based formats typically use PNG layers
    sla::Resolution     res;
    sla::PixelDim       pxdim;
    std::array<bool, 2> mirror;

    double w  = m_printer_config->display_width.getFloat();
    double h  = m_printer_config->display_height.getFloat();
    auto   pw = size_t(m_printer_config->display_pixels_x.getInt());
    auto   ph = size_t(m_printer_config->display_pixels_y.getInt());

    mirror[X] = m_printer_config->display_mirror_x.getBool();
    mirror[Y] = m_printer_config->display_mirror_y.getBool();

    auto                         ro = m_printer_config->display_orientation.getInt();
    sla::RasterBase::Orientation orientation =
        ro == sla::RasterBase::roPortrait ? sla::RasterBase::roPortrait :
                                            sla::RasterBase::roLandscape;

    if (orientation == sla::RasterBase::roPortrait) {
        std::swap(w, h);
        std::swap(pw, ph);
    }

    res   = sla::Resolution{pw, ph};
    pxdim = sla::PixelDim{w / pw, h / ph};
    sla::RasterBase::Trafo tr{orientation, mirror};

    double gamma = m_printer_config->gamma_correction.getFloat();

    return sla::create_raster_grayscale_aa(res, pxdim, gamma, tr);
}

sla::RasterEncoder ZipArchiveFormatWriter::get_encoder() const
{
    // Temporary compatibility path until PNG/ZIP layer encoder lands.
    return AnycubicFallbackRasterEncoder{};
}

void ZipArchiveFormatWriter::export_print(const std::string     fname,
                                          const SLAPrint       &print,
                                          const ThumbnailsList &thumbnails,
                                          const std::string    &projectname)
{
    // ZIP-based formats will use SL1Archive (ZIP + PNG) for now
    // Full format-specific implementations coming in Phase 2
    AnycubicSLAArchive archiver(*m_printer_config, ANYCUBIC_SLA_FORMAT_VERSION_1);
    archiver.export_print(fname, print, thumbnails, projectname);
}

} // namespace Slic3r
