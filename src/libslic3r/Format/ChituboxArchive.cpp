///|/ Copyright (c) Prusa Research 2026
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher.
#include "ChituboxArchive.hpp"
#include "ChituboxRasterEncoder.hpp"
#include "CTBEncryptedArchive.hpp"
#include "CTBEncryptedCrypto.hpp"
#include "ChituboxPreview.hpp"
#include <ctime>
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/CBDDLPFreshExportPlan.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include "libslic3r/SLA/CTBEncryptedMotion.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>
#include <boost/uuid/detail/sha1.hpp>

namespace Slic3r {
namespace {
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

float cfg_float_or(const DynamicPrintConfig &cfg, const std::string &key, float fallback)
{
    return cfg.has(key) ? cfg.opt_float(key) : fallback;
}
} // namespace


namespace {
uint32_t read_le32(const uint8_t *bytes)
{
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}
void store_le32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (i * 8));
}
struct CbddlpRasterEncoder {
    unsigned planes;
    bool canonical=false;
    sla::EncodedRaster operator()(const void *data, size_t width, size_t height, size_t components) const
    {
        if (!data || !width || !height || width > 32768 || height > 32768 || components != 1)
            throw ExportError("CBDDLP requires a nonempty grayscale mask up to 32768 pixels per axis");
        const auto *pixels = static_cast<const uint8_t *>(data);
        const size_t pixel_count = width * height;
        const size_t packet_header = 8 + 4 * planes;
        std::vector<uint8_t> packet(packet_header, 0);
        uint64_t exposed = 0;
        for (size_t i = 0; i < pixel_count; ++i) if (pixels[i] >= (planes==1?255:256/planes-1)) ++exposed;
        store_le32(packet.data(), uint32_t(exposed));
        store_le32(packet.data() + 4, uint32_t(exposed >> 32));
        // Internal packet only: exposed count, lengths, then RLE exposure planes.
        // The exporter strips this bookkeeping; only the native bitplanes reach
        // the printer file. Plane zero uses 255; subsequent planes use
        // floor(256 / AA) * plane - 1, exactly as the original encoder.
        for (unsigned plane = 0; plane < planes; ++plane) {
            const uint8_t threshold = plane ? uint8_t((256/planes) * plane - 1) : uint8_t(255);
            const size_t begin = packet.size();
            auto white=[&](uint8_t value) {
                if(!canonical)return value>=threshold;
                const unsigned count=sla::mask_plane_count(value,planes);
                return plane ? count>=plane : count==planes;
            };
            for (size_t pixel = 0; pixel < pixel_count;) {
                const bool exposed_pixel = white(pixels[pixel]);
                size_t end = pixel + 1;
                while (end < pixel_count && end - pixel < 125 && white(pixels[end]) == exposed_pixel)
                    ++end;
                packet.push_back(uint8_t((exposed_pixel ? 0x80 : 0) | (end - pixel)));
                pixel = end;
            }
            if (packet.size() - begin > UINT32_MAX) throw ExportError("CBDDLP bitplane exceeds 4 GiB");
            store_le32(packet.data() + 8 + 4 * plane, uint32_t(packet.size() - begin));
        }
        return sla::EncodedRaster(std::move(packet), "cbddlp");
    }
};
struct LayerPayload {
    const uint8_t *bytes;
    uint32_t size;
    uint32_t address = 0;
    bool write = true;
};
} // namespace

unsigned UVToolsChituboxArchive::cbddlp_plane_count() const {
    if (m_version == 1) return 1;
    if (m_version != 2 || m_cfg.sla_cbddlp_aa.value < 1 || m_cfg.sla_cbddlp_aa.value > 16)
        throw ExportError("CBDDLP/Photon v2 requires 1–16 AA planes");
    return unsigned(m_cfg.sla_cbddlp_aa.value);
}

// CTB v1/v2 share grayscale RLE; CBDDLP/Photon use versioned binary planes.
sla::RasterEncoder UVToolsChituboxArchive::get_encoder() const
{
    if (m_magic == 0x12fd0019) return CbddlpRasterEncoder{cbddlp_plane_count()};
    if (m_magic == 0x12fd0086) return chitubox::Gray7RasterEncoder{};
    throw ExportError("This container requires a distinct codec; ordinary CTB v2 cannot replace it");
}

sla::RasterEncoder UVToolsChituboxArchive::get_document_encoder(const sla::LayerDocument &document) const
{
    const auto &fresh=document.fresh_job();
    if(m_magic==0x12fd0019 && fresh && sla::mask_bitplanes(fresh->mask_precision))
        return CbddlpRasterEncoder{cbddlp_plane_count(),true};
    return get_encoder();
}

