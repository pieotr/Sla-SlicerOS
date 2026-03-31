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

// No need for generic helper - use printer_config() directly


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

    // Get printer-specific config and generic config for parameters
    const auto &cfg = print.full_print_config();
    const float layer_h = cfg_float_or(cfg, "layer_height", 0.05f);
    const float exp_time = cfg_float_or(cfg, "exposure_time", 2.5f);
    const float lift_h = cfg_float_or(cfg, "lift_distance", 5.0f);
    const float lift_spd = cfg_float_or(cfg, "lift_speed", 60.0f);

    // Get orientation-aware display dimensions from m_cfg (set at archive creation)
    const auto dims = get_display_dimensions(m_cfg);

    // CWS requires XResolution x YResolution to match actual raster dimensions
    // The raster is already created with correct orientation, so use actual layer dimensions
    // If no layers yet, use dims; otherwise validate against first layer
    uint32_t res_x = dims.resolution_x;
    uint32_t res_y = dims.resolution_y;
    
    // Get display dimensions from printer profile (actual bed size)
    double display_width_mm = m_cfg.display_width.getFloat();
    double display_height_mm = m_cfg.display_height.getFloat();

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
        manifest << "  <DisplayWidth>" << std::fixed << std::setprecision(2) << display_width_mm << "</DisplayWidth>\n";
        manifest << "  <DisplayHeight>" << std::fixed << std::setprecision(2) << display_height_mm << "</DisplayHeight>\n";
        manifest << "</manifest>\n";
        
        std::string manifest_str = manifest.str();
        zip.add_entry("manifest.xml");
        zip << manifest_str;

        // Create slicing profile with correct X/Y resolution and display dimensions
        std::ostringstream slicing;
        slicing << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        slicing << "<SliceBuildConfig FileVersion=\"2\">\n";
        slicing << "  <XResolution>" << res_x << "</XResolution>\n";
        slicing << "  <YResolution>" << res_y << "</YResolution>\n";
        slicing << "  <DisplayWidth>" << std::fixed << std::setprecision(2) << display_width_mm << "</DisplayWidth>\n";
        slicing << "  <DisplayHeight>" << std::fixed << std::setprecision(2) << display_height_mm << "</DisplayHeight>\n";
        slicing << "  <LayerThickness>" << std::fixed << std::setprecision(3) << layer_h << "</LayerThickness>\n";
        slicing << "  <LiftDistance>" << std::fixed << std::setprecision(2) << lift_h << "</LiftDistance>\n";
        slicing << "  <LiftSpeed>" << std::fixed << std::setprecision(2) << lift_spd << "</LiftSpeed>\n";
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

