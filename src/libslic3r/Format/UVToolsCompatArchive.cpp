#include "libslic3r/SLA/PrinterRaster.hpp"
#include "libslic3r/SLA/ColorMask.hpp"
///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "UVToolsCompatArchive.hpp"
#include "CrealityCXDLPv1Archive.hpp"
#include "CrealityContourArea.hpp"
#include "CrealityChecksum.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <filesystem>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/Zipper.hpp"

namespace Slic3r {

namespace {

template <class T>
T clamp_cast(float value, T lo, T hi)
{
    const float clamped = std::max(float(lo), std::min(float(hi), value));
    return static_cast<T>(clamped);
}

std::string layer_name(const std::string &project, uint32_t index)
{
    std::ostringstream ss;
    ss << project << std::setw(5) << std::setfill('0') << index << ".png";
    return ss.str();
}

// Helper to get actual display dimensions and resolution respecting orientation
struct DisplayDimensions {
    double display_width_mm;
    double display_height_mm;
    uint32_t resolution_x;
    uint32_t resolution_y;
};

DisplayDimensions get_display_dimensions(const SLAPrinterConfig &cfg)
{
    DisplayDimensions dims{};
    double w = cfg.display_width.getFloat();
    double h = cfg.display_height.getFloat();
    uint32_t pw = uint32_t(cfg.display_pixels_x.getInt());
    uint32_t ph = uint32_t(cfg.display_pixels_y.getInt());

    auto ro = cfg.display_orientation.getInt();
    bool is_portrait = ro == sla::RasterBase::roPortrait;

    if (is_portrait) {
        std::swap(w, h);
        std::swap(pw, ph);
    }

    dims.display_width_mm = w;
    dims.display_height_mm = h;
    dims.resolution_x = pw;
    dims.resolution_y = ph;
    return dims;
}

// No need for generic helper - use printer_config() directly


std::unique_ptr<sla::RasterBase> make_common_raster(const SLAPrinterConfig &cfg)
{
    return sla::create_printer_raster(cfg);
}

void write_u16_be(std::ofstream &out, uint16_t v)
{
    out.put(char((v >> 8) & 0xFF));
    out.put(char(v & 0xFF));
}

void write_u32_be(std::ofstream &out, uint32_t v)
{
    out.put(char((v >> 24) & 0xFF));
    out.put(char((v >> 16) & 0xFF));
    out.put(char((v >> 8) & 0xFF));
    out.put(char(v & 0xFF));
}

void write_u16_le(std::ofstream &out, uint16_t v)
{
    out.put(char(v & 0xFF));
    out.put(char((v >> 8) & 0xFF));
}

void write_u32_le(std::ofstream &out, uint32_t v)
{
    out.put(char(v & 0xFF));
    out.put(char((v >> 8) & 0xFF));
    out.put(char((v >> 16) & 0xFF));
    out.put(char((v >> 24) & 0xFF));
}

void write_f32_le(std::ofstream &out, float v)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    write_u32_le(out, bits);
}

void write_f32_be(std::ofstream &out, float v)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    write_u32_be(out, bits);
}

void write_be_len_nt_string(std::ofstream &out, const std::string &s)
{
    const std::string stored = s + '\0';
    write_u32_be(out, uint32_t(stored.size()));
    out.write(stored.data(), std::streamsize(stored.size()));
}

void write_utf16be_string(std::ofstream &out, const std::string &s)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(s.size() * 2);
    for (unsigned char c : s) {
        bytes.push_back(0);
        bytes.push_back(c);
    }

    write_u32_be(out, uint32_t(bytes.size()));
    if (!bytes.empty())
        out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
}

float cfg_float_or(const DynamicPrintConfig &cfg, const std::string &key, float fallback)
{
    return cfg.has(key) ? cfg.opt_float(key) : fallback;
}

int cfg_int_or(const DynamicPrintConfig &cfg, const std::string &key, int fallback)
{
    return cfg.has(key) ? cfg.opt_int(key) : fallback;
}

struct FDGRasterEncoder {
    sla::EncodedRaster operator()(const void *ptr, size_t w, size_t h, size_t num_components) const
    {
        const uint8_t *src = reinterpret_cast<const uint8_t *>(ptr);
        std::vector<uint8_t> out;
        out.reserve((w * h * num_components) / 2 + 4096);

        auto add_run = [&out](uint8_t color, uint32_t stride) {
            out.push_back(uint8_t((color & 0x7F) | 0x80));
            if (stride <= 1)
                return;

            uint32_t remaining = stride - 1;
            while (remaining > 0) {
                const uint8_t rep = uint8_t(std::min<uint32_t>(remaining, 0x7D));
                out.push_back(rep);
                remaining -= rep;
            }
        };

        const int halfWidth = int(w) / 2;

        for (size_t y = 0; y < h; ++y) {
            uint8_t  color  = 0xFF;
            uint32_t stride = 0;
            const size_t row_begin = y * w * num_components;

            for (size_t x = 0; x < w; ++x) {
                const uint8_t p = src[row_begin + x * num_components];
                uint8_t       g = uint8_t((p >> 1) & 0x7F);
                if (g > 0x7C)
                    g = 0x7C;

                // Match UVtools: emit run at halfwidth boundary
                if (int(x) == halfWidth && color != 0xFF) {
                    add_run(color, stride);
                    color = g;
                    stride = 1;
                } else if (color == 0xFF) {
                    color  = g;
                    stride = 1;
                } else if (g != color) {
                    add_run(color, stride);
                    color  = g;
                    stride = 1;
                } else {
                    ++stride;
                }
            }

            // End of row: emit final run and reset
            if (color != 0xFF)
                add_run(color, stride);
        }

        return sla::EncodedRaster(std::move(out), "fdgrle");
    }
};


} // namespace

// RGB565 encoder for GR1/MDLP formats (big-endian RGB565 packed pixels)
struct GR1RasterEncoder {
    sla::EncodedRaster operator()(const void *ptr, size_t w, size_t h, size_t num_components) const
    {
        const uint8_t *src = reinterpret_cast<const uint8_t *>(ptr);
        std::vector<uint8_t> out;
        out.reserve(w * h * 2);

        for (size_t i = 0; i < w * h * num_components; i += num_components) {
            uint8_t gray = src[i];
            // Convert 8-bit grayscale to RGB565 big-endian
            // R:5bits, G:6bits, B:5bits
            uint8_t r = (gray >> 3) & 0x1F;
            uint8_t g = (gray >> 2) & 0x3F;
            uint8_t b = (gray >> 3) & 0x1F;
            
            uint16_t rgb565 = (r << 11) | (g << 5) | b;
            out.push_back((rgb565 >> 8) & 0xFF);
            out.push_back(rgb565 & 0xFF);
        }

        return sla::EncodedRaster(std::move(out), "gr1");
    }
};

// Chitubox RLE encoder (8-bit RLE with 125-run limit)
struct ChituboxRasterEncoder {
    sla::EncodedRaster operator()(const void *ptr, size_t w, size_t h, size_t num_components) const
    {
        const uint8_t *src = reinterpret_cast<const uint8_t *>(ptr);
        std::vector<uint8_t> out;
        out.reserve((w * h * num_components) / 2 + 4096);

        auto add_run = [&out](uint8_t color, uint32_t stride) {
            // Chitubox 8-bit RLE: (count << 7) | (value & 0x7F) for each run
            // Max run length is 125 (0x7D)
            while (stride > 0) {
                uint8_t rep = uint8_t(std::min<uint32_t>(stride, 0x7D));
                out.push_back(uint8_t((rep << 7) | (color & 0x7F)));
                stride -= rep;
            }
        };

        for (size_t y = 0; y < h; ++y) {
            uint8_t  color  = 0xFF;
            uint32_t stride = 0;
            const size_t row_begin = y * w * num_components;

            for (size_t x = 0; x < w; ++x) {
                const uint8_t p = src[row_begin + x * num_components];
                uint8_t       g = p;  // Use full byte, don't compress

                if (color == 0xFF) {
                    color  = g;
                    stride = 1;
                } else if (g != color) {
                    add_run(color, stride);
                    color  = g;
                    stride = 1;
                } else {
                    ++stride;
                }
            }

            // Emit final run of row
            if (color != 0xFF) {
                add_run(color, stride);
            }
        }

        return sla::EncodedRaster(std::move(out), "cbddlp");
    }
};

// UVToolsRLEBinaryBase implementation
std::unique_ptr<sla::RasterBase> UVToolsRLEBinaryBase::create_raster() const
{
    return sla::create_printer_raster(m_cfg);
}

// FDG encoder implementation
sla::RasterEncoder UVToolsFDGArchive::get_encoder() const
{
    return FDGRasterEncoder{};
}

// CXDLP encoder implementation
sla::RasterEncoder UVToolsCXDLPArchive::get_encoder() const
{
    const auto dimensions=get_display_dimensions(m_cfg);
    // Match UVtools' rounded pixel dimensions and float PixelArea.
    const float pitch_x=float(std::round(float(dimensions.display_width_mm)/dimensions.resolution_x*10000.f)/10000.f);
    const float pitch_y=float(std::round(float(dimensions.display_height_mm)/dimensions.resolution_y*10000.f)/10000.f);
    const float pixel_area=pitch_x*pitch_y;
    return [pixel_area](const void *data,size_t width,size_t height,size_t components) {
        const auto raster=CrealityCXDLPv1Archive::encode_mask(data,width,height,components);
        const double area=sla::creality_largest_contour_area(static_cast<const uint8_t *>(data),width,height)*pixel_area*1000.;
        if(!std::isfinite(area) || area>UINT32_MAX) throw ExportError("CXDLP contour area exceeds 32 bits");
        const uint32_t value=uint32_t(area);
        const auto *bytes=static_cast<const uint8_t *>(raster.data());
        std::vector<uint8_t> output(bytes,bytes+raster.size());
        for(unsigned i=0;i<4;++i) output[i]=uint8_t(value>>(24-8*i));
        output.insert(output.end(),{13,10});
        return sla::EncodedRaster(std::move(output),"cxdlp");
    };
}

// CXDLP raster creation - PNG-based line format
std::unique_ptr<sla::RasterBase> UVToolsCXDLPArchive::create_raster() const
{
    return sla::create_printer_raster(m_cfg);
}

