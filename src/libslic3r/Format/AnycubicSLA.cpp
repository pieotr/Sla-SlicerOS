#include "libslic3r/SLA/AnycubicFreshExportPlan.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
///|/ Copyright (c) 2023 Mimoja @Mimoja
///|/ Copyright (c) Prusa Research 2022 Tomáš Mészáros @tamasmeszaros, Lukáš Hejl @hejllukas
///|/ Copyright (c) 2022 ole00 @ole00
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "AnycubicSLA.hpp"

#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <fstream> // IWYU pragma: keep
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <map>
#include <vector>
#include <cassert>
#include <cstdio>

#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include "libslic3r/SLA/RasterRounding.hpp"
#include "LocalesUtils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/libslic3r.h"


#define TAG_INTRO "ANYCUBIC\0\0\0\0"
#define TAG_HEADER "HEADER\0\0\0\0\0\0"
#define TAG_PREVIEW "PREVIEW\0\0\0\0\0"
#define TAG_LAYERS "LAYERDEF\0\0\0\0"

#define CFG_LIFT_DISTANCE "LIFT_DISTANCE"
#define CFG_LIFT_SPEED "LIFT_SPEED"
#define CFG_RETRACT_SPEED "RETRACT_SPEED"
#define CFG_DELAY_BEFORE_EXPOSURE "DELAY_BEFORE_EXPOSURE"
#define CFG_BOTTOM_LIFT_SPEED "BOTTOM_LIFT_SPEED"
#define CFG_BOTTOM_LIFT_DISTANCE "BOTTOM_LIFT_DISTANCE"
#define CFG_ANTIALIASING "ANTIALIASING"


#define PREV_W 224
#define PREV_H 168
#define PREV_DPI 42


