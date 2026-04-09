///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "CrealityCXDLPv4.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>

#include <boost/log/trivial.hpp>

#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/RasterBase.hpp"

namespace Slic3r {

namespace {

static constexpr uint32_t MAGIC_SIZE = 9;
static constexpr const char MAGIC[] = "CXSW3DV2";
static constexpr uint16_t CXDLPV4_VERSION = 4;

static constexpr size_t LAYER_DEF_SIZE =
    3 * sizeof(float) + // z, exposure, light off
    7 * sizeof(uint32_t); // addr, size, type, centroid, area, unk1, unk2

static constexpr size_t LAYER_DEF_EX_SIZE =
    11 * sizeof(float);

static constexpr size_t PRINT_PARAMS_SIZE =
    10 * sizeof(float) +
    1 * sizeof(uint32_t) +
    2 * sizeof(float) +
    4 * sizeof(uint32_t);

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
    std::streamoff remaining = size;
    while (remaining > 0 && in) {
        const std::streamsize chunk = std::streamsize(std::min<std::streamoff>(remaining, std::streamoff(buf.size())));
        in.read(buf.data(), chunk);
        const std::streamsize got = in.gcount();
        if (got <= 0)
            break;

        for (std::streamsize i = 0; i < got; ++i) {
            const uint8_t idx = uint8_t(checksum) ^ uint8_t(buf[size_t(i)]);
            checksum = table[idx] ^ (checksum >> 8);
        }
        remaining -= got;
    }

    return checksum;
}

// RGB15 RLE Encoder for CXDLPV4 preview images
// Encodes RGB15 (5-5-5 + 1 reserved bit) pixels with RLE compression
// Black image: all pixels are 0x0000
std::vector<uint8_t> encode_cxdlpv4_rgb15_rle(uint32_t width, uint32_t height)
{
    std::vector<uint8_t> rle;

    // Generate a completely black image (0x0000 for all RGB15 pixels)
    // RGB15 format: XRRRRRGGGGGBBBBB (where X is reserved/unused)
    // Black = 0x0000 (all color components zero)
    
    // Total pixel count
    const uint32_t pixel_count = width * height;
    
    // RLE encoding: each pixel is 2 bytes (RGB15 little-endian)
    // For a completely black image, we can compress it as:
    // Single RLE entry: color=0x0000, run-length=pixel_count
    
    // RLE entry format for CXDLPV4:
    // [run-length-encoded entry]
    // First byte: color or length indicator
    // Subsequent bytes: color data and/or extended length
    
    // Simple approach: write each pixel as 2 bytes
    // For black: 0x00 0x00 per pixel = 2*pixel_count bytes uncompressed
    // RLE will compress this significantly
    
    // Write all black pixels as RGB15 (little-endian 0x0000)
    for (uint32_t i = 0; i < pixel_count; ++i) {
        rle.push_back(0x00);  // Low byte of RGB15 black
        rle.push_back(0x00);  // High byte of RGB15 black
    }
    
    return rle;
}



struct CXDLPv4RasterEncoder
{
    sla::EncodedRaster operator()(const void *ptr,
                                  size_t      w,
                                  size_t      h,
                                  size_t      num_components) const
    {
        const uint8_t *src = reinterpret_cast<const uint8_t *>(ptr);
        const size_t n = w * h * num_components;

        std::vector<uint8_t> out;
        out.reserve(n / 2 + 1024);

        uint8_t color = uint8_t(0xFF >> 1);
        uint32_t stride = 0;

        auto flush_run = [&out, &color, &stride]() {
            if (stride == 0)
                return;

            uint8_t code = color;
            if (stride > 1)
                code |= 0x80;

            out.push_back(code);
            if (stride <= 1)
                return;

            if (stride <= 0x7F) {
                out.push_back(uint8_t(stride));
            } else if (stride <= 0x3FFF) {
                out.push_back(uint8_t((stride >> 8) | 0x80));
                out.push_back(uint8_t(stride));
            } else if (stride <= 0x1FFFFF) {
                out.push_back(uint8_t((stride >> 16) | 0xC0));
                out.push_back(uint8_t(stride >> 8));
                out.push_back(uint8_t(stride));
            } else {
                out.push_back(uint8_t((stride >> 24) | 0xE0));
                out.push_back(uint8_t(stride >> 16));
                out.push_back(uint8_t(stride >> 8));
                out.push_back(uint8_t(stride));
            }
        };

        for (size_t i = 0; i < n; ++i) {
            const uint8_t grey7 = uint8_t(src[i] >> 1);
            if (grey7 == color) {
                ++stride;
            } else {
                flush_run();
                color = grey7;
                stride = 1;
            }
        }
        flush_run();

        return sla::EncodedRaster(std::move(out), "cxdlpv4rle");
    }
};

} // namespace