void UVToolsCWSArchive::export_print(const std::string fname, const SLAPrint &print,
                                   const ThumbnailsList &, const std::string &)
{
    // Native CWS: field names and layer commands follow UVtools' CWSFile
    // and GCodeBuilder. Commands live only inside this SLA container.
    const size_t count = m_layers.size();
    if (!count || count != print.print_layers().size()) throw ExportError("CWS layer count mismatch");
    const auto dims = get_display_dimensions(m_cfg);
    if (dims.resolution_x > UINT16_MAX || dims.resolution_y > UINT16_MAX ||
        dims.display_width_mm <= 0 || dims.display_height_mm <= 0)
        throw ExportError("CWS requires positive display dimensions and 16-bit resolution");
    // A fixed non-numeric prefix avoids ambiguous layer suffixes, XML escaping,
    // and archive path traversal through user-provided project names.
    const std::string project = "sla_job";
    const bool xml_cws = m_cfg.sla_archive_format.value == "cwsxml";
    const bool rgb_cws = m_cfg.sla_archive_format.value == "cwsrgb";
    const bool novamaker = m_cfg.sla_archive_format.value == "cwsnovamaker" || rgb_cws;
    if(rgb_cws && dims.resolution_x%3)throw ExportError("RGB.CWS requires an LCD raster width divisible by three after orientation");
    const auto &material = print.material_config();
    const auto bottom = sla::bottom_layers(print, count);
    auto parameter = [&](const char *key, bool is_bottom = false) {
        return double(sla::motion_parameter(print, key, is_bottom));
    };
    auto milliseconds = [](double seconds) {
        if (!std::isfinite(seconds) || seconds < 0 || seconds * 1000 > UINT32_MAX)
            throw ExportError("CWS time is outside its millisecond range");
        return uint32_t(std::lround(seconds * 1000));
    };
    const double thickness = print.default_object_config().layer_height.value;
    std::ostringstream manifest, profile, commands;
    manifest.imbue(std::locale::classic()); profile.imbue(std::locale::classic()); commands.imbue(std::locale::classic());
    profile << std::setprecision(9); commands << std::setprecision(9);
    manifest << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<manifest FileVersion=\"1\"><Slices>\n";
    for (size_t i = 0; i < count; ++i)
        manifest << "<Slice><name>" << layer_name(project, uint32_t(i)) << "</name></Slice>\n";
    manifest << "</Slices><SliceProfile><name>" << project << ".slicing</name></SliceProfile>"
             << "<GCode><name>" << project << ".gcode</name></GCode></manifest>\n";
    profile << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<SliceBuildConfig FileVersion=\"2\">\n";
    auto xml = [&](const char *name, auto value) { profile << '<' << name << '>' << value << "</" << name << ">\n"; };
    xml("XResolution", dims.resolution_x); xml("YResolution", dims.resolution_y);
    xml("DotsPermmX", dims.resolution_x / dims.display_width_mm);
    xml("DotsPermmY", dims.resolution_y / dims.display_height_mm);
    xml("BlankTime", milliseconds(parameter("wait_before_cure")));
    xml("TopTime", milliseconds(parameter("wait_after_lift")));
    xml("LiftDistance", parameter("lift_height")); xml("LiftFeedRate", parameter("lift_speed"));
    xml("BottomLiftFeedRate", parameter("lift_speed", true)); xml("LiftRetractRate", parameter("retract_speed"));
    xml("SettleTime", milliseconds(parameter("wait_before_cure"))); xml("Direction", "Bottom_Up");
    profile << "<InkConfig><Name>Resin</Name><SliceHeight>" << thickness << "</SliceHeight><LayerTime>"
            << milliseconds(material.exposure_time.value) << "</LayerTime><FirstLayerTime>"
            << milliseconds(material.initial_exposure_time.value) << "</FirstLayerTime><NumberofBottomLayers>"
            << bottom << "</NumberofBottomLayers></InkConfig><SelectedInk>Resin</SelectedInk>\n</SliceBuildConfig>\n";
    commands << "; SLA-Slicer CWS exposure sequence\n";
    auto header = [&](const char *name, auto value) { commands << ";(" << name << " = " << value << ")\n"; };
    header("Pix per mm X", dims.resolution_x / dims.display_width_mm);
    header("Pix per mm Y", dims.resolution_y / dims.display_height_mm);
    header("X Resolution", dims.resolution_x); header("Y Resolution", dims.resolution_y);
    header("Layer Thickness", thickness); header("Layer Time", milliseconds(material.exposure_time.value));
    header("Bottom Layers Time", milliseconds(material.initial_exposure_time.value));
    header("Number of Bottom Layers", bottom); header("Blanking Layer Time", milliseconds(parameter("wait_before_cure")));
    header("Build Direction", "Bottom_Up"); header("Lift Distance", parameter("lift_height"));
    header("Z Lift Feed Rate", parameter("lift_speed")); header("Z Bottom Lift Feed Rate", parameter("lift_speed", true));
    header("Z Lift Retract Rate", parameter("retract_speed"));
    header("Platform X Size", dims.display_width_mm); header("Platform Y Size", dims.display_height_mm);
    header("Platform Z Size", m_cfg.max_print_height.value);
    header("Layer Light PWM", parameter("light_pwm")); header("Bottom Layer Light PWM", parameter("light_pwm", true));
    header("Flip X", "False"); header("Flip Y", "False"); // Masks already have the printer transform.
    commands << ";Number of Slices = " << count << "\nG21\nG91\nM17\nM106 S0\n;<Slice> Blank\nG28 Z\nM400\n";
    double previous_z = 0;
    for (size_t i = 0; i < count; ++i) {
        const bool is_bottom = i < bottom;
        auto param = [&](const char *key) { return parameter(key, is_bottom); };
        const double z = sla::layer_z(print, i), delta = z - previous_z;
        if (!std::isfinite(z) || delta <= 0 || z > m_cfg.max_print_height.value)
            throw ExportError("CWS layers require increasing Z within the printer height");
        const double lift = param("lift_height"), lift2 = param("lift_height2"), retract2 = param("retract_height2");
        if (lift < 0 || lift2 < 0 || retract2 < 0 || retract2 > lift + lift2)
            throw ExportError("CWS retract distance exceeds lift distance");
        auto move = [&](double distance, double speed) {
            if (!distance) return 0.;
            if (!std::isfinite(distance) || !std::isfinite(speed) || speed <= 0)
                throw ExportError("CWS movement requires finite distance and positive speed");
            commands << "G1 Z" << distance << " F" << speed << '\n';
            return std::abs(distance) * 60 / speed;
        };
        // XML.CWS uses <Takes> for motion duration. NovaMaker uses a leading
        // zero on <Delay>, which UVtools recognizes as synchronization.
        auto sync = [&](double seconds) {
            commands << (xml_cws ? ";<Takes> " : ";<Delay> 0")
                     << milliseconds(seconds) << "\nM400\n";
        };
        commands << ";LAYER_START:" << i << "\n;PositionZ:" << z << "mm\n";
        double lift_seconds = 0;
        if (lift > 0) {
            lift_seconds += move(delta + lift, param("lift_speed"));
            lift_seconds += move(lift2, param("lift_speed2"));
        } else lift_seconds = move(delta + lift2, param(lift2 > 0 ? "lift_speed2" : "lift_speed"));
        sync(lift_seconds);
        if (param("wait_after_lift") > 0)
            commands << ";<Delay> " << milliseconds(param("wait_after_lift")) << "\n";
        double retract_seconds = move(-(lift + lift2 - retract2), param("retract_speed"));
        retract_seconds += move(-retract2, param("retract_speed2"));
        sync(retract_seconds);
        commands << ";<Delay> " << milliseconds(param("wait_before_cure")) << "\n;<Slice> " << i << "\n";
        if (sla::layer_exposure(print, i) > 0 && param("light_pwm") > 0)
            commands << "M106 S" << int(param("light_pwm")) << "\n;<Delay> "
                     << milliseconds(sla::layer_exposure(print, i)) << "\nM106 S0\n";
        commands << ";<Slice> Blank\n";
        if (param("wait_after_cure") > 0)
            commands << ";<Delay> " << milliseconds(param("wait_after_cure")) << "\n";
        commands << ";LAYER_END:" << i << "\n";
        previous_z = z;
    }
    commands << "M106 S0\n;<Slice> Blank\nM400\nM18\n;<Completed>\n";
    Zipper zip(fname, Zipper::FAST_COMPRESSION);
    if (novamaker) {
        if (bottom > UINT16_MAX) throw ExportError("NovaMaker bottom layer count exceeds its 16-bit field");
        std::ostringstream settings;
        settings.imbue(std::locale::classic()); settings << std::setprecision(9);
        auto field = [&](const char *key, auto value) { settings << key << " = " << value << '\n'; };
        settings << "# SLA-Slicer NovaMaker CWS\n# conf version 1.0\n";
        field("xppm", dims.resolution_x/dims.display_width_mm);
        field("yppm", dims.resolution_y/dims.display_height_mm);
        field("xres", dims.resolution_x); field("yres", dims.resolution_y);
        field("thickness", thickness); field("layers_num", count); field("head_layers_num", bottom);
        field("layers_expo_ms", milliseconds(material.exposure_time.value));
        field("head_layers_expo_ms", milliseconds(material.initial_exposure_time.value));
        field("wait_before_expo_ms", milliseconds(parameter("wait_before_cure")));
        field("lift_distance", parameter("lift_height")); field("lift_up_speed", parameter("lift_speed"));
        field("lift_down_speed", parameter("retract_speed"));
        field("lift_when_finished", 0); // The actual footer does not raise Z.
        zip.add_entry("slice.conf"); zip << settings.str();
    } else {
        zip.add_entry("manifest.xml"); zip << manifest.str();
        zip.add_entry(project + ".slicing"); zip << profile.str();
    }
    zip.add_entry(project + ".gcode"); zip << commands.str();
    sla::EncodedRaster converted;
    const sla::EncodedRaster *previous=nullptr;
    for (size_t i = 0; i < count; ++i) {
        if (xml_cws || rgb_cws) {
            // Flat model sections often share consecutive masks. Reuse the
            // conversion only after an exact compressed-byte comparison;
            // per-layer Z/exposure/motion commands remain independent.
            if(!previous || previous->size()!=m_layers[i].size() ||
                std::memcmp(previous->data(),m_layers[i].data(),m_layers[i].size())!=0) {
                png::ImageGreyscale mask;
                if (!png::decode_png(png::ReadBuf{m_layers[i].data(),m_layers[i].size()},mask) ||
                    mask.cols!=dims.resolution_x || mask.rows!=dims.resolution_y)
                    throw ExportError("CWS mask does not match the printer resolution");
                converted=rgb_cws?sla::encode_bgr_packed_mask(mask):sla::encode_rgba_mask(mask);
            }
            previous=&m_layers[i];
            zip.add_entry(layer_name(project,uint32_t(i)),converted.data(),converted.size());
        } else zip.add_entry(layer_name(project,uint32_t(i)),m_layers[i].data(),m_layers[i].size());
    }
    zip.finalize();
}

