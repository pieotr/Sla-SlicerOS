#include "libslic3r/SLA/ExportPrinterValidation.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "CrealityCXDLPv4.hpp"
#include "CrealityChecksum.hpp"
#include "CrealityCXDLPv4Counts.hpp"
#include "ChituboxPreview.hpp"
#include "CrealityContourArea.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/CrealityCXDLPv4FreshExportPlan.hpp"
#include <unordered_map>
#include <string_view>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>

#include <boost/log/trivial.hpp>

#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
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

// Final native row values are shared by serialization and metadata estimation.
// The timing view follows the reference's two-decimal public layer settings;
// it does not round or alter the stored motion/exposure values.
struct CXDLPv4LayerSettings {
    float z, exposure, light_off;
    std::array<float, 11> extended;
};
float cxdlp_round2(float value) { return std::nearbyint(value * 100.f) / 100.f; }
float cxdlp_layer_seconds(const CXDLPv4LayerSettings &row,
                          const std::array<float, 4> &fallback_speeds, bool fresh = false)
{
    auto round2 = [&](float value) { return fresh ? sla::cxdlpv4_public_round2(value) : cxdlp_round2(value); };
    const auto &e = row.extended;
    for (float value : e)
        if (!std::isfinite(value) || value < 0)
            throw ExportError("CXDLPv4 layer settings must be finite and nonnegative");
    if (!std::isfinite(row.exposure) || row.exposure < 0 ||
        !std::isfinite(row.light_off) || row.light_off < 0 || e[5] > e[0])
        throw ExportError("Invalid CXDLPv4 exposure, light-off or retract distance");
    const float first = round2(round2(e[0]) - e[2]);
    const float second = round2(e[2]);
    if (first < 0)
        throw ExportError("CXDLPv4 first lift cannot be represented in the timing view");
    const float total = fresh ? round2(first + second) : first + second;
    const float retract2 = std::clamp(round2(e[5]), 0.f, total);
    float motor = 0.f;
    auto phase = [&](float distance, float speed, size_t fallback_index) {
        if (distance <= 0) return;
        float represented_speed = round2(speed);
        // Layer setters use raw global values when rounded speed is zero.
        // This projection changes only the estimate, never the stored speed.
        if (represented_speed <= 0) represented_speed = fallback_speeds[fallback_index];
        if (!std::isfinite(represented_speed) || represented_speed <= 0)
            throw ExportError("CXDLPv4 moving phase requires a finite positive reference speed");
        motor += distance / (represented_speed / 60.f);
    };
    phase(first, e[1], 0); phase(second, e[3], 1);
    phase(retract2, e[6], 3); phase(fresh ? round2(total - retract2) : total - retract2, e[4], 2);
    const float rests = round2(e[9]) + round2(row.exposure) +
        round2(e[7]) + round2(e[8]);
    return rests + std::max(round2(motor), round2(row.light_off));
}

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