std::unique_ptr<sla::RasterBase> CrealityCXDLPv4Archive::create_raster() const
{
    sla::Resolution     res;
    sla::PixelDim       pxdim;
    std::array<bool, 2> mirror;

    double w  = m_cfg.display_width.getFloat();
    double h  = m_cfg.display_height.getFloat();
    auto   pw = size_t(m_cfg.display_pixels_x.getInt());
    auto   ph = size_t(m_cfg.display_pixels_y.getInt());

    mirror[X] = m_cfg.display_mirror_x.getBool();
    mirror[Y] = m_cfg.display_mirror_y.getBool();

    auto                         ro = m_cfg.display_orientation.getInt();
    sla::RasterBase::Orientation orientation =
        ro == sla::RasterBase::roPortrait ? sla::RasterBase::roPortrait : sla::RasterBase::roLandscape;

    if (orientation == sla::RasterBase::roPortrait) {
        std::swap(w, h);
        std::swap(pw, ph);
    }

    res   = sla::Resolution{pw, ph};
    pxdim = sla::PixelDim{w / pw, h / ph};
    sla::RasterBase::Trafo tr{orientation, mirror};

    double gamma = m_cfg.gamma_correction.getFloat();
    return sla::create_raster_grayscale_aa(res, pxdim, gamma, tr);
}

sla::RasterEncoder CrealityCXDLPv4Archive::get_encoder() const
{
    return CXDLPv4RasterEncoder{};
}