void UVToolsFDGArchive::export_print(const std::string     fname,
                                     const SLAPrint       &print,
                                     const ThumbnailsList &,
                                     const std::string    &)
{
    const auto  &cfg          = print.full_print_config();
    const float layer_h       = cfg_float_or(cfg, "layer_height", 0.05f);
    const float initial_h     = cfg_float_or(cfg, "initial_layer_height", layer_h);
    const float exp           = cfg_float_or(cfg, "exposure_time", 2.5f);
    const float initial_exp   = cfg_float_or(cfg, "initial_exposure_time", 20.0f);
    const auto   bottom_count = uint32_t(std::max(0, cfg_int_or(cfg, "initial_layer_count", 6)));

    const uint32_t layer_count = uint32_t(m_layers.size());
    if (layer_count == 0)
        throw RuntimeError("No rasterized layers available for FDG export");

    // Get orientation-aware display dimensions and resolution
    const auto dims = get_display_dimensions(m_cfg);

    // FDG Header: 124 bytes (32 fields with mixed uint/float/ushort)
    const uint32_t header_size = 124u;
    // LayerDef: 36 bytes (same as CTB, 9 fields)
    const uint32_t layer_def_size = 36u;
    const uint32_t layer_defs_offset = header_size;
    const uint32_t layer_data_offset = layer_defs_offset + layer_def_size * layer_count;

    std::vector<uint32_t> data_offsets(layer_count);
    uint32_t current_data = layer_data_offset;
    for (uint32_t i = 0; i < layer_count; ++i) {
        data_offsets[i] = current_data;
        current_data += uint32_t(m_layers[i].size());
    }

    // Calculate total model height
    float total_height = initial_h * float(std::min(layer_count, bottom_count)) + 
                        layer_h * float(std::max(0u, layer_count - bottom_count));

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for FDG export");

    // Write FDG header (124 bytes) following exact UVtools FDGFile.cs field order:
    write_u32_le(out, 0xBD3C7AC8u);                      // 0: Magic
    write_u32_le(out, 2u);                               // 1: Version
    write_u32_le(out, layer_count);                      // 2: LayerCount
    write_u32_le(out, bottom_count);                     // 3: BottomLayersCount
    write_u32_le(out, 0u);                               // 4: ProjectorType
    write_u32_le(out, bottom_count);                     // 5: BottomLayersCount2 (duplicate)
    write_u32_le(out, dims.resolution_x);               // 6: ResolutionX
    write_u32_le(out, dims.resolution_y);               // 7: ResolutionY
    write_f32_le(out, layer_h);                          // 8: LayerHeightMillimeter
    write_f32_le(out, exp);                              // 9: LayerExposureSeconds
    write_f32_le(out, initial_exp);                      // 10: BottomExposureSeconds
    write_u32_le(out, 0u);                               // 11: PreviewLargeOffsetAddress
    write_u32_le(out, 0u);                               // 12: PreviewSmallOffsetAddress
    write_u32_le(out, layer_defs_offset);                // 13: LayersDefinitionOffsetAddress
    write_u32_le(out, 0u);                               // 14: PrintTime
    write_u32_le(out, 1u);                               // 15: AntiAliasLevel
    write_u16_le(out, 255u);                             // 16: LightPWM (ushort!)
    write_u16_le(out, 255u);                             // 17: BottomLightPWM (ushort!)
    write_u32_le(out, 0u);                               // 18: Padding1
    write_u32_le(out, 0u);                               // 19: Padding2
    write_f32_le(out, total_height);                     // 20: OverallHeightMillimeter
    write_f32_le(out, 68.04f);                           // 21: BedSizeX
    write_f32_le(out, 120.96f);                          // 22: BedSizeY
    write_f32_le(out, 150.0f);                           // 23: BedSizeZ
    write_u32_le(out, 0u);                               // 24: EncryptionKey
    write_u32_le(out, 1u);                               // 25: AntiAliasLevelInfo
    write_u32_le(out, 0x4Cu);                            // 26: EncryptionMode
    write_f32_le(out, 0.0f);                             // 27: VolumeMl
    write_f32_le(out, 0.0f);                             // 28: WeightG
    write_f32_le(out, 0.0f);                             // 29: CostDollars
    write_u32_le(out, 0u);                               // 30: MachineNameAddress
    write_u32_le(out, 0u);                               // 31: MachineNameSize

    // Write layer definitions (36 bytes per layer = 9 fields)
    for (uint32_t i = 0; i < layer_count; ++i) {
        const float z = (i < bottom_count) ? initial_h * float(i + 1) : 
                        (initial_h * float(bottom_count) + layer_h * float(i - bottom_count + 1));
        const float lexp = (i < bottom_count) ? initial_exp : exp;

        write_f32_le(out, z);                            // 0: PositionZ
        write_f32_le(out, lexp);                         // 1: ExposureTime
        write_f32_le(out, 0.0f);                         // 2: LightOffSeconds
        write_u32_le(out, data_offsets[i]);              // 3: DataAddress
        write_u32_le(out, uint32_t(m_layers[i].size())); // 4: DataSize
        write_u32_le(out, 0u);                           // 5: PageNumber
        write_u32_le(out, 36u);                          // 6: TableSize
        write_u32_le(out, 0u);                           // 7: Unknown3
        write_u32_le(out, 0u);                           // 8: Unknown4
    }

    // Write layer RLE data
    for (uint32_t i = 0; i < layer_count; ++i)
        out.write(reinterpret_cast<const char *>(m_layers[i].data()), std::streamsize(m_layers[i].size()));

    out.close();
}

