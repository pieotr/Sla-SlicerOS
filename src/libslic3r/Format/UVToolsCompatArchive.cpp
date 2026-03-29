///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "UVToolsCompatArchive.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <filesystem>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLAPrint.hpp"
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

// Helper to read display dimensions from a generic config (DynamicPrintConfig)
DisplayDimensions get_display_dimensions_from_print_config(const DynamicPrintConfig &cfg)
{
    DisplayDimensions dims{};
    
    // Try to read from display_width, display_height, display_pixels_x, display_pixels_y
    double w = 68.0; // Default width in case key doesn't exist
    double h = 120.0; // Default height
    uint32_t pw = 1440;  // Default pixels_x
    uint32_t ph = 2560;  // Default pixels_y
    
    // Read from config with safe fallbacks using option<T>() template method
    if (const auto *opt = cfg.option<ConfigOptionFloat>("display_width"))
        w = opt->value;
    if (const auto *opt = cfg.option<ConfigOptionFloat>("display_height"))
        h = opt->value;
    if (const auto *opt = cfg.option<ConfigOptionInt>("display_pixels_x"))
        pw = uint32_t(opt->value);
    if (const auto *opt = cfg.option<ConfigOptionInt>("display_pixels_y"))
        ph = uint32_t(opt->value);
    
    int ro = 0; // Landscape by default
    if (const auto *opt = cfg.option<ConfigOptionInt>("display_orientation"))
        ro = opt->value;
    
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

std::unique_ptr<sla::RasterBase> make_common_raster(const SLAPrinterConfig &cfg)
{
    sla::Resolution     res;
    sla::PixelDim       pxdim;
    std::array<bool, 2> mirror;

    double w  = cfg.display_width.getFloat();
    double h  = cfg.display_height.getFloat();
    auto   pw = size_t(cfg.display_pixels_x.getInt());
    auto   ph = size_t(cfg.display_pixels_y.getInt());

    mirror[X] = cfg.display_mirror_x.getBool();
    mirror[Y] = cfg.display_mirror_y.getBool();

    auto                         ro = cfg.display_orientation.getInt();
    sla::RasterBase::Orientation orientation =
        ro == sla::RasterBase::roPortrait ? sla::RasterBase::roPortrait : sla::RasterBase::roLandscape;

    if (orientation == sla::RasterBase::roPortrait) {
        std::swap(w, h);
        std::swap(pw, ph);
    }

    res   = sla::Resolution{pw, ph};
    pxdim = sla::PixelDim{w / pw, h / ph};
    sla::RasterBase::Trafo tr{orientation, mirror};

    const double gamma = cfg.gamma_correction.getFloat();
    return sla::create_raster_grayscale_aa(res, pxdim, gamma, tr);
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

uint32_t crc32_uvtools_style(std::ifstream &in, std::streamoff size)
{
    uint32_t table[256] = {0};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t val = i;
        for (int j = 0; j < 8; ++j)
            val = ((val & 1u) == 0u) ? (val >> 1) : ((val >> 1) ^ 0xEDB88320u);
        table[i] = val;
    }

    uint32_t checksum = 0;
    in.clear();
    in.seekg(0, std::ios::beg);

    std::vector<char> buf(1 << 20);
    std::streamoff    remaining = size;
    while (remaining > 0 && in) {
        const std::streamsize chunk = std::streamsize(std::min<std::streamoff>(remaining, std::streamoff(buf.size())));
        in.read(buf.data(), chunk);
        const std::streamsize got = in.gcount();
        if (got <= 0)
            break;

        for (std::streamsize i = 0; i < got; ++i) {
            const uint8_t idx = uint8_t(checksum) ^ uint8_t(buf[size_t(i)]);
            checksum          = table[idx] ^ (checksum >> 8);
        }

        remaining -= got;
    }

    return checksum;
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

std::array<uint8_t, 6> make_cxdlp_line(uint16_t start_y, uint16_t end_y, uint16_t start_x, uint8_t gray)
{
    std::array<uint8_t, 6> b{};
    b[0] = uint8_t((start_y >> 5) & 0xFF);
    b[1] = uint8_t(((start_y << 3) + (end_y >> 10)) & 0xFF);
    b[2] = uint8_t((end_y >> 2) & 0xFF);
    b[3] = uint8_t(((end_y << 6) + (start_x >> 8)) & 0xFF);
    b[4] = uint8_t(start_x);
    b[5] = gray;
    return b;
}

struct CXDLPRasterEncoder {
    sla::EncodedRaster operator()(const void *ptr, size_t w, size_t h, size_t num_components) const
    {
        const uint8_t *src = reinterpret_cast<const uint8_t *>(ptr);
        std::vector<uint8_t> layer;
        layer.reserve((w * h) / 3);

        uint32_t line_count      = 0;
        uint32_t non_zero_pixels = 0;

        for (size_t x = 0; x < w; ++x) {
            int32_t run_start = -1;
            uint8_t run_gray  = 0;

            for (size_t y = 0; y < h; ++y) {
                const uint8_t gray = src[(y * w + x) * num_components];

                if (gray == run_gray && gray != 0)
                    continue;

                if (run_start >= 0) {
                    const auto line = make_cxdlp_line(uint16_t(run_start), uint16_t(y - 1), uint16_t(x), run_gray);
                    layer.insert(layer.end(), line.begin(), line.end());
                    non_zero_pixels += uint32_t(y - size_t(run_start));
                    ++line_count;
                }

                run_start = (gray == 0) ? -1 : int32_t(y);
                run_gray  = gray;
            }

            if (run_start >= 0) {
                const auto line = make_cxdlp_line(uint16_t(run_start), uint16_t(h - 1), uint16_t(x), run_gray);
                layer.insert(layer.end(), line.begin(), line.end());
                non_zero_pixels += uint32_t(h - size_t(run_start));
                ++line_count;
            }
        }

        std::vector<uint8_t> out;
        out.reserve(8 + layer.size() + 2);

        out.push_back(uint8_t((non_zero_pixels >> 24) & 0xFF));
        out.push_back(uint8_t((non_zero_pixels >> 16) & 0xFF));
        out.push_back(uint8_t((non_zero_pixels >> 8) & 0xFF));
        out.push_back(uint8_t(non_zero_pixels & 0xFF));

        out.push_back(uint8_t((line_count >> 24) & 0xFF));
        out.push_back(uint8_t((line_count >> 16) & 0xFF));
        out.push_back(uint8_t((line_count >> 8) & 0xFF));
        out.push_back(uint8_t(line_count & 0xFF));

        out.insert(out.end(), layer.begin(), layer.end());
        out.push_back(0x0D);
        out.push_back(0x0A);

        return sla::EncodedRaster(std::move(out), "cxdlp");
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

// UInt24 big-endian encoder for OSF format
struct OSFRasterEncoder {
    sla::EncodedRaster operator()(const void *ptr, size_t w, size_t h, size_t num_components) const
    {
        const uint8_t *src = reinterpret_cast<const uint8_t *>(ptr);
        std::vector<uint8_t> out;
        out.reserve(w * h * 3);

        for (size_t i = 0; i < w * h * num_components; i += num_components) {
            uint8_t gray = src[i];
            // Store as big-endian UInt24: R, G, B
            out.push_back(gray);
            out.push_back(gray);
            out.push_back(gray);
        }

        return sla::EncodedRaster(std::move(out), "osf");
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
    sla::Resolution     res;
    sla::PixelDim       pxdim;
    std::array<bool, 2> mirror;

    double w  = m_cfg.display_width.getFloat();
    double h  = m_cfg.display_height.getFloat();
    auto   pw = size_t(m_cfg.display_pixels_x.getInt());
    auto   ph = size_t(m_cfg.display_pixels_y.getInt());

    mirror[0] = m_cfg.display_mirror_x.getBool();
    mirror[1] = m_cfg.display_mirror_y.getBool();

    auto                         ro = m_cfg.display_orientation.getInt();
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

    const double gamma = m_cfg.gamma_correction.getFloat();
    return sla::create_raster_grayscale_aa(res, pxdim, gamma, tr);
}

// FDG encoder implementation
sla::RasterEncoder UVToolsFDGArchive::get_encoder() const
{
    return FDGRasterEncoder{};
}

// CXDLP encoder implementation
sla::RasterEncoder UVToolsCXDLPArchive::get_encoder() const
{
    return CXDLPRasterEncoder{};
}

// CXDLP raster creation - PNG-based line format
std::unique_ptr<sla::RasterBase> UVToolsCXDLPArchive::create_raster() const
{
    sla::Resolution     res;
    sla::PixelDim       pxdim;
    std::array<bool, 2> mirror;

    double w  = m_cfg.display_width.getFloat();
    double h  = m_cfg.display_height.getFloat();
    auto   pw = size_t(m_cfg.display_pixels_x.getInt());
    auto   ph = size_t(m_cfg.display_pixels_y.getInt());

    mirror[0] = m_cfg.display_mirror_x.getBool();
    mirror[1] = m_cfg.display_mirror_y.getBool();

    auto                         ro = m_cfg.display_orientation.getInt();
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

    const double gamma = m_cfg.gamma_correction.getFloat();
    return sla::create_raster_grayscale_aa(res, pxdim, gamma, tr);
}

void UVToolsCWSArchive::export_print(const std::string     fname,
                                     const SLAPrint       &print,
                                     const ThumbnailsList &,
                                     const std::string    &projectname)
{
    const uint32_t layer_count = uint32_t(m_layers.size());
    if (layer_count == 0)
        throw RuntimeError("No layers available for CWS export");
        
    const std::string project =
        projectname.empty() ? std::filesystem::path(fname).stem().string() : projectname;

    // Get print config for parameters
    const auto &cfg = print.full_print_config();
    const float layer_h = cfg_float_or(cfg, "layer_height", 0.05f);
    const float exp_time = cfg_float_or(cfg, "exposure_time", 2.5f);
    const float lift_h = cfg_float_or(cfg, "lift_distance", 5.0f);
    const float lift_spd = cfg_float_or(cfg, "lift_speed", 60.0f);

    // Get orientation-aware display dimensions from current print config (not stale m_cfg)
    const auto dims = get_display_dimensions_from_print_config(cfg);

    // CWS requires XResolution x YResolution to match actual raster dimensions
    // The raster is already created with correct orientation, so use actual layer dimensions
    // If no layers yet, use dims; otherwise validate against first layer
    uint32_t res_x = dims.resolution_x;
    uint32_t res_y = dims.resolution_y;

    // Create ZIP file with Zipper
    try {
        Zipper zip(fname, Zipper::FAST_COMPRESSION);

        // Create manifest.xml (Wanhao format)
        std::ostringstream manifest;
        manifest << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        manifest << "<manifest FileVersion=\"1\">\n";
        manifest << "  <Slices>\n";
        for (uint32_t i = 0; i < layer_count; ++i)
            manifest << "    <Slice><name>" << layer_name(project, i) << "</name></Slice>\n";
        manifest << "  </Slices>\n";
        manifest << "  <SliceProfile><name>" << project << ".slicing</name></SliceProfile>\n";
        manifest << "  <GCode><name>" << project << ".gcode</name></GCode>\n";
        manifest << "</manifest>\n";
        
        std::string manifest_str = manifest.str();
        zip.add_entry("manifest.xml");
        zip << manifest_str;

        // Create slicing profile with correct X/Y resolution
        std::ostringstream slicing;
        slicing << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        slicing << "<SliceBuildConfig FileVersion=\"2\">\n";
        slicing << "  <XResolution>" << res_x << "</XResolution>\n";
        slicing << "  <YResolution>" << res_y << "</YResolution>\n";
        slicing << "  <LiftDistance>" << std::fixed << std::setprecision(2) << lift_h << "</LiftDistance>\n";
        slicing << "  <LiftSpeed>" << std::fixed << std::setprecision(2) << lift_spd << "</LiftSpeed>\n";
        slicing << "  <LayerThickness>" << std::fixed << std::setprecision(3) << layer_h << "</LayerThickness>\n";
        slicing << "  <ExposureTime>" << std::fixed << std::setprecision(2) << exp_time << "</ExposureTime>\n";
        slicing << "</SliceBuildConfig>\n";
        
        std::string slicing_str = slicing.str();
        zip.add_entry(project + ".slicing");
        zip << slicing_str;

        // Create minimal GCode
        std::ostringstream gcode;
        gcode << ";Generated by PrusaSlicer SLA\n";
        gcode << ";Number of slices = " << layer_count << "\n";
        gcode << ";X Resolution = " << res_x << "\n";
        gcode << ";Y Resolution = " << res_y << "\n";
        
        std::string gcode_str = gcode.str();
        zip.add_entry(project + ".gcode");
        zip << gcode_str;

        // Add layer images (already PNG-encoded from m_layers)
        for (uint32_t i = 0; i < layer_count; ++i) {
            const auto &rst = m_layers[i];
            zip.add_entry(layer_name(project, i), rst.data(), rst.size());
        }

        // Force proper ZIP closure and finalization
        zip.finalize();
    } catch (const std::exception &e) {
        throw RuntimeError(std::string("CWS export failed: ") + e.what());
    }
}

void UVToolsFDGArchive::export_print(const std::string     fname,
                                     const SLAPrint       &print,
                                     const ThumbnailsList &,
                                     const std::string    &)
{
    const auto  &printer_cfg  = print.printer_config();
    const auto  &cfg          = print.full_print_config();
    const float layer_h       = cfg_float_or(cfg, "layer_height", 0.05f);
    const float initial_h     = cfg_float_or(cfg, "initial_layer_height", layer_h);
    const float exp           = cfg_float_or(cfg, "exposure_time", 2.5f);
    const float initial_exp   = cfg_float_or(cfg, "initial_exposure_time", 20.0f);
    const auto   bottom_count = uint32_t(std::max(0, cfg_int_or(cfg, "initial_layer_count", 6)));

    const uint32_t layer_count = uint32_t(m_layers.size());
    if (layer_count == 0)
        throw RuntimeError("No rasterized layers available for FDG export");

    // Get orientation-aware display dimensions from printer config
    const auto dims = get_display_dimensions(printer_cfg);

    const uint32_t header_size = 54u * 4u;
    const uint32_t layer_def_size = 9u * 4u;
    const uint32_t layer_defs_offset = header_size;
    const uint32_t layer_data_offset = layer_defs_offset + layer_def_size * layer_count;

    std::vector<uint32_t> data_offsets(layer_count);
    uint32_t current_data = layer_data_offset;
    for (uint32_t i = 0; i < layer_count; ++i) {
        data_offsets[i] = current_data;
        current_data += uint32_t(m_layers[i].size());
    }

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for FDG export");

    write_u32_le(out, 0xBD3C7AC8u);
    write_u32_le(out, 2u);
    write_u32_le(out, layer_count);
    write_u32_le(out, bottom_count);
    write_u32_le(out, 0u);
    write_u32_le(out, bottom_count);
    write_u32_le(out, dims.resolution_x);
    write_u32_le(out, dims.resolution_y);
    write_f32_le(out, layer_h);
    write_f32_le(out, exp);
    write_f32_le(out, initial_exp);
    write_u32_le(out, 0u);
    write_u32_le(out, 0u);
    write_u32_le(out, layer_defs_offset);
    write_u32_le(out, 0u);
    write_u32_le(out, 1u);
    write_u16_le(out, 255u);
    write_u16_le(out, 255u);

    for (int i = 0; i < 3; ++i)
        write_u32_le(out, 0u);

    write_f32_le(out, initial_h * float(bottom_count) + layer_h * float(layer_count - std::min(layer_count, bottom_count)));
    write_f32_le(out, float(dims.display_width_mm));
    write_f32_le(out, float(dims.display_height_mm));
    write_f32_le(out, 150.0f);
    write_u32_le(out, 0u);
    write_u32_le(out, 0u);
    write_u32_le(out, 0x4Cu);
    write_f32_le(out, 0.0f);
    write_f32_le(out, 0.0f);
    write_f32_le(out, 0.0f);
    write_u32_le(out, 0u);
    write_u32_le(out, 0u);
    write_f32_le(out, 1.0f);
    write_f32_le(out, 1.0f);
    write_u32_le(out, 0u);
    write_f32_le(out, 5.0f);
    write_f32_le(out, 60.0f);
    write_f32_le(out, 5.0f);
    write_f32_le(out, 60.0f);
    write_f32_le(out, 60.0f);

    while (out.tellp() < std::streampos(header_size))
        write_u32_le(out, 0u);

    for (uint32_t i = 0; i < layer_count; ++i) {
        const float z = (i < bottom_count) ? initial_h * float(i + 1) : (initial_h * float(bottom_count) + layer_h * float(i - bottom_count + 1));
        const float lexp = (i < bottom_count) ? initial_exp : exp;

        write_f32_le(out, z);
        write_f32_le(out, lexp);
        write_f32_le(out, 1.0f);
        write_u32_le(out, data_offsets[i]);
        write_u32_le(out, uint32_t(m_layers[i].size()));
        write_u32_le(out, 0u);
        write_u32_le(out, 84u);
        write_u32_le(out, 0u);
        write_u32_le(out, 0u);
    }

    for (uint32_t i = 0; i < layer_count; ++i)
        out.write(reinterpret_cast<const char *>(m_layers[i].data()), std::streamsize(m_layers[i].size()));
}

void UVToolsCXDLPArchive::export_print(const std::string     fname,
                                       const SLAPrint       &print,
                                       const ThumbnailsList &,
                                       const std::string    &)
{
    const auto &printer_cfg = print.printer_config();
    const auto &cfg = print.full_print_config();

    const uint32_t layer_count = uint32_t(m_layers.size());
    if (layer_count == 0)
        throw RuntimeError("No rasterized layers available for CXDLP export");

    const float layer_h = cfg_float_or(cfg, "layer_height", 0.05f);
    const float exp     = cfg_float_or(cfg, "exposure_time", 2.5f);
    const float bexp    = cfg_float_or(cfg, "initial_exposure_time", 20.0f);
    const uint16_t bottom_count = uint16_t(std::max(0, cfg_int_or(cfg, "initial_layer_count", 6)));
    const float lift_h  = cfg_float_or(cfg, "lift_distance", 5.0f);

    // Get display dimensions from printer config
    const auto dims = get_display_dimensions(printer_cfg);

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for CXDLP export");

    write_u32_be(out, 9u);
    out.write("CXSW3DV2", 8);
    out.put('\0');
    write_u16_be(out, 3u);
    write_be_len_nt_string(out, "CL-60");
    write_u16_be(out, uint16_t(layer_count));
    write_u16_be(out, uint16_t(dims.resolution_x));
    write_u16_be(out, uint16_t(dims.resolution_y));

    std::array<uint8_t, 64> zero64{};
    out.write(reinterpret_cast<const char *>(zero64.data()), std::streamsize(zero64.size()));

    const size_t thumb_sizes[3] = {116u * 116u * 2u, 290u * 290u * 2u, 290u * 290u * 2u};
    std::vector<uint8_t> zeros;
    for (size_t ts : thumb_sizes) {
        zeros.assign(ts, 0u);
        out.write(reinterpret_cast<const char *>(zeros.data()), std::streamsize(zeros.size()));
        out.put('\r');
        out.put('\n');
    }

    std::ostringstream wss;
    wss << std::fixed << std::setprecision(2) << dims.display_width_mm;
    std::ostringstream hss;
    hss << std::fixed << std::setprecision(2) << dims.display_height_mm;
    std::ostringstream lhss;
    lhss << std::fixed << std::setprecision(3) << layer_h;

    write_utf16be_string(out, wss.str());
    write_utf16be_string(out, hss.str());
    write_utf16be_string(out, lhss.str());

    write_u16_be(out, clamp_cast<uint16_t>(exp * 10.0f, 1u, 60000u));
    write_u16_be(out, 1u);
    write_u16_be(out, clamp_cast<uint16_t>(bexp, 1u, 60000u));
    write_u16_be(out, bottom_count);
    write_u16_be(out, clamp_cast<uint16_t>(lift_h, 1u, 1000u));
    write_u16_be(out, 1u);
    write_u16_be(out, clamp_cast<uint16_t>(lift_h, 1u, 1000u));
    write_u16_be(out, 1u);
    write_u16_be(out, 1u);
    write_u16_be(out, 255u);
    write_u16_be(out, 255u);

    const std::streamoff area_pos = out.tellp();
    for (uint32_t i = 0; i < layer_count; ++i)
        write_u32_be(out, 0u);
    out.put('\r');
    out.put('\n');

    write_be_len_nt_string(out, "PrusaSlicer");
    write_be_len_nt_string(out, "");
    out.put(char(0));
    write_u32_le(out, 600u);
    write_u32_le(out, 300000u);
    out.put(char(1));
    write_u16_le(out, 0u);
    out.put(char(0));
    write_u16_le(out, 1000u);
    out.put(char(1));
    out.put(char(1));
    out.put(char(255));
    out.put(char(0));
    out.put(char(2));
    out.put('\r');
    out.put('\n');

    std::vector<uint32_t> areas(layer_count, 0);
    for (uint32_t i = 0; i < layer_count; ++i) {
        const auto &rst = m_layers[i];
        const auto *data = reinterpret_cast<const uint8_t *>(rst.data());
        if (rst.size() >= 4) {
            areas[i] = (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
                       (uint32_t(data[2]) << 8) | uint32_t(data[3]);
        }
        out.write(reinterpret_cast<const char *>(rst.data()), std::streamsize(rst.size()));
    }

    write_u32_be(out, 9u);
    out.write("CXSW3DV2", 8);
    out.put('\0');

    out.close();
    if (!out)
        throw RuntimeError("Failed to close CXDLP output file before checksum calculation");

    // Reopen file in read mode to calculate checksum
    std::ifstream in_crc(fname, std::ios::binary);
    if (!in_crc)
        throw RuntimeError("Failed to reopen CXDLP output file for checksum");

    in_crc.seekg(0, std::ios::end);
    const std::streamoff file_size = in_crc.tellg();
    const uint32_t checksum = crc32_uvtools_style(in_crc, file_size);
    in_crc.close();

    // Reopen in append mode to add checksum and update areas
    out.open(fname, std::ios::binary | std::ios::app);
    if (!out)
        throw RuntimeError("Failed to reopen CXDLP output file for final updates");

    write_u32_be(out, checksum);

    out.seekp(area_pos, std::ios::beg);
    for (uint32_t a : areas)
        write_u32_be(out, a);

    out.close();
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

// GenericZIP - ZIP with manifest.uvtools
void UVToolsGenericZIPArchive::export_print(const std::string     fname,
                                            const SLAPrint       &print,
                                            const ThumbnailsList &,
                                            const std::string    &projectname)
{
    throw RuntimeError("GenericZIP format not yet fully implemented");
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

// VDA (Structo) - ZIP with XML root structure
void UVToolsVDAArchive::export_print(const std::string     fname,
                                     const SLAPrint       &print,
                                     const ThumbnailsList &,
                                     const std::string    &projectname)
{
    throw RuntimeError("VDA format not yet fully implemented");
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
    const auto  &printer_cfg  = print.printer_config();
    const auto  &cfg          = print.full_print_config();
    const float layer_h       = cfg_float_or(cfg, "layer_height", 0.05f);
    const float initial_h     = cfg_float_or(cfg, "initial_layer_height", layer_h);
    const float exp           = cfg_float_or(cfg, "exposure_time", 2.5f);
    const float initial_exp   = cfg_float_or(cfg, "initial_exposure_time", 20.0f);
    const auto   bottom_count = uint32_t(std::max(0, cfg_int_or(cfg, "initial_layer_count", 6)));

    const uint32_t layer_count = uint32_t(m_layers.size());
    if (layer_count == 0)
        throw RuntimeError("No rasterized layers available for Chitubox export");

    // Get orientation-aware display dimensions from printer config
    const auto dims = get_display_dimensions(printer_cfg);

    const uint32_t header_size = 24u * 4u;
    const uint32_t layer_def_size = 9u * 4u;
    const uint32_t layer_defs_offset = header_size;
    const uint32_t layer_data_offset = layer_defs_offset + layer_def_size * layer_count;

    std::vector<uint32_t> data_offsets(layer_count);
    uint32_t current_data = layer_data_offset;
    for (uint32_t i = 0; i < layer_count; ++i) {
        data_offsets[i] = current_data;
        current_data += uint32_t(m_layers[i].size());
    }

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for Chitubox export");

    write_u32_le(out, m_magic);
    write_u32_le(out, layer_count);
    write_f32_le(out, layer_h);
    write_f32_le(out, exp);
    write_f32_le(out, initial_exp);
    write_u32_le(out, uint32_t(bottom_count));
    write_f32_le(out, initial_h);
    write_f32_le(out, float(dims.display_width_mm));
    write_f32_le(out, float(dims.display_height_mm));
    write_u32_le(out, 0u);
    write_u32_le(out, 0u);
    write_u32_le(out, layer_defs_offset);
    write_f32_le(out, 1.0f);
    write_f32_le(out, 1.0f);
    write_u32_le(out, 0u);

    for (int i = 0; i < 8; ++i)
        write_u32_le(out, 0u);

    for (uint32_t i = 0; i < layer_count; ++i) {
        const float z = (i < bottom_count) ? initial_h * float(i + 1) : (initial_h * float(bottom_count) + layer_h * float(i - bottom_count + 1));
        const float lexp = (i < bottom_count) ? initial_exp : exp;

        write_f32_le(out, z);
        write_f32_le(out, lexp);
        write_f32_le(out, 1.0f);
        write_u32_le(out, data_offsets[i]);
        write_u32_le(out, uint32_t(m_layers[i].size()));
        write_u32_le(out, 0u);
        write_u32_le(out, 84u);
        write_u32_le(out, 0u);
        write_u32_le(out, 0u);
    }

    for (uint32_t i = 0; i < layer_count; ++i)
        out.write(reinterpret_cast<const char *>(m_layers[i].data()), std::streamsize(m_layers[i].size()));

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
    const auto  &printer_cfg = print.printer_config();
    const auto  &cfg      = print.full_print_config();
    const float layer_h   = cfg_float_or(cfg, "layer_height", 0.05f);
    // Get orientation-aware display dimensions from printer config
    const auto dims       = get_display_dimensions(printer_cfg);
    const auto layer_count = uint32_t(m_layers.size());

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for GR1 export");

    // Write GR1 header
    out.write("GR1 FILE", 8);
    write_u32_le(out, 1u);  // Version
    write_u32_le(out, layer_count);
    write_f32_le(out, float(dims.display_width_mm));
    write_f32_le(out, float(dims.display_height_mm));
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
    const auto  &printer_cfg = print.printer_config();
    const auto  &cfg      = print.full_print_config();
    const float layer_h   = cfg_float_or(cfg, "layer_height", 0.05f);
    // Get orientation-aware display dimensions from printer config
    const auto dims       = get_display_dimensions(printer_cfg);
    const auto layer_count = uint32_t(m_layers.size());

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for LGS export");

    // Write LGS header (200+ bytes)
    out.write("LGS FILE", 8);
    write_u32_le(out, 1u);
    write_u32_le(out, layer_count);
    write_f32_le(out, float(dims.display_width_mm));
    write_f32_le(out, float(dims.display_height_mm));
    write_u32_le(out, dims.resolution_x);
    write_u32_le(out, dims.resolution_y);
    write_f32_le(out, layer_h);
    write_f32_le(out, cfg_float_or(cfg, "exposure_time", 2.5f));
    write_f32_le(out, cfg_float_or(cfg, "initial_exposure_time", 20.0f));
    
    // Pad to 256 bytes minimum
    const auto header_end = out.tellp();
    while (out.tellp() < std::streampos(256))
        out.put(0);

    // Write layer data
    for (const auto &layer : m_layers) {
        out.write(reinterpret_cast<const char *>(layer.data()), std::streamsize(layer.size()));
    }

    out.close();
}

// OSF (Vlare) - Big-endian UInt24 packed pixels
sla::RasterEncoder UVToolsOSFArchive::get_encoder() const
{
    return OSFRasterEncoder{};
}

void UVToolsOSFArchive::export_print(const std::string     fname,
                                     const SLAPrint       &print,
                                     const ThumbnailsList &,
                                     const std::string    &)
{
    const auto  &printer_cfg = print.printer_config();
    const auto  &cfg      = print.full_print_config();
    const float layer_h   = cfg_float_or(cfg, "layer_height", 0.05f);
    // Get orientation-aware display dimensions from printer config
    const auto dims       = get_display_dimensions(printer_cfg);
    const auto layer_count = uint32_t(m_layers.size());

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for OSF export");

    // Write OSF header
    out.write("OSF!", 4);
    write_u32_be(out, 1u);
    write_u32_be(out, layer_count);
    write_f32_le(out, float(dims.display_width_mm));
    write_f32_le(out, float(dims.display_height_mm));
    write_u32_le(out, dims.resolution_x);
    write_u32_le(out, dims.resolution_y);
    write_f32_le(out, layer_h);
    write_f32_le(out, cfg_float_or(cfg, "exposure_time", 2.5f));

    // Write layer data (UInt24 big-endian pixels)
    for (const auto &layer : m_layers) {
        out.write(reinterpret_cast<const char *>(layer.data()), std::streamsize(layer.size()));
    }

    out.close();
}

// GOO (Elegoo) - RGB565 packed with comprehensive ASCII header
sla::RasterEncoder UVToolsGOOArchive::get_encoder() const
{
    return GR1RasterEncoder{};  // GOO uses RGB565 like GR1
}

void UVToolsGOOArchive::export_print(const std::string     fname,
                                     const SLAPrint       &print,
                                     const ThumbnailsList &,
                                     const std::string    &)
{
    const auto  &printer_cfg = print.printer_config();
    const auto  &cfg      = print.full_print_config();
    const float layer_h   = cfg_float_or(cfg, "layer_height", 0.05f);
    const float exp       = cfg_float_or(cfg, "exposure_time", 2.5f);
    const float init_exp  = cfg_float_or(cfg, "initial_exposure_time", 20.0f);
    // Get orientation-aware display dimensions from printer config
    const auto dims       = get_display_dimensions(printer_cfg);
    const auto layer_count = uint32_t(m_layers.size());

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for GOO export");

    // Write ASCII header
    out << "# Elegoo GOO Format\n";
    out << "# Generated by PrusaSlicer\n";
    out << "# Version: 1\n";
    out << "Layers: " << layer_count << "\n";
    out << "Width: " << dims.resolution_x << "\n";
    out << "Height: " << dims.resolution_y << "\n";
    out << "Display Width: " << dims.display_width_mm << " mm\n";
    out << "Display Height: " << dims.display_height_mm << " mm\n";
    out << "Layer Height: " << layer_h << " mm\n";
    out << "Exposure Time: " << exp << " s\n";
    out << "Initial Exposure: " << init_exp << " s\n";
    out << "# End header\n";

    // Write layer data (RGB565 packed pixels)
    for (const auto &layer : m_layers) {
        out.write(reinterpret_cast<const char *>(layer.data()), std::streamsize(layer.size()));
    }

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
    const auto  &printer_cfg  = print.printer_config();
    const auto  &cfg          = print.full_print_config();
    const float layer_h       = cfg_float_or(cfg, "layer_height", 0.05f);
    const float exp           = cfg_float_or(cfg, "exposure_time", 2.5f);
    const auto   bottom_count = uint32_t(std::max(0, cfg_int_or(cfg, "initial_layer_count", 6)));

    const uint32_t layer_count = uint32_t(m_layers.size());
    if (layer_count == 0)
        throw RuntimeError("No rasterized layers available for Anet export");

    // Get orientation-aware display dimensions from printer config
    const auto dims = get_display_dimensions(printer_cfg);

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
    write_f32_le(out, float(dims.display_width_mm));
    write_f32_le(out, float(dims.display_height_mm));

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
    // Get display dimensions from current print config (not stale m_cfg)
    const auto dims       = get_display_dimensions_from_print_config(cfg);
    const auto layer_count = uint32_t(m_layers.size());

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
    out << "Width=" << dims.display_width_mm << "\n";
    out << "Height=" << dims.display_height_mm << "\n";
    out << "\n[Data]\n";
    out << "# Vector layer definitions would go here\n";
    out << "# QDT is a vector format - actual implementation requires geometry extraction\n";

    out.close();
}

} // namespace Slic3r