void CrealityCXDLPv4Archive::export_print(const std::string     fname,
                                          const SLAPrint       &print,
                                          const ThumbnailsList &thumbnails,
                                          const std::string    &)
{
    const uint32_t layer_count = uint32_t(m_layers.size());
    if (layer_count == 0)
        throw RuntimeError("No rasterized layers available for CXDLPv4 export");

    const auto &cfg = print.full_print_config();

    double w = m_cfg.display_width.getFloat();
    double h = m_cfg.display_height.getFloat();
    uint32_t res_x = uint32_t(m_cfg.display_pixels_x.getInt());
    uint32_t res_y = uint32_t(m_cfg.display_pixels_y.getInt());
    if (m_cfg.display_orientation.getInt() == sla::RasterBase::roPortrait) {
        std::swap(w, h);
        std::swap(res_x, res_y);
    }

    const float layer_height = float(cfg.opt_float("layer_height"));
    const uint32_t bottom_layers = uint32_t(std::max(0, cfg.opt_int("faded_layers")));
    const float bottom_exposure = float(cfg.opt_float("initial_exposure_time"));
    const float normal_exposure = float(cfg.opt_float("exposure_time"));

    // Conservative defaults in machine units (mm/min), accepted by UVtools parser.
    const float bottom_lift_h = 6.0f;
    const float bottom_lift_s = 60.0f;
    const float lift_h = 6.0f;
    const float lift_s = 80.0f;
    const float retract_s = 150.0f;

    const std::string printer_model = m_cfg.printer_model.value.empty() ? "CL-89" : m_cfg.printer_model.value;

    // Header layout per UVtools CrealityCXDLPv4File:
    // MagicSize (4) + Magic (9) + Version (2) + PrinterModel (4+len+1) +
    // ResX (2) + ResY (2) + BedX (4) + BedY (4) + BedZ (4) + PrintHeight (4) +
    // LayerHeight (4) + BottomLayersCount (4) + PreviewSmallOff (4) +
    // LayersDefOff (4) + LayerCount (4) + PreviewLargeOff (4) +
    // PrintTime (4) + ProjectorType (4) + PrintParamsOff (4) + PrintParamsSize (4) +
    // AntiAlias (4) + LightPWM (2) + BottomLightPWM (2) + EncKey (4) +
    // SlicerAddr (4) + SlicerSize (4)
    const uint32_t header_size =
        4 + 9 + 2 + // magic size + magic + version
        4 + uint32_t(printer_model.size() + 1) + // be length + nt string
        2 + 2 + // resolution x, y (ushort each)
        5 * 4 + // bed x/y/z, print height, layer height (float)
        10 * 4 + // bottom layers..anti alias (uint32)
        2 + 2 + // light pwm fields (ushort each)
        3 * 4; // 3 final uint32 fields

    // Preview dimensions for CXDLPV4
    static constexpr uint32_t PREVIEW_SMALL_WIDTH = 120;
    static constexpr uint32_t PREVIEW_SMALL_HEIGHT = 120;
    static constexpr uint32_t PREVIEW_LARGE_WIDTH = 300;
    static constexpr uint32_t PREVIEW_LARGE_HEIGHT = 300;
    static constexpr uint32_t PREVIEW_HEADER_SIZE = 16;  // 4 uint32s per preview header

    // Pre-calculate preview image sizes (black RGB15 images)
    // RGB15 = 2 bytes per pixel, no RLE compression for simplicity
    const uint32_t preview_small_image_size = PREVIEW_SMALL_WIDTH * PREVIEW_SMALL_HEIGHT * 2;  // RGB15: 2 bytes per pixel
    const uint32_t preview_large_image_size = PREVIEW_LARGE_WIDTH * PREVIEW_LARGE_HEIGHT * 2;  // RGB15: 2 bytes per pixel

    // Calculate offsets with actual preview image data
    const uint32_t preview_small_offset = header_size;
    const uint32_t preview_large_offset = preview_small_offset + PREVIEW_HEADER_SIZE + preview_small_image_size;
    const uint32_t print_params_offset = preview_large_offset + PREVIEW_HEADER_SIZE + preview_large_image_size;
    const uint32_t layer_defs_offset = print_params_offset + uint32_t(PRINT_PARAMS_SIZE);

    std::vector<uint32_t> data_sizes;
    data_sizes.reserve(layer_count);
    uint32_t data_blob_offset = layer_defs_offset + uint32_t(LAYER_DEF_SIZE * layer_count);
    
    // Calculate slicer section offset (after all layer data)
    uint32_t total_layer_data_size = 0;
    uint32_t slicer_offset = 0;  // Will be calculated after knowing all layer sizes
    static constexpr uint32_t slicer_size = 76;  // Fixed slicer info size
    
    BOOST_LOG_TRIVIAL(debug) << "[CXDLPv4] Starting data_sizes calculation for " << layer_count << " layers";
    BOOST_LOG_TRIVIAL(debug) << "[CXDLPv4] data_blob_offset=" << data_blob_offset << ", LAYER_DEF_EX_SIZE=" << LAYER_DEF_EX_SIZE;
    BOOST_LOG_TRIVIAL(debug) << "[CXDLPv4] preview_small size=" << preview_small_image_size << " preview_large size=" << preview_large_image_size;
    
    for (size_t i = 0; i < m_layers.size(); ++i) {
        const sla::EncodedRaster &rst = m_layers[i];
        const uint32_t rle_size = uint32_t(rst.size());
        const uint32_t layer_blob = uint32_t(LAYER_DEF_EX_SIZE + rle_size);
        
        BOOST_LOG_TRIVIAL(debug) << "[CXDLPv4] Layer " << i << ": RLE size=" << rle_size << ", total blob=" << layer_blob;
        
        // Validate that each layer has at least the LayerDefEx header
        if (layer_blob < uint32_t(LAYER_DEF_EX_SIZE)) {
            throw RuntimeError(
                std::string("Layer ") + std::to_string(i) + 
                " has invalid data size (" + std::to_string(layer_blob) + 
                "), expected at least " + std::to_string(LAYER_DEF_EX_SIZE) + " bytes");
        }
        
        data_sizes.push_back(layer_blob);
        total_layer_data_size += layer_blob;
    }
    
    // Calculate slicer offset
    slicer_offset = data_blob_offset + total_layer_data_size;
    
    // Verify m_layers count matches expected layer_count
    if (m_layers.size() != size_t(layer_count)) {
        throw RuntimeError(
            std::string("Mismatch: m_layers.size()=") + 
            std::to_string(m_layers.size()) + " but layer_count=" +
            std::to_string(layer_count));
    }
    
    BOOST_LOG_TRIVIAL(debug) << "[CXDLPv4] All data_sizes calculated successfully";

    try {
        std::ofstream out(fname, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!out)
            throw RuntimeError("Failed to open output file for CXDLPv4 export");

        // Header
        write_u32_be(out, MAGIC_SIZE);
        const char magic_with_null[9] = {'C','X','S','W','3','D','V','2','\0'};
        out.write(magic_with_null, 9);
        write_u16_be(out, CXDLPV4_VERSION);
        write_be_len_nt_string(out, printer_model);

        write_u16_le(out, uint16_t(res_x & 0xFFFF));
        write_u16_le(out, uint16_t(res_y & 0xFFFF));
        write_f32_le(out, float(w));
        write_f32_le(out, float(h));
        write_f32_le(out, float(m_cfg.max_print_height.getFloat()));
        write_f32_le(out, float(layer_count) * layer_height);
        write_f32_le(out, layer_height);
        write_u32_le(out, std::min(bottom_layers, layer_count));
        write_u32_le(out, preview_small_offset);  // preview small offset
        write_u32_le(out, layer_defs_offset);
        write_u32_le(out, layer_count);
        write_u32_le(out, preview_large_offset);  // preview large offset
        write_u32_le(out, uint32_t(print.print_statistics().estimated_print_time));
        write_u32_le(out, 1); // projector type
        write_u32_le(out, print_params_offset);
        write_u32_le(out, uint32_t(PRINT_PARAMS_SIZE));
        write_u32_le(out, 1); // anti alias
        write_u16_le(out, 255); // light pwm (ushort)
        write_u16_le(out, 255); // bottom light pwm (ushort)
        write_u32_le(out, 0); // encryption key
        write_u32_le(out, slicer_offset);  // slicer address - NOW WITH CORRECT OFFSET
        write_u32_le(out, slicer_size);    // slicer size

        // Generate black preview images (RGB15 format, uncompressed for now)
        auto preview_small_data = encode_cxdlpv4_rgb15_rle(PREVIEW_SMALL_WIDTH, PREVIEW_SMALL_HEIGHT);
        auto preview_large_data = encode_cxdlpv4_rgb15_rle(PREVIEW_LARGE_WIDTH, PREVIEW_LARGE_HEIGHT);

        // Write Preview sections with encoded image data
        // PreviewSmall header
        write_u32_le(out, PREVIEW_SMALL_WIDTH);
        write_u32_le(out, PREVIEW_SMALL_HEIGHT);
        write_u32_le(out, preview_small_offset + PREVIEW_HEADER_SIZE);  // ImageOffset (after header)
        write_u32_le(out, uint32_t(preview_small_data.size()));  // ImageLength
        
        // Write PreviewSmall image data
        out.write(reinterpret_cast<const char*>(preview_small_data.data()), preview_small_data.size());
        
        // PreviewLarge header
        write_u32_le(out, PREVIEW_LARGE_WIDTH);
        write_u32_le(out, PREVIEW_LARGE_HEIGHT);
        write_u32_le(out, preview_large_offset + PREVIEW_HEADER_SIZE);  // ImageOffset (after header)
        write_u32_le(out, uint32_t(preview_large_data.size()));  // ImageLength
        
        // Write PreviewLarge image data
        out.write(reinterpret_cast<const char*>(preview_large_data.data()), preview_large_data.size());

        // Print parameters section
        const SLAPrintStatistics stats = print.print_statistics();
        const float material_ml = float((stats.objects_used_material + stats.support_used_material) / 1000.0);
        write_f32_le(out, bottom_lift_h);
        write_f32_le(out, bottom_lift_s);
        write_f32_le(out, lift_h);
        write_f32_le(out, lift_s);
        write_f32_le(out, retract_s);
        write_f32_le(out, material_ml);
        write_f32_le(out, 0.0f); // grams
        write_f32_le(out, 0.0f); // cost
        write_f32_le(out, 0.0f); // bottom light off
        write_f32_le(out, 0.0f); // light off
        write_u32_le(out, std::min(bottom_layers, layer_count));
        write_f32_le(out, normal_exposure);
        write_f32_le(out, bottom_exposure);
        write_u32_le(out, 0);
        write_u32_le(out, 0);
        write_u32_le(out, 0);
        write_u32_le(out, 0);

        // Layer definitions table
        uint32_t cur_data_offset = data_blob_offset;
        for (uint32_t i = 0; i < layer_count; ++i) {
            const bool is_bottom = i < std::min(bottom_layers, layer_count);
            write_f32_le(out, (float(i) + 1.0f) * layer_height); // position z
            write_f32_le(out, is_bottom ? bottom_exposure : normal_exposure);
            write_f32_le(out, 0.0f); // light off
            write_u32_le(out, cur_data_offset);
            write_u32_le(out, data_sizes[size_t(i)]);
            write_u32_le(out, 0); // datatype
            write_u32_le(out, 0); // centroid
            write_u32_le(out, 0); // largest area
            write_u32_le(out, 0);
            write_u32_le(out, 0);

            cur_data_offset += data_sizes[size_t(i)];
        }

        // Per-layer extended definitions + payload
        for (uint32_t i = 0; i < layer_count; ++i) {
            const bool is_bottom = i < std::min(bottom_layers, layer_count);
            write_f32_le(out, is_bottom ? bottom_lift_h : lift_h);
            write_f32_le(out, is_bottom ? bottom_lift_s : lift_s);
            write_f32_le(out, 0.0f); // lift height 2
            write_f32_le(out, 0.0f); // lift speed 2
            write_f32_le(out, retract_s);
            write_f32_le(out, 0.0f); // retract height 2
            write_f32_le(out, 0.0f); // retract speed 2
            write_f32_le(out, 0.0f); // rest before lift
            write_f32_le(out, 0.0f); // rest after lift
            write_f32_le(out, 0.0f); // rest after retract
            write_f32_le(out, 255.0f); // light pwm

            const sla::EncodedRaster &rst = m_layers[size_t(i)];
            out.write(reinterpret_cast<const char *>(rst.data()), std::streamsize(rst.size()));
        }

        // Write Slicer Info section (76 bytes)
        write_u32_le(out, 0); // Checksum placeholder
        write_f32_le(out, 300.0f); // RestTime fields
        write_f32_le(out, 0.0f);
        write_f32_le(out, 300.0f);
        write_f32_le(out, 0.0f);
        write_f32_le(out, 80.0f);
        write_f32_le(out, 0.0f);
        write_f32_le(out, 0.0f);
        write_u32_le(out, uint32_t(std::time(nullptr))); // Timestamp
        write_u32_le(out, 0); // PerLayerSettings
        for (int j = 0; j < 8; ++j) {
            write_u32_le(out, 0); // Padding
        }
        write_f32_le(out, normal_exposure);
        write_f32_le(out, bottom_exposure);
        write_u32_le(out, 0);
        write_u32_le(out, 0);

        out.flush();
        out.close();

        std::ifstream in(fname, std::ios::binary | std::ios::in);
        if (!in)
            throw RuntimeError("Failed to reopen CXDLPv4 output file for checksum");

        in.seekg(0, std::ios::end);
        const std::streamoff file_size = in.tellg();
        const uint32_t checksum = crc32_uvtools_style(in, file_size);
        in.close();

        std::ofstream out_append(fname, std::ios::binary | std::ios::out | std::ios::app);
        write_u32_be(out_append, checksum);
    } catch (std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << e.what();
        throw;
    }
}

} // namespace Slic3r