void UVToolsCXDLPArchive::export_print(const std::string fname, const SLAPrint &print,
                                       const ThumbnailsList &thumbnails, const std::string &)
{
    const uint16_t version=m_cfg.sla_archive_format.value=="cxdlpv2"?2:3;
    const auto dimensions=get_display_dimensions(m_cfg);
    const auto &material=print.material_config();
    const size_t count=m_layers.size();
    if(!count || count>UINT16_MAX || count!=print.print_layers().size() ||
       !dimensions.resolution_x || !dimensions.resolution_y || dimensions.resolution_x>16384 || dimensions.resolution_y>8192)
        throw ExportError("CXDLP resolution or layer count exceeds the schema limits");
    if(std::abs(dimensions.display_width_mm*100-std::round(dimensions.display_width_mm*100))>1e-6 ||
       std::abs(dimensions.display_height_mm*1000-std::round(dimensions.display_height_mm*1000))>1e-6)
        throw ExportError("CXDLP display width requires 2 decimal places and height 3");
    sla::require_single_stage_motion(print,"CXDLP v2/v3",true);
    if(material.sla_transition_layers.value!=0) throw ExportError("CXDLP v2/v3 cannot store transition exposures");
    const double layer_height=print.default_object_config().layer_height.value;
    for(size_t i=0;i<count;++i)
        if(std::abs(double(print.print_layers()[i].level())*SCALING_FACTOR-(i+1)*layer_height)>1e-5)
            throw ExportError("CXDLP v2/v3 requires uniform layer height including the first layer");
    auto parameter=[&](const char *key,bool bottom=false){return double(sla::motion_parameter(print,key,bottom));};
    auto integer=[](double value,const char *name,bool positive=false) {
        if(!std::isfinite(value) || value<(positive?1.:0.) || value>UINT16_MAX || std::abs(value-std::round(value))>1e-6)
            throw ExportError(std::string("CXDLP v2/v3 requires a ")+(positive?"positive ":"")+"16-bit integer for "+name);
        return uint16_t(std::llround(value));
    };
    if(parameter("wait_before_cure")!=parameter("wait_before_cure",true))
        throw ExportError("CXDLP v2/v3 requires the same bottom and normal wait before exposure");
    const std::array<uint16_t,11> settings{
        integer(material.exposure_time.value*10.,"exposure in tenths of a second",true),
        integer(parameter("wait_before_cure"),"wait before exposure",true),
        integer(material.initial_exposure_time.value,"bottom exposure in seconds",true),uint16_t(sla::bottom_layers(print,count)),
        integer(parameter("lift_height",true),"bottom lift height",true),integer(parameter("lift_speed",true)/60.,"bottom lift speed in mm/s",true),
        integer(parameter("lift_height"),"lift height",true),integer(parameter("lift_speed")/60.,"lift speed in mm/s",true),
        integer(parameter("retract_speed")/60.,"retract speed in mm/s",true),
        integer(parameter("light_pwm",true),"bottom PWM"),integer(parameter("light_pwm"),"PWM")};
    auto be32=[](const void *data){const auto *p=static_cast<const uint8_t *>(data);return(uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];};
    for(const auto &layer:m_layers) {
        if(layer.size()<10 || layer.size()!=10+uint64_t(be32(static_cast<const uint8_t *>(layer.data())+4))*6)
            throw ExportError("Incomplete CXDLP raster table");
    }
    const auto &model=m_cfg.printer_model.value;
    if(model.size()>255 || model.find('\0')!=std::string::npos ||
       (model.compare(0,2,"CL")!=0 && model.compare(0,2,"CT")!=0))
        throw ExportError("CXDLP requires the actual Creality printer model starting with CL or CT in the printer profile");
    std::ofstream output(fname,std::ios::binary|std::ios::trunc);
    if(!output) throw FileIOError("Cannot write CXDLP output");
    write_u32_be(output,9);output.write("CXSW3DV2\0",9);write_u16_be(output,version);
    write_be_len_nt_string(output,model);write_u16_be(output,uint16_t(count));
    write_u16_be(output,uint16_t(dimensions.resolution_x));write_u16_be(output,uint16_t(dimensions.resolution_y));
    const std::array<char,64> padding{};output.write(padding.data(),padding.size());
    const auto valid=std::find_if(thumbnails.begin(),thumbnails.end(),[](const ThumbnailData &image){return image.is_valid() && image.width<=4096 && image.height<=4096;});
    for(const unsigned size:{116,290,290}) {
        const ThumbnailData *source=valid==thumbnails.end()?nullptr:&*valid;
        size_t width=0,height=0,left=0,top=0;
        if(source) {
            const double scale=std::min(double(size)/source->width,double(size)/source->height);
            width=std::max<size_t>(1,std::lround(source->width*scale));height=std::max<size_t>(1,std::lround(source->height*scale));
            left=(size-width)/2;top=(size-height)/2;
        }
        for(size_t y=0;y<size;++y) for(size_t x=0;x<size;++x) {
            uint16_t rgb=0;
            if(source && x>=left && x<left+width && y>=top && y<top+height) {
                const size_t sx=std::min<size_t>(source->width-1,(x-left)*source->width/width);
                const size_t sy=source->height-1-std::min<size_t>(source->height-1,(y-top)*source->height/height);
                const auto *rgba=source->pixels.data()+4*(sy*source->width+sx);
                const unsigned r=(unsigned(rgba[0])*rgba[3]+127)/255,g=(unsigned(rgba[1])*rgba[3]+127)/255,b=(unsigned(rgba[2])*rgba[3]+127)/255;
                rgb=uint16_t(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
            }
            write_u16_be(output,rgb);
        }
        output.write("\r\n",2);
    }
    auto numeric=[&](double value){std::ostringstream text;text.imbue(std::locale::classic());text<<std::setprecision(12)<<value;write_utf16be_string(output,text.str());};
    numeric(dimensions.display_width_mm);numeric(dimensions.display_height_mm);numeric(layer_height);
    for(const auto value:settings) write_u16_be(output,value);
    // Complete the area table BEFORE calculating the checksum. Append-mode
    // seekp cannot update it and used to leave a corrupt table after the CRC.
    for(const auto &layer:m_layers) write_u32_be(output,be32(layer.data()));
    output.write("\r\n",2);
    if(version>=3) {
    write_be_len_nt_string(output,"SLA-Slicer");write_be_len_nt_string(output,"");
    output.put(0);write_u32_le(output,600);write_u32_le(output,300000);
    output.put(0);write_u16_le(output,0);output.put(0);write_u16_le(output,1000);
    output.put(1);output.put(1);output.put(char(255));output.put(0);output.put(2);output.write("\r\n",2);
    }
    for(const auto &layer:m_layers) output.write(static_cast<const char *>(layer.data()),layer.size());
    write_u32_be(output,9);output.write("CXSW3DV2\0",9);
    output.close();if(!output) throw FileIOError("Cannot finish CXDLP output before checksum");
    std::ifstream input(fname,std::ios::binary);
    if(!input) throw FileIOError("Cannot read CXDLP output for checksum");
    const auto checksum=sla::creality_checksum(input,version);
    input.close();output.open(fname,std::ios::binary|std::ios::app);write_u32_be(output,checksum);
    output.close();if(!output) throw FileIOError("Cannot append CXDLP checksum");
}

// ============================================================================
// ZIP-BASED FORMAT IMPLEMENTATIONS
// ============================================================================

std::unique_ptr<sla::RasterBase> UVToolsZIPArchiveBase::create_raster() const
{
    return make_common_raster(m_cfg);
}

sla::RasterEncoder UVToolsZIPArchiveBase::get_encoder() const
{
    return sla::PNGRasterEncoder{};
}

void UVToolsChituboxZIPArchive::export_print(const std::string fname, const SLAPrint &print,
                                           const ThumbnailsList &thumbnails, const std::string &)
{
    const auto screen=get_display_dimensions(m_cfg);
    const size_t count=m_layers.size(), bottom=sla::bottom_layers(print,count);
    if(!count || count!=print.print_layers().size() || bottom>UINT16_MAX)
        throw ExportError("Chitubox ZIP layer count is outside the schema limits");
    const auto &material=print.material_config();
    auto parameter=[&](const char *key,bool base=false){return double(sla::motion_parameter(print,key,base));};
    auto milliseconds=[](double seconds){
        if(!std::isfinite(seconds) || seconds<0 || seconds*1000>UINT32_MAX)
            throw ExportError("Chitubox ZIP delay exceeds the millisecond range");
        return uint32_t(std::llround(seconds*1000));
    };
    std::ostringstream sequence; sequence.imbue(std::locale::classic()); sequence<<std::setprecision(12);
    auto header=[&](const char *name,const auto &value){sequence<<';'<<name<<':'<<value<<'\n';};
    const auto &stats=print.print_statistics();
    const double volume=(stats.objects_used_material+stats.support_used_material)/1000.;
    header("fileName","sla_job"); header("machineType","SLA-Slicer");
    header("estimatedPrintTime",sla::lcd_print_time(print)); header("volume",volume);
    header("weight",volume*material.material_density.value);
    header("price",material.bottle_volume.value>0?volume*material.bottle_cost.value/material.bottle_volume.value:0.);
    header("layerHeight",print.default_object_config().layer_height.value);
    header("resolutionX",screen.resolution_x); header("resolutionY",screen.resolution_y);
    header("machineX",screen.display_width_mm); header("machineY",screen.display_height_mm);
    header("machineZ",m_cfg.max_print_height.value); header("projectType","Normal"); header("mirror",0);
    header("normalExposureTime",material.exposure_time.value);
    header("bottomLayExposureTime",material.initial_exposure_time.value); header("bottomLayerExposureTime",material.initial_exposure_time.value);
    header("bottomLayCount",bottom); header("bottomLayerCount",bottom); header("totalLayer",count);
    header("normalDropSpeed",parameter("retract_speed"));
    header("normalLayerLiftSpeed",parameter("lift_speed")); header("normalLayerLiftHeight",parameter("lift_height"));
    header("bottomLayerLiftSpeed",parameter("lift_speed",true)); header("bottomLayerLiftHeight",parameter("lift_height",true));
    header("bottomLightOffTime",parameter("wait_before_cure",true)); header("lightOffTime",parameter("wait_before_cure"));
    header("bottomPWMLight",int(parameter("light_pwm",true))); header("PWMLight",int(parameter("light_pwm")));
    header("antiAliasLevel",1);
    sequence<<"G21\nG90\nM106 S0\nM17\nG28 Z0\n";
    double previous_z=0.;
    for(size_t i=0;i<count;++i) {
        const double z=double(print.print_layers()[i].level())*SCALING_FACTOR;
        if(!std::isfinite(z) || z<=previous_z || z>m_cfg.max_print_height.value)
            throw ExportError("Chitubox ZIP requires increasing layer Z within the printer height");
        auto param=[&](const char *key){return parameter(key,i<bottom);};
        const double lift=param("lift_height"), lift2=param("lift_height2"), retract2=param("retract_height2");
        if(lift<0 || lift2<0 || retract2<0 || retract2>lift+lift2)
            throw ExportError("Chitubox ZIP retract distance exceeds lift distance");
        sequence<<";LAYER_START:"<<i<<"\n;PositionZ:"<<z<<"mm\nM6054 \""<<i+1<<".png\"\n";
        double position=previous_z;
        auto move=[&](double target,double speed){
            if(target==position) return;
            if(!std::isfinite(target) || !std::isfinite(speed) || speed<=0.)
                throw ExportError("Chitubox ZIP movement requires finite positions and positive speeds");
            sequence<<"G0 Z"<<target<<" F"<<speed<<'\n'; position=target;
        };
        if(lift>0) move(z+lift,param("lift_speed"));
        if(lift2>0) move(z+lift+lift2,param("lift_speed2"));
        if(lift+lift2==0.) move(z,param("lift_speed"));
        // Leading-zero delays are UVtools' movement synchronization markers.
        // G0 completes before the explicit exposure/wait intervals below.
        sequence<<"G4 P00\n";
        if(lift+lift2>0. && param("wait_after_lift")>0.) sequence<<"G4 P"<<milliseconds(param("wait_after_lift"))<<'\n';
        move(z+retract2,param("retract_speed"));
        move(z,param("retract_speed2"));
        sequence<<"G4 P00\nG4 P"<<milliseconds(param("wait_before_cure"))<<'\n';
        const double exposure=sla::layer_exposure(print,i);
        const int pwm=int(param("light_pwm"));
        if(exposure>0. && pwm>0)
            sequence<<"M106 S"<<pwm<<"\nG4 P"<<milliseconds(exposure)<<"\nM106 S0\n";
        if(param("wait_after_cure")>0.) sequence<<"G4 P"<<milliseconds(param("wait_after_cure"))<<'\n';
        sequence<<";LAYER_END:"<<i<<'\n'; previous_z=z;
    }
    sequence<<"M106 S0\nM18\n";
    Zipper zip(fname);
    zip.add_entry("run.gcode"); zip<<sequence.str();
    for(size_t i=0;i<count;++i) zip.add_entry(std::to_string(i+1)+".png",m_layers[i].data(),m_layers[i].size());
    size_t preview=0;
    for(const auto &thumbnail:thumbnails) {
        if(!thumbnail.is_valid() || thumbnail.width>2048 || thumbnail.height>2048) continue;
        std::vector<uint8_t> pixels(thumbnail.pixels.size());
        const size_t row_bytes=size_t(thumbnail.width)*4;
        for(size_t row=0;row<thumbnail.height;++row)
            std::copy_n(thumbnail.pixels.data()+(thumbnail.height-row-1)*row_bytes,row_bytes,pixels.data()+row*row_bytes);
        const auto encoded=sla::PNGRasterEncoder{}(pixels.data(),thumbnail.width,thumbnail.height,4);
        if(!encoded.size()) throw ExportError("Cannot encode Chitubox ZIP preview");
        zip.add_entry(preview?"preview_cropping.png":"preview.png",encoded.data(),encoded.size());
        if(++preview==2) break;
    }
    zip.finalize();
}

// GenericZIP - ZIP with manifest.uvtools
void UVToolsGenericZIPArchive::export_print(const std::string     fname,
                                            const SLAPrint       &print,
                                            const ThumbnailsList &thumbnails,
                                            const std::string    &projectname)
{
    // UVtools GenericZIP intentionally stores images and screen geometry.
    // Exposure and movement belong to the receiving printer, not this schema.
    const auto dimensions = get_display_dimensions(m_cfg);
    const double height = print.default_object_config().layer_height.value;
    if (m_layers.empty() || m_layers.size() != print.print_layers().size() ||
        dimensions.resolution_x > UINT16_MAX || dimensions.resolution_y > UINT16_MAX ||
        !std::isfinite(height) || height <= 0.)
        throw ExportError("Generic ZIP requires masks, positive layer height and 16-bit screen resolution");
    for (size_t i = 0; i < m_layers.size(); ++i)
        if (std::abs(double(print.print_layers()[i].level())*SCALING_FACTOR - double(i+1)*height) > 1e-5)
            throw ExportError("Generic ZIP cannot store a different first layer height or variable Z positions");
    std::ostringstream manifest;
    manifest.imbue(std::locale::classic()); manifest << std::setprecision(12);
    manifest << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Manifest>\n"
        << "<CreatedBy>SLA-Slicer</CreatedBy><UpdatedBy>SLA-Slicer</UpdatedBy>\n"
        << "<LayerHeight>" << height << "</LayerHeight>\n"
        << "<ResolutionX>" << dimensions.resolution_x << "</ResolutionX>\n"
        << "<ResolutionY>" << dimensions.resolution_y << "</ResolutionY>\n"
        << "<DisplayWidth>" << dimensions.display_width_mm << "</DisplayWidth>\n"
        << "<DisplayHeight>" << dimensions.display_height_mm << "</DisplayHeight>\n"
        << "<MachineZ>" << m_cfg.max_print_height.value << "</MachineZ>\n</Manifest>\n";
    Zipper zip(fname);
    for (size_t i = 0; i < m_layers.size(); ++i) {
        std::ostringstream name; name << std::setw(8) << std::setfill('0') << i+1 << ".png";
        zip.add_entry(name.str(), m_layers[i].data(), m_layers[i].size());
    }
    size_t preview = 0;
    for (const auto &thumbnail : thumbnails) {
        if (!thumbnail.is_valid() || thumbnail.width > 2048 || thumbnail.height > 2048) continue;
        std::vector<uint8_t> top_down(thumbnail.pixels.size());
        const size_t row_bytes = size_t(thumbnail.width) * 4;
        for (size_t row = 0; row < thumbnail.height; ++row)
            std::copy_n(thumbnail.pixels.data() + (thumbnail.height-row-1)*row_bytes, row_bytes, top_down.data()+row*row_bytes);
        const auto png = sla::PNGRasterEncoder{}(top_down.data(), thumbnail.width, thumbnail.height, 4);
        if (!png.size()) throw ExportError("Cannot encode Generic ZIP preview");
        zip.add_entry(preview == 0 ? "preview.png" : "preview_cropping.png", png.data(), png.size());
        if (++preview == 2) break;
    }
    zip.add_entry("manifest.uvtools"); zip << manifest.str(); zip.finalize();
}

// ZCode - ZIP with XML task/profile structure
void UVToolsZCodeArchive::export_print(const std::string     fname,
                                       const SLAPrint       &print,
                                       const ThumbnailsList &,
                                       const std::string    &projectname)
{
    throw RuntimeError("ZCode format not yet fully implemented");
}

// ZCodeX - ZCode variant with JSON metadata
void UVToolsZCodeXArchive::export_print(const std::string     fname,
                                        const SLAPrint       &print,
                                        const ThumbnailsList &,
                                        const std::string    &projectname)
{
    throw RuntimeError("ZCodeX format not yet fully implemented");
}

// OSLA (Open SLA) - ZIP with binary header + PNG layers
void UVToolsOSLAArchive::export_print(const std::string     fname,
                                      const SLAPrint       &print,
                                      const ThumbnailsList &,
                                      const std::string    &projectname)
{
    throw RuntimeError("OSLA format not yet fully implemented");
}

// JXS (GigaKnightSung) - ZIP with INI/JSON config
void UVToolsJXSArchive::export_print(const std::string     fname,
                                     const SLAPrint       &print,
                                     const ThumbnailsList &,
                                     const std::string    &projectname)
{
    throw RuntimeError("JXS format not yet fully implemented");
}

// NanoDLP - ZIP with multi-manifest JSON system
void UVToolsNanoDLPArchive::export_print(const std::string     fname,
                                         const SLAPrint       &print,
                                         const ThumbnailsList &,
                                         const std::string    &projectname)
{
    throw RuntimeError("NanoDLP format not yet fully implemented");
}

// UVJ - ZIP with config.json + /slice images
void UVToolsUVJArchive::export_print(const std::string     fname,
                                     const SLAPrint       &print,
                                     const ThumbnailsList &,
                                     const std::string    &projectname)
{
    throw RuntimeError("UVJ format not yet fully implemented");
}

// Voxeldance Additive ZIP, following UVtools VDARoot.
void UVToolsVDAArchive::export_print(const std::string     fname,
                                     const SLAPrint       &print,
                                     const ThumbnailsList &,
                                     const std::string    &projectname)
{
    const auto dimensions = get_display_dimensions(m_cfg);
    const double height = print.default_object_config().layer_height.value;
    if (m_layers.empty() || m_layers.size() != print.print_layers().size() ||
        dimensions.resolution_x > UINT16_MAX || dimensions.resolution_y > UINT16_MAX ||
        !std::isfinite(height) || height <= 0.)
        throw ExportError("Voxeldance Additive ZIP requires masks, positive layer height and 16-bit resolution");
    for (size_t i = 0; i < m_layers.size(); ++i)
        if (std::abs(double(print.print_layers()[i].level())*SCALING_FACTOR - double(i+1)*height) > 1e-5)
            throw ExportError("Voxeldance Additive ZIP requires uniform Z for UVtools compatibility");
    auto layer_filename = [](size_t index) {
        std::ostringstream name; name << std::setw(4) << std::setfill('0') << index+1 << ".png"; return name.str();
    };
    std::ostringstream manifest;
    manifest.imbue(std::locale::classic()); manifest << std::setprecision(12);
    manifest << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<root>\n"
        << "<FileInfo><Version><Major>1</Major><Minor>2</Minor></Version>"
        << "<Written><By ApplicationName=\"SLA-Slicer\" ApplicationVersion=\"1\"/></Written></FileInfo>\n"
        << "<Slices><Count>1</Count><thickness>" << height << "</thickness>"
        << "<startHeight>" << height << "</startHeight><endHeight>"
        << double(print.print_layers().back().level())*SCALING_FACTOR << "</endHeight><layersCount>" << m_layers.size() << "</layersCount></Slices>\n"
        << "<Machines><FileType>ZIP File</FileType><Resolution>" << dimensions.resolution_x << '*' << dimensions.resolution_y << "P</Resolution>"
        << "<PixelXSize>" << dimensions.display_width_mm/dimensions.resolution_x*1000. << "um</PixelXSize>"
        << "<PixelYSize>" << dimensions.display_height_mm/dimensions.resolution_y*1000. << "um</PixelYSize>"
        << "<Anti-Aliasing>1</Anti-Aliasing><XLength>" << dimensions.display_width_mm << "</XLength>"
        << "<YWidth>" << dimensions.display_height_mm << "</YWidth><ZHeight>" << m_cfg.max_print_height.value << "</ZHeight></Machines>\n<Layers>\n";
    for (size_t i = 0; i < m_layers.size(); ++i)
        manifest << "<VDALayer><Index>" << i << "</Index><zvalue>" << double(print.print_layers()[i].level())*SCALING_FACTOR
                 << "</zvalue><filename>" << layer_filename(i) << "</filename></VDALayer>\n";
    manifest << "</Layers></root>\n";
    Zipper zip(fname);
    for (size_t i = 0; i < m_layers.size(); ++i) zip.add_entry(layer_filename(i), m_layers[i].data(), m_layers[i].size());
    zip.add_entry("sla_job.xml"); zip << manifest.str(); zip.finalize();
}

// VDT - ZIP with manifest.json + 10 preview variants
void UVToolsVDTArchive::export_print(const std::string     fname,
                                     const SLAPrint       &print,
                                     const ThumbnailsList &,
                                     const std::string    &projectname)
{
    throw RuntimeError("VDT format not yet fully implemented");
}

// Klipper - ZIP with GCode + PNG layers
void UVToolsKlipperArchive::export_print(const std::string     fname,
                                         const SLAPrint       &print,
                                         const ThumbnailsList &,
                                         const std::string    &projectname)
{
    throw RuntimeError("Klipper format not yet fully implemented");
}

// FlashForgeSVGX - ZIP with SVG vector graphics per layer
void UVToolsFlashForgeSVGXArchive::export_print(const std::string     fname,
                                                const SLAPrint       &print,
                                                const ThumbnailsList &,
                                                const std::string    &projectname)
{
    throw RuntimeError("FlashForgeSVGX format not yet fully implemented");
}

// ============================================================================
// BINARY RLE FORMAT IMPLEMENTATIONS (continued)
// ============================================================================

// Chitubox - Binary RLE with preview previews
sla::RasterEncoder UVToolsChituboxArchive::get_encoder() const
{
    return ChituboxRasterEncoder{};
}

void UVToolsChituboxArchive::export_print(const std::string     fname,
                                          const SLAPrint       &print,
                                          const ThumbnailsList &,
                                          const std::string    &)
{
    sla::require_single_stage_motion(print, "CTB v2", true);
    const auto  &cfg          = print.full_print_config();
    const float layer_h       = cfg_float_or(cfg, "layer_height", 0.05f);
    const float exp           = cfg_float_or(cfg, "exposure_time", 2.5f);
    const float initial_exp   = cfg_float_or(cfg, "initial_exposure_time", 20.0f);
    const auto   bottom_count = uint32_t(sla::bottom_layers(print, m_layers.size()));

    const uint32_t layer_count = uint32_t(m_layers.size());
    if (layer_count == 0)
        throw RuntimeError("No rasterized layers available for Chitubox export");

    // Get orientation-aware display dimensions and resolution
    const auto dims = get_display_dimensions(m_cfg);

    // Get printer bed dimensions (full dimensions, not just usable display area)
    double bed_size_x = dims.display_width_mm;
    double bed_size_y = dims.display_height_mm;
    double bed_size_z = m_cfg.max_print_height.value;

    // CTB Header: 112 bytes (fields 0-28: 24 uint/float + 2 ushort + 4 uint)
    const uint32_t header_size = 112u;
    // LayerDef: 36 bytes (9 fields: 3 float + 6 uint = 12 + 24 = 36)
    const uint32_t layer_def_size = 36u;
    const uint32_t print_parameters_size = 60u;
    const uint32_t layer_defs_offset = header_size + print_parameters_size;
    const uint32_t layer_data_offset = layer_defs_offset + layer_def_size * layer_count;

    std::vector<uint32_t> data_offsets(layer_count);
    uint32_t current_data = layer_data_offset;
    for (uint32_t i = 0; i < layer_count; ++i) {
        data_offsets[i] = current_data;
        current_data += uint32_t(m_layers[i].size());
    }

    // Calculate total model height
    const float total_height = sla::layer_z(print, layer_count-1);

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for Chitubox export");

    // Write header (112 bytes) following exact UVtools ChituboxFile.cs field order:
    // FieldOrder 0-28 in precise sequence
    write_u32_le(out, m_magic);                          // 0: Magic
    write_u32_le(out, 2u);                               // 1: This writer emits v2, without v3+ extension blocks
    write_f32_le(out, float(bed_size_x));                // 2: BedSizeX (mm)
    write_f32_le(out, float(bed_size_y));                // 3: BedSizeY (mm)
    write_f32_le(out, float(bed_size_z));                // 4: BedSizeZ (mm)
    write_u32_le(out, 0u);                               // 5: Unknown1
    write_u32_le(out, 0u);                               // 6: Unknown2
    write_f32_le(out, total_height);                     // 7: TotalHeightMillimeter
    write_f32_le(out, layer_h);                          // 8: LayerHeightMillimeter
    write_f32_le(out, exp);                              // 9: LayerExposureSeconds
    write_f32_le(out, initial_exp);                      // 10: BottomExposureSeconds
    write_f32_le(out, 0.0f);                             // 11: LightOffDelay (none by default)
    write_u32_le(out, bottom_count);                     // 12: BottomLayersCount
    write_u32_le(out, dims.resolution_x);               // 13: ResolutionX (pixels)
    write_u32_le(out, dims.resolution_y);               // 14: ResolutionY (pixels)
    write_u32_le(out, 0u);                               // 15: PreviewLargeOffsetAddress (0 = no preview)
    write_u32_le(out, layer_defs_offset);                // 16: LayersDefinitionOffsetAddress
    write_u32_le(out, layer_count);                      // 17: LayerCount
    write_u32_le(out, 0u);                               // 18: PreviewSmallOffsetAddress (0 = no preview)
    write_u32_le(out, sla::lcd_print_time(print));        // 19: Estimated print time
    write_u32_le(out, 0u);                               // 20: ProjectorType (0 = normal, 1 = LCD mirrored)
    write_u32_le(out, header_size);                      // 21: PrintParametersOffsetAddress
    write_u32_le(out, print_parameters_size);            // 22: PrintParametersSize
    write_u32_le(out, 1u);                               // 23: AntiAliasLevel (1 = no AA)
    write_u16_le(out, uint16_t(sla::motion_parameter(print, "light_pwm", false)));
    write_u16_le(out, uint16_t(sla::motion_parameter(print, "light_pwm", true)));
    write_u32_le(out, 0u);                               // 26: EncryptionKey (0 = no encryption)
    write_u32_le(out, 0u);                               // 27: SlicerOffset (for extended metadata)
    write_u32_le(out, 0u);                               // 28: SlicerSize

    // UVtools ChituboxFile.PrintParameters, fields 0..14 (v2).
    for (const bool bottom : {true, false}) {
        write_f32_le(out, sla::motion_parameter(print, "lift_height", bottom));
        write_f32_le(out, sla::motion_parameter(print, "lift_speed", bottom));
    }
    write_f32_le(out, sla::motion_parameter(print, "retract_speed", false));
    const auto &stats = print.print_statistics();
    const auto &material = print.material_config();
    const float volume = float((stats.objects_used_material + stats.support_used_material) / 1000.);
    write_f32_le(out, volume);
    write_f32_le(out, float(volume * material.material_density.value));
    write_f32_le(out, material.bottle_volume.value > 0 ? float(volume * material.bottle_cost.value / material.bottle_volume.value) : 0.f);
    write_f32_le(out, sla::motion_parameter(print, "wait_before_cure", true));
    write_f32_le(out, sla::motion_parameter(print, "wait_before_cure", false));
    write_u32_le(out, bottom_count);
    for (unsigned i = 0; i < 4; ++i) write_u32_le(out, 0);

    // Write layer definition table (36 bytes per layer = 9 fields)
    for (uint32_t i = 0; i < layer_count; ++i) {
        const float z = sla::layer_z(print, i);
        const float lexp = sla::layer_exposure(print, i);

        write_f32_le(out, z);                            // 0: PositionZ
        write_f32_le(out, lexp);                         // 1: ExposureTime
        write_f32_le(out, sla::motion_parameter(print, "wait_before_cure", i < bottom_count));
        write_u32_le(out, data_offsets[i]);              // 3: DataAddress (layer RLE offset)
        write_u32_le(out, uint32_t(m_layers[i].size())); // 4: DataSize (layer RLE size)
        write_u32_le(out, 0u);                           // 5: PageNumber
        write_u32_le(out, 36u);                          // 6: TableSize (36 = sizeof LayerDef)
        write_u32_le(out, 0u);                           // 7: Unknown3
        write_u32_le(out, 0u);                           // 8: Unknown4
    }

    // Write layer raster data
    for (uint32_t i = 0; i < layer_count; ++i)
        out.write(reinterpret_cast<const char *>(m_layers[i].data()), std::streamsize(m_layers[i].size()));

    out.flush();
    if (!out) throw RuntimeError("Cannot finish writing CTB data");
    out.close();
}

// CTB Encrypted - Chitubox with XOR encryption
void UVToolsCTBEncryptedArchive::export_print(const std::string     fname,
                                              const SLAPrint       &print,
                                              const ThumbnailsList &,
                                              const std::string    &)
{
    throw RuntimeError("CTB Encrypted format not yet fully implemented");
}

// ============================================================================
// PACKED-PIXEL FORMAT IMPLEMENTATIONS
// ============================================================================

std::unique_ptr<sla::RasterBase> UVToolsPackedPixelBase::create_raster() const
{
    return make_common_raster(m_cfg);
}

// GR1 (Makerbase) - Big-endian RGB565 packed format with vector lines
sla::RasterEncoder UVToolsGR1Archive::get_encoder() const
{
    return GR1RasterEncoder{};
}

void UVToolsGR1Archive::export_print(const std::string     fname,
                                     const SLAPrint       &print,
                                     const ThumbnailsList &,
                                     const std::string    &projectname)
{
    const auto  &cfg      = print.full_print_config();
    const float layer_h   = cfg_float_or(cfg, "layer_height", 0.05f);
    // Get orientation-aware display dimensions from m_cfg (set at archive creation)
    const auto dims       = get_display_dimensions(m_cfg);
    const auto layer_count = uint32_t(m_layers.size());

    // Get display dimensions from printer profile (actual bed size, no orientation swap)
    double display_width_mm = m_cfg.display_width.getFloat();
    double display_height_mm = m_cfg.display_height.getFloat();

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for GR1 export");

    // Write GR1 header
    out.write("GR1 FILE", 8);
    write_u32_le(out, 1u);  // Version
    write_u32_le(out, layer_count);
    write_f32_le(out, float(display_width_mm));
    write_f32_le(out, float(display_height_mm));
    write_u32_le(out, dims.resolution_x);
    write_u32_le(out, dims.resolution_y);
    write_f32_le(out, layer_h);
    
    // Exposure and timing parameters
    write_f32_le(out, cfg_float_or(cfg, "exposure_time", 2.5f));
    write_f32_le(out, cfg_float_or(cfg, "initial_exposure_time", 20.0f));
    
    // Write layer data
    for (const auto &layer : m_layers) {
        out.write(reinterpret_cast<const char *>(layer.data()), std::streamsize(layer.size()));
    }

    out.close();
}

// LGS (Longer Orange) - Packed pixels with 200+ byte header
sla::RasterEncoder UVToolsLGSArchive::get_encoder() const
{
    return GR1RasterEncoder{};  // LGS uses same RGB565 encoding as GR1
}

void UVToolsLGSArchive::export_print(const std::string     fname,
                                     const SLAPrint       &print,
                                     const ThumbnailsList &,
                                     const std::string    &projectname)
{
    const auto  &cfg      = print.full_print_config();
    const float layer_h   = cfg_float_or(cfg, "layer_height", 0.05f);
    const auto dims       = get_display_dimensions(m_cfg);
    const auto layer_count = uint32_t(m_layers.size());
    double display_width_mm = m_cfg.display_width.getFloat();
    double display_height_mm = m_cfg.display_height.getFloat();

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for LGS export");

    // LGS header - "Longer3D" (8 bytes) + 44 fields (all little-endian)
    out.write("Longer3D", 8);
    write_u32_le(out, 0xFF000001u);  // Uint_08
    write_u32_le(out, 1u);           // Uint_0c
    write_u32_le(out, 30u);          // PrinterModel
    write_u32_le(out, 0u);           // Uint_14
    write_u32_le(out, 34u);          // MagicKey
    write_f32_le(out, 15.404f);      // PixelPerMmX
    write_f32_le(out, 4.866f);       // PixelPerMmY
    write_f32_le(out, float(dims.resolution_x)); // ResolutionX
    write_f32_le(out, float(dims.resolution_y)); // ResolutionY
    write_f32_le(out, layer_h);      // LayerHeight
    write_f32_le(out, cfg_float_or(cfg, "exposure_time", 2.5f));       // ExposureTimeMs
    write_f32_le(out, cfg_float_or(cfg, "initial_exposure_time", 20.0f)); // BottomExposureTimeMs
    write_f32_le(out, 10.0f);        // Float_38
    write_f32_le(out, 1000.0f);      // WaitTimeBeforeCureMs
    write_f32_le(out, 2000.0f);      // BottomWaitTimeBeforeCureMs
    write_f32_le(out, 5.0f);         // BottomHeight
    write_f32_le(out, 0.6f);         // Float_48
    write_f32_le(out, 4.0f);         // BottomLiftHeight
    write_f32_le(out, 5.0f);         // LiftHeight
    write_f32_le(out, 150.0f);       // LiftSpeed
    write_f32_le(out, 150.0f);       // LiftSpeed_
    write_f32_le(out, 90.0f);        // BottomLiftSpeed
    write_f32_le(out, 90.0f);        // BottomLiftSpeed_
    write_f32_le(out, 5.0f);         // Float_64
    write_f32_le(out, 60.0f);        // Float_68
    write_f32_le(out, 10.0f);        // Float_6c
    write_f32_le(out, 600.0f);       // Float_70
    write_f32_le(out, 600.0f);       // Float_74
    write_f32_le(out, 2.0f);         // Float_78
    write_f32_le(out, 0.2f);         // Float_7c
    write_f32_le(out, 60.0f);        // Float_80
    write_f32_le(out, 1.0f);         // Float_84
    write_f32_le(out, 6.0f);         // Float_88
    write_f32_le(out, 150.0f);       // Float_8c
    write_f32_le(out, 1001.0f);      // Float_90
    write_f32_le(out, 140.0f);       // MachineZ
    write_u32_le(out, 0u);           // Uint_98
    write_u32_le(out, 0u);           // Uint_9c
    write_u32_le(out, 0u);           // Uint_a0
    write_u32_le(out, layer_count);  // LayerCount
    write_u32_le(out, 4u);           // Uint_a8
    write_u32_le(out, 120u);         // PreviewSizeX
    write_u32_le(out, 150u);         // PreviewSizeY

    // Write layer data
    for (const auto &layer : m_layers) {
        out.write(reinterpret_cast<const char *>(layer.data()), std::streamsize(layer.size()));
    }

    out.close();
}

// GOO RLE Encoder - encodes grayscale image to RLE compression
// Based on UVtools format
std::vector<uint8_t> encode_goo_rle(const uint8_t* src, size_t pixel_count)
{
    std::vector<uint8_t> rle;
    rle.push_back(0x55);  // Layer magic
    
    uint8_t previous_color = 0;
    uint8_t current_color = 0;
    uint32_t stride = 0;

    auto add_rep = [&rle, &current_color, &previous_color, &stride]() {
        if (stride == 0) return;
        
        size_t first_byte_idx = rle.size();
        rle.push_back(0);

        // Try color difference compression
        uint8_t color_diff = (current_color > previous_color) 
            ? (current_color - previous_color) 
            : (previous_color - current_color);
        
        if (color_diff <= 0x0F && stride <= 0xFF && current_color > 0 && current_color < 0xFF) {
            // Use difference mode (0x2)
            rle[first_byte_idx] = 0x80;  // 0b10 << 6
            rle[first_byte_idx] |= (color_diff & 0x0F);
            
            if (stride > 1) {
                rle[first_byte_idx] |= 0x10;  // 0b01 << 4 (extended run length)
                rle.push_back(static_cast<uint8_t>(stride));
            }
            
            if (current_color < previous_color) {
                rle[first_byte_idx] |= 0x20;  // 0b01 << 5 (negative difference)
            }
        } else {
            // Use standard mode
            uint8_t chunk_type = 0;
            if (current_color == 0xFF) {
                chunk_type = 3;  // 0b11
            } else if (current_color == 0x00) {
                chunk_type = 0;  // 0b00
            } else {
                chunk_type = 1;  // 0b01
                rle.push_back(current_color);
            }
            
            rle[first_byte_idx] = chunk_type << 6;
            rle[first_byte_idx] |= (stride & 0x0F);
            
            // Determine run length encoding type based on stride value
            if (stride <= 0x0F) {
                // Run length in byte0[3:0], no extra bytes
                // run_len_type = 0b00 (bits [5:4])
            } else if (stride <= 0xFFF) {
                // Run length = byte1[7:0] (8 bits) + byte0[3:0] (4 bits) = 12 bits
                rle[first_byte_idx] |= 0x10;  // 0b01 << 4
                rle.push_back(static_cast<uint8_t>(stride >> 4));
            } else if (stride <= 0xFFFFF) {
                // Run length = byte1[7:0] (8 bits) + byte2[7:0] (8 bits) + byte0[3:0] (4 bits) = 20 bits
                rle[first_byte_idx] |= 0x20;  // 0b10 << 4
                rle.push_back(static_cast<uint8_t>(stride >> 12));
                rle.push_back(static_cast<uint8_t>(stride >> 4));
            } else if (stride <= 0xFFFFFFF) {
                // Run length = byte1[7:0] + byte2[7:0] + byte3[7:0] + byte0[3:0] = 28 bits
                rle[first_byte_idx] |= 0x30;  // 0b11 << 4
                rle.push_back(static_cast<uint8_t>(stride >> 20));
                rle.push_back(static_cast<uint8_t>(stride >> 12));
                rle.push_back(static_cast<uint8_t>(stride >> 4));
            }
        }
    };

    for (size_t i = 0; i < pixel_count; i++) {
        if (current_color == src[i]) {
            stride++;
        } else {
            add_rep();
            stride = 1;
            previous_color = current_color;
            current_color = src[i];
        }
    }

    add_rep();

    // Calculate and add checksum
    uint8_t checksum = 0;
    for (size_t i = 1; i < rle.size(); i++) {
        checksum += rle[i];
    }
    rle.push_back(~checksum);

    return rle;
}

// GOO (Elegoo) - Grayscale with RLE compression
sla::RasterEncoder UVToolsGOOArchive::get_encoder() const
{
    // Keep compressed layers rather than allocating resolution * layer_count
    // bytes until export. The native stream can be written without re-encoding.
    return [](const void *ptr, size_t w, size_t h, size_t num_components) {
        if (!w || !h || num_components != 1) throw RuntimeError("GOO requires a nonempty grayscale raster");
        const uint8_t *src = reinterpret_cast<const uint8_t *>(ptr);
        return sla::EncodedRaster(encode_goo_rle(src, w * h), "goo");
    };
}

void UVToolsGOOArchive::export_print(const std::string     fname,
                                     const SLAPrint       &print,
                                     const ThumbnailsList &,
                                     const std::string    &)
{
    const auto  &cfg      = print.full_print_config();
    const float layer_h   = cfg_float_or(cfg, "layer_height", 0.05f);
    const float exp       = cfg_float_or(cfg, "exposure_time", 2.5f);
    const float init_exp  = cfg_float_or(cfg, "initial_exposure_time", 20.0f);
    const float lightoff  = cfg_float_or(cfg, "light_off_delay", 0.0f);
    const auto dims       = get_display_dimensions(m_cfg);
    const auto layer_count = uint32_t(m_layers.size());

    if (!layer_count) throw RuntimeError("No rasterized layers available for GOO export");
    const auto bottom_count = uint32_t(sla::bottom_layers(print, layer_count));

    double display_width_mm = dims.display_width_mm;
    double display_height_mm = dims.display_height_mm;

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for GOO export");

    // Generate current timestamp
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm* timeinfo = std::localtime(&time);
    char timestamp_buf[24] = {0};
    std::strftime(timestamp_buf, sizeof(timestamp_buf), "%Y-%m-%d %H:%M:%S", timeinfo);
    std::string timestamp(timestamp_buf);

    // Get machine name from config, default to "Anycubic Photon Mono"
    std::string machine_name = m_cfg.printer_model.value.substr(0, 31);

    // ============ WRITE HEADER ============
    out.write("V3.0", 4);
    out.put(0x07);
    out.put(0x00);
    out.put(0x00);
    out.put(0x00);
    out.write("DLP", 3);
    out.put(0x00);
    
    // SoftwareName (32 bytes) - use "UVtools" for compatibility
    std::string software = "SLA-Slicer";
    out.write(software.c_str(), software.length());
    out.write(std::string(32 - software.length(), '\0').c_str(), 32 - software.length());
    
    // SoftwareVersion (24 bytes)
    std::string version = "1.0";
    out.write(version.c_str(), version.length());
    out.write(std::string(24 - version.length(), '\0').c_str(), 24 - version.length());
    
    // FileCreateTime (24 bytes)
    out.write(timestamp.c_str(), timestamp.length());
    out.write(std::string(24 - timestamp.length(), '\0').c_str(), 24 - timestamp.length());
    
    // MachineName (32 bytes)
    out.write(machine_name.c_str(), machine_name.length());
    out.write(std::string(32 - machine_name.length(), '\0').c_str(), 32 - machine_name.length());
    
    // MachineType (32 bytes)
    out.write("DLP", 3);
    out.write(std::string(29, '\0').c_str(), 29);
    
    // ProfileName (32 bytes)
    out.write(software.c_str(), software.length());
    out.write(std::string(32 - software.length(), '\0').c_str(), 32 - software.length());
    
    // AntiAliasingLevel, GreyLevel, BlurLevel
    write_u16_be(out, 8u);
    write_u16_be(out, 1u);
    write_u16_be(out, 0u);
    
    // SmallPreview565 (116×116×2)
    for (int i = 0; i < 116 * 116; i++) {
        out.put(0);
        out.put(0);
    }
    out.put(0x0D);
    out.put(0x0A);
    
    // BigPreview565 (290×290×2)
    for (int i = 0; i < 290 * 290; i++) {
        out.put(0);
        out.put(0);
    }
    out.put(0x0D);
    out.put(0x0A);
    
    // LayerCount, Resolution
    write_u32_be(out, layer_count);
    write_u16_be(out, dims.resolution_x);
    write_u16_be(out, dims.resolution_y);
    
    // Mirroring
    out.put(0);
    out.put(0);
    
    // Display dimensions and machine Z
    write_f32_be(out, float(display_width_mm));
    write_f32_be(out, float(display_height_mm));
    write_f32_be(out, float(m_cfg.max_print_height.value));
    write_f32_be(out, layer_h);
    
    // Exposure and delay
    write_f32_be(out, exp);
    out.put(1);  // DelayMode = WaitTime
    write_f32_be(out, lightoff);
    
    // Timing parameters
    write_f32_be(out, sla::motion_parameter(print, "wait_after_cure", true));
    write_f32_be(out, sla::motion_parameter(print, "wait_after_lift", true));
    write_f32_be(out, sla::motion_parameter(print, "wait_before_cure", true));
    write_f32_be(out, sla::motion_parameter(print, "wait_after_cure", false));
    write_f32_be(out, sla::motion_parameter(print, "wait_after_lift", false));
    write_f32_be(out, sla::motion_parameter(print, "wait_before_cure", false));
    write_f32_be(out, init_exp);  // BottomExposureTime
    write_u32_be(out, bottom_count);
    
    // Movement parameters
    write_f32_be(out, sla::motion_parameter(print, "lift_height", true));
    write_f32_be(out, sla::motion_parameter(print, "lift_speed", true));
    write_f32_be(out, sla::motion_parameter(print, "lift_height", false));
    write_f32_be(out, sla::motion_parameter(print, "lift_speed", false));
    write_f32_be(out, sla::motion_parameter(print, "lift_height", true) + sla::motion_parameter(print, "lift_height2", true) - sla::motion_parameter(print, "retract_height2", true));
    write_f32_be(out, sla::motion_parameter(print, "retract_speed", true));
    write_f32_be(out, sla::motion_parameter(print, "lift_height", false) + sla::motion_parameter(print, "lift_height2", false) - sla::motion_parameter(print, "retract_height2", false));
    write_f32_be(out, sla::motion_parameter(print, "retract_speed", false));
    
    // Secondary movement parameters
    write_f32_be(out, sla::motion_parameter(print, "lift_height2", true));
    write_f32_be(out, sla::motion_parameter(print, "lift_speed2", true));
    write_f32_be(out, sla::motion_parameter(print, "lift_height2", false));
    write_f32_be(out, sla::motion_parameter(print, "lift_speed2", false));
    write_f32_be(out, sla::motion_parameter(print, "retract_height2", true));
    write_f32_be(out, sla::motion_parameter(print, "retract_speed2", true));
    write_f32_be(out, sla::motion_parameter(print, "retract_height2", false));
    write_f32_be(out, sla::motion_parameter(print, "retract_speed2", false));
    
    // PWM values
    write_u16_be(out, uint16_t(sla::motion_parameter(print, "light_pwm", true)));
    write_u16_be(out, uint16_t(sla::motion_parameter(print, "light_pwm", false)));
    
    // Per-layer settings
    out.put(1);
    
    // PrintTime
    write_u32_be(out, sla::lcd_print_time(print));
    
    // Volume/Material
    const auto &stats = print.print_statistics();
    const auto &material = print.material_config();
    const float volume = float((stats.objects_used_material + stats.support_used_material) / 1000.);
    write_f32_be(out, volume);
    write_f32_be(out, float(volume * material.material_density.value));
    write_f32_be(out, material.bottle_volume.value > 0 ? float(volume * material.bottle_cost.value / material.bottle_volume.value) : 0.f);
    
    // Currency symbol
    out.write("$", 1);
    out.write(std::string(7, '\0').c_str(), 7);
    
    // LayerDefAddress - points to start of first LayerDef
    // Remaining header: 4 (LayerDefAddress) + 1 (GrayScaleLevel) + 2 (TransitionLayerCount) = 7 bytes
    uint32_t layer_def_address = static_cast<uint32_t>(out.tellp()) + 7;
    write_u32_be(out, layer_def_address);
    
    // GrayScaleLevel
    out.put(1);
    
    // TransitionLayerCount
    write_u16_be(out, uint16_t(std::min(65535, std::max(0, print.material_config().sla_transition_layers.value))));
    
    // ============ WRITE LAYERS ============
    for (size_t i = 0; i < m_layers.size(); ++i) {
        const auto &layer_data = m_layers[i];
        const bool is_bottom = i < bottom_count;
        // Write LayerDef metadata
        write_u16_be(out, 0u);  // Pause
        write_f32_be(out, 0.0f);  // PausePositionZ
        write_f32_be(out, sla::layer_z(print, i));
        write_f32_be(out, sla::layer_exposure(print, i));
        write_f32_be(out, lightoff);  // LightOffDelay
        for (const char *key : {"wait_after_cure", "wait_after_lift", "wait_before_cure", "lift_height", "lift_speed", "lift_height2", "lift_speed2"})
            write_f32_be(out, sla::motion_parameter(print, key, is_bottom));
        write_f32_be(out, sla::motion_parameter(print, "lift_height", is_bottom) + sla::motion_parameter(print, "lift_height2", is_bottom) - sla::motion_parameter(print, "retract_height2", is_bottom));
        for (const char *key : {"retract_speed", "retract_height2", "retract_speed2"})
            write_f32_be(out, sla::motion_parameter(print, key, is_bottom));
        write_u16_be(out, uint16_t(sla::motion_parameter(print, "light_pwm", is_bottom)));
        
        // Delimiter
        out.put(0x0D);
        out.put(0x0A);
        
        // Encode layer to RLE
        // Write DataLength
        write_u32_be(out, static_cast<uint32_t>(layer_data.size()));
        
        // Write RLE data
        out.write(reinterpret_cast<const char*>(layer_data.data()), layer_data.size());
        
        // Delimiter
        out.put(0x0D);
        out.put(0x0A);
    }
    
    // ============ WRITE FOOTER ============
    out.put(0x00);  // Padding byte 1
    out.put(0x00);  // Padding byte 2
    out.put(0x00);  // Padding byte 3
    out.put(0x07);  // Magic byte 1
    out.put(0x00);  // Magic byte 2
    out.put(0x00);  // Magic byte 3
    out.put(0x00);  // Magic byte 4
    out.put(0x44);  // 'D'
    out.put(0x4C);  // 'L'
    out.put(0x50);  // 'P'
    out.put(0x00);  // Magic byte 11 (null terminator)

    out.flush();
    if (!out) throw RuntimeError("Cannot finish writing GOO data");
    out.close();
}

// ============================================================================
// OTHER/SPECIAL FORMAT IMPLEMENTATIONS
// ============================================================================

// Anet N4/N7 - Binary with UTF-16BE strings and BMP preview
std::unique_ptr<sla::RasterBase> UVToolsAnetArchive::create_raster() const
{
    return make_common_raster(m_cfg);
}

sla::RasterEncoder UVToolsAnetArchive::get_encoder() const
{
    return GR1RasterEncoder{};  // Anet uses similar RGB565 encoding
}

void UVToolsAnetArchive::export_print(const std::string     fname,
                                      const SLAPrint       &print,
                                      const ThumbnailsList &,
                                      const std::string    &)
{
    const auto  &cfg          = print.full_print_config();
    const float layer_h       = cfg_float_or(cfg, "layer_height", 0.05f);
    const float exp           = cfg_float_or(cfg, "exposure_time", 2.5f);
    const auto   bottom_count = uint32_t(std::max(0, cfg_int_or(cfg, "initial_layer_count", 6)));

    const uint32_t layer_count = uint32_t(m_layers.size());
    if (layer_count == 0)
        throw RuntimeError("No rasterized layers available for Anet export");

    // Get orientation-aware display dimensions from m_cfg (set at archive creation)
    const auto dims = get_display_dimensions(m_cfg);
    
    // Get display dimensions from printer profile (actual bed size, no orientation swap)
    double display_width_mm = m_cfg.display_width.getFloat();
    double display_height_mm = m_cfg.display_height.getFloat();

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for Anet export");

    // Write Anet binary header format
    write_u32_le(out, 0x09FDC0D9u);  // Magic
    write_u32_le(out, layer_count);
    write_u32_le(out, uint32_t(bottom_count));
    write_f32_le(out, layer_h);
    write_f32_le(out, exp);
    write_f32_le(out, exp);
    write_u32_le(out, dims.resolution_x);
    write_u32_le(out, dims.resolution_y);
    write_f32_le(out, float(display_width_mm));
    write_f32_le(out, float(display_height_mm));

    // Layer data offset (simplified - point directly after header)
    uint32_t data_offset = 256u;
    write_u32_le(out, data_offset);

    // Padding to reach data offset
    while (out.tellp() < std::streampos(data_offset))
        out.put(0);

    // Write layer data (placeholder - actual implementation would need per-layer encoding)
    for (const auto &layer : m_layers) {
        out.write(reinterpret_cast<const char *>(layer.data()), std::streamsize(layer.size()));
    }

    out.close();
}

// QDT (Emake3D) - Text-based vector line format
std::unique_ptr<sla::RasterBase> UVToolsQDTArchive::create_raster() const
{
    return make_common_raster(m_cfg);
}

sla::RasterEncoder UVToolsQDTArchive::get_encoder() const
{
    // QDT is vector-based, uses PNG as placeholder for compatibility
    return sla::PNGRasterEncoder{};
}

void UVToolsQDTArchive::export_print(const std::string     fname,
                                     const SLAPrint       &print,
                                     const ThumbnailsList &,
                                     const std::string    &projectname)
{
    const auto  &cfg      = print.full_print_config();
    const float layer_h   = cfg_float_or(cfg, "layer_height", 0.05f);
    // Get orientation-aware display dimensions from m_cfg (set at archive creation)
    const auto dims       = get_display_dimensions(m_cfg);
    const auto layer_count = uint32_t(m_layers.size());

    // Get display dimensions from printer profile (actual bed size, no orientation swap)
    double display_width_mm = m_cfg.display_width.getFloat();
    double display_height_mm = m_cfg.display_height.getFloat();

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for QDT export");

    // Write QDT text header
    out << "[Desc]\n";
    out << "Name=Generated by PrusaSlicer\n";
    out << "Version=1.0\n";
    out << "\n[Layers]\n";
    out << "Count=" << layer_count << "\n";
    out << "Height=" << layer_h << "\n";
    out << "ExposureTime=" << cfg_float_or(cfg, "exposure_time", 2.5f) << "\n";
    out << "LiftDistance=5\n";
    out << "LiftSpeed=100\n";
    out << "\n[Resolution]\n";
    out << "X=" << dims.resolution_x << "\n";
    out << "Y=" << dims.resolution_y << "\n";
    out << "Width=" << display_width_mm << "\n";
    out << "Height=" << display_height_mm << "\n";
    out << "\n[Data]\n";
    out << "# Vector layer definitions would go here\n";
    out << "# QDT is a vector format - actual implementation requires geometry extraction\n";

    out.close();
}

} // namespace Slic3r
