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

#define LAYER_SIZE_ESTIMATE (32 * 1024)

namespace Slic3r {

const AnycubicLegacyGeometry *anycubic_legacy_geometry(const std::string &extension)
{
    // Values mirror AnycubicFile's physical-geometry getters in the vendored
    // UVtools source; these are millimetres, not raster pixel counts.
    static const std::map<std::string, AnycubicLegacyGeometry> machines{
        {"pws", {68.04,120.96,165}}, {"pw0", {55.44,98.637,150}},
        {"pwx", {192,120,245}}, {"dlp", {102.40,57.60,165}},
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

void fill_preview(anycubicsla_format_preview &p,
                  anycubicsla_format_misc   &/*m*/,
                  const ThumbnailsList &thumbnails)
{

    p.preview_w    = PREV_W;
    p.preview_h    = PREV_H;
    p.preview_dpi  = PREV_DPI;
    p.payload_size = sizeof(p) - sizeof(p.tag) - sizeof(p.payload_size);
                     
    std::memset(p.pixels, 0 , sizeof(p.pixels));
    const auto source = std::find_if(thumbnails.begin(), thumbnails.end(), [](const ThumbnailData &thumbnail) {
        return thumbnail.is_valid() && thumbnail.width <= 4096 && thumbnail.height <= 4096;
    });
    if (source == thumbnails.end()) return;
    // Aspect-fit the renderer or edited-mask thumbnail instead of silently
    // writing a black preview whenever its resolution differs from 224x168.
    const double scale = std::min(double(PREV_W)/source->width, double(PREV_H)/source->height);
    const size_t width = std::max<size_t>(1, std::lround(source->width*scale));
    const size_t height = std::max<size_t>(1, std::lround(source->height*scale));
    const size_t left = (PREV_W-width)/2, top = (PREV_H-height)/2;
    for (size_t y=0;y<height;++y) for (size_t x=0;x<width;++x) {
        const size_t sx = std::min<size_t>(source->width-1, x*source->width/width);
        const size_t sy = source->height-1-std::min<size_t>(source->height-1, y*source->height/height);
        const auto *rgba = source->pixels.data()+4*(sy*source->width+sx);
        const unsigned r = (unsigned(rgba[0])*rgba[3]+127)/255;
        const unsigned g = (unsigned(rgba[1])*rgba[3]+127)/255;
        const unsigned b = (unsigned(rgba[2])*rgba[3]+127)/255;
        const uint16_t pixel = uint16_t(((b>>3)<<11)|((g>>2)<<5)|(r>>3));
        const size_t offset = 2*((top+y)*PREV_W+left+x);
        p.pixels[offset] = uint8_t(pixel); p.pixels[offset+1] = uint8_t(pixel>>8);
    }
}

void fill_header(anycubicsla_format_header &h,
                 anycubicsla_format_misc   &m,
                 const SLAPrint     &print,
                 std::uint32_t       layer_count)
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
    sla::require_single_stage_motion(print, "Anycubic", false);
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
    h.lift_speed_mms = sla::motion_parameter(print, "lift_speed", false) / 60.f;
    m.bottom_lift_speed_mms = sla::motion_parameter(print, "lift_speed", true) / 60.f;
    h.retract_speed_mms = sla::motion_parameter(print, "retract_speed", false) / 60.f;
    h.print_time_s = float(sla::lcd_print_time(print));


    h.payload_size  = sizeof(h) - sizeof(h.tag) - sizeof(h.payload_size);

    // The single-pitch header field describes the X axis. Newer versions store
    // both independent pitches in MACHINE; averaging would describe neither.
    const double pixel_size_x_um = (h.res_x > 0) ? (display_w_mm / double(h.res_x) * 1000.0) : 50.0;
    h.pixel_size_um = static_cast<float>(pixel_size_x_um);
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

void AnycubicSLAArchive::export_print(const std::string     fname,
                               const SLAPrint       &print,
                               const ThumbnailsList &thumbnails,
                               const std::string    &/*projectname*/)
{
    std::uint32_t layer_count = m_layers.size();

    anycubicsla_format_intro         intro = {};
    anycubicsla_format_header        header = {};
    anycubicsla_format_preview       preview = {};
    anycubicsla_format_layers_header layers_header = {};
    anycubicsla_format_misc          misc = {};
    std::vector<uint8_t>      layer_images;
    std::uint32_t             image_offset;

    if (m_version != ANYCUBIC_SLA_FORMAT_VERSION_1 &&
        m_version != ANYCUBIC_SLA_FORMAT_VERSION_515 &&
        m_version != ANYCUBIC_SLA_FORMAT_VERSION_516 &&
        m_version != ANYCUBIC_SLA_FORMAT_VERSION_517)
        throw ExportError("Unsupported Anycubic container version");

    intro.version             = m_version;
    intro.area_num            = 4;
    const uint32_t intro_size = 48 + (m_version >= ANYCUBIC_SLA_FORMAT_VERSION_516 ? 4 : 0) +
                                              (m_version >= ANYCUBIC_SLA_FORMAT_VERSION_517 ? 4 : 0);
    intro.header_data_offset  = intro_size;
    intro.preview_data_offset = intro_size + sizeof(header);
    intro.layer_data_offset   = intro.preview_data_offset + sizeof(preview);
    const bool machine_table = m_version >= ANYCUBIC_SLA_FORMAT_VERSION_516;
    if (machine_table) {
        intro.machine_data_offset = intro.layer_data_offset;
        intro.layer_data_offset += 224;
        ++intro.area_num;
    }
    intro.image_data_offset = intro.layer_data_offset +
                              sizeof(layers_header) +
                              (sizeof(anycubicsla_format_layer) * layer_count);

    fill_header(header, misc, print, layer_count);
    if (!machine_table) {
        std::string extension = boost::filesystem::path(fname).extension().string();
        if (!extension.empty()) extension.erase(0, 1);
        boost::algorithm::to_lower(extension);
        const auto *geometry = anycubic_legacy_geometry(extension);
        double width = m_cfg.display_width.value, height = m_cfg.display_height.value;
        if (m_cfg.display_orientation.value == sladoPortrait) std::swap(width, height);
        // Permit floating-point/profile rounding, without hiding size changes.
        if (!geometry || std::abs(width - geometry->width) > .005 ||
            std::abs(height - geometry->height) > .005 ||
            std::abs(m_cfg.max_print_height.value - geometry->machine_z) > .005)
            throw ExportError("Legacy Anycubic ." + extension +
                " cannot store custom machine dimensions. Match the oriented screen width, height and Z to its printer, or select a format with a machine table.");
    }
    fill_preview(preview, misc, thumbnails);
    std::vector<uint32_t> nonzero(layer_count);
    double volume_mm3 = 0., previous_z = 0.;
    const auto &printer = print.printer_config();
    const double pixel_area = printer.display_width.value * printer.display_height.value /
                              (double(header.res_x) * header.res_y);
    for (size_t layer = 0; layer < layer_count; ++layer) {
        const auto &mask = m_layers[layer];
        const auto *bytes = static_cast<const uint8_t *>(mask.data());
        uint64_t decoded = 0;
        for (size_t offset = 0; offset < mask.size(); ++offset) {
            const unsigned color = m_pws ? bytes[offset] >> 7 : bytes[offset] >> 4;
            uint32_t run = bytes[offset] & (m_pws ? 127 : 15);
            if (!m_pws && (color == 0 || color == 15)) {
                if (++offset >= mask.size()) throw ExportError("Truncated Anycubic raster run");
                run = (run << 8) | bytes[offset];
            }
            decoded += run;
            if (color) nonzero[layer] += run;
        }
        if (decoded != uint64_t(header.res_x) * header.res_y)
            throw ExportError("Anycubic raster size differs from the printer resolution");
        const double z = double(print.print_layers().at(layer).level()) * SCALING_FACTOR;
        volume_mm3 += nonzero[layer] * pixel_area * (z-previous_z);
        previous_z = z;
    }
    header.volume_ml = float(volume_mm3 / 1000.);
    header.weight_g = header.volume_ml * print.material_config().material_density.value;
    header.price = print.material_config().bottle_volume.value > 0 ?
        header.volume_ml * print.material_config().bottle_cost.value / print.material_config().bottle_volume.value : 0.f;

    try {
        // open the file and write the contents
        std::ofstream out;
        out.open(fname, std::ios::binary | std::ios::out | std::ios::trunc);
        anycubicsla_write_intro(out, intro);
        anycubicsla_write_header(out, header);
        anycubicsla_write_preview(out, preview);
        if (machine_table) {
            // Native MACHINE v224 records both physical axes instead of
            // relying on UVtools' filename-dependent device defaults.
            out.write("MACHINE\0\0\0\0\0", 12); anycubicsla_write_int32(out, 224);
            auto fixed_string = [&](const std::string &value, size_t length) {
                if (value.size() >= length) throw ExportError("Anycubic machine string is too long");
                out.write(value.data(), value.size());
                std::string padding(length-value.size(), '\0'); out.write(padding.data(), padding.size());
            };
            fixed_string(m_cfg.printer_model.value.empty() ? "SLA printer" : m_cfg.printer_model.value, 96);
            fixed_string(m_pws ? "pwsimg" : "pw0img", 16);
            anycubicsla_write_int32(out, 16); anycubicsla_write_int32(out, 1);
            double width=m_cfg.display_width.value, height=m_cfg.display_height.value;
            if (m_cfg.display_orientation.value == sladoPortrait) std::swap(width,height);
            anycubicsla_write_float(out, float(width)); anycubicsla_write_float(out, float(height));
            anycubicsla_write_float(out, float(m_cfg.max_print_height.value));
            anycubicsla_write_int32(out, 518); anycubicsla_write_int32(out, 6506241);
            anycubicsla_write_float(out, float(width/header.res_x*1000.));
            anycubicsla_write_float(out, float(height/header.res_y*1000.));
            for (unsigned i=0;i<8;++i) anycubicsla_write_int32(out, 0);
            anycubicsla_write_int32(out, 1); anycubicsla_write_int32(out, 0);
            out.put(char(header.res_x&255)); out.put(char(header.res_x>>8));
            out.put(char(header.res_y&255)); out.put(char(header.res_y>>8));
            for (unsigned i=0;i<4;++i) anycubicsla_write_int32(out, 0);
        }

        layers_header.payload_size = intro.image_data_offset - intro.layer_data_offset -
                        sizeof(layers_header.tag)  - sizeof(layers_header.payload_size);
        layers_header.layer_count = layer_count;
        anycubicsla_write_layers_header(out, layers_header);

        //layers
        layer_images.reserve(layer_count * LAYER_SIZE_ESTIMATE);
        image_offset = intro.image_data_offset;
        size_t i = 0;
        for (const sla::EncodedRaster &rst : m_layers) {
            anycubicsla_format_layer l;
            std::memset(&l, 0, sizeof(l));
            l.image_offset = image_offset;
            l.image_size = rst.size();
            if (i < header.bottom_layer_count) {
                l.exposure_time_s = header.bottom_exposure_time_s;
                l.layer_height_mm = misc.bottom_layer_height_mm;
                l.lift_distance_mm = misc.bottom_lift_distance_mm;
                l.lift_speed_mms = misc.bottom_lift_speed_mms;
            } else {
                l.exposure_time_s = header.exposure_time_s;
                l.layer_height_mm = header.layer_height_mm;
                l.lift_distance_mm = header.lift_distance_mm;
                l.lift_speed_mms = header.lift_speed_mms;
            }
            l.exposure_time_s = sla::layer_exposure(print, i);
            l.layer_height_mm = float(double(print.print_layers().at(i).level() - (i ? print.print_layers().at(i-1).level() : 0)) * SCALING_FACTOR);
            l.nonzero_pixel_count = nonzero[i];
            image_offset += l.image_size;
            anycubicsla_write_layer(out, l);
            // add the rle encoded layer image into the buffer
            const char* img_start = reinterpret_cast<const char*>(rst.data());
            const char* img_end = img_start + rst.size();
            std::copy(img_start, img_end, std::back_inserter(layer_images));
            i++;
        }
        const char* img_buffer = reinterpret_cast<const char*>(layer_images.data());
        out.write(img_buffer, layer_images.size());
        out.flush();
        if (!out) throw RuntimeError("Cannot finish writing Anycubic data");
        out.close();
    } catch(std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << e.what();
        // Rethrow the exception
        throw;
    }

}

} // namespace Slic3r