struct CXDLPv4RasterEncoder
{
    sla::EncodedRaster operator()(const void *ptr,
                                  size_t      w,
                                  size_t      h,
                                  size_t      num_components) const
    {
        if(!ptr || num_components!=1 || !w || !h || w>32768 || h>32768 || uint64_t(w)*h>(uint64_t(128)<<20))
            throw ExportError("CXDLPv4 requires a grayscale mask within the raster memory limits");
        const uint8_t *src = reinterpret_cast<const uint8_t *>(ptr);
        const size_t n = w * h;

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
                if (++stride == 8) {
                    // The low bit is not represented in this codec. Skip
                    // complete equal runs eight pixels at a time, retaining
                    // the exact seven-bit palette and original RLE boundaries.
                    // memcpy permits unaligned input without aliasing or
                    // endianness assumptions; never read beyond the raster.
                    constexpr uint64_t low_bits = UINT64_C(0x0101010101010101);
                    const uint64_t expected = uint64_t(color << 1) * low_bits;
                    size_t next = i + 1;
                    while (n - next >= sizeof(uint64_t)) {
                        uint64_t pixels;
                        std::memcpy(&pixels, src + next, sizeof(pixels));
                        if ((pixels & ~low_bits) != expected)
                            break;
                        next += sizeof(pixels);
                    }
                    stride += uint32_t(next - i - 1);
                    i = next - 1;
                }
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

sla::EncodedRaster CrealityCXDLPv4Archive::encode_mask(const void *pixels,size_t width,size_t height,size_t components)
{
    return CXDLPv4RasterEncoder{}(pixels,width,height,components);
}

std::vector<uint8_t> CrealityCXDLPv4Archive::decode_mask(const uint8_t *data,size_t length,size_t width,size_t height)
{
    if(!data || !width || !height || width>32768 || height>32768 || uint64_t(width)*height>(uint64_t(128)<<20))
        throw RuntimeError("CXDLPv4 mask exceeds the raster limits");
    std::vector<uint8_t> pixels(width*height,0);size_t position=0;
    for(size_t cursor=0;cursor<length;) {
        const uint8_t code=data[cursor++];uint32_t run=1;
        if(code&128) {
            if(cursor==length)throw RuntimeError("Missing CXDLPv4 run length");
            const uint8_t prefix=data[cursor++];unsigned continuation;
            if(prefix<128) {run=prefix;continuation=0;}
            else if(prefix<192) {run=prefix&63;continuation=1;}
            else if(prefix<224) {run=prefix&31;continuation=2;}
            else if(prefix<240) {run=prefix&15;continuation=3;}
            else throw RuntimeError("Invalid CXDLPv4 run prefix");
            if(continuation>length-cursor)throw RuntimeError("Truncated CXDLPv4 run");
            while(continuation--)run=(run<<8)|data[cursor++];
        }
        if(!run || run>pixels.size()-position)throw RuntimeError("CXDLPv4 run exceeds the mask");
        const auto gray=uint8_t(code&127);
        std::fill_n(pixels.begin()+position,run,gray?uint8_t((gray<<1)|1):uint8_t(0));position+=run;
    }
    if(position!=pixels.size())throw RuntimeError("CXDLPv4 mask is incomplete");
    return pixels;
}

void CrealityCXDLPv4Archive::crypt_mask(std::vector<uint8_t> &data,uint32_t seed,uint32_t layer_index)
{
    if(!seed)return;
    // Original UVtools LayerRleCryptBuffer, with unsigned 32-bit wraparound.
    const uint32_t increment=seed*0x2d83cdacu+0xd8a83423u;
    uint32_t key=(layer_index*0x1e1530cdu+0xec3d47cdu)*increment;
    for(size_t i=0;i<data.size();++i) {
        data[i]^=uint8_t(key>>(8*(i%4)));
        if(i%4==3)key+=increment;
    }
}

std::unique_ptr<sla::RasterBase> CrealityCXDLPv4Archive::create_raster() const
{
    return sla::create_printer_raster(m_cfg);
}

sla::RasterEncoder CrealityCXDLPv4Archive::get_encoder() const
{
    return encode_mask;
}

void CrealityCXDLPv4Archive::export_print(const std::string fname, const SLAPrint &print,
    const ThumbnailsList &thumbnails, const std::string &)
{
    export_print_impl(fname, print, thumbnails, nullptr);
}

void CrealityCXDLPv4Archive::export_fresh_print(const std::string &fname, const SLAPrint &print,
    const ThumbnailsList &thumbnails, const sla::CXDLPv4FreshExportPlan &fresh)
{
    export_print_impl(fname, print, thumbnails, &fresh);
}

void CrealityCXDLPv4Archive::export_fresh_print_with_encoding(const std::string &fname,
    const SLAPrint &print, const ThumbnailsList &thumbnails,
    const sla::CXDLPv4FreshExportPlan &fresh, const std::vector<sla::EncodedRaster> &encoding)
{
    export_print_impl(fname, print, thumbnails, &fresh, &encoding);
}

void CrealityCXDLPv4Archive::export_print_impl(const std::string &fname, const SLAPrint &print,
    const ThumbnailsList &thumbnails, const sla::CXDLPv4FreshExportPlan *fresh,
    const std::vector<sla::EncodedRaster> *encoding)
{
    const auto &layers = encoding ? *encoding : m_layers;
    const uint32_t layer_count = uint32_t(layers.size());
    if (layer_count == 0 || layer_count>100000 || layer_count!=print.print_layers().size())
        throw ExportError("CXDLPv4 layer count is outside the supported limits");

    const auto &cfg = print.full_print_config();
    std::array<float, 2> fresh_material{};
    if (fresh) {
        const auto &material = print.material_config();
        fresh_material = sla::cxdlpv4_fresh_material_metrics(fresh->material_ml,
            material.material_density.value, material.bottle_cost.value, material.bottle_volume.value);
    }
    using F = sla::LayerParameter;
    if (fresh && fresh->rows.size() != layer_count)
        throw ExportError("CXDLPv4 fresh rows do not match the encoded masks");
    auto global_parameter = [&](const char *key, bool bottom) -> float {
        if (!fresh) return sla::motion_parameter(print, key, bottom);
        static const std::map<std::string, F> fields{
            {"lift_height",F::LiftDistance},{"lift_height2",F::LiftDistance2},
            {"lift_speed",F::LiftSpeed},{"lift_speed2",F::LiftSpeed2},
            {"retract_speed",F::RetractSpeed},{"retract_height2",F::RetractDistance2},
            {"retract_speed2",F::RetractSpeed2},{"wait_before_cure",F::WaitBefore},
            {"wait_after_cure",F::WaitAfter},{"wait_after_lift",F::WaitAfterLift},{"light_pwm",F::LightPWM}};
        return float((bottom ? fresh->globals.bottom : fresh->globals.normal).at(fields.at(key)));
    };

    double w = m_cfg.display_width.getFloat();
    double h = m_cfg.display_height.getFloat();
    uint32_t res_x = uint32_t(m_cfg.display_pixels_x.getInt());
    uint32_t res_y = uint32_t(m_cfg.display_pixels_y.getInt());
    if (m_cfg.display_orientation.getInt() == sla::RasterBase::roPortrait) {
        std::swap(w, h);
        std::swap(res_x, res_y);
    }

    if (fresh) {
        // Physical material accounting uses the same float32 dimensions stored on wire.
        w = double(float(fresh->geometry.width_mm)); h = double(float(fresh->geometry.height_mm));
        res_x = uint32_t(fresh->geometry.columns); res_y = uint32_t(fresh->geometry.rows);
    }
    const float layer_height = fresh ? float(fresh->globals.layer_height) : float(cfg.opt_float("layer_height"));
    const uint32_t bottom_layers = fresh ? uint32_t(fresh->globals.bottom_layers) : uint32_t(sla::bottom_layers(print, layer_count));
    const float bottom_exposure = fresh ? float(fresh->globals.bottom.at(F::Exposure)) : float(cfg.opt_float("initial_exposure_time"));
    const float normal_exposure = fresh ? float(fresh->globals.normal.at(F::Exposure)) : float(cfg.opt_float("exposure_time"));

    // Profile values use the same mm/min units as the UVtools specification.
    const float bottom_lift_h = global_parameter("lift_height", true);
    const float bottom_lift_s = global_parameter("lift_speed", true);
    const float lift_h = global_parameter("lift_height", false);
    const float lift_s = global_parameter("lift_speed", false);
    const float retract_s = global_parameter("retract_speed", false);

    std::vector<CXDLPv4LayerSettings> settings;
    settings.reserve(layer_count);
    float complete_time = 300.f; // UVtools FileFormat.ExtraPrintTime.
    for (size_t i = 0; i < layer_count; ++i) {
        const bool bottom = i < bottom_layers;
        auto parameter = [&](const char *key) { return global_parameter(key, bottom); };
        CXDLPv4LayerSettings row{sla::layer_z(print, i), sla::layer_exposure(print, i), 0.f,
            {parameter("lift_height") + parameter("lift_height2"), parameter("lift_speed"),
             parameter("lift_height2"), parameter("lift_speed2"), parameter("retract_speed"),
             parameter("retract_height2"), parameter("retract_speed2"), parameter("wait_after_cure"),
             parameter("wait_after_lift"), parameter("wait_before_cure"), parameter("light_pwm")}};
        if (fresh) {
            const auto &values = fresh->rows[i];
            auto v = [&](F field) { return float(values.at(field)); };
            row = {v(F::PositionZ),v(F::Exposure),v(F::LightOff),
                {float(values.at(F::LiftDistance)+values.at(F::LiftDistance2)),v(F::LiftSpeed),
                 v(F::LiftDistance2),v(F::LiftSpeed2),v(F::RetractSpeed),v(F::RetractDistance2),
                 v(F::RetractSpeed2),v(F::WaitAfter),v(F::WaitAfterLift),v(F::WaitBefore),v(F::LightPWM)}};
        }
        // CXDLP has no stored bottom retract globals. During a new decode
        // they remain FileFormat defaults (100/80) until AFTER row CopyTo.
        // Lift and normal retract fallback getters expose raw native globals.
        const std::array<float, 4> fallback_speeds{
            parameter("lift_speed"), parameter("lift_speed2"),
            bottom ? 100.f : retract_s,
            bottom ? 80.f : global_parameter("retract_speed2", false)};
        complete_time += cxdlp_layer_seconds(row, fallback_speeds, fresh != nullptr);
        settings.push_back(row);
    }
    const auto counts = sla::cxdlpv4_generated_counts(layer_count,
        fresh ? int(fresh->globals.bottom_layers) : print.material_config().sla_bottom_layers.value,
        fresh ? int(fresh->globals.transition_layers) : print.material_config().sla_transition_layers.value,
        [&](size_t i) { return settings[i].exposure; });
    complete_time = fresh ? sla::cxdlpv4_public_round2(complete_time) : cxdlp_round2(complete_time);
    if (!std::isfinite(complete_time) || complete_time < 0 || double(complete_time) > UINT32_MAX)
        throw ExportError("CXDLPv4 complete print time exceeds its uint32 range");
    const uint32_t stored_print_time = uint32_t(complete_time); // Reference truncates positive seconds.

    const std::string printer_model = m_cfg.printer_model.value;
    sla::require_cxdlpv4_printer_identifier(printer_model,fresh!=nullptr);
    const auto *document=print.layer_document();
    if(!document || document->layer_count()!=layer_count)
        throw ExportError("CXDLPv4 requires its canonical layer masks for geometry metadata");
    struct Geometry {uint32_t area=0,nonzero=0;};
    std::unordered_map<std::string_view,size_t> geometry_cache;
    std::vector<size_t> representative(layer_count),unique;
    std::vector<Geometry> geometries(layer_count);
    std::vector<uint32_t> areas(layer_count);
    const float pitch_x=float(std::round(float(w)/res_x*10000.f)/10000.f),pitch_y=float(std::round(float(h)/res_y*10000.f)/10000.f);
    const double volume_pixel_area=w/res_x*h/res_y;
    for(size_t i=0;i<layer_count;++i) {
        const auto png=document->encoded_layer(i);
        const std::string_view key(static_cast<const char *>(png.buf),png.sz);
        const auto found=geometry_cache.find(key);
        if(found!=geometry_cache.end()) representative[i]=found->second;
        else {
            representative[i]=i;unique.push_back(i);
            if(geometry_cache.size()<1024)geometry_cache.emplace(key,i);
        }
    }
    const int workers=int(std::clamp<uint64_t>((uint64_t(512)<<20)/
        std::max<uint64_t>(1,uint64_t(res_x)*res_y*4),1,8));
    tbb::task_arena arena(workers);
    arena.execute([&] { execution::for_each(ExecutionTBB{},size_t(0),unique.size(),[&](size_t index) {
        const size_t i=unique[index];
        const auto mask=document->image(i);
        if(mask.cols!=res_x || mask.rows!=res_y)throw ExportError("CXDLPv4 mask dimensions differ from the printer");
        auto &geometry=geometries[i];
        // Trace only the occupied rectangle, as UVtools does. Empty screen
        // margins must not be flooded/allocated for every individual layer.
        // Translation leaves contour area unchanged; gray 1 is dropped by RLE.
        size_t left=mask.cols,right=0,top=mask.rows,bottom=0;
        for(size_t y=0;y<mask.rows;++y) {
            const auto begin=mask.buf.begin()+y*mask.cols,end=begin+mask.cols;
            const auto first=std::find_if(begin,end,[](uint8_t value){return value>=2;});
            if(first==end)continue;
            const auto last=std::find_if(std::make_reverse_iterator(end),std::make_reverse_iterator(first),
                                         [](uint8_t value){return value>=2;});
            // The already located outer span contains every exposed pixel
            // of this row. Count there instead of scanning all screen margins
            // again in a separate full-image pass. Gray 1 still quantizes to 0.
            geometry.nonzero+=uint32_t(std::count_if(first,last.base(),[](uint8_t value){return value>=2;}));
            left=std::min(left,size_t(first-begin));right=std::max(right,size_t(last.base()-begin));
            top=std::min(top,y);bottom=y+1;
        }
        if(top==mask.rows)return;
        const size_t columns=right-left,rows=bottom-top;
        std::vector<uint8_t> cropped(columns*rows);
        for(size_t y=0;y<rows;++y)
            std::transform(mask.buf.begin()+(top+y)*mask.cols+left,mask.buf.begin()+(top+y)*mask.cols+right,
                           cropped.begin()+y*columns,[](uint8_t value){return value>=2?value:uint8_t(0);});
        const double area=sla::creality_largest_contour_area(cropped.data(),columns,rows)*float(pitch_x*pitch_y)*1000.;
        if(!std::isfinite(area) || area>UINT32_MAX)throw ExportError("CXDLPv4 contour area exceeds 32 bits");
        geometry.area=uint32_t(area);
    }); });
    double volume_mm3=0,previous_z=0;
    sla::ExposurePositionValidator positions(fresh ? fresh->position_policy : sla::ExposurePositionPolicy::IncreasingLayers);
    for(size_t i=0;i<layer_count;++i) {
        const auto &geometry=geometries[representative[i]];
        areas[i]=geometry.area;
        const double z=fresh ? double(settings[i].z) : double(print.print_layers()[i].level())*SCALING_FACTOR;
        positions.append(z);
        volume_mm3+=geometry.nonzero*volume_pixel_area*(z-previous_z);previous_z=z;
    }

    // The validated owned plan accounts for the union of all masks at one Z.
    // The ordinary count above remains the fast path for increasing model layers.
    if (fresh && fresh->position_policy == sla::ExposurePositionPolicy::RepeatedExposureHeights)
        volume_mm3 = double(fresh->material_ml) * 1000.;

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
    static constexpr uint32_t PREVIEW_HEADER_SIZE = 32;  // 8 uint32s per UVtools Preview schema

    const auto preview_small_data=encode_chitubox_rgb15(PREVIEW_SMALL_WIDTH,PREVIEW_SMALL_HEIGHT,thumbnails);
    const auto preview_large_data=encode_chitubox_rgb15(PREVIEW_LARGE_WIDTH,PREVIEW_LARGE_HEIGHT,thumbnails);
    const uint32_t preview_small_image_size=uint32_t(preview_small_data.size());
    const uint32_t preview_large_image_size=uint32_t(preview_large_data.size());

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
    
    for (size_t i = 0; i < layers.size(); ++i) {
        const sla::EncodedRaster &rst = layers[i];
        if(rst.size()>UINT32_MAX-LAYER_DEF_EX_SIZE)throw ExportError("CXDLPv4 layer data exceeds 32-bit offsets");
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
        
        if(uint64_t(data_blob_offset)+total_layer_data_size+layer_blob+slicer_size+4>UINT32_MAX)
            throw ExportError("CXDLPv4 file exceeds 32-bit offsets");
        data_sizes.push_back(layer_blob);
        total_layer_data_size += layer_blob;
    }
    
    // Calculate slicer offset
    slicer_offset = data_blob_offset + total_layer_data_size;
    
    // Verify encoded layer count matches expected layer_count
    if (layers.size() != size_t(layer_count)) {
        throw RuntimeError(
            std::string("Mismatch: layers.size()=") + 
            std::to_string(layers.size()) + " but layer_count=" +
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
        write_f32_le(out, fresh ? settings.back().z : sla::layer_z(print, layer_count-1));
        write_f32_le(out, layer_height);
        write_u32_le(out, counts.bottom);
        write_u32_le(out, preview_small_offset);  // preview small offset
        write_u32_le(out, layer_defs_offset);
        write_u32_le(out, layer_count);
        write_u32_le(out, preview_large_offset);  // preview large offset
        write_u32_le(out, stored_print_time);
        write_u32_le(out, 0); // Canonical masks already contain printer mirroring.
        write_u32_le(out, print_params_offset);
        write_u32_le(out, uint32_t(PRINT_PARAMS_SIZE));
        write_u32_le(out, 1); // anti alias
        write_u16_le(out, uint16_t(global_parameter("light_pwm", false)));
        write_u16_le(out, uint16_t(global_parameter("light_pwm", true)));
        write_u32_le(out, 0); // encryption key
        write_u32_le(out, slicer_offset);  // slicer address - NOW WITH CORRECT OFFSET
        write_u32_le(out, slicer_size);    // slicer size

        // Generate preview images from thumbnail data (or black if no thumbnails)

        // Write Preview sections with encoded image data
        // PreviewSmall header
        write_u32_le(out, PREVIEW_SMALL_WIDTH);
        write_u32_le(out, PREVIEW_SMALL_HEIGHT);
        write_u32_le(out, preview_small_offset + PREVIEW_HEADER_SIZE);  // ImageOffset (after header)
        write_u32_le(out, uint32_t(preview_small_data.size()));  // ImageLength
        for(unsigned field=0;field<4;++field)write_u32_le(out,0);
        
        // Write PreviewSmall image data
        out.write(reinterpret_cast<const char*>(preview_small_data.data()), preview_small_data.size());
        
        // PreviewLarge header
        write_u32_le(out, PREVIEW_LARGE_WIDTH);
        write_u32_le(out, PREVIEW_LARGE_HEIGHT);
        write_u32_le(out, preview_large_offset + PREVIEW_HEADER_SIZE);  // ImageOffset (after header)
        write_u32_le(out, uint32_t(preview_large_data.size()));  // ImageLength
        for(unsigned field=0;field<4;++field)write_u32_le(out,0);
        
        // Write PreviewLarge image data
        out.write(reinterpret_cast<const char*>(preview_large_data.data()), preview_large_data.size());

        // Print parameters section
        // Seven-bit encoding drops gray 1. Volume and contours must describe
        // the mask actually sent to the printer, including any user edits.
        const float material_ml = float(volume_mm3 / 1000.0);
        write_f32_le(out, fresh ? float(fresh->globals.bottom.at(F::LiftDistance) + fresh->globals.bottom.at(F::LiftDistance2))
                               : bottom_lift_h + global_parameter("lift_height2", true));
        write_f32_le(out, bottom_lift_s);
        write_f32_le(out, fresh ? float(fresh->globals.normal.at(F::LiftDistance) + fresh->globals.normal.at(F::LiftDistance2))
                               : lift_h + global_parameter("lift_height2", false));
        write_f32_le(out, lift_s);
        write_f32_le(out, retract_s);
        write_f32_le(out, material_ml);
        write_f32_le(out, fresh ? fresh_material[0] : float(material_ml * print.material_config().material_density.value));
        write_f32_le(out, fresh ? fresh_material[1] : print.material_config().bottle_volume.value > 0 ?
            float(material_ml * print.material_config().bottle_cost.value / print.material_config().bottle_volume.value) : 0.f);
        write_f32_le(out, fresh ? float(fresh->globals.bottom.at(F::LightOff)) : 0.f); // bottom light off
        write_f32_le(out, fresh ? float(fresh->globals.normal.at(F::LightOff)) : 0.f); // light off
        write_u32_le(out, counts.bottom);
        write_f32_le(out, normal_exposure);
        write_f32_le(out, bottom_exposure);
        write_u32_le(out, 0);
        write_u32_le(out, 0);
        write_u32_le(out, 0);
        write_u32_le(out, 0);

        // Layer definitions table
        uint32_t cur_data_offset = data_blob_offset;
        for (uint32_t i = 0; i < layer_count; ++i) {
            write_f32_le(out, settings[i].z);
            write_f32_le(out, settings[i].exposure);
            write_f32_le(out, settings[i].light_off);
            write_u32_le(out, cur_data_offset);
            write_u32_le(out, data_sizes[size_t(i)]);
            write_u32_le(out, 0); // datatype
            write_u32_le(out, 0); // centroid
            write_u32_le(out, areas[i]); // largest external contour area in mm² × 1000
            write_u32_le(out, 0);
            write_u32_le(out, 0);

            cur_data_offset += data_sizes[size_t(i)];
        }

        // Per-layer extended definitions + payload
        for (uint32_t i = 0; i < layer_count; ++i) {
            for (float value : settings[i].extended)
                write_f32_le(out, value);

            const sla::EncodedRaster &rst = layers[size_t(i)];
            out.write(reinterpret_cast<const char *>(rst.data()), std::streamsize(rst.size()));
        }

        // Write Slicer Info section (76 bytes)
        write_f32_le(out, global_parameter("lift_height2", true));
        write_f32_le(out, global_parameter("lift_speed2", true));
        write_f32_le(out, global_parameter("lift_height2", false));
        write_f32_le(out, global_parameter("lift_speed2", false));
        write_f32_le(out, global_parameter("retract_height2", false));
        write_f32_le(out, global_parameter("retract_speed2", false));
        write_f32_le(out, global_parameter("wait_after_lift", false));
        write_u32_le(out, 1); // Enable the per-layer extended parameters.
        write_u32_le(out, uint32_t(std::time(nullptr) / 60)); // UTC minutes since epoch
        write_u32_le(out, 1); // AntiAliasLevel
        write_u32_le(out, 0); // SoftwareVersion
        write_f32_le(out, global_parameter("wait_before_cure", false));
        write_f32_le(out, global_parameter("wait_after_cure", false));
        write_f32_le(out, normal_exposure);
        write_f32_le(out, bottom_exposure);
        write_f32_le(out, global_parameter("wait_after_lift", fresh ? false : true));
        write_u32_le(out, counts.transition);
        write_u32_le(out, 0);
        write_u32_le(out, 0);

        out.flush();
        if (!out) throw RuntimeError("Cannot finish writing CXDLPv4 data");
        out.close();

        std::ifstream in(fname, std::ios::binary | std::ios::in);
        if (!in)
            throw RuntimeError("Failed to reopen CXDLPv4 output file for checksum");

        const uint32_t checksum = sla::creality_checksum(in,4);
        in.close();

        std::ofstream out_append(fname, std::ios::binary | std::ios::out | std::ios::app);
        write_u32_be(out_append, checksum);
        out_append.flush();
        if (!out_append) throw RuntimeError("Cannot write CXDLPv4 checksum");
    } catch (std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << e.what();
        throw;
    }
}

} // namespace Slic3r
