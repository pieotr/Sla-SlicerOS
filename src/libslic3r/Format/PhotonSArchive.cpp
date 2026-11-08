///|/ Native port of UVtools AnycubicPhotonSFile.cs (AGPLv3).
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher.
#include "ChituboxArchive.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace Slic3r {
namespace {
constexpr size_t columns = 1440, rows = 2560;
constexpr double width_mm = 68.04, height_mm = 120.96;
constexpr unsigned preview_width = 224, preview_height = 168;
void be32(std::ostream &out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.put(char(value >> shift));
}
void be_double(std::ostream &out, double value) {
    uint64_t bits; std::memcpy(&bits, &value, sizeof bits);
    for (int shift = 56; shift >= 0; shift -= 8) out.put(char(bits >> shift));
}
void validate_geometry(const SLAPrinterConfig &cfg) {
    const bool portrait = cfg.display_orientation.value == sladoPortrait;
    const size_t x = portrait ? cfg.display_pixels_y.value : cfg.display_pixels_x.value;
    const size_t y = portrait ? cfg.display_pixels_x.value : cfg.display_pixels_y.value;
    const double w = portrait ? cfg.display_height.value : cfg.display_width.value;
    const double h = portrait ? cfg.display_width.value : cfg.display_height.value;
    if (!std::isfinite(w) || !std::isfinite(h) || !std::isfinite(cfg.max_print_height.value) ||
        x != columns || y != rows || std::abs(w-width_mm) > .001 || std::abs(h-height_mm) > .001 ||
        std::abs(cfg.max_print_height.value-165.) > .001)
        throw ExportError("Photon S requires a 1440x2560 screen, 68.04x120.96 mm and 165 mm machine Z");
    if (!cfg.display_mirror_x.value || cfg.display_mirror_y.value)
        throw ExportError("Photon S requires horizontal display mirroring");
}
// UVtools stores the count minus one with seven bits reversed; bit zero is
// the binary color. Every run includes 1..128 pixels and may cross scanlines.
unsigned run_length(uint8_t token) {
    unsigned count = 0;
    for (unsigned bit = 0; bit < 7; ++bit)
        count |= ((token >> (7-bit)) & 1u) << bit;
    return count+1;
}
void write_preview(std::ostream &out, const ThumbnailsList &thumbnails) {
    const ThumbnailData *image = nullptr;
    for (const auto &candidate : thumbnails)
        if (candidate.is_valid() && (!image || candidate.pixels.size() > image->pixels.size())) image = &candidate;
    const double scale = image ? std::min(double(preview_width)/image->width, double(preview_height)/image->height) : 1.;
    const unsigned w = image ? std::max(1u, unsigned(std::round(image->width*scale))) : 0;
    const unsigned h = image ? std::max(1u, unsigned(std::round(image->height*scale))) : 0;
    const unsigned left = (preview_width-w)/2, top = (preview_height-h)/2;
    for (unsigned y = 0; y < preview_height; ++y)
    for (unsigned x = 0; x < preview_width; ++x) {
        uint16_t bgr = 0;
        if (image && x >= left && x < left+w && y >= top && y < top+h) {
            const unsigned sx = std::min(image->width-1, (x-left)*image->width/w);
            const unsigned sy = std::min(image->height-1, (y-top)*image->height/h);
            const size_t offset = (size_t(image->height-1-sy)*image->width+sx)*4;
            const auto *p = image->pixels.data()+offset;
            bgr = uint16_t((uint16_t(p[2]&0xf8)<<8) | (uint16_t(p[1]&0xfc)<<3) | (p[0]>>3));
        }
        // The header is big endian, but UVtools DATATYPE_BGR565 is little endian.
        out.put(char(bgr)); out.put(char(bgr>>8));
    }
}
}