namespace Slic3r {

const AnycubicLegacyGeometry *anycubic_legacy_geometry(const std::string &extension)
{
    // Values mirror AnycubicFile's physical-geometry getters in the vendored
    // UVtools source; these are millimetres, not raster pixel counts.
    static const std::map<std::string, AnycubicLegacyGeometry> machines{
        {"pws", {68.04,120.96,165}}, {"pw0", {55.44,98.637,150}},
        {"pwx", {192,120,245}}, {"dlp", {102.40,57.60,165}},
        {"dl2p", {130.56,73.44,165}},
        {"pwmo", {82.62,130.56,165}}, {"pm3n", {143.36,89.60,165}},
        {"pm4n", {153.408,87.040,165}}, {"pwms", {82.62,130.56,160}},
        {"pwma", {134.40,84,165}}, {"pwmx", {192,120,245}},
        {"pmx2", {196.61,122.88,200}}, {"pwmb", {198.15,123.84,245}},
        {"px6s", {195.84,122.40,200}}, {"pmsq", {120,128,200}},
        {"pm3", {163.84,102.40,180}}, {"pm3m", {298.08,165.60,300}},
        {"pm3r", {218.88,123.12,250}}, {"pm5", {218.88,122.88,200}},
        {"pm5s", {218.88,122.88,200}}, {"m5sp", {223.6416,126.976,200}}
    };
    const auto found = machines.find(extension);
    return found == machines.end() ? nullptr : &found->second;
}

const std::vector<uint16_t> &anycubic_available_versions(const std::string &extension)
{
    static const std::map<std::string, std::vector<uint16_t>> versions{
        {"pws",{1}}, {"pw0",{1}}, {"pwx",{1}},
        {"pwmx",{1,515,516}}, {"pwmo",{1,515,516}}, {"pwms",{1,515,516}},
        {"pmsq",{1,515,516}}, {"dlp",{1,515,516}},
        {"pwma",{515,516}}, {"pm3",{515,516}}, {"pm3m",{515,516}},
        {"pwmb",{515,516,517}}, {"dl2p",{515,516,517}},
        {"pmx2",{515,516,517}}, {"pm3r",{515,516,517}},
        {"pm3n",{517}}, {"pm5",{517}}, {"px6s",{517}},
        {"pm5s",{518}}, {"m5sp",{518}},
        {"pm4n",{1,515,516,517,518}}, {"pwc",{1,515,516,517,518}}
    };
    std::string normalized=extension;
    if (!normalized.empty() && normalized.front()=='.') normalized.erase(0,1);
    boost::algorithm::to_lower(normalized);
    const auto found=versions.find(normalized);
    static const std::vector<uint16_t> unsupported;
    return found==versions.end() ? unsupported : found->second;
}

uint16_t anycubic_resolve_version(const std::string &extension, const std::string &requested, uint16_t default_version)
{
    const auto &available = anycubic_available_versions(extension);
    const std::string display_extension = !extension.empty() && extension.front() == '.' ? extension : "." + extension;
    uint16_t version = default_version;
    if (requested != "auto") {
        // Match canonical strings; stoi would accept partial or signed input.
        auto it = std::find_if(available.begin(), available.end(), [&](uint16_t candidate) {
            return requested == std::to_string(candidate);
        });
        if (it == available.end())
            throw std::invalid_argument("Anycubic file version '" + requested + "' is unavailable for " + display_extension +
                ". Correct Anycubic file version in Printer Settings > General > Output.");
        version = *it;
    }
    if (std::find(available.begin(), available.end(), version) == available.end())
        throw std::invalid_argument("Invalid Anycubic default version for " + display_extension);
    return version;
}

static void anycubicsla_get_pixel_span(const std::uint8_t* ptr, const std::uint8_t* end,
                               std::uint8_t& pixel, size_t& span_len)
{
    size_t max_len;

    span_len = 0;
    pixel = (*ptr) & 0xF0;
    // the maximum length of the span depends on the pixel color
    max_len = (pixel == 0 || pixel == 0xF0) ? 0xFFF : 0xF;
    while (ptr < end && span_len < max_len && ((*ptr) & 0xF0) == pixel) {
        span_len++;
        ptr++;
    }
}

struct AnycubicSLARasterEncoder
{
    sla::EncodedRaster operator()(const void *ptr,
                                  size_t      w,
                                  size_t      h,
                                  size_t      num_components)
    {
        std::vector<uint8_t> dst;
        size_t               span_len;
        std::uint8_t         pixel;
        auto                 size = w * h * num_components;
        dst.reserve(size);

        const std::uint8_t *src = reinterpret_cast<const std::uint8_t *>(ptr);
        const std::uint8_t *src_end = src + size;
        while (src < src_end) {
            anycubicsla_get_pixel_span(src, src_end, pixel, span_len);
            src += span_len;
            // fully transparent of fully opaque pixel
            if (pixel == 0 || pixel == 0xF0) {
                pixel = pixel | (span_len >> 8);
                std::copy(&pixel, (&pixel) + 1, std::back_inserter(dst));
                pixel = span_len & 0xFF;
                std::copy(&pixel, (&pixel) + 1, std::back_inserter(dst));
            }
            // antialiased pixel
            else {
                pixel = pixel | span_len;
                std::copy(&pixel, (&pixel) + 1, std::back_inserter(dst));
            }
        }

        return sla::EncodedRaster(std::move(dst), "pwimg");
    }
};

// PWS is a binary plane with 7-bit run lengths. PW0's grayscale nibble
// encoding is not interchangeable, even though the surrounding tables match.
struct AnycubicPWSRasterEncoder {
    sla::EncodedRaster operator()(const void *data, size_t width, size_t height, size_t components) const {
        if (components != 1 || !width || !height) throw ExportError("PWS requires a grayscale mask");
        const auto *pixels = static_cast<const uint8_t *>(data);
        std::vector<uint8_t> encoded;
        size_t position = 0, count = width * height;
        while (position < count) {
            const bool white = pixels[position] >= 128;
            size_t run = 1;
            while (run < 127 && position+run < count && (pixels[position+run] >= 128) == white) ++run;
            encoded.push_back(uint8_t((white ? 128 : 0) | run));
            position += run;
        }
        return sla::EncodedRaster(std::move(encoded), "pwsimg");
    }
};

using ConfMap = std::map<std::string, std::string>;

typedef struct anycubicsla_format_intro
{
    char          tag[12];
    std::uint32_t version;  // value 1 (also known as 515, 516 and 517)
    std::uint32_t area_num; // Number of tables - usually 4
    std::uint32_t header_data_offset;
    std::uint32_t software_data_offset; // unused in version 1
    std::uint32_t preview_data_offset;
    std::uint32_t layer_color_offset; // unused in version 1
    std::uint32_t layer_data_offset;
    std::uint32_t extra_data_offset; // unused here (only used in version 516)
    std::uint32_t machine_data_offset; // serialized from version 516
    std::uint32_t image_data_offset;
    std::uint32_t model_data_offset; // serialized from version 517
} anycubicsla_format_intro;

typedef struct anycubicsla_format_header
{
    char          tag[12];
    std::uint32_t payload_size;
    float         pixel_size_um;
    float         layer_height_mm;
    float         exposure_time_s;
    float         delay_before_exposure_s;
    float         bottom_exposure_time_s;
    float         bottom_layer_count;
    float         lift_distance_mm;
    float         lift_speed_mms;
    float         retract_speed_mms;
    float         volume_ml;
    std::uint32_t antialiasing;
    std::uint32_t res_x;
    std::uint32_t res_y;
    float         weight_g;
    float         price;
    std::uint32_t price_currency;
    std::uint32_t per_layer_override; // ? unknown meaning ?
    std::uint32_t print_time_s;
    std::uint32_t transition_layer_count;
    std::uint32_t transition_layer_type; // usually 0

} anycubicsla_format_header;

typedef struct anycubicsla_format_preview
{
    char          tag[12];
    std::uint32_t payload_size;
    std::uint32_t preview_w;
    std::uint32_t preview_dpi;
    std::uint32_t preview_h;
    // raw image data in BGR565 format
     std::uint8_t pixels[PREV_W * PREV_H * 2];
} anycubicsla_format_preview;

typedef struct anycubicsla_format_layers_header
{
    char          tag[12];
    std::uint32_t payload_size;
    std::uint32_t layer_count;
} anycubicsla_format_layers_header;

typedef struct anycubicsla_format_layer
{
    std::uint32_t image_offset;
    std::uint32_t image_size;
    float         lift_distance_mm;
    float         lift_speed_mms;
    float         exposure_time_s;
    float         layer_height_mm;
    std::uint32_t nonzero_pixel_count;
    std::uint32_t padding;
} anycubicsla_format_layer;

typedef struct anycubicsla_format_misc
{
    float bottom_layer_height_mm;
    float bottom_lift_distance_mm;
    float bottom_lift_speed_mms;

} anycubicsla_format_misc;

class AnycubicSLAFormatConfigDef : public ConfigDef
{
public:
    AnycubicSLAFormatConfigDef()
    {
        add(CFG_LIFT_DISTANCE, coFloat);
        add(CFG_LIFT_SPEED, coFloat);
        add(CFG_RETRACT_SPEED, coFloat);
        add(CFG_DELAY_BEFORE_EXPOSURE, coFloat);
        add(CFG_BOTTOM_LIFT_DISTANCE, coFloat);
        add(CFG_BOTTOM_LIFT_SPEED, coFloat);
        add(CFG_ANTIALIASING, coInt);
    }
};

class AnycubicSLAFormatDynamicConfig : public DynamicConfig
{
public:
    AnycubicSLAFormatDynamicConfig(){};
    const ConfigDef *def() const override { return &config_def; }

private:
    AnycubicSLAFormatConfigDef config_def;
};

namespace {

float get_cfg_value_f(const DynamicConfig &cfg,
                      const std::string   &key,
                      const float         &def = 0.f)
{
    if (cfg.has(key)) {
        if (auto opt = cfg.option(key))
            return opt->getFloat();
    }

    return def;
}

int get_cfg_value_i(const DynamicConfig &cfg,
                    const std::string   &key,
                    const int           &def = 0)
{
    if (cfg.has(key)) {
        if (auto opt = cfg.option(key))
            return opt->getInt();
    }

    return def;
}

template<class T> void crop_value(T &val, T val_min, T val_max)
{
    if (val < val_min) {
        val = val_min;
    } else if (val > val_max) {
        val = val_max;
    }
}

// RGB565 previews share the renderer's bottom-up RGBA orientation. Composite
// alpha onto black and aspect-fit both preview sizes without stretching.
std::vector<uint8_t> fill_preview_pixels(size_t target_width, size_t target_height,
                                        const ThumbnailsList &thumbnails)
{
    std::vector<uint8_t> pixels(target_width * target_height * 2, 0);
    const auto source = std::find_if(thumbnails.begin(), thumbnails.end(), [](const ThumbnailData &thumbnail) {
        return thumbnail.is_valid() && thumbnail.width <= 4096 && thumbnail.height <= 4096;
    });
    if (source == thumbnails.end()) return pixels;
    const double scale = std::min(double(target_width)/source->width, double(target_height)/source->height);
    const size_t width = std::max<size_t>(1, std::lround(source->width*scale));
    const size_t height = std::max<size_t>(1, std::lround(source->height*scale));
    const size_t left = (target_width-width)/2, top = (target_height-height)/2;
    for (size_t y=0;y<height;++y) for (size_t x=0;x<width;++x) {
        const size_t sx = std::min<size_t>(source->width-1, x*source->width/width);
        const size_t sy = source->height-1-std::min<size_t>(source->height-1, y*source->height/height);
        const auto *rgba = source->pixels.data()+4*(sy*source->width+sx);
        const unsigned r = (unsigned(rgba[0])*rgba[3]+127)/255;
        const unsigned g = (unsigned(rgba[1])*rgba[3]+127)/255;
        const unsigned b = (unsigned(rgba[2])*rgba[3]+127)/255;
        const uint16_t pixel = uint16_t(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
        const size_t offset = 2*((top+y)*target_width+left+x);
        pixels[offset] = uint8_t(pixel); pixels[offset+1] = uint8_t(pixel>>8);
    }
    return pixels;
}

void fill_preview(anycubicsla_format_preview &p, anycubicsla_format_misc &,
                  const ThumbnailsList &thumbnails)
{
    p.preview_w = PREV_W; p.preview_h = PREV_H; p.preview_dpi = PREV_DPI;
    p.payload_size = sizeof(p) - sizeof(p.tag) - sizeof(p.payload_size);
    const auto pixels = fill_preview_pixels(PREV_W, PREV_H, thumbnails);
    std::copy(pixels.begin(), pixels.end(), p.pixels);
}

// Consume the completed native header and rows, before archive publication.
// Scope deliberately PMX2/PWMB/PM4n515; no ownership or generic LCD policy change.
float anycubic515_round2(float value, bool away=false)
{
    if(!std::isfinite(value) || value<0)
        throw ExportError("Anycubic515 time requires finite nonnegative native parameters");
    if(value>=1e8f)return value; // System.MathF.Round large-float contract
    const float scaled=value*100.f;
    return (away?std::round(scaled):std::nearbyint(scaled))/100.f;
}
uint32_t anycubic515_native_time(const anycubicsla_format_header &header,
                               const std::vector<anycubicsla_format_layer> &rows)
{
    const sla::ScopedNearestRounding rounding;
    const float wait=header.delay_before_exposure_s;
    // ResetParameters inherits both wait and LightOff from this common field.
    (void)anycubic515_round2(wait);
    const float global_lift_speed=anycubic515_round2(header.lift_speed_mms*60.f,true);
    const float retract_speed=anycubic515_round2(header.retract_speed_mms*60.f,true);
    float total=300.f; // FileFormat.ExtraPrintTime, not generic SLA statistics
    // AnycubicFile.BottomLayerCount exposes a ushort. Refuse native counts
    // outside that public domain; never cast an out-of-range float to C++ uint16.
    const double native_bottom_count=header.bottom_layer_count;
    if(!std::isfinite(native_bottom_count) || native_bottom_count<0 ||
       std::floor(native_bottom_count)!=native_bottom_count || native_bottom_count>UINT16_MAX)
        throw ExportError("Anycubic515 bottom count cannot be represented by the reference ushort field");
    // During Decode, every LayerDef.CopyTo runs before legacy BottomFix.
    // Bottom LiftSpeed therefore still falls back to base default100;
    // normal layers fall back to the converted HEADER speed.
    const uint16_t decoded_bottom_count=uint16_t(header.bottom_layer_count);
    for(size_t index=0;index<rows.size();++index) {
        const auto &row=rows[index];
        // LayerDef.CopyTo applies ordinary Layer setters to these three fields.
        const float exposure=anycubic515_round2(row.exposure_time_s);
        const float lift=anycubic515_round2(row.lift_distance_mm);
        float lift_speed=anycubic515_round2(anycubic515_round2(row.lift_speed_mms*60.f,true));
        if(lift_speed<=0)lift_speed=index<decoded_bottom_count ? 100.f : global_lift_speed;
        float motor=0.f;
        auto move=[&](float distance,float velocity) {
            if(distance==0)return;
            if(velocity<=0)throw ExportError("Anycubic515 active native motion requires positive speed");
            const float seconds=distance/(velocity/60.f);
            motor+=seconds;
            if(!std::isfinite(seconds) || !std::isfinite(motor))
                throw ExportError("Anycubic515 native motor time exceeds float range");
        };
        // Version515 has one lift/retract phase, zero second distance.
        move(lift,lift_speed);move(lift,retract_speed);
        motor=anycubic515_round2(motor);
        const float rests=wait+exposure;
        const float layer_time=rests+std::max(motor,wait);
        total+=layer_time;
        if(!std::isfinite(rests) || !std::isfinite(layer_time) || !std::isfinite(total))
            throw ExportError("Anycubic515 native time exceeds float range");
    }
    total=anycubic515_round2(total);
    if(double(total)>double(UINT32_MAX))
        throw ExportError("Anycubic515 native time exceeds uint32");
    return uint32_t(total); // Reference setter truncates the final float.
}

void fill_header(anycubicsla_format_header &h,
                 anycubicsla_format_misc   &m,
                 const SLAPrint     &print,
                 std::uint32_t       layer_count,
                 bool supports_extra_motion = false,
                 bool defer_native_time = false)
{
    CNumericLocalesSetter locales_setter;

    float        bottle_volume_ml;
    float        bottle_cost;
    float        material_density;
    auto        &cfg     = print.full_print_config();
    auto         mat_opt = cfg.option("material_notes");
    std::string  mnotes  = mat_opt? cfg.option("material_notes")->serialize() : "";
    // create a config parser from the material notes
    Slic3r::AnycubicSLAFormatDynamicConfig mat_cfg;
    SLAPrintStatistics              stats = print.print_statistics();

    // sanitize the string config
    boost::replace_all(mnotes, "\\n", "\n");
    boost::replace_all(mnotes, "\\r", "\r");
    mat_cfg.load_from_ini_string(mnotes,
                                 ForwardCompatibilitySubstitutionRule::Enable);

    h.layer_height_mm        = get_cfg_value_f(cfg, "layer_height");
    if (supports_extra_motion) {
        // EXTRA stores two lift stages and independent bottom/normal retract
        // speeds. It has no retract-height, after-wait or PWM fields.
        for (bool bottom : {false, true}) {
            for (const char *key : {"retract_height2", "wait_after_cure", "wait_after_lift"})
                if (sla::motion_parameter(print, key, bottom) != 0)
                    throw ExportError(std::string("Modern Anycubic cannot store ") +
                        (bottom ? "bottom " : "normal ") + key);
            if (sla::motion_parameter(print, "light_pwm", bottom) != 255)
                throw ExportError("Modern Anycubic cannot store a custom UV PWM");
            for (const char *key : {"lift_height", "lift_height2", "lift_speed", "lift_speed2",
                                   "retract_speed", "retract_speed2", "wait_before_cure"}) {
                const float value = sla::motion_parameter(print, key, bottom);
                if (!std::isfinite(value) || value < 0)
                    throw ExportError(std::string("Modern Anycubic requires finite nonnegative ") + key);
            }
            if ((sla::motion_parameter(print, "lift_height", bottom) > 0 &&
                 sla::motion_parameter(print, "lift_speed", bottom) <= 0) ||
                (sla::motion_parameter(print, "lift_height2", bottom) > 0 &&
                 sla::motion_parameter(print, "lift_speed2", bottom) <= 0) ||
                ((sla::motion_parameter(print, "lift_height", bottom) +
                  sla::motion_parameter(print, "lift_height2", bottom)) > 0 &&
                 sla::motion_parameter(print, "retract_speed", bottom) <= 0))
                throw ExportError("Modern Anycubic requires positive speeds for enabled motion");
        }
    } else {
        sla::require_single_stage_motion(print, "Anycubic", false);
    }
    if (sla::motion_parameter(print, "wait_before_cure", true) != sla::motion_parameter(print, "wait_before_cure", false))
        throw ExportError("This Anycubic encoder requires the same normal and bottom wait before exposure");
    m.bottom_layer_height_mm = get_cfg_value_f(cfg, "initial_layer_height");
    h.exposure_time_s        = get_cfg_value_f(cfg, "exposure_time");
    h.bottom_exposure_time_s = get_cfg_value_f(cfg, "initial_exposure_time");
    h.bottom_layer_count = uint32_t(sla::bottom_layers(print, layer_count));
    if (layer_count < h.bottom_layer_count) {
        h.bottom_layer_count = layer_count;
    }
    double display_w_mm = get_cfg_value_f(cfg, "display_width");
    double display_h_mm = get_cfg_value_f(cfg, "display_height");
    std::uint32_t res_x = static_cast<std::uint32_t>(get_cfg_value_i(cfg, "display_pixels_x"));
    std::uint32_t res_y = static_cast<std::uint32_t>(get_cfg_value_i(cfg, "display_pixels_y"));

    const auto ro = get_cfg_value_i(cfg, "display_orientation");
    if (ro == sla::RasterBase::roPortrait) {
        std::swap(display_w_mm, display_h_mm);
        std::swap(res_x, res_y);
    }

    h.res_x = res_x;
    h.res_y = res_y;
    bottle_volume_ml = get_cfg_value_f(cfg, "bottle_volume");
    bottle_cost = get_cfg_value_f(cfg, "bottle_cost");
    material_density = print.material_config().material_density.value;

    h.volume_ml = (stats.objects_used_material + stats.support_used_material) / 1000;
    h.weight_g           = h.volume_ml * material_density;
    h.price              = bottle_volume_ml > 0 ? (h.volume_ml * bottle_cost) / bottle_volume_ml : 0.f;
    h.price_currency     = '$';
    h.antialiasing       = 1;
    h.per_layer_override = 1;

    // TODO - expose these variables to the UI rather than using material notes
    if (mat_cfg.has(CFG_ANTIALIASING)) {
        h.antialiasing = get_cfg_value_i(mat_cfg, CFG_ANTIALIASING);
        if (h.antialiasing != 1) throw ExportError("This Anycubic encoder requires antialiasing level 1");
    } else {
        h.antialiasing = 1;
    }

    // The GUI's canonical speeds are mm/min; this container stores mm/s.
    h.delay_before_exposure_s = sla::motion_parameter(print, "wait_before_cure", false);
    h.lift_distance_mm = sla::motion_parameter(print, "lift_height", false);
    m.bottom_lift_distance_mm = sla::motion_parameter(print, "lift_height", true);
    if (supports_extra_motion) {
        // LayerDef and HEADER store total travel; EXTRA stores the split.
        h.lift_distance_mm += sla::motion_parameter(print, "lift_height2", false);
        m.bottom_lift_distance_mm += sla::motion_parameter(print, "lift_height2", true);
    }
    h.lift_speed_mms = sla::motion_parameter(print, "lift_speed", false) / 60.f;
    m.bottom_lift_speed_mms = sla::motion_parameter(print, "lift_speed", true) / 60.f;
    h.retract_speed_mms = sla::motion_parameter(print, "retract_speed", false) / 60.f;
    h.print_time_s = defer_native_time ? 0 : float(sla::lcd_print_time(print));


    h.payload_size  = sizeof(h) - sizeof(h.tag) - sizeof(h.payload_size);

    // AnycubicFile.EncodeInternally uses PixelSizeMicronsMax for the single
    // HEADER pitch. MACHINE retains both physical axes (and v518 both pitches).
    const double pixel_size_x_um = (h.res_x > 0) ? (display_w_mm / double(h.res_x) * 1000.0) : 50.0;
    const double pixel_size_y_um = (h.res_y > 0) ? (display_h_mm / double(h.res_y) * 1000.0) : 50.0;
    h.pixel_size_um = static_cast<float>(std::max(pixel_size_x_um,pixel_size_y_um));
}

} // namespace

std::unique_ptr<sla::RasterBase> AnycubicSLAArchive::create_raster() const
{
    return sla::create_printer_raster(m_cfg);
}

sla::RasterEncoder AnycubicSLAArchive::get_encoder() const
{
    if (m_pws) return AnycubicPWSRasterEncoder{};
    return AnycubicSLARasterEncoder{};
}

// Endian safe write of little endian 32bit ints
static void anycubicsla_write_int32(std::ofstream &out, std::uint32_t val)
{
    const char i1 = (val & 0xFF);
    const char i2 = (val >> 8) & 0xFF;
    const char i3 = (val >> 16) & 0xFF;
    const char i4 = (val >> 24) & 0xFF;

    out.write((const char *) &i1, 1);
    out.write((const char *) &i2, 1);
    out.write((const char *) &i3, 1);
    out.write((const char *) &i4, 1);
}
static void anycubicsla_write_float(std::ofstream &out, float val)
{
    static_assert(sizeof(float)==sizeof(std::uint32_t));
    std::uint32_t bits;
    std::memcpy(&bits,&val,sizeof(bits));
    anycubicsla_write_int32(out,bits);
}

static void anycubicsla_write_intro(std::ofstream &out, anycubicsla_format_intro &i)
{
    out.write(TAG_INTRO, sizeof(i.tag));
    anycubicsla_write_int32(out, i.version);
    anycubicsla_write_int32(out, i.area_num);
    anycubicsla_write_int32(out, i.header_data_offset);
    anycubicsla_write_int32(out, i.software_data_offset);
    anycubicsla_write_int32(out, i.preview_data_offset);
    anycubicsla_write_int32(out, i.layer_color_offset);
    anycubicsla_write_int32(out, i.layer_data_offset);
    anycubicsla_write_int32(out, i.extra_data_offset);
    if (i.version >= ANYCUBIC_SLA_FORMAT_VERSION_516) anycubicsla_write_int32(out, i.machine_data_offset);
    anycubicsla_write_int32(out, i.image_data_offset);
    if (i.version >= ANYCUBIC_SLA_FORMAT_VERSION_517) anycubicsla_write_int32(out, i.model_data_offset);
}

static void anycubicsla_write_header(std::ofstream &out, anycubicsla_format_header &h)
{
    out.write(TAG_HEADER, sizeof(h.tag));
    anycubicsla_write_int32(out, h.payload_size);
    anycubicsla_write_float(out, h.pixel_size_um);
    anycubicsla_write_float(out, h.layer_height_mm);
    anycubicsla_write_float(out, h.exposure_time_s);
    anycubicsla_write_float(out, h.delay_before_exposure_s);
    anycubicsla_write_float(out, h.bottom_exposure_time_s);
    anycubicsla_write_float(out, h.bottom_layer_count);
    anycubicsla_write_float(out, h.lift_distance_mm);
    anycubicsla_write_float(out, h.lift_speed_mms);
    anycubicsla_write_float(out, h.retract_speed_mms);
    anycubicsla_write_float(out, h.volume_ml);
    anycubicsla_write_int32(out, h.antialiasing);
    anycubicsla_write_int32(out, h.res_x);
    anycubicsla_write_int32(out, h.res_y);
    anycubicsla_write_float(out, h.weight_g);
    anycubicsla_write_float(out, h.price);
    anycubicsla_write_int32(out, h.price_currency);
    anycubicsla_write_int32(out, h.per_layer_override);
    anycubicsla_write_int32(out, h.print_time_s);
    anycubicsla_write_int32(out, h.transition_layer_count);
    anycubicsla_write_int32(out, h.transition_layer_type);
}

static void anycubicsla_write_preview(std::ofstream &out, anycubicsla_format_preview &p)
{
    out.write(TAG_PREVIEW, sizeof(p.tag));
    anycubicsla_write_int32(out, p.payload_size);
    anycubicsla_write_int32(out, p.preview_w);
    anycubicsla_write_int32(out, p.preview_dpi);
    anycubicsla_write_int32(out, p.preview_h);
    out.write((const char*) p.pixels, sizeof(p.pixels));
}

static void anycubicsla_write_layers_header(std::ofstream &out, anycubicsla_format_layers_header &h)
{
    out.write(TAG_LAYERS, sizeof(h.tag));
    anycubicsla_write_int32(out, h.payload_size);
    anycubicsla_write_int32(out, h.layer_count);
}

static void anycubicsla_write_layer(std::ofstream &out, anycubicsla_format_layer &l)
{
    anycubicsla_write_int32(out, l.image_offset);
    anycubicsla_write_int32(out, l.image_size);
    anycubicsla_write_float(out, l.lift_distance_mm);
    anycubicsla_write_float(out, l.lift_speed_mms);
    anycubicsla_write_float(out, l.exposure_time_s);
    anycubicsla_write_float(out, l.layer_height_mm);
    anycubicsla_write_int32(out, l.nonzero_pixel_count);
    anycubicsla_write_int32(out, l.padding);
}

// Photon Workshop 516/517/518 have eight/nine/eleven tables. Keep addresses independent
// of C++ struct padding and write all integers/floats explicitly little endian.
// See vendored UVtools AnycubicFile FileMark/Header/SubLayerDefinition schemas.
static void export_anycubic_modern(const std::string &filename, const SLAPrinterConfig &cfg,
    const std::vector<sla::EncodedRaster> &rasters, const SLAPrint &print,
    const ThumbnailsList &thumbnails, uint16_t version, const sla::AnycubicFreshExportPlan *fresh=nullptr)
{
    if (rasters.empty() || rasters.size() > UINT32_MAX || rasters.size() != print.print_layers().size())
        throw ExportError("Modern Anycubic requires a nonempty, complete layer stack");
    anycubicsla_format_header header{};
    anycubicsla_format_misc misc{};
    fill_header(header, misc, print, uint32_t(rasters.size()), true);
    if(fresh) {
        header.layer_height_mm=fresh->nominal_height;
        header.exposure_time_s=float(fresh->globals.normal.at(sla::LayerParameter::Exposure));
        header.bottom_exposure_time_s=float(fresh->globals.bottom.at(sla::LayerParameter::Exposure));
        header.bottom_layer_count=fresh->bottom_layers;header.delay_before_exposure_s=fresh->wait_before;
        header.print_time_s=fresh->estimated_print_time;
        misc.bottom_layer_height_mm=fresh->rows.front().height;
        // Serialize the planned native mm/s directly. Passing through a
        // float mm/min configuration first introduces a second rounding.
        using F=sla::LayerParameter;
        header.lift_distance_mm=float(fresh->globals.normal.at(F::LiftDistance)+fresh->globals.normal.at(F::LiftDistance2));
        misc.bottom_lift_distance_mm=float(fresh->globals.bottom.at(F::LiftDistance)+fresh->globals.bottom.at(F::LiftDistance2));
        header.lift_speed_mms=float(fresh->globals.normal.at(F::LiftSpeed)/60.);
        misc.bottom_lift_speed_mms=float(fresh->globals.bottom.at(F::LiftSpeed)/60.);
        header.retract_speed_mms=float(fresh->globals.normal.at(F::RetractSpeed)/60.);
    }
    if (!header.res_x || !header.res_y || header.res_x > UINT16_MAX || header.res_y > UINT16_MAX)
        throw ExportError("Modern Anycubic machine resolution must fit in 16 bits per axis");
    double width=cfg.display_width.value, height=cfg.display_height.value;
    if (cfg.display_orientation.value == sladoPortrait) std::swap(width,height);
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
        !std::isfinite(cfg.max_print_height.value) || cfg.max_print_height.value <= 0)
        throw ExportError("Modern Anycubic requires positive finite machine dimensions");
    const std::string machine_name = cfg.printer_model.value.empty() ? "SLA printer" : cfg.printer_model.value;
    if (machine_name.size() >= 96) throw ExportError("Anycubic machine string is too long");
    const auto &available=anycubic_available_versions(boost::filesystem::path(filename).extension().string());
    if (std::find(available.begin(),available.end(),version)==available.end())
        throw ExportError("Anycubic version is not supported by the output extension");
    const uint32_t count=uint32_t(rasters.size());
    auto checked_address = [](uint64_t value) {
        if (value > UINT32_MAX) throw ExportError("Anycubic container exceeds its 32-bit address limit");
        return uint32_t(value);
    };
    const bool has_model = version >= 517, has_sublayers = version >= 518;
    const uint32_t header_size = has_sublayers ? 96 : has_model ? 92 : 84;
    const uint32_t machine_size = has_sublayers ? 224 : 156;
    const uint32_t header_at = has_sublayers ? 64 : has_model ? 56 : 52;
    const uint32_t preview_at=header_at+16+header_size, color_at=preview_at+28+224*168*2;
    const uint32_t layers_at=color_at+28;
    const uint32_t extra_at=checked_address(uint64_t(layers_at)+20+uint64_t(count)*32);
    const uint32_t machine_at=checked_address(uint64_t(extra_at)+72);
    const uint32_t after_machine=checked_address(uint64_t(machine_at)+machine_size);
    const uint32_t software_at=has_model ? after_machine : 0;
    const uint32_t model_at=has_model ? checked_address(uint64_t(software_at)+164) : 0;
    const uint32_t after_model=has_model ? checked_address(uint64_t(model_at)+48) : after_machine;
    const uint32_t sublayers_at=has_sublayers ? after_model : 0;
    const uint32_t preview2_at=has_sublayers ? checked_address(uint64_t(sublayers_at)+24+uint64_t(count)*44) : 0;
    uint32_t image_at=has_sublayers ? checked_address(uint64_t(preview2_at)+28+330*190*2) : after_model;
    const uint32_t first_image_at=image_at;
    std::vector<anycubicsla_format_layer> layers(count);
    const double pixel_area=fresh ? fresh->geometry.width_mm/header.res_x*fresh->geometry.height_mm/header.res_y :
        width*height/(double(header.res_x)*header.res_y);
    uint32_t min_x=header.res_x, min_y=header.res_y, max_x=0, max_y=0;
    double volume=0., previous_z=0.;
    for (size_t i=0;i<rasters.size();++i) {
        const auto &raster=rasters[i];
        const auto *bytes=static_cast<const uint8_t *>(raster.data());
        uint64_t decoded=0, nonzero=0;
        for (size_t p=0;p<raster.size();++p) {
            const unsigned color=bytes[p]>>4;
            uint32_t run=bytes[p]&15;
            if (color==0 || color==15) {
                if (++p>=raster.size()) throw ExportError("Truncated Anycubic raster run");
                run=(run<<8)|bytes[p];
            }
            if (!run || decoded+run>uint64_t(header.res_x)*header.res_y)
                throw ExportError("Invalid Anycubic raster run");
            if (color) {
                nonzero+=run;
                const uint32_t start_y=uint32_t(decoded/header.res_x), end_y=uint32_t((decoded+run-1)/header.res_x);
                min_y=std::min(min_y,start_y); max_y=std::max(max_y,end_y+1);
                min_x=std::min(min_x,start_y==end_y?uint32_t(decoded%header.res_x):0u);
                max_x=std::max(max_x,start_y==end_y?uint32_t((decoded+run-1)%header.res_x)+1:header.res_x);
            }
            decoded+=run;
        }
        if (decoded!=uint64_t(header.res_x)*header.res_y || nonzero>UINT32_MAX)
            throw ExportError("Anycubic raster size differs from the printer resolution");
        const double z=fresh ? previous_z+fresh->rows[i].height : double(print.print_layers()[i].level())*SCALING_FACTOR;
        if (!std::isfinite(z) || z<=previous_z || z>cfg.max_print_height.value)
            throw ExportError("Modern Anycubic requires increasing layer Z within the machine height");
        volume+=nonzero*pixel_area*(z-previous_z)/1000.;
        auto &layer=layers[i];
        layer.image_offset=image_at; layer.image_size=checked_address(raster.size());
        image_at=checked_address(uint64_t(image_at)+raster.size());
        const bool bottom=i<size_t(header.bottom_layer_count);
        layer.lift_distance_mm=fresh?fresh->rows[i].lift:bottom?misc.bottom_lift_distance_mm:header.lift_distance_mm;
        layer.lift_speed_mms=fresh?fresh->rows[i].speed_mms:bottom?misc.bottom_lift_speed_mms:header.lift_speed_mms;
        layer.exposure_time_s=fresh?fresh->rows[i].exposure:sla::layer_exposure(print,i);
        layer.layer_height_mm=fresh?fresh->rows[i].height:float(z-previous_z);
        layer.nonzero_pixel_count=uint32_t(nonzero); previous_z=z;
    }
    if(fresh) {
        const auto &m=print.material_config();
        const auto totals=sla::cbddlp_material_totals(volume,m.material_density.value,m.bottle_cost.value,m.bottle_volume.value);
        header.volume_ml=totals[0];header.weight_g=totals[1];header.price=totals[2];
    } else {
        header.volume_ml=float(volume); header.weight_g=header.volume_ml*print.material_config().material_density.value;
        header.price=print.material_config().bottle_volume.value>0 ?
            header.volume_ml*print.material_config().bottle_cost.value/print.material_config().bottle_volume.value:0.f;
    }
    header.payload_size=header_size;
    header.transition_layer_count=uint32_t(std::min<size_t>(count-size_t(header.bottom_layer_count),
        size_t(std::max(0,print.material_config().sla_transition_layers.value))));
    if(fresh)header.transition_layer_count=fresh->transition_layers;
    // Validate everything before touching a destination. A failed write also
    // leaves the previous file intact; only a completed staging file replaces it.
    struct Staging {
        std::string name;
        ~Staging() { if (!name.empty()) std::remove(name.c_str()); }
    } staging{(boost::filesystem::path(filename).parent_path()/boost::filesystem::unique_path(".anycubic-%%%%-%%%%-%%%%-%%%%.tmp")).string()};
    std::ofstream out(staging.name,std::ios::binary|std::ios::trunc);
    if (!out) throw ExportError("Cannot create Anycubic staging file");
    auto u32=[&](uint32_t v) { anycubicsla_write_int32(out,v); };
    auto f32=[&](float v) { anycubicsla_write_float(out,v); };
    auto fixed=[&](const std::string &value,size_t size) {
        out.write(value.data(),value.size()); const std::string zeros(size-value.size(),'\0'); out.write(zeros.data(),zeros.size());
    };
    auto table=[&](const std::string &name,uint32_t size) { fixed(name,12); u32(size); };
    auto position=[&](uint32_t expected) {
        if (!out || out.tellp()!=std::streampos(expected)) throw ExportError("Invalid Anycubic table layout or write failure");
    };
    fixed("ANYCUBIC",12); u32(version); u32(has_sublayers ? 11 : has_model ? 9 : 8); u32(header_at); u32(software_at); u32(preview_at);
    u32(color_at); u32(layers_at); u32(extra_at); u32(machine_at); u32(first_image_at);
    if (has_model) u32(model_at);
    if (has_sublayers) { u32(sublayers_at); u32(preview2_at); }
    position(header_at); anycubicsla_write_header(out,header);
    u32(1); // AdvancedMode: retain distinct bottom/normal EXTRA motion.
    if (has_model) {
        u32(0); // Grey and BlurLevel (two ushort fields).
        u32(0); // ResinType.
    }
    if (has_sublayers) u32(0); // IntelligentMode (explicit exposure).
    position(preview_at);
    anycubicsla_format_preview preview{}; fill_preview(preview,misc,thumbnails); anycubicsla_write_preview(out,preview);
    position(color_at); u32(0); u32(16); for (unsigned i=0;i<16;++i) out.put(char(255)); u32(0);
    position(layers_at); table("LAYERDEF",4+count*32); u32(count);
    for (auto &layer:layers) anycubicsla_write_layer(out,layer);
    position(extra_at); table("EXTRA",24); // Original schema has 56 bytes after the table marker, despite the value 24.
    for (bool bottom:{true,false}) {
        using F=sla::LayerParameter;
        auto value=[&](F field,const char *key,bool speed=false) {
            if(fresh) {
                const auto &group=bottom?fresh->globals.bottom:fresh->globals.normal;
                return float(group.at(field)/(speed?60.:1.));
            }
            return sla::motion_parameter(print,key,bottom)/(speed?60.f:1.f);
        };
        u32(2); f32(value(F::LiftDistance,"lift_height"));
        f32(value(F::LiftSpeed,"lift_speed",true));
        f32(value(F::RetractSpeed2,"retract_speed2",true));
        f32(value(F::LiftDistance2,"lift_height2"));
        f32(value(F::LiftSpeed2,"lift_speed2",true));
        f32(value(F::RetractSpeed,"retract_speed",true));
    }
    position(machine_at); table("MACHINE",machine_size); fixed(machine_name,96); fixed("pw0img",16);
    u32(16); u32(has_sublayers ? 15 : has_model ? 7 : 1); f32(float(width)); f32(float(height)); f32(float(cfg.max_print_height.value));
    u32(available.back()); u32(6506241);
    if (has_sublayers) {
        f32(float(width/header.res_x*1000.)); f32(float(height/header.res_y*1000.));
        for (unsigned i=0;i<8;++i) u32(0);
        u32(1); u32(0);
        out.put(char(header.res_x)); out.put(char(header.res_x>>8)); out.put(char(header.res_y)); out.put(char(header.res_y>>8));
        for (unsigned i=0;i<4;++i) u32(0);
    }
    if (has_model) {
        position(software_at); fixed("Sla-SlicerOS",32); u32(164); fixed("native-"+std::to_string(version),32); fixed("portable",64); fixed("3.3-CoreProfile",32);
        position(model_at); table("MODEL",48);
        const bool empty=max_x==0;
        auto coordinate=[](double value) { return float(std::round(value*10000.)/10000.); };
        f32(empty?0:coordinate(min_x*width/header.res_x-width/2));
        f32(empty?0:coordinate(min_y*height/header.res_y-height/2)); f32(0);
        f32(empty?0:coordinate(max_x*width/header.res_x-width/2));
        f32(empty?0:coordinate(max_y*height/header.res_y-height/2)); f32(float(previous_z)); u32(0); f32(0);
    }
    if (has_sublayers) {
        position(sublayers_at); table("SUBIMGS",24+count*44); u32(count); u32(1);
        for (const auto &layer:layers) {
            u32(layer.image_offset); u32(layer.image_size); u32(layer.nonzero_pixel_count);
            for (unsigned i=0;i<8;++i) u32(0);
        }
        position(preview2_at); table("PREVIEW2",28+330*190*2); u32(330); u32(4293); u32(190);
        const auto second=fill_preview_pixels(330,190,thumbnails);
        out.write(reinterpret_cast<const char *>(second.data()),second.size());
    }
    position(first_image_at);
    for (const auto &raster:rasters) out.write(static_cast<const char *>(raster.data()),raster.size());
    position(image_at); out.flush();
    if (!out) throw ExportError("Cannot finish writing Anycubic data");
    out.close();
    if (out.fail() || std::rename(staging.name.c_str(),filename.c_str())!=0)
        throw ExportError("Cannot replace Anycubic output file");
    staging.name.clear();
}

void AnycubicSLAArchive::export_fresh_print(const std::string &filename,const SLAPrint &print,
                                          const ThumbnailsList &thumbnails,const sla::AnycubicFreshExportPlan &fresh)
{
    const auto extension=boost::filesystem::path(filename).extension().string();
    const auto version=anycubic_resolve_version(extension,m_cfg.sla_anycubic_version.value,m_version);
    if(version<516 || version>518 || m_pws)throw ExportError("Fresh Anycubic requires a grayscale modern machine schema");
    fresh.source.validate(m_layers.size());
    export_anycubic_modern(filename,m_cfg,m_layers,print,thumbnails,version,&fresh);
}

void AnycubicSLAArchive::export_print(const std::string     fname,
                               const SLAPrint       &print,
                               const ThumbnailsList &thumbnails,
                               const std::string    &/*projectname*/)
{
    const uint16_t version = anycubic_resolve_version(
        boost::filesystem::path(fname).extension().string(), m_cfg.sla_anycubic_version.value, m_version);
    if (version >= ANYCUBIC_SLA_FORMAT_VERSION_516 && version <= ANYCUBIC_SLA_FORMAT_VERSION_518) {
        export_anycubic_modern(fname,m_cfg,m_layers,print,thumbnails,version);
        return;
    }
    if (version != ANYCUBIC_SLA_FORMAT_VERSION_1 && version != ANYCUBIC_SLA_FORMAT_VERSION_515)
        throw ExportError("Unsupported Anycubic container version");
    if (m_layers.empty() || m_layers.size() > UINT32_MAX || print.print_layers().size() != m_layers.size())
        throw ExportError("Legacy Anycubic requires a nonempty, complete layer stack");
    auto checked = [](uint64_t value) {
        if (value > UINT32_MAX) throw ExportError("Legacy Anycubic file exceeds 32-bit addressing");
        return uint32_t(value);
    };
    const uint32_t count = checked(m_layers.size());
    anycubicsla_format_intro intro{};
    anycubicsla_format_header header{};
    anycubicsla_format_preview preview{};
    anycubicsla_format_layers_header layers_header{};
    anycubicsla_format_misc misc{};
    // SLAArchiveWriter::create records the actual selected codec in this
    // writer's config. A destination suffix is not the writer identity.
    const auto &selected_codec=m_cfg.sla_archive_format.value;
    const bool native_515_clock=version==515 &&
        (selected_codec=="pmx2" || selected_codec=="pwmb" || selected_codec=="pm4n");
    fill_header(header, misc, print, count, false, native_515_clock);
    double width = m_cfg.display_width.value, height = m_cfg.display_height.value;
    if (m_cfg.display_orientation.value == sladoPortrait) std::swap(width, height);
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
        !std::isfinite(m_cfg.max_print_height.value) || m_cfg.max_print_height.value <= 0 ||
        !header.res_x || !header.res_y || uint64_t(header.res_x) * header.res_y > UINT32_MAX)
        throw ExportError("Legacy Anycubic requires positive finite machine dimensions and a 32-bit pixel count");
    std::string extension = boost::filesystem::path(fname).extension().string().substr(1);
    boost::algorithm::to_lower(extension);
    const auto *geometry = anycubic_legacy_geometry(extension);
    // PWC is the original generic extension: versions 1/515 have no MACHINE
    // table or printer identity. Its original decoder returns zero dimensions.
    // Preserve the available pixel pitch, masks and parameters; the GUI warns
    // that physical dimensions require PWC 516 or newer.
    if (extension != "pwc" && (!geometry || std::abs(width - geometry->width) > .005 ||
        std::abs(height - geometry->height) > .005 ||
        std::abs(m_cfg.max_print_height.value - geometry->machine_z) > .005))
        throw ExportError("Legacy Anycubic ." + extension +
            " cannot store custom machine dimensions. Match the oriented screen width, height and Z to its printer, or select a format with a machine table.");
    fill_preview(preview, misc, thumbnails);
    static_assert(sizeof(header) == 96 && sizeof(preview) == 28 + PREV_W * PREV_H * 2 &&
                  sizeof(anycubicsla_format_layer) == 32 && sizeof(layers_header) == 20);
    intro.version = version;
    intro.area_num = version == 515 ? 5 : 4;
    intro.header_data_offset = 48;
    intro.preview_data_offset = 48 + 96;
    const uint32_t after_preview = intro.preview_data_offset + sizeof(preview);
    intro.layer_color_offset = version == 515 ? after_preview : 0;
    intro.layer_data_offset = after_preview + (version == 515 ? 28 : 0);
    intro.image_data_offset = checked(uint64_t(intro.layer_data_offset) + 20 + uint64_t(count) * 32);
    // Original 515 points ExtraAddress at the first mask but writes no EXTRA.
    intro.extra_data_offset = version == 515 ? intro.image_data_offset : 0;
    layers_header.payload_size = checked(4 + uint64_t(count) * 32);
    layers_header.layer_count = count;
    header.transition_layer_count = uint32_t(std::min<size_t>(count-size_t(header.bottom_layer_count),
        size_t(std::max(0,print.material_config().sla_transition_layers.value))));