void UVToolsChituboxArchive::export_print(const std::string fname,const SLAPrint &print,
                                         const ThumbnailsList &thumbnails,const std::string &)
{
    export_print_impl(fname,print,thumbnails,nullptr);
}
void UVToolsChituboxArchive::export_fresh_print(const std::string &fname,const SLAPrint &print,
                                             const ThumbnailsList &thumbnails,const sla::CBDDLPFreshExportPlan &fresh)
{
    if(m_magic!=0x12fd0019 || m_version!=2)throw ExportError("CBDDLP fresh plan requires the binary v2 schema");
    fresh.source.validate(m_layers.size());
    export_print_impl(fname,print,thumbnails,&fresh);
}
void UVToolsChituboxArchive::export_print_impl(const std::string &fname,const SLAPrint &print,
                                             const ThumbnailsList &thumbnails,const sla::CBDDLPFreshExportPlan *fresh)
{
    if (m_magic != 0x12fd0019 && m_magic != 0x12fd0086)
        throw ExportError("This container requires a distinct codec; ordinary CTB v2 cannot replace it");
    if(m_version!=1 && m_version!=2)
        throw ExportError("Unsupported native Chitubox container version");
    const bool legacy_v1=m_version==1;
    if(!fresh && m_magic==0x12fd0019) ChituboxCBDDLPArchive::validate_configuration(print,m_version);
    else if(legacy_v1)ChituboxCTBv1Archive::validate_configuration(print);
    std::string lower_filename = fname;
    std::transform(lower_filename.begin(), lower_filename.end(), lower_filename.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    const std::string encrypted_suffix = ".encrypted.ctb";
    if (lower_filename.size() >= encrypted_suffix.size() &&
        lower_filename.compare(lower_filename.size() - encrypted_suffix.size(), encrypted_suffix.size(), encrypted_suffix) == 0)
        throw ExportError("Ordinary Chitubox v2 cannot be exported as encrypted CTB v5");
    const bool bitplanes = m_magic == 0x12fd0019;
    const unsigned plane_count = bitplanes ? cbddlp_plane_count() : 1;
    if(!fresh)sla::require_single_stage_motion(print, bitplanes ? "CBDDLP/Photon v2" : "CTB v2", true);
    const auto  &cfg          = print.full_print_config();
    const float layer_h       = fresh ? fresh->nominal_height : cfg_float_or(cfg, "layer_height", 0.05f);
    const float exp           = fresh ? fresh->default_exposure[0] : cfg_float_or(cfg, "exposure_time", 2.5f);
    const float initial_exp   = fresh ? fresh->default_exposure[1] : cfg_float_or(cfg, "initial_exposure_time", 20.0f);
    const auto   bottom_count = fresh ? fresh->bottom_layers : uint32_t(sla::bottom_layers(print, m_layers.size()));

    const uint32_t layer_count = uint32_t(m_layers.size());
    if (layer_count == 0)
        throw RuntimeError("No rasterized layers available for Chitubox export");

    // Get orientation-aware display dimensions and resolution
    auto dims = get_display_dimensions(m_cfg);
    if(fresh) {
        dims.resolution_x=fresh->geometry.columns;dims.resolution_y=fresh->geometry.rows;
        dims.display_width_mm=fresh->geometry.width_mm;dims.display_height_mm=fresh->geometry.height_mm;
    }
    // CTB v2 has light-off delay, not v4 rest-time fields. Its delay includes
    // the complete lift/retract cycle; storing only the requested rest would
    // cause the firmware to consume that rest while the platform is moving.
    auto light_off_delay = [&](bool bottom) {
        if(fresh)return fresh->default_light_off[bottom?1:0];
        const double height = fresh ? fresh->lift_height[bottom?1:0] : sla::motion_parameter(print, "lift_height", bottom);
        const double lift_speed = fresh ? fresh->lift_speed[bottom?1:0] : sla::motion_parameter(print, "lift_speed", bottom);
        const double retract_speed = sla::motion_parameter(print, "retract_speed", bottom);
        if (height > 0 && (lift_speed <= 0 || retract_speed <= 0))
            throw ExportError("CTB enabled lift and retract require positive speeds");
        const double movement = height > 0 ? height * 60. / lift_speed + height * 60. / retract_speed : 0.;
        return float(std::round((movement + sla::motion_parameter(print, "wait_before_cure", bottom)) * 100.) / 100.);
    };

    // Get printer bed dimensions (full dimensions, not just usable display area)
    double bed_size_x = dims.display_width_mm;
    double bed_size_y = dims.display_height_mm;
    double bed_size_z = m_cfg.max_print_height.value;

    // CTB v1/v2 header: 112 bytes; v1 omits the following print parameters.
    const uint32_t header_size = 112u;
    // LayerDef: 36 bytes (9 fields: 3 float + 6 uint = 12 + 24 = 36)
    const uint32_t layer_def_size = 36u;
    const uint32_t print_parameters_size = legacy_v1 ? 0u : 60u;
    // All Chitubox v1/v2 variants carry the renderer previews. Addresses in
    // the header and layer table account for both variable-length payloads.
    const auto preview_large = encode_chitubox_rgb15(400, 300, thumbnails);
    const auto preview_small = encode_chitubox_rgb15(200, 125, thumbnails);
    const uint32_t preview_large_offset = header_size + print_parameters_size;
    const uint32_t preview_small_offset = preview_large_offset + 32 + uint32_t(preview_large.size());
    const uint32_t layer_defs_offset = preview_small_offset + 32 + uint32_t(preview_small.size());
    if (uint64_t(layer_defs_offset) + uint64_t(layer_def_size) * layer_count * plane_count > UINT32_MAX ||
        m_layers.size() != print.print_layers().size())
        throw ExportError("Chitubox layer table exceeds the v2 schema limits");
    const uint32_t layer_data_offset = layer_defs_offset + layer_def_size * layer_count * plane_count;
    std::vector<LayerPayload> payloads(size_t(layer_count) * plane_count);
    double volume_mm3 = 0., previous_z = 0.;
    const uint64_t expected_pixels = uint64_t(dims.resolution_x) * dims.resolution_y;
    const double pixel_area = fresh ? dims.display_width_mm/dims.resolution_x*dims.display_height_mm/dims.resolution_y :
        dims.display_width_mm * dims.display_height_mm / expected_pixels;
    for (uint32_t i = 0; i < layer_count; ++i) {
        uint64_t exposed = 0;
        const auto &raster = m_layers[i];
        const auto *bytes = static_cast<const uint8_t *>(raster.data());
        if (bitplanes) {
            if (raster.size() < 8+4*plane_count) throw ExportError("Incomplete CBDDLP internal raster packet");
            exposed = read_le32(bytes) | (uint64_t(read_le32(bytes + 4)) << 32);
            if (exposed > expected_pixels) throw ExportError("Invalid CBDDLP exposed pixel count");
            size_t offset = 8+4*plane_count;
            for (unsigned plane = 0; plane < plane_count; ++plane) {
                const uint32_t size = read_le32(bytes + 8 + 4 * plane);
                if (size > raster.size() - offset) throw ExportError("Incomplete CBDDLP bitplane");
                uint64_t pixels = 0;
                for (size_t n = 0; n < size; ++n) {
                    const unsigned run = bytes[offset + n] & 0x7f;
                    if (!run || pixels + run > expected_pixels) throw ExportError("Invalid CBDDLP bitplane run");
                    pixels += run;
                }
                if (pixels != expected_pixels) throw ExportError("CBDDLP bitplane dimensions do not match the printer");
                payloads[size_t(plane) * layer_count + i] = {bytes + offset, size};
                offset += size;
            }
            if (offset != raster.size()) throw ExportError("Unexpected CBDDLP raster packet bytes");
        } else {
            if (raster.size() > UINT32_MAX) throw ExportError("CTB layer exceeds 4 GiB");
            payloads[i] = {bytes, uint32_t(raster.size())};
            exposed = chitubox::exposed_pixels(raster, expected_pixels);
        }
        const double z = fresh ? double(fresh->rows[i].z) : double(print.print_layers()[i].level()) * SCALING_FACTOR;
        volume_mm3 += exposed * pixel_area * (z - previous_z);
        previous_z = z;
    }
    // CBDDLP firmware permits repeated bitplanes to share data addresses.
    // Bound the index; verify byte equality after hashing, without copying the
    // payloads or ever merging their independent Z/exposure table entries.
    std::map<std::array<unsigned, 5>, std::vector<size_t>> hashes;
    uint64_t current_data = layer_data_offset;
    for (size_t i = 0; i < payloads.size(); ++i) {
        auto &payload = payloads[i];
        if (bitplanes) {
            boost::uuids::detail::sha1 hash;
            hash.process_bytes(payload.bytes, payload.size);
            unsigned digest[5]; hash.get_digest(digest);
            const std::array<unsigned, 5> key{digest[0], digest[1], digest[2], digest[3], digest[4]};
            const auto found = hashes.find(key);
            if (found != hashes.end()) {
                for (const size_t candidate : found->second) {
                    const auto &previous = payloads[candidate];
                    if (previous.size == payload.size && !std::memcmp(previous.bytes, payload.bytes, payload.size)) {
                        payload.address = previous.address; payload.write = false; break;
                    }
                }
            }
            if (payload.write && hashes.size() < 1024) hashes[key].push_back(i);
        }
        if (!payload.write) continue;
        if (current_data + payload.size > UINT32_MAX)
            throw ExportError("Chitubox v2 export exceeds the supported 4 GiB file size");
        payload.address = uint32_t(current_data);
        current_data += payload.size;
    }

    // Calculate total model height
    const float total_height = fresh ? fresh->rows.back().z : sla::layer_z(print, layer_count-1);

    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    if (!out)
        throw RuntimeError("Failed to open output file for Chitubox export");

    // Write header (112 bytes) following exact UVtools ChituboxFile.cs field order:
    // FieldOrder 0-28 in precise sequence
    write_u32_le(out, m_magic);                          // 0: Magic
    write_u32_le(out, m_version);                        // 1: Explicit v1/v2, without v3+ extension blocks
    write_f32_le(out, float(bed_size_x));                // 2: BedSizeX (mm)
    write_f32_le(out, float(bed_size_y));                // 3: BedSizeY (mm)
    write_f32_le(out, float(bed_size_z));                // 4: BedSizeZ (mm)
    write_u32_le(out, 0u);                               // 5: Unknown1
    write_u32_le(out, 0u);                               // 6: Unknown2
    write_f32_le(out, total_height);                     // 7: TotalHeightMillimeter
    write_f32_le(out, layer_h);                          // 8: LayerHeightMillimeter
    write_f32_le(out, exp);                              // 9: LayerExposureSeconds
    write_f32_le(out, initial_exp);                      // 10: BottomExposureSeconds
    write_f32_le(out, light_off_delay(false));           // 11: Normal movement plus rest
    write_u32_le(out, bottom_count);                     // 12: BottomLayersCount
    write_u32_le(out, dims.resolution_x);               // 13: ResolutionX (pixels)
    write_u32_le(out, dims.resolution_y);               // 14: ResolutionY (pixels)
    write_u32_le(out, preview_large_offset);             // 15: RGB15 large preview record
    write_u32_le(out, layer_defs_offset);                // 16: LayersDefinitionOffsetAddress
    write_u32_le(out, layer_count);                      // 17: LayerCount
    write_u32_le(out, preview_small_offset);             // 18: RGB15 small preview record
    write_u32_le(out, fresh ? fresh->estimated_print_time : sla::lcd_print_time(print));        // 19: Estimated print time
    // This describes the assumed projection; the pixels are already oriented.
    // Match the format's normal/mirrored flag without transforming them again.
    write_u32_le(out, (m_cfg.display_mirror_x.value || m_cfg.display_mirror_y.value) ? 1u : 0u);
    write_u32_le(out, legacy_v1 ? 0u : header_size);      // 21: v1 has no print-parameters block
    // The original encoder declares the schema size even when v1 omits it.
    write_u32_le(out, 60u);                             // 22: PrintParametersSize
    write_u32_le(out, plane_count);                      // 23: CBDDLP bitplanes, CTB grayscale
    write_u16_le(out, (fresh ? fresh->pwm[0] : uint16_t(sla::motion_parameter(print, "light_pwm", false))));
    write_u16_le(out, (fresh ? fresh->pwm[1] : uint16_t(sla::motion_parameter(print, "light_pwm", true))));
    write_u32_le(out, 0u);                               // 26: EncryptionKey (0 = no encryption)
    write_u32_le(out, 0u);                               // 27: SlicerOffset (for extended metadata)
    write_u32_le(out, 0u);                               // 28: SlicerSize

    // UVtools ChituboxFile.PrintParameters, fields 0..14 (v2 only).
    if(!legacy_v1) {
    for (const bool bottom : {true, false}) {
        write_f32_le(out, fresh ? fresh->lift_height[bottom?1:0] : sla::motion_parameter(print, "lift_height", bottom));
        write_f32_le(out, fresh ? fresh->lift_speed[bottom?1:0] : sla::motion_parameter(print, "lift_speed", bottom));
    }
    write_f32_le(out, (fresh ? fresh->retract_speed : sla::motion_parameter(print, "retract_speed", false)));
    const auto &material = print.material_config();
    const float volume = float(volume_mm3 / 1000.);
    const auto totals=fresh ? sla::cbddlp_material_totals(volume_mm3/1000.,material.material_density.value,
        material.bottle_cost.value,material.bottle_volume.value) : std::array<float,3>{volume,
        float(volume*material.material_density.value),material.bottle_volume.value>0?float(volume*material.bottle_cost.value/material.bottle_volume.value):0.f};
    for(float total:totals)write_f32_le(out,total);
    write_f32_le(out, light_off_delay(true));
    write_f32_le(out, light_off_delay(false));
    write_u32_le(out, bottom_count);
    for (unsigned i = 0; i < 4; ++i) write_u32_le(out, 0);
    }

    {
        auto preview=[&](uint32_t at,uint32_t width,uint32_t height,const std::vector<uint8_t> &pixels) {
            write_u32_le(out,width);write_u32_le(out,height);
            write_u32_le(out,at+32);write_u32_le(out,uint32_t(pixels.size()));
            for(unsigned n=0;n<4;++n)write_u32_le(out,0);
            out.write(reinterpret_cast<const char *>(pixels.data()),pixels.size());
        };
        preview(preview_large_offset,400,300,preview_large);
        preview(preview_small_offset,200,125,preview_small);
    }

    // Write layer definition table (36 bytes per layer = 9 fields)
    for (unsigned plane = 0; plane < plane_count; ++plane)
    for (uint32_t i = 0; i < layer_count; ++i) {
        const auto &payload = payloads[size_t(plane) * layer_count + i];
        const float z = fresh ? fresh->rows[i].z : sla::layer_z(print, i);
        const float lexp = fresh ? fresh->rows[i].exposure : sla::layer_exposure(print, i);

        write_f32_le(out, z);                            // 0: PositionZ
        write_f32_le(out, lexp);                         // 1: ExposureTime
        write_f32_le(out, fresh ? fresh->rows[i].light_off : light_off_delay(i < bottom_count));
        write_u32_le(out, payload.address);              // 3: DataAddress (layer RLE offset)
        write_u32_le(out, payload.size); // 4: DataSize (layer RLE size)
        write_u32_le(out, 0u);                           // 5: PageNumber
        write_u32_le(out, 36u);                          // 6: TableSize (36 = sizeof LayerDef)
        write_u32_le(out, 0u);                           // 7: Unknown3
        write_u32_le(out, 0u);                           // 8: Unknown4
    }

    // Write layer raster data
    for (const auto &payload : payloads)
        if (payload.write) out.write(reinterpret_cast<const char *>(payload.bytes), payload.size);

    out.flush();
    if (!out) throw RuntimeError("Cannot finish writing CTB data");
    out.close();
}

void ChituboxCBDDLPArchive::validate_configuration(const SLAPrint &print, unsigned version) {
    if (version!=1 && version!=2) throw ExportError("CBDDLP/Photon requires version 1 or 2");
    sla::require_single_stage_motion(print,"CBDDLP/Photon",true);
    if (version == 1) ChituboxCTBv1Archive::validate_configuration(print);
    else {
        const auto aa=print.printer_config().sla_cbddlp_aa.value;
        if (aa<1 || aa>16) throw ExportError("CBDDLP/Photon v2 requires 1–16 AA planes");
    }
}

void ChituboxCTBv1Archive::validate_configuration(const SLAPrint &print) {
    sla::require_single_stage_motion(print,"Chitubox v1",true);
    // The original v1 encoder omits PrintParameters. These are its decoder
    // defaults, not a claim about any printer's firmware-side movement.
    // In particular its normal LiftHeight defaults to DefaultLayerHeight=.05.
    // Reject custom values rather than silently dropping them on export.
    for(const bool bottom:{false,true}) {
        for(const char *key:{"lift_height","lift_speed","retract_speed"}) {
            const double value=sla::motion_parameter(print,key,bottom);
            const double expected=std::string(key)=="lift_height"?(bottom?6.:.05):100.;
            if(!std::isfinite(value)||std::abs(value-expected)>1e-6)
                throw ExportError("Chitubox v1 cannot store custom lift/retract settings; select version 2 or newer");
        }
    }
}
namespace chitubox_detail {
void u32(std::vector<uint8_t> &data,size_t offset,uint32_t value) {
    for(unsigned n=0;n<4;++n)data.at(offset+n)=uint8_t(value>>(8*n));
}
void f32(std::vector<uint8_t> &data,size_t offset,float value) {
    uint32_t bits;std::memcpy(&bits,&value,4);u32(data,offset,bits);
}
std::string name(const SLAPrint &print,const char *key,const std::string &fallback="") {
    const auto *value=print.full_print_config().option<ConfigOptionString>(key);
    return value&&!value->value.empty()?value->value:fallback;
}
void validate_name(const std::string &name) {
    if(name.size()>4096)throw ExportError("CTB metadata name exceeds 4096 UTF-8 bytes");
    for(unsigned char byte:name)if(byte<32)throw ExportError("CTB names cannot contain control characters");
}
}

std::unique_ptr<sla::RasterBase> CTBEncryptedArchive::create_raster() const {return sla::create_printer_raster(m_cfg);}
sla::RasterEncoder CTBEncryptedArchive::get_encoder() const {return chitubox::Gray7RasterEncoder{};}
namespace chitubox_detail {
void validate_parameters(const SLAPrint &print, const std::string &format) {
    const auto &cfg=print.printer_config();const auto &material=print.material_config();
    if(!std::isfinite(float(cfg.display_width.value))||!std::isfinite(float(cfg.display_height.value))||
       cfg.display_width.value<=0||cfg.display_height.value<=0||cfg.display_pixels_x.value<=0||cfg.display_pixels_y.value<=0||
       cfg.display_pixels_x.value>32768||cfg.display_pixels_y.value>32768||
       uint64_t(cfg.display_pixels_x.value)*cfg.display_pixels_y.value>(uint64_t(128)<<20)||
       !std::isfinite(float(cfg.max_print_height.value))||cfg.max_print_height.value<=0)
        throw ExportError(("Invalid " + format + " printer geometry"));
    if(cfg.display_mirror_y.value)throw ExportError((format + " stores only no mirror or horizontal mirror"));
    for(double value:{print.default_object_config().layer_height.value,material.initial_layer_height.value})
        if(!std::isfinite(float(value))||value<.001||std::abs(std::round(value*1000)/1000-value)>1e-7)
            throw ExportError((format + " layer heights require positive millimetres with 0.001 mm precision"));
    for(double value:{material.exposure_time.value,material.initial_exposure_time.value})
        if(!std::isfinite(float(value))||value<0||std::abs(std::round(value*100)/100-value)>1e-6)
            throw ExportError((format + " exposure requires nonnegative seconds with 0.01 s precision"));
    for(int value:{material.sla_bottom_layers.value,material.sla_transition_layers.value})
        if(value<0||value>UINT16_MAX)throw ExportError((format + " layer counts exceed uint16 decoder"));
    for(bool bottom:{false,true}) {
        for(const char *key:{"lift_height","lift_height2","lift_speed","lift_speed2","retract_height2","retract_speed","retract_speed2",
                            "wait_before_cure","wait_after_cure","wait_after_lift","light_pwm"}) {
            const double value=sla::motion_parameter(print,key,bottom);
            if(!std::isfinite(value)||value<0||std::abs(std::round(value*100)/100-value)>1e-5||
               (std::string(key)=="light_pwm"&&value>255))throw ExportError("Invalid " + format + " " + key);
        }
        auto param=[&](const char *key){return sla::motion_parameter(print,key,bottom);};
        const float total=param("lift_height")+param("lift_height2"),retract2=param("retract_height2");
        if(retract2>total||!std::isfinite(total)||
           (param("lift_height")>0&&param("lift_speed")<=0)||(param("lift_height2")>0&&param("lift_speed2")<=0)||
           (total>retract2&&param("retract_speed")<=0)||(retract2>0&&param("retract_speed2")<=0))
            throw ExportError(("Invalid " + format + " two-stage movement"));
    }
    for(double value:{material.material_density.value,material.bottle_volume.value,material.bottle_cost.value})
        if(!std::isfinite(float(value))||value<0)throw ExportError(("Invalid " + format + " material statistics"));
    chitubox_detail::validate_name(cfg.printer_model.value);
    for(const char *key:{"sla_material_settings_id","material_type"})chitubox_detail::validate_name(chitubox_detail::name(print,key));
}

} // namespace chitubox_detail

void CTBEncryptedArchive::validate_configuration(const SLAPrint &print) {
    chitubox_detail::validate_parameters(print, "CTB encrypted");
}

sla::RasterEncoder ChituboxCTBModernArchive::get_encoder() const {
    return chitubox::Gray7RasterEncoder{};
}

void ChituboxCTBModernArchive::validate_configuration(const SLAPrint &print, unsigned version) {
    if (version < 3 || version > 5) throw ExportError("Ordinary CTB requires version 3, 4 or 5");
    chitubox_detail::validate_parameters(print, "CTB v" + std::to_string(version));
    if (version == 3) {
        // V3 has per-layer lift/retract/PWM, but no two-stage or rest fields.
        for (bool bottom : {false, true})
            for (const char *key : {"lift_height2", "retract_height2", "wait_after_cure", "wait_after_lift"})
                if (sla::motion_parameter(print, key, bottom) != 0)
                    throw ExportError(std::string("CTB v3 cannot store ") + (bottom ? "bottom " : "normal ") + key);
    }
}

void ChituboxCTBModernArchive::export_print(const std::string filename, const SLAPrint &print,
                                          const ThumbnailsList &thumbnails, const std::string &) {
    using chitubox_detail::u32;
    using chitubox_detail::f32;
    validate_configuration(print, m_version);
    if (m_gktwo && m_version != 4) throw ExportError("GKtwo CTB requires version 4");
    if (m_layers.empty() || m_layers.size() != print.print_layers().size() || m_layers.size() > UINT32_MAX)
        throw ExportError("CTB requires a complete nonempty layer stack");
    const auto dims = get_display_dimensions(m_cfg);
    const auto &material = print.material_config();
    const bool extended_motion = m_version >= 4;
    auto param = [&](const char *key, bool bottom) { return sla::motion_parameter(print, key, bottom); };
    auto round2 = [](float value) { return std::nearbyint(value * 100.f) / 100.f; };
    auto lift_total = [&](bool bottom) { return round2(param("lift_height", bottom) + param("lift_height2", bottom)); };
    auto exposure = [&](size_t index) { return round2(sla::layer_exposure(print, index)); };
    auto motor_time = [&](bool bottom) {
        const float h2 = param("lift_height2", bottom), h1 = lift_total(bottom) - h2;
        const float r2 = param("retract_height2", bottom), r1 = lift_total(bottom) - r2;
        float time = 0;
        if (h1 > 0) time += h1 / (param("lift_speed", bottom) / 60.f);
        if (h2 > 0) time += h2 / (param("lift_speed2", bottom) / 60.f);
        if (r1 > 0) time += r1 / (param("retract_speed", bottom) / 60.f);
        if (r2 > 0) time += r2 / (param("retract_speed2", bottom) / 60.f);
        return round2(time);
    };
    auto light_off = [&](bool bottom) {
        return extended_motion ? 0.f : round2(motor_time(bottom) + param("wait_before_cure", bottom));
    };
    const uint32_t bottom = uint32_t(sla::bottom_layers(print, m_layers.size()));
    const uint32_t transition = uint32_t(std::min<size_t>(material.sla_transition_layers.value, m_layers.size() - bottom));
    const auto large = encode_chitubox_rgb15(400, 300, thumbnails);
    const auto small = encode_chitubox_rgb15(200, 125, thumbnails);
    std::string model = m_cfg.printer_model.value.empty() ? (m_gktwo ? "GKtwo" : "Unknown") : m_cfg.printer_model.value;
    if (m_version == 5) model.push_back('\0');
    const auto resin_type = chitubox_detail::name(print, "material_type");
    const auto resin_name = chitubox_detail::name(print, "sla_material_settings_id", resin_type);
    const std::string disclaimer = m_gktwo
        ? "Layout and record format for the jxs,ctb and cbddlp file types are the copyrighted programs or codes of CBD Technology (China) Inc.The Customer or User shall not in any manner reproduce, distribute, modify, decompile, disassemble, decrypt, extract, reverse engineer, lease, assign, or sublicense the said programs or codes."
        : "Layout and record format for the ctb and cbddlp file types are the copyrighted programs or codes of CBD Technology (China) Inc..The Customer or User shall not in any manner reproduce, distribute, modify, decompile, disassemble, decrypt, extract, reverse engineer, lease, assign, or sublicense the said programs or codes.";

    // Original schema: header112, preview32, print60, slicer76+UTF8 name,
    // v4 parameters464 (including380 reserved), resin40+strings, layer36.
    const uint32_t large_offset = 112, small_offset = large_offset + 32 + uint32_t(large.size());
    const uint32_t print_offset = small_offset + 32 + uint32_t(small.size());
    const uint32_t slicer_offset = print_offset + 60;
    const uint32_t model_offset = slicer_offset + 76;
    const uint32_t disclaimer_offset = model_offset + uint32_t(model.size());
    const uint32_t v4_offset = extended_motion ? disclaimer_offset + uint32_t(disclaimer.size()) : 0;
    const uint32_t resin_offset = m_version == 5 ? v4_offset + 464 : 0;
    const uint32_t table_offset = !extended_motion ? disclaimer_offset :
        m_version == 4 ? v4_offset + 464 : resin_offset + 40 + uint32_t(resin_type.size() + resin_name.size() + model.size());
    uint64_t cursor = uint64_t(table_offset) + 36 * m_layers.size();
    if (cursor > UINT32_MAX) throw ExportError("CTB main layer table exceeds its uint32 addressing range");
    std::vector<uint64_t> addresses;
    addresses.reserve(m_layers.size());
    double volume_mm3 = 0, previous_z = 0;
    const uint64_t pixels = uint64_t(dims.resolution_x) * dims.resolution_y;
    const double area = dims.display_width_mm * dims.display_height_mm / pixels;
    float estimate = 300.f; // Same fixed preparation allowance as UVtools.
    for (size_t i = 0; i < m_layers.size(); ++i) {
        const double z = double(print.print_layers()[i].level()) * SCALING_FACTOR;
        if (!std::isfinite(z) || z <= previous_z || std::abs(std::round(z * 1000) / 1000 - z) > 1e-7)
            throw ExportError("CTB layer Z requires increasing values with 0.001 mm precision");
        const auto count = chitubox::exposed_pixels(m_layers[i], pixels);
        volume_mm3 += count * area * (z - previous_z);
        previous_z = z;
        if (m_layers[i].size() > UINT32_MAX - 84 || cursor > uint64_t(INT64_MAX) - 84 - m_layers[i].size())
            throw ExportError("CTB layer size or output stream offset exceeds its schema range");
        addresses.push_back(cursor + 84);
        cursor += 84 + m_layers[i].size();
        const bool b = i < bottom;
        estimate += exposure(i) + motor_time(b) + param("wait_before_cure", b) +
            param("wait_after_cure", b) + param("wait_after_lift", b);
    }
    if (!std::isfinite(estimate) || double(estimate) > UINT32_MAX)
        throw ExportError("CTB time estimate exceeds uint32");
    const double volume = volume_mm3 / 1000, mass = volume * material.material_density.value;
    const double cost = material.bottle_volume.value > 0 ? volume * material.bottle_cost.value / material.bottle_volume.value : 0;
    if (!std::isfinite(float(volume)) || !std::isfinite(float(mass)) || !std::isfinite(float(cost)))
        throw ExportError("CTB material statistics exceed float32");

    std::vector<uint8_t> header(112, 0), parameters(60, 0), slicer(76, 0), v4(extended_motion ? 464 : 0, 0), resin;
    u32(header, 0, m_gktwo ? 0xFF220810 : extended_motion ? 0x12FD0106 : 0x12FD0086);
    u32(header, 4, m_version); f32(header, 8, float(dims.display_width_mm)); f32(header, 12, float(dims.display_height_mm));
    f32(header, 16, float(m_cfg.max_print_height.value)); f32(header, 28, sla::layer_z(print, m_layers.size() - 1));
    f32(header, 32, float(print.default_object_config().layer_height.value));
    f32(header, 36, round2(float(material.exposure_time.value))); f32(header, 40, round2(float(material.initial_exposure_time.value)));
    f32(header, 44, light_off(false)); u32(header, 48, bottom); u32(header, 52, dims.resolution_x); u32(header, 56, dims.resolution_y);
    u32(header, 60, large_offset); u32(header, 64, table_offset); u32(header, 68, uint32_t(m_layers.size()));
    u32(header, 72, small_offset); u32(header, 76, uint32_t(estimate)); u32(header, 80, m_cfg.display_mirror_x.value ? 1 : 0);
    u32(header, 84, print_offset); u32(header, 88, 60); u32(header, 92, 1);
    const auto normal_pwm = uint16_t(param("light_pwm", false)), bottom_pwm = uint16_t(param("light_pwm", true));
    header[96] = uint8_t(normal_pwm); header[97] = uint8_t(normal_pwm >> 8);
    header[98] = uint8_t(bottom_pwm); header[99] = uint8_t(bottom_pwm >> 8);
    u32(header, 100, m_seed); u32(header, 104, slicer_offset); u32(header, 108, 76);
    f32(parameters, 0, lift_total(true)); f32(parameters, 4, param("lift_speed", true));
    f32(parameters, 8, lift_total(false)); f32(parameters, 12, param("lift_speed", false));
    f32(parameters, 16, param("retract_speed", false)); f32(parameters, 20, float(volume));
    f32(parameters, 24, float(mass)); f32(parameters, 28, float(cost));
    f32(parameters, 32, light_off(true)); f32(parameters, 36, light_off(false)); u32(parameters, 40, bottom);
    f32(slicer, 0, param("lift_height2", true)); f32(slicer, 4, extended_motion ? param("lift_speed2", true) : 0.f);
    f32(slicer, 8, param("lift_height2", false)); f32(slicer, 12, extended_motion ? param("lift_speed2", false) : 0.f);
    f32(slicer, 16, param("retract_height2", false)); f32(slicer, 20, extended_motion ? param("retract_speed2", false) : 0.f);
    f32(slicer, 24, param("wait_after_lift", false)); u32(slicer, 28, model_offset); u32(slicer, 32, uint32_t(model.size()));
    slicer[36] = 0x0F; slicer[39] = uint8_t(m_version * 0x10); u32(slicer, 40, uint32_t(std::time(nullptr) / 60));
    u32(slicer, 44, 8); u32(slicer, 48, m_version == 3 ? 0x1060300 : m_version == 4 ? 0x1090000 : 0x2000000);
    f32(slicer, 52, extended_motion ? param("wait_before_cure", false) : 0.f);
    f32(slicer, 56, param("wait_after_lift", false)); u32(slicer, 60, transition); u32(slicer, 64, v4_offset);
    if (extended_motion) {
        f32(v4, 0, param("retract_speed", true)); f32(v4, 4, param("retract_speed2", true));
        f32(v4, 12, 4); f32(v4, 20, 4); f32(v4, 24, param("wait_before_cure", false));
        f32(v4, 28, param("wait_after_lift", false)); f32(v4, 32, param("wait_after_cure", false));
        f32(v4, 36, param("retract_height2", true)); u32(v4, 48, 5); u32(v4, 52, uint32_t(m_layers.size() - 1));
        u32(v4, 72, disclaimer_offset); u32(v4, 76, uint32_t(disclaimer.size())); u32(v4, 80, resin_offset);
    }
    if (m_version == 5) {
        resin.resize(40 + resin_type.size() + resin_name.size() + model.size(), 0);
        u32(resin, 8, resin_offset + 40 + uint32_t(resin_type.size() + resin_name.size()));
        u32(resin, 12, uint32_t(resin_type.size())); u32(resin, 16, resin_offset + 40);
        u32(resin, 20, uint32_t(resin_name.size())); u32(resin, 24, resin_offset + 40 + uint32_t(resin_type.size()));
        u32(resin, 28, uint32_t(model.size())); f32(resin, 32, float(material.material_density.value));
        size_t at = 40;
        for (const std::string *name : std::array<const std::string *, 3>{&resin_type, &resin_name, &model}) {
            std::copy(name->begin(), name->end(), resin.begin() + at); at += name->size();
        }
    }
    std::ofstream out(filename, std::ios::binary | std::ios::trunc);
    if (!out) throw ExportError("Cannot open CTB output");
    auto write = [&](const std::vector<uint8_t> &bytes) { out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()); };
    auto preview = [&](uint32_t offset, uint32_t width, uint32_t height, const std::vector<uint8_t> &image) {
        std::vector<uint8_t> record(32, 0); u32(record, 0, width); u32(record, 4, height);
        u32(record, 8, offset + 32); u32(record, 12, uint32_t(image.size())); write(record); write(image);
    };
    auto definition = [&](size_t i, uint64_t address) {
        std::vector<uint8_t> record(36, 0); f32(record, 0, sla::layer_z(print, i)); f32(record, 4, exposure(i));
        f32(record, 8, light_off(i < bottom)); u32(record, 12, uint32_t(address)); u32(record, 16, uint32_t(m_layers[i].size()));
        u32(record, 20, uint32_t(address >> 32)); u32(record, 24, 84); return record;
    };
    write(header); preview(large_offset, 400, 300, large); preview(small_offset, 200, 125, small);
    write(parameters); write(slicer); out.write(model.data(), model.size());
    if (extended_motion) { out.write(disclaimer.data(), disclaimer.size()); write(v4); }
    write(resin);
    for (size_t i = 0; i < m_layers.size(); ++i) write(definition(i, addresses[i]));
    for (size_t i = 0; i < m_layers.size(); ++i) {
        const bool b = i < bottom;
        // Both copies of LayerDef point to RLE. The decoder locates this
        // extended record at DataAddress-84, independently of its TableSize.
        auto record = definition(i, addresses[i]); record.resize(84, 0);
        u32(record, 36, 84 + uint32_t(m_layers[i].size())); f32(record, 40, lift_total(b));
        f32(record, 44, param("lift_speed", b)); f32(record, 48, param("lift_height2", b));
        f32(record, 52, extended_motion ? param("lift_speed2", b) : 0.f); f32(record, 56, param("retract_speed", b));
        f32(record, 60, param("retract_height2", b)); f32(record, 64, extended_motion ? param("retract_speed2", b) : 0.f);
        f32(record, 68, param("wait_after_cure", b)); f32(record, 72, param("wait_after_lift", b));
        f32(record, 76, extended_motion ? param("wait_before_cure", b) : 0.f); f32(record, 80, param("light_pwm", b));
        write(record);
        const auto *data = static_cast<const uint8_t *>(m_layers[i].data());
        std::vector<uint8_t> payload(data, data + m_layers[i].size());
        ctb_encrypted::xor_layer(payload, uint32_t(i), m_seed); write(payload);
    }
    out.flush(); if (!out) throw ExportError("Cannot finish CTB output");
}