sla::EncodedRaster ChituboxPhotonSArchive::encode_mask(const void *ptr, size_t w, size_t h, size_t components) {
    if (!ptr || !w || !h || w > 32768 || h > 32768 || components != 1)
        throw ExportError("Photon S requires a nonempty single-channel mask");
    const auto *pixels = static_cast<const uint8_t *>(ptr);
    std::vector<uint8_t> encoded;
    for (size_t begin = 0; begin < w*h;) {
        const bool white = pixels[begin] > 127;
        size_t end = begin+1;
        while (end < w*h && end-begin < 128 && (pixels[end] > 127) == white) ++end;
        const unsigned count = unsigned(end-begin-1);
        uint8_t token = white ? 1 : 0;
        for (unsigned bit = 0; bit < 7; ++bit) token |= uint8_t(((count>>bit)&1u) << (7-bit));
        encoded.push_back(token); begin = end;
    }
    return sla::EncodedRaster(std::move(encoded), "photons");
}
std::unique_ptr<sla::RasterBase> ChituboxPhotonSArchive::create_raster() const {
    validate_geometry(m_cfg); return sla::create_printer_raster(m_cfg);
}
sla::RasterEncoder ChituboxPhotonSArchive::get_encoder() const { return encode_mask; }
void ChituboxPhotonSArchive::validate_configuration(const SLAPrint &print) {
    validate_geometry(print.printer_config());
    sla::require_single_stage_motion(print, "Photon S", false);
    for (const bool bottom : {false, true})
        for (const char *key : {"lift_height", "lift_speed", "retract_speed", "wait_before_cure"}) {
            const double value = sla::motion_parameter(print,key,bottom);
            if (!std::isfinite(value) || value < 0)
                throw ExportError(std::string("Photon S requires a finite nonnegative ")+key);
        }
    for (const char *key : {"lift_height", "lift_speed", "wait_before_cure"})
        if (sla::motion_parameter(print,key,true) != sla::motion_parameter(print,key,false))
            throw ExportError(std::string("Photon S requires the same bottom and normal ")+key);
    if (print.material_config().sla_transition_layers.value != 0)
        throw ExportError("Photon S cannot store transition exposures");
    const double layer_height = print.default_object_config().layer_height.value;
    if (!std::isfinite(layer_height) || layer_height <= 0 ||
        !std::isfinite(print.material_config().initial_layer_height.value))
        throw ExportError("Photon S requires a finite positive layer height");
    for (const double exposure : {print.material_config().exposure_time.value, print.material_config().initial_exposure_time.value})
        if (!std::isfinite(exposure) || exposure < 0)
            throw ExportError("Photon S requires finite nonnegative exposure times");
    if (std::abs(print.material_config().initial_layer_height.value-layer_height) > 1e-6)
        throw ExportError("Photon S requires uniform initial and normal layer heights");
    const double lift = sla::motion_parameter(print,"lift_height",false);
    const double speed = sla::motion_parameter(print,"lift_speed",false);
    const double retract = sla::motion_parameter(print,"retract_speed",false);
    if (lift > 0 && (speed <= 0 || retract <= 0)) throw ExportError("Photon S enabled moves require positive speeds");
    for (const double value : {speed, retract})
        if (std::abs(value-std::round(value/60.*100.)/100.*60.) > 1e-4)
            throw ExportError("Photon S speed precision is 0.01 mm/s (0.6 mm/min)");
}
void ChituboxPhotonSArchive::export_print(const std::string fname, const SLAPrint &print,
                                         const ThumbnailsList &thumbnails, const std::string &) {
    validate_geometry(m_cfg);
    validate_configuration(print);
    if (m_layers.empty() || m_layers.size() != print.print_layers().size() || m_layers.size() > UINT32_MAX)
        throw ExportError("Photon S requires a complete nonempty layer stack");
    const double layer_height = print.default_object_config().layer_height.value;
    const double lift = sla::motion_parameter(print,"lift_height",false);
    const double speed = sla::motion_parameter(print,"lift_speed",false);
    const double retract = sla::motion_parameter(print,"retract_speed",false);
    const double delay = (lift > 0 ? lift*60./speed+lift*60./retract : 0.) + sla::motion_parameter(print,"wait_before_cure",false);
    const auto bottom = sla::bottom_layers(print,m_layers.size());
    if (bottom > UINT16_MAX) throw ExportError("Photon S bottom layer count exceeds UVtools limits");
    double volume = 0.;
    for (size_t i = 0; i < m_layers.size(); ++i) {
        if (std::abs(sla::layer_z(print,i)-(i+1)*layer_height) > 1e-5)
            throw ExportError("Photon S cannot store variable layer Z positions");
        const auto &mask = m_layers[i];
        if (mask.size() > UINT32_MAX/8-4) throw ExportError("Photon S layer exceeds its bit-size field");
        const auto *data = static_cast<const uint8_t *>(mask.data());
        uint64_t pixels = 0, exposed = 0;
        for (size_t n = 0; n < mask.size(); ++n) {
            const unsigned count = run_length(data[n]); pixels += count;
            if (data[n]&1) exposed += count;
        }
        if (pixels != columns*rows) throw ExportError("Photon S mask dimensions do not match the printer");
        volume += double(exposed)*width_mm*height_mm/(columns*rows)*layer_height/1000.;
    }
    // All capability/payload checks precede opening the destination.
    std::ofstream out(fname,std::ios::binary|std::ios::trunc);
    if (!out) throw ExportError("Cannot open Photon S output");
    be32(out,2); out.put(0); out.put(49);
    // Preserve the actual hardware pitch. UVtools' constructor also uses
    // 0.04725, although its OnBeforeEncode rounds this field to 0.047.
    be_double(out,width_mm/columns);
    be_double(out,layer_height);
    be_double(out,print.material_config().exposure_time.value);
    be_double(out,std::round(delay*100.)/100.);
    be_double(out,print.material_config().initial_exposure_time.value);
    be32(out,uint32_t(bottom));
    be_double(out,lift); be_double(out,speed/60.); be_double(out,retract/60.); be_double(out,volume);
    be32(out,preview_width); be32(out,42); be32(out,preview_height); be32(out,10);
    write_preview(out,thumbnails); be32(out,uint32_t(m_layers.size()));
    for (const auto &mask : m_layers) {
        be32(out,44944); be32(out,0); be32(out,0); be32(out,columns); be32(out,rows);
        be32(out,uint32_t((mask.size()+4)*8)); be32(out,2684702720u);
        out.write(static_cast<const char *>(mask.data()),mask.size());
    }
    out.flush(); if (!out) throw ExportError("Cannot finish Photon S output");
}
} // namespace Slic3r