    std::vector<anycubicsla_format_layer> definitions(count);
    uint32_t image_at = intro.image_data_offset;
    double volume_mm3 = 0., previous_z = 0.;
    const double pixel_area = width * height / (double(header.res_x) * header.res_y);
    for (size_t i = 0; i < count; ++i) {
        const auto &mask = m_layers[i];
        const auto *bytes = static_cast<const uint8_t *>(mask.data());
        uint64_t decoded = 0, nonzero = 0;
        for (size_t offset = 0; offset < mask.size(); ++offset) {
            const unsigned color = m_pws ? bytes[offset] >> 7 : bytes[offset] >> 4;
            uint32_t run = bytes[offset] & (m_pws ? 127 : 15);
            if (!m_pws && (color == 0 || color == 15)) {
                if (++offset >= mask.size()) throw ExportError("Truncated Anycubic raster run");
                run = (run << 8) | bytes[offset];
            }
            decoded += run;
            if (!run || decoded > uint64_t(header.res_x) * header.res_y)
                throw ExportError("Invalid Anycubic raster run");
            if (color) nonzero += run;
        }
        if (decoded != uint64_t(header.res_x) * header.res_y)
            throw ExportError("Anycubic raster size differs from the printer resolution");
        const double z = double(print.print_layers()[i].level()) * SCALING_FACTOR;
        if (z <= previous_z || z > m_cfg.max_print_height.value + .0001)
            throw ExportError("Legacy Anycubic requires increasing layer Z within machine height");
        auto &layer = definitions[i];
        layer.image_offset = image_at;
        layer.image_size = checked(mask.size());
        image_at = checked(uint64_t(image_at) + mask.size());
        const bool bottom = i < header.bottom_layer_count;
        layer.lift_distance_mm = bottom ? misc.bottom_lift_distance_mm : header.lift_distance_mm;
        layer.lift_speed_mms = bottom ? misc.bottom_lift_speed_mms : header.lift_speed_mms;
        layer.exposure_time_s = sla::layer_exposure(print, i);
        layer.layer_height_mm = float(z - previous_z);
        layer.nonzero_pixel_count = checked(nonzero);
        volume_mm3 += nonzero * pixel_area * (z - previous_z);
        previous_z = z;
    }
    if(native_515_clock)header.print_time_s=anycubic515_native_time(header,definitions);
    header.volume_ml = float(volume_mm3 / 1000.);
    header.weight_g = header.volume_ml * print.material_config().material_density.value;
    header.price = print.material_config().bottle_volume.value > 0 ?
        header.volume_ml * print.material_config().bottle_cost.value / print.material_config().bottle_volume.value : 0.f;