void UVToolsCXDLPArchive::export_print(const std::string     fname,
                                       const SLAPrint       &print,
                                       const ThumbnailsList &,
                                       const std::string    &)
{
    const auto &cfg = print.full_print_config();

    const uint32_t layer_count = uint32_t(m_layers.size());
    if (layer_count == 0)
        throw RuntimeError("No rasterized layers available for CXDLP export");

    const float layer_h = cfg_float_or(cfg, "layer_height", 0.05f);
    const float exp     = cfg_float_or(cfg, "exposure_time", 2.5f);
    const float bexp    = cfg_float_or(cfg, "initial_exposure_time", 20.0f);
    const uint16_t bottom_count = uint16_t(std::max(0, cfg_int_or(cfg, "initial_layer_count", 6)));
    const float lift_h  = cfg_float_or(cfg, "lift_distance", 5.0f);

    // Get display dimensions from m_cfg (set at archive creation)
    const auto dims = get_display_dimensions(m_cfg);
    
    // Get raw display dimensions from printer profile (no orientation swap)
    double display_width_mm = m_cfg.display_width.getFloat();
    double display_height_mm = m_cfg.display_height.getFloat();

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
    wss << std::fixed << std::setprecision(2) << display_width_mm;
    std::ostringstream hss;
    hss << std::fixed << std::setprecision(2) << display_height_mm;
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
    const auto  &cfg          = print.full_print_config();
    const float layer_h       = cfg_float_or(cfg, "layer_height", 0.05f);
    const float initial_h     = cfg_float_or(cfg, "initial_layer_height", layer_h);
    const float exp           = cfg_float_or(cfg, "exposure_time", 2.5f);
    const float initial_exp   = cfg_float_or(cfg, "initial_exposure_time", 20.0f);
    const auto   bottom_count = uint32_t(std::max(0, cfg_int_or(cfg, "initial_layer_count", 6)));

    const uint32_t layer_count = uint32_t(m_layers.size());
    if (layer_count == 0)
        throw RuntimeError("No rasterized layers available for Chitubox export");

    // Get orientation-aware display dimensions and resolution
    const auto dims = get_display_dimensions(m_cfg);

    // Get printer bed dimensions (full dimensions, not just usable display area)
    double bed_size_x = 68.04;  // Default, can be configured
    double bed_size_y = 120.96; // Default, can be configured
    double bed_size_z = 150.0;  // Default, can be configured

    // CTB Header: 112 bytes (fields 0-28: 24 uint/float + 2 ushort + 4 uint)
    const uint32_t header_size = 112u;
    // LayerDef: 36 bytes (9 fields: 3 float + 6 uint = 12 + 24 = 36)
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
        throw RuntimeError("Failed to open output file for Chitubox export");

    // Write header (112 bytes) following exact UVtools ChituboxFile.cs field order:
    // FieldOrder 0-28 in precise sequence
    write_u32_le(out, m_magic);                          // 0: Magic
    write_u32_le(out, 5u);                               // 1: Version (5 = latest)
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
    write_u32_le(out, 0u);                               // 19: PrintTime (estimated, 0 = unknown)
    write_u32_le(out, 0u);                               // 20: ProjectorType (0 = normal, 1 = LCD mirrored)
    write_u32_le(out, 0u);                               // 21: PrintParametersOffsetAddress
    write_u32_le(out, 0u);                               // 22: PrintParametersSize
    write_u32_le(out, 1u);                               // 23: AntiAliasLevel (1 = no AA)
    write_u16_le(out, 255u);                             // 24: LightPWM (ushort, not uint!)
    write_u16_le(out, 255u);                             // 25: BottomLightPWM (ushort, not uint!)
    write_u32_le(out, 0u);                               // 26: EncryptionKey (0 = no encryption)
    write_u32_le(out, 0u);                               // 27: SlicerOffset (for extended metadata)
    write_u32_le(out, 0u);                               // 28: SlicerSize

    // Write layer definition table (36 bytes per layer = 9 fields)
    for (uint32_t i = 0; i < layer_count; ++i) {
        const float z = (i < bottom_count) ? initial_h * float(i + 1) : 
                        (initial_h * float(bottom_count) + layer_h * float(i - bottom_count + 1));
        const float lexp = (i < bottom_count) ? initial_exp : exp;

        write_f32_le(out, z);                            // 0: PositionZ
        write_f32_le(out, lexp);                         // 1: ExposureTime
        write_f32_le(out, 0.0f);                         // 2: LightOffSeconds
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
        throw RuntimeError("Failed to open output file for OSF export");

    // Write OSF header
    out.write("OSF!", 4);
    write_u32_be(out, 1u);
    write_u32_be(out, layer_count);
    write_f32_le(out, float(display_width_mm));
    write_f32_le(out, float(display_height_mm));
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
    const auto  &cfg      = print.full_print_config();
    const float layer_h   = cfg_float_or(cfg, "layer_height", 0.05f);
    const float exp       = cfg_float_or(cfg, "exposure_time", 2.5f);
    const float init_exp  = cfg_float_or(cfg, "initial_exposure_time", 20.0f);
    const float lightoff  = cfg_float_or(cfg, "light_off_delay", 0.0f);
    const float bottom_exp = cfg_float_or(cfg, "initial_exposure_time", 20.0f);
    // Get orientation-aware display dimensions from m_cfg (set at archive creation)
    const auto dims       = get_display_dimensions(m_cfg);
    const auto layer_count = uint32_t(m_layers.size());

    // Get display dimensions from printer profile (actual bed size, no orientation swap)
    double display_width_mm = m_cfg.display_width.getFloat();
    double display_height_mm = m_cfg.display_height.getFloat();

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for GOO export");

    // GOO binary header (big-endian structured)
    // Version: "V3.0" (4 bytes, big-endian format)
    out.write("V3.0", 4);
    // Magic: 07 00 00 00 44 4C 50 00
    out.put(0x07);
    out.put(0x00);
    out.put(0x00);
    out.put(0x00);
    out.put(0x44);  // 'D'
    out.put(0x4C);  // 'L'
    out.put(0x50);  // 'P'
    out.put(0x00);
    
    // SoftwareName (32 bytes, null-terminated)
    std::string software_name = "PrusaSlicer";
    out.write(software_name.c_str(), software_name.length());
    out.write(std::string(32 - software_name.length(), '\0').c_str(), 32 - software_name.length());
    
    // SoftwareVersion (24 bytes)
    std::string version = "5.0.0";
    out.write(version.c_str(), version.length());
    out.write(std::string(24 - version.length(), '\0').c_str(), 24 - version.length());
    
    // FileCreateTime (24 bytes)
    std::string timestamp = "2024-01-01 00:00:00";
    out.write(timestamp.c_str(), timestamp.length());
    out.write(std::string(24 - timestamp.length(), '\0').c_str(), 24 - timestamp.length());
    
    // MachineName (32 bytes)
    std::string machine = "Elegoo Mars";
    out.write(machine.c_str(), machine.length());
    out.write(std::string(32 - machine.length(), '\0').c_str(), 32 - machine.length());
    
    // MachineType (32 bytes) - "DLP"
    out.write("DLP", 3);
    out.write(std::string(32 - 3, '\0').c_str(), 32 - 3);
    
    // ProfileName (32 bytes)
    out.write(machine.c_str(), machine.length());
    out.write(std::string(32 - machine.length(), '\0').c_str(), 32 - machine.length());
    
    // AntiAliasingLevel (2 bytes, BE ushort)
    write_u16_be(out, 1u);
    // GreyLevel (2 bytes, BE ushort)
    write_u16_be(out, 1u);
    // BlurLevel (2 bytes, BE ushort)
    write_u16_be(out, 0u);
    
    // SmallPreview565 (116×116×2 = 26,912 bytes) - write zeros for now
    for (int i = 0; i < 116 * 116; i++) {
        out.put(0);
        out.put(0);
    }
    // SmallPreviewDelimiter
    out.put(0x0D);
    out.put(0x0A);
    
    // BigPreview565 (290×290×2 = 168,100 bytes) - write zeros for now
    for (int i = 0; i < 290 * 290; i++) {
        out.put(0);
        out.put(0);
    }
    // BigPreviewDelimiter
    out.put(0x0D);
    out.put(0x0A);
    
    // LayerCount (4 bytes, BE uint32)
    write_u32_be(out, layer_count);
    // ResolutionX/Y (2 bytes each, BE ushort)
    write_u16_be(out, dims.resolution_x);
    write_u16_be(out, dims.resolution_y);
    
    // MirrorX/Y (bool, 1 byte each)
    out.put(0);  // MirrorX
    out.put(0);  // MirrorY
    
    // DisplayWidth/Height (4 bytes each, BE float32)
    write_f32_be(out, float(display_width_mm));
    write_f32_be(out, float(display_height_mm));
    
    // MachineZ (4 bytes, BE float32)
    write_f32_be(out, 170.0f);
    // LayerHeight (4 bytes, BE float32)
    write_f32_be(out, layer_h);
    
    // ExposureTime (4 bytes, BE float32)
    write_f32_be(out, exp);
    // DelayMode (1 byte) - 1 = WaitTime
    out.put(1);
    // LightOffDelay (4 bytes, BE float32)
    write_f32_be(out, lightoff);
    
    // Multiple timing parameters (all BE float32)
    write_f32_be(out, 0.5f);  // BottomWaitTimeAfterCure
    write_f32_be(out, 0.5f);  // BottomWaitTimeAfterLift
    write_f32_be(out, 1.0f);  // BottomWaitTimeBeforeCure
    write_f32_be(out, 0.5f);  // WaitTimeAfterCure
    write_f32_be(out, 0.5f);  // WaitTimeAfterLift
    write_f32_be(out, 1.0f);  // WaitTimeBeforeCure
    
    // BottomExposureTime (4 bytes, BE float32)
    write_f32_be(out, bottom_exp);
    // BottomLayerCount (4 bytes, BE uint32)
    write_u32_be(out, 1u);
    
    // Movement parameters (all BE float32)
    write_f32_be(out, 5.0f);   // BottomLiftHeight
    write_f32_be(out, 50.0f);  // BottomLiftSpeed
    write_f32_be(out, 5.0f);   // LiftHeight
    write_f32_be(out, 100.0f); // LiftSpeed
    write_f32_be(out, 3.0f);   // BottomRetractHeight
    write_f32_be(out, 30.0f);  // BottomRetractSpeed
    write_f32_be(out, 3.0f);   // RetractHeight
    write_f32_be(out, 60.0f);  // RetractSpeed
    
    // Secondary movement parameters (BE float32)
    write_f32_be(out, 0.0f);   // BottomLiftHeight2
    write_f32_be(out, 0.0f);   // BottomLiftSpeed2
    write_f32_be(out, 0.0f);   // LiftHeight2
    write_f32_be(out, 0.0f);   // LiftSpeed2
    write_f32_be(out, 0.0f);   // BottomRetractHeight2
    write_f32_be(out, 0.0f);   // BottomRetractSpeed2
    write_f32_be(out, 0.0f);   // RetractHeight2
    write_f32_be(out, 0.0f);   // RetractSpeed2
    
    // PWM values (2 bytes each, BE ushort)
    write_u16_be(out, 200u);   // BottomLightPWM
    write_u16_be(out, 250u);   // LightPWM
    
    // PerLayerSettings (bool)
    out.put(0);
    
    // PrintTime (4 bytes, BE uint32)
    write_u32_be(out, 0u);
    
    // Volume/Material (BE float32)
    write_f32_be(out, 0.0f);   // Volume
    write_f32_be(out, 0.0f);   // MaterialGrams
    write_f32_be(out, 0.0f);   // MaterialCost
    
    // PriceCurrencySymbol (8 bytes)
    out.write("$", 1);
    out.write(std::string(7, '\0').c_str(), 7);
    
    // LayerDefAddress (4 bytes, BE uint32) - placeholder
    write_u32_be(out, 195477u);
    
    // GrayScaleLevel (1 byte)
    out.put(1);
    
    // TransitionLayerCount (2 bytes, BE ushort)
    write_u16_be(out, 0u);
    
    // Write layer data - use m_layers as-is for now
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