void CTBEncryptedArchive::export_print(const std::string filename,const SLAPrint &print,const ThumbnailsList &thumbnails,
                                      const std::string &) {
    using chitubox_detail::u32;using chitubox_detail::f32;
    if(m_version!=4&&m_version!=5)throw ExportError("CTB encrypted writer supports versions 4 and 5");
    validate_configuration(print);
    if(m_layers.empty()||m_layers.size()!=print.print_layers().size()||m_layers.size()>UINT32_MAX)
        throw ExportError("CTB encrypted requires a complete nonempty layer stack");
    const auto dims=get_display_dimensions(m_cfg);const auto &material=print.material_config();
    const auto large=encode_chitubox_rgb15(400,300,thumbnails),small=encode_chitubox_rgb15(200,125,thumbnails);
    const std::string model=m_cfg.printer_model.value.empty()?"Unknown":m_cfg.printer_model.value;
    const auto resin_type=chitubox_detail::name(print,"material_type");
    const auto resin_name=chitubox_detail::name(print,"sla_material_settings_id",resin_type);
    constexpr uint64_t checksum=0xCAFEBABE;constexpr uint32_t xor_seed=0xEFBEADDE;
    constexpr uint32_t header_size=48,settings_size=288,layer_size=88,pointer_size=16;
    const uint32_t large_offset=header_size+settings_size,small_offset=large_offset+16+uint32_t(large.size());
    const uint32_t model_offset=small_offset+16+uint32_t(small.size()),disclaimer_offset=model_offset+uint32_t(model.size());
    // Preserve the actual on-disk protocol disclaimer, rather than replacing
    // it with application branding. It is a field of the original schema.
    const std::string disclaimer="Layout and record format for the ctb and cbddlp file types are the copyrighted programs or codes of CBD Technology (China) Inc..The Customer or User shall not in any manner reproduce, distribute, modify, decompile, disassemble, decrypt, extract, reverse engineer, lease, assign, or sublicense the said programs or codes.";
    const uint32_t resin_offset=disclaimer_offset+uint32_t(disclaimer.size());
    std::vector<uint8_t> resin;
    if(m_version==5) {
        resin.resize(40+resin_type.size()+resin_name.size()+model.size(),0);
        u32(resin,8,resin_offset+40+uint32_t(resin_type.size()+resin_name.size()));
        u32(resin,12,uint32_t(resin_type.size()));u32(resin,16,resin_offset+40);
        u32(resin,20,uint32_t(resin_name.size()));u32(resin,24,resin_offset+40+uint32_t(resin_type.size()));
        u32(resin,28,uint32_t(model.size()));f32(resin,32,float(material.material_density.value));
        size_t cursor=40;for(const auto *name:{&resin_type,&resin_name,&model}) {
            std::copy(name->begin(),name->end(),resin.begin()+cursor);cursor+=name->size();
        }
    }
    const uint32_t pointers_offset=resin_offset+uint32_t(resin.size());
    uint64_t cursor=uint64_t(pointers_offset)+m_layers.size()*pointer_size;
    std::vector<uint64_t> addresses;addresses.reserve(m_layers.size());
    double volume=0,previous_z=0;uint64_t previous_pixels=0;
    const uint64_t expected=uint64_t(dims.resolution_x)*dims.resolution_y;
    for(size_t i=0;i<m_layers.size();++i) {
        const auto &raster=m_layers[i];const double z=double(print.print_layers()[i].level())*SCALING_FACTOR;
        if(!std::isfinite(z)||z<=previous_z||z>m_cfg.max_print_height.value||std::abs(std::round(z*1000)/1000-z)>1e-6)
            throw ExportError("Invalid CTB encrypted layer Z");
        if(!i||raster.size()!=m_layers[i-1].size()||std::memcmp(raster.data(),m_layers[i-1].data(),raster.size()))
            previous_pixels=chitubox::exposed_pixels(raster,expected);
        volume+=previous_pixels*dims.display_width_mm/dims.resolution_x*dims.display_height_mm/dims.resolution_y*(z-previous_z)/1000;
        previous_z=z;addresses.push_back(cursor);cursor+=layer_size+raster.size();
        if(cursor>UINT32_MAX-44)throw ExportError("CTB encrypted file exceeds its signature address field");
    }
    // Native float fields retain small totals. UVtools' public display/setter
    // rounding to three decimals is not a limitation of the file schema.
    const double mass=volume*material.material_density.value;
    const double cost=material.bottle_volume.value>0?volume*material.bottle_cost.value/material.bottle_volume.value:0;
    for(double value:{volume,mass,cost})if(!std::isfinite(float(value)))throw ExportError("CTB encrypted material statistics overflow");
    const uint32_t signature_offset=uint32_t(cursor)+(m_version==5?8:0);
    const auto signature=ctb_encrypted::signature(checksum);
    std::vector<uint8_t> header(header_size,0),settings(settings_size,0);
    u32(header,0,0x12FD0107);u32(header,4,settings_size);u32(header,8,header_size);u32(header,16,m_version);
    u32(header,20,uint32_t(signature.size()));u32(header,24,signature_offset);header[32]=header[34]=1;u32(header,40,42);
    u32(settings,0,uint32_t(checksum));u32(settings,4,uint32_t(checksum>>32));u32(settings,8,pointers_offset);
    f32(settings,12,float(dims.display_width_mm));f32(settings,16,float(dims.display_height_mm));f32(settings,20,float(m_cfg.max_print_height.value));
    f32(settings,32,float(previous_z));f32(settings,36,float(print.default_object_config().layer_height.value));
    f32(settings,40,float(material.exposure_time.value));f32(settings,44,float(material.initial_exposure_time.value));
    const uint32_t bottom=uint32_t(sla::bottom_layers(print,m_layers.size()));
    u32(settings,52,bottom);u32(settings,56,dims.resolution_x);u32(settings,60,dims.resolution_y);u32(settings,64,uint32_t(m_layers.size()));
    u32(settings,68,large_offset);u32(settings,72,small_offset);u32(settings,80,m_cfg.display_mirror_x.value?1:0);
    // Named printers require fixed protocol placeholders. Anonymous numeric
    // identity is retained in its own native motion; forcing .05 could destroy
    // the height==FIRST predicate. Timing uses the actual fields written below.
    const bool named_tilting=sla::ctb_encrypted_named_tilting_vat(model);
    auto param=[&](const char *key,bool is_bottom){
        if (named_tilting) if (const auto fixed=sla::ctb_encrypted_fixed_parameter(key)) return *fixed;
        return sla::motion_parameter(print,key,is_bottom);
    };
    auto total_height=[&](bool is_bottom){return std::nearbyint((param("lift_height",is_bottom)+param("lift_height2",is_bottom))*100.f)/100.f;};
    const bool tilting=sla::ctb_encrypted_tilting_motion(model,
        float(print.default_object_config().layer_height.value),
        std::max(total_height(false)-param("lift_height2",false),0.f),param("lift_speed",false));
    auto exposure=[&](size_t index){return std::nearbyint(sla::layer_exposure(print,index)*100.f)/100.f;};
    // Match the format's original estimator: rounded float motor time per
    // layer and the UVtools fixed 300-second preparation allowance. This is
    // an estimate field, not a measurement of a particular printer startup.
    float estimate=300.f;
    for(size_t i=0;i<m_layers.size();++i) {
        const bool b=i<bottom;const float height2=param("lift_height2",b),height1=total_height(b)-height2;
        const float retract2=param("retract_height2",b),retract1=height1+height2-retract2;
        float movement=0;
        if(height1>0)movement+=height1/(param("lift_speed",b)/60.f);
        if(height2>0)movement+=height2/(param("lift_speed2",b)/60.f);
        if(retract2>0)movement+=retract2/(param("retract_speed2",b)/60.f);
        if(retract1>0)movement+=retract1/(param("retract_speed",b)/60.f);
        movement=tilting ? 4.f : std::nearbyint(movement*100.f)/100.f;
        estimate+=param("wait_before_cure",b)+exposure(i)+param("wait_after_cure",b)+param("wait_after_lift",b)+movement;
    }
    if(!std::isfinite(estimate)||double(estimate)>UINT32_MAX)throw ExportError("CTB encrypted time estimate exceeds uint32");
    u32(settings,76,uint32_t(estimate));
    f32(settings,84,total_height(true));f32(settings,88,param("lift_speed",true));
    f32(settings,92,total_height(false));f32(settings,96,param("lift_speed",false));f32(settings,100,param("retract_speed",false));
    f32(settings,104,float(volume));f32(settings,108,float(mass));f32(settings,112,float(cost));u32(settings,120,1);
    const auto pwm=uint16_t(param("light_pwm",false)),bottom_pwm=uint16_t(param("light_pwm",true));
    settings[124]=uint8_t(pwm);settings[125]=uint8_t(pwm>>8);settings[126]=uint8_t(bottom_pwm);settings[127]=uint8_t(bottom_pwm>>8);
    u32(settings,128,xor_seed);f32(settings,132,param("lift_height2",true));f32(settings,136,param("lift_speed2",true));
    f32(settings,140,param("lift_height2",false));f32(settings,144,param("lift_speed2",false));
    f32(settings,148,param("retract_height2",false));f32(settings,152,param("retract_speed2",false));f32(settings,156,param("wait_after_lift",false));
    u32(settings,160,model_offset);u32(settings,164,uint32_t(model.size()));settings[168]=0x0f;
    // Every layer carries its own Z/exposure/motion. Permit those fields even
    // when this particular job happens to use only the global values.
    settings[171]=0x40;u32(settings,172,uint32_t(std::time(nullptr)/60));u32(settings,176,8);
    f32(settings,180,param("wait_before_cure",false));f32(settings,184,param("wait_after_lift",false));
    u32(settings,188,uint32_t(std::min<size_t>(material.sla_transition_layers.value,m_layers.size()-bottom)));
    f32(settings,192,param("retract_speed",true));f32(settings,196,param("retract_speed2",true));
    f32(settings,204,4);f32(settings,212,4);f32(settings,216,param("wait_before_cure",false));
    f32(settings,220,param("wait_after_lift",false));f32(settings,224,param("wait_after_cure",false));
    f32(settings,228,param("retract_height2",true));u32(settings,240,4);u32(settings,244,uint32_t(m_layers.size()-1));
    u32(settings,264,disclaimer_offset);u32(settings,268,uint32_t(disclaimer.size()));u32(settings,276,m_version==5?resin_offset:0);
    const auto encrypted_settings=ctb_encrypted::aes_cbc(std::move(settings),true);
    std::ofstream out(filename,std::ios::binary|std::ios::trunc);if(!out)throw ExportError("Cannot open CTB encrypted output");
    auto write=[&](const std::vector<uint8_t> &bytes){out.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());};
    write(header);write(encrypted_settings);
    auto preview=[&](uint32_t offset,uint32_t width,uint32_t height,const std::vector<uint8_t> &bytes) {
        std::vector<uint8_t> table(16,0);u32(table,0,width);u32(table,4,height);u32(table,8,offset+16);u32(table,12,uint32_t(bytes.size()));write(table);write(bytes);
    };
    preview(large_offset,400,300,large);preview(small_offset,200,125,small);
    out.write(model.data(),model.size());out.write(disclaimer.data(),disclaimer.size());write(resin);
    for(uint64_t address:addresses) {std::vector<uint8_t> table(pointer_size,0);u32(table,0,uint32_t(address));u32(table,4,uint32_t(address>>32));u32(table,8,layer_size);write(table);}
    for(size_t i=0;i<m_layers.size();++i) {
        const bool is_bottom=i<bottom;std::vector<uint8_t> table(layer_size,0);
        u32(table,0,layer_size);f32(table,4,sla::layer_z(print,i));f32(table,8,exposure(i));
        u32(table,16,uint32_t(addresses[i]+layer_size));u32(table,20,uint32_t((addresses[i]+layer_size)>>32));u32(table,24,uint32_t(m_layers[i].size()));
        f32(table,40,total_height(is_bottom));f32(table,44,param("lift_speed",is_bottom));
        f32(table,48,param("lift_height2",is_bottom));f32(table,52,param("lift_speed2",is_bottom));f32(table,56,param("retract_speed",is_bottom));
        f32(table,60,param("retract_height2",is_bottom));f32(table,64,param("retract_speed2",is_bottom));f32(table,68,param("wait_after_cure",is_bottom));
        f32(table,72,param("wait_after_lift",is_bottom));f32(table,76,param("wait_before_cure",is_bottom));f32(table,80,param("light_pwm",is_bottom));write(table);
        const auto *data=static_cast<const uint8_t *>(m_layers[i].data());std::vector<uint8_t> bytes(data,data+m_layers[i].size());
        ctb_encrypted::xor_layer(bytes,uint32_t(i),xor_seed);write(bytes);
    }
    std::vector<uint8_t> trailer(m_version==5?8:0,0);if(m_version==5)u32(trailer,0,1109414650);write(trailer);write(signature);
    trailer.assign(4,0);u32(trailer,0,1833054899);write(trailer);out.flush();if(!out)throw ExportError("Cannot finish CTB encrypted output");
}
} // namespace Slic3r