    struct Staging {
        std::string name;
        ~Staging() { if (!name.empty()) std::remove(name.c_str()); }
    } staging{(boost::filesystem::path(fname).parent_path()/boost::filesystem::unique_path(".anycubic-%%%%-%%%%-%%%%-%%%%.tmp")).string()};
    std::ofstream out(staging.name, std::ios::binary | std::ios::trunc);
    if (!out) throw ExportError("Cannot create Anycubic staging file");
    anycubicsla_write_intro(out, intro);
    anycubicsla_write_header(out, header);
    anycubicsla_write_preview(out, preview);
    if (version == 515) {
        anycubicsla_write_int32(out, 0);  // UseFullGreyscale
        anycubicsla_write_int32(out, 16); // GreyMaxCount
        const std::array<char,16> grey{char(255),char(255),char(255),char(255),char(255),char(255),char(255),char(255),
            char(255),char(255),char(255),char(255),char(255),char(255),char(255),char(255)};
        out.write(grey.data(),grey.size()); // Original AA=1 color mapping
        anycubicsla_write_int32(out, 0);
    }
    anycubicsla_write_layers_header(out, layers_header);
    for (auto &layer : definitions) anycubicsla_write_layer(out, layer);
    if (!out || out.tellp() != std::streampos(intro.image_data_offset))
        throw ExportError("Invalid legacy Anycubic table layout");
    // Stream existing encoded masks directly. No second full-stack buffer.
    for (const auto &mask : m_layers) out.write(static_cast<const char *>(mask.data()),mask.size());
    out.flush();
    if (!out || out.tellp() != std::streampos(image_at)) throw ExportError("Cannot finish writing Anycubic data");
    out.close();
    if (out.fail()) throw ExportError("Cannot close Anycubic staging file");
    if (std::rename(staging.name.c_str(),fname.c_str()) != 0)
        throw ExportError("Cannot replace Anycubic output file");
    staging.name.clear();

}

} // namespace Slic3r
