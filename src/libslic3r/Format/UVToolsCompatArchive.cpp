#include "libslic3r/SLA/PrinterRaster.hpp"
#include "libslic3r/SLA/ColorMask.hpp"
///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "UVToolsCompatArchive.hpp"
#include "libslic3r/SLA/CWSFreshExportPlan.hpp"
#include "FDGArchive.hpp"
#include "PhrozenArchive.hpp"
#include "ChituboxPreview.hpp"
#include <map>
#include <limits>
#include <boost/uuid/detail/sha1.hpp>
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




} // namespace

// Packed grayscale RGB565 helper for the legacy Anet sketch.
struct RGB565GrayscaleRasterEncoder {
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

// UVToolsRLEBinaryBase implementation
std::unique_ptr<sla::RasterBase> UVToolsRLEBinaryBase::create_raster() const
{
    return sla::create_printer_raster(m_cfg);
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
    export_print_impl(fname,print,nullptr);
}
void UVToolsCWSArchive::export_fresh_print(const std::string &fname, const SLAPrint &print,
                                         const sla::CWSFreshExportPlan &fresh)
{
    if(m_cfg.sla_archive_format.value!="cws")throw ExportError("Owned CWS plan requires the cws dialect");
    export_print_impl(fname,print,&fresh);
}
void UVToolsCWSArchive::export_print_impl(const std::string &fname, const SLAPrint &print,
                                        const sla::CWSFreshExportPlan *fresh)
{
    // Native CWS: field names and layer commands follow UVtools' CWSFile
    // and GCodeBuilder. Commands live only inside this SLA container.
    const size_t count = m_layers.size();
    if (!count || count != print.print_layers().size()) throw ExportError("CWS layer count mismatch");
    auto dims = get_display_dimensions(m_cfg);
    if(fresh) {
        fresh->source.validate(count);
        const auto &g=fresh->source.geometry;
        dims.resolution_x=g.columns;dims.resolution_y=g.rows;
        dims.display_width_mm=g.width_mm;dims.display_height_mm=g.height_mm;
    }
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
    const auto bottom = fresh ? fresh->source.globals.bottom_layers : sla::bottom_layers(print, count);
    auto parameter = [&](const char *key, bool is_bottom = false) {
        if(fresh) {
            using F=sla::LayerParameter;
            const std::map<std::string,F> fields{{"wait_before_cure",F::WaitBefore},{"wait_after_cure",F::WaitAfter},
                {"wait_after_lift",F::WaitAfterLift},{"lift_height",F::LiftDistance},{"lift_speed",F::LiftSpeed},
                {"retract_speed",F::RetractSpeed},{"light_pwm",F::LightPWM}};
            return (is_bottom ? fresh->represented.globals.bottom : fresh->represented.globals.normal).at(fields.at(key));
        }
        return double(sla::motion_parameter(print, key, is_bottom));
    };
    auto milliseconds = [](double seconds) {
        if (!std::isfinite(seconds) || seconds < 0 || seconds * 1000 > UINT32_MAX)
            throw ExportError("CWS time is outside its millisecond range");
        return uint32_t(std::lround(seconds * 1000));
    };
    const double thickness = fresh ? fresh->source.globals.layer_height : print.default_object_config().layer_height.value;
    const double normal_exposure=fresh ? fresh->represented.globals.normal.at(sla::LayerParameter::Exposure) : material.exposure_time.value;
    const double bottom_exposure=fresh ? fresh->represented.globals.bottom.at(sla::LayerParameter::Exposure) : material.initial_exposure_time.value;
    std::ostringstream manifest, profile, commands;
    manifest.imbue(std::locale::classic()); profile.imbue(std::locale::classic()); commands.imbue(std::locale::classic());
    profile << std::setprecision(fresh?17:9); commands << std::setprecision(fresh?17:9);
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
            << milliseconds(normal_exposure) << "</LayerTime><FirstLayerTime>"
            << milliseconds(bottom_exposure) << "</FirstLayerTime><NumberofBottomLayers>"
            << bottom << "</NumberofBottomLayers></InkConfig><SelectedInk>Resin</SelectedInk>\n</SliceBuildConfig>\n";
    commands << "; SLA-Slicer CWS exposure sequence\n";
    auto header = [&](const char *name, auto value) { commands << ";(" << name << " = " << value << ")\n"; };
    header("Pix per mm X", dims.resolution_x / dims.display_width_mm);
    header("Pix per mm Y", dims.resolution_y / dims.display_height_mm);
    header("X Resolution", dims.resolution_x); header("Y Resolution", dims.resolution_y);
    header("Layer Thickness", thickness); header("Layer Time", milliseconds(normal_exposure));
    header("Bottom Layers Time", milliseconds(bottom_exposure));
    header("Number of Bottom Layers", bottom); header("Blanking Layer Time", milliseconds(parameter("wait_before_cure")));
    header("Build Direction", "Bottom_Up"); header("Lift Distance", parameter("lift_height"));
    header("Z Lift Feed Rate", parameter("lift_speed")); header("Z Bottom Lift Feed Rate", parameter("lift_speed", true));
    header("Z Lift Retract Rate", parameter("retract_speed"));
    header("Platform X Size", dims.display_width_mm); header("Platform Y Size", dims.display_height_mm);
    header("Platform Z Size", m_cfg.max_print_height.value);
    header("Layer Light PWM", parameter("light_pwm")); header("Bottom Layer Light PWM", parameter("light_pwm", true));
    header("Flip X", "False"); header("Flip Y", "False"); // Masks already have the printer transform.
    if(!fresh) {
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
                     << milliseconds(seconds) << '\n';
            // XML's Takes marker is not parsed as WaitSyncDelayDetected by
            // the generic reader. A zero-time sync marker prevents its
            // fallback from subtracting motor/preparation time from waits.
            if(xml_cws)commands<<";<Delay> 00\n";
            commands<<"M400\n";
        };
        commands << ";LAYER_START:" << i << "\n;PositionZ:" << z << "mm\n";
        double lift_seconds = 0;
        if (lift > 0) {
            lift_seconds += move(delta + lift, param("lift_speed"));
            lift_seconds += move(lift2, param("lift_speed2"));
        } else lift_seconds = move(delta + lift2, param(lift2 > 0 ? "lift_speed2" : "lift_speed"));
        sync(lift_seconds);
        // Commit the Z-only prelude before cure, as in the owned writer.
        // Repeating the same image index changes parser state, not motion.
        if(lift+lift2==0)commands<<";CWS_Z_ADVANCE_COMMIT:"<<i<<"\n;LAYER_START:"<<i<<'\n';
        if (param("wait_after_lift") > 0)
            commands << ";<Delay> " << milliseconds(param("wait_after_lift")) << "\n";
        double retract_seconds = move(-(lift + lift2 - retract2), param("retract_speed"));
        retract_seconds += move(-retract2, param("retract_speed2"));
        sync(retract_seconds);
        commands << ";<Delay> " << milliseconds(param("wait_before_cure")) << "\n;<Slice> " << i << "\n";
        if (sla::layer_exposure(print, i) > 0)
            commands << "M106 S" << int(param("light_pwm")) << "\n;<Delay> "
                     << milliseconds(sla::layer_exposure(print, i)) << "\nM106 S0\n";
        commands << ";<Slice> Blank\n";
        if (param("wait_after_cure") > 0)
            commands << ";<Delay> " << milliseconds(param("wait_after_cure")) << "\n";
        commands << ";LAYER_END:" << i << "\n";
        previous_z = z;
    }
    commands << "M106 S0\n;<Slice> Blank\nM400\nM18\n;<Completed>\n";
    } else commands<<fresh->commands;
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
        field("layers_expo_ms", milliseconds(normal_exposure));
        field("head_layers_expo_ms", milliseconds(bottom_exposure));
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

// OSLA (Open SLA) - ZIP with binary header + PNG layers
void UVToolsOSLAArchive::export_print(const std::string     fname,
                                      const SLAPrint       &print,
                                      const ThumbnailsList &,
                                      const std::string    &projectname)
{
    throw RuntimeError("OSLA format not yet fully implemented");
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

// Klipper - ZIP with GCode + PNG layers
void UVToolsKlipperArchive::export_print(const std::string     fname,
                                         const SLAPrint       &print,
                                         const ThumbnailsList &,
                                         const std::string    &projectname)
{
    throw RuntimeError("Klipper format not yet fully implemented");
}

// ============================================================================
// BINARY RLE FORMAT IMPLEMENTATIONS (continued)
// ============================================================================

// ============================================================================
// PACKED-PIXEL FORMAT IMPLEMENTATIONS
// ============================================================================

std::unique_ptr<sla::RasterBase> UVToolsPackedPixelBase::create_raster() const
{
    return make_common_raster(m_cfg);
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
    return RGB565GrayscaleRasterEncoder{};  // Anet uses similar RGB565 encoding
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

} // namespace Slic3r

namespace Slic3r {
namespace fdg_detail {
constexpr uint32_t header_size = 216, layer_size = 36;
void u32(std::vector<uint8_t> &buffer, size_t offset, uint32_t value) {
    for (unsigned byte=0;byte<4;++byte) buffer.at(offset+byte)=uint8_t(value>>(byte*8));
}
void f32(std::vector<uint8_t> &buffer, size_t offset, float value) {
    uint32_t bits;std::memcpy(&bits,&value,sizeof bits);u32(buffer,offset,bits);
}
void write(std::ostream &out,const std::vector<uint8_t> &bytes) {
    out.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());
}
float light_off(const SLAPrint &print,bool bottom) {
    const double height=sla::motion_parameter(print,"lift_height",bottom);
    const double speed=sla::motion_parameter(print,"lift_speed",bottom);
    const double retract=sla::motion_parameter(print,"retract_speed",bottom);
    const double movement=height>0?height*60./speed+height*60./retract:0.;
    return float(std::round((movement+sla::motion_parameter(print,"wait_before_cure",bottom))*100.)/100.);
}
uint64_t exposed_pixels(const sla::EncodedRaster &mask,uint64_t expected) {
    const auto *data=static_cast<const uint8_t *>(mask.data());
    uint64_t pixels=0,exposed=0;uint8_t color=0;
    for(size_t i=0;i<mask.size();++i) {
        const uint8_t code=data[i];
        if(code&0x80) color=code&0x7f;
        const unsigned count=(code&0x80)?1:code;
        if(!count || pixels+count>expected) throw ExportError("Invalid FDG raster run");
        pixels+=count;if(color)exposed+=count;
    }
    if(pixels!=expected)throw ExportError("FDG mask dimensions do not match the printer");
    return exposed;
}

}
using namespace fdg_detail;
void FDGArchive::crypt_mask(std::vector<uint8_t> &bytes,uint32_t seed,uint32_t index) {
    if(!seed)return;
    const uint32_t increment=(seed-0x1dcb76c3u)^0x257e2431u;
    uint32_t key=increment*0x82391efdu*(index^0x110bdacdu);
    for(size_t i=0;i<bytes.size();++i) {
        bytes[i]^=uint8_t(key>>(8*(i%4)));
        if(i%4==3)key+=increment;
    }
}
std::unique_ptr<sla::RasterBase> FDGArchive::create_raster() const {return sla::create_printer_raster(m_cfg);}
sla::RasterEncoder FDGArchive::get_encoder() const {return PhrozenPHZArchive::encode_mask;}
void FDGArchive::validate_configuration(const SLAPrint &print) {
    const auto &cfg=print.printer_config();
    if(!std::isfinite(cfg.display_width.value)||!std::isfinite(cfg.display_height.value)||
       !std::isfinite(cfg.max_print_height.value)||cfg.display_width.value<=0||cfg.display_height.value<=0||
       cfg.max_print_height.value<=0||cfg.display_pixels_x.value<=0||cfg.display_pixels_y.value<=0||
       cfg.display_pixels_x.value>32768||cfg.display_pixels_y.value>32768||
       uint64_t(cfg.display_pixels_x.value)*cfg.display_pixels_y.value>(uint64_t(128)<<20))
        throw ExportError("Invalid FDG screen geometry");
    sla::require_single_stage_motion(print,"FDG v2",true);
    // ProjectorType distinguishes only no mirror and horizontal mirror.
    if(cfg.display_mirror_y.value)
        throw ExportError("FDG v2 cannot describe a vertical display mirror");
    for(const bool bottom:{false,true}) {
        for(const char *key:{"lift_height","lift_speed","retract_speed","wait_before_cure","light_pwm"}) {
            const double value=sla::motion_parameter(print,key,bottom);
            if(!std::isfinite(value)||value<0||(std::string(key)=="light_pwm"&&value>255))
                throw ExportError(std::string("Invalid FDG ")+key);
        }
        if(sla::motion_parameter(print,"lift_height",bottom)>0 &&
           (sla::motion_parameter(print,"lift_speed",bottom)<=0||sla::motion_parameter(print,"retract_speed",bottom)<=0))
            throw ExportError("FDG enabled moves require positive speeds");
    }
    for(const unsigned char c:cfg.printer_model.value)
        if(c<32||c>126)throw ExportError("FDG firmware model name must be printable ASCII");
    if(cfg.printer_model.value.size()>4096)throw ExportError("FDG firmware model name is too long");
    for(const double value:{print.default_object_config().layer_height.value,print.material_config().initial_layer_height.value})
        if(!std::isfinite(float(value))||value<=0)throw ExportError("FDG requires finite positive layer heights");
    for(const double value:{print.material_config().exposure_time.value,print.material_config().initial_exposure_time.value,
        print.material_config().material_density.value})
        if(!std::isfinite(float(value))||value<0)throw ExportError("Invalid FDG exposure/density");
    for(const double value:{print.material_config().bottle_cost.value,print.material_config().bottle_volume.value})
        if(!std::isfinite(value)||value<0)throw ExportError("Invalid FDG resin cost/volume");
    if(print.material_config().sla_bottom_layers.value<0 || print.material_config().sla_bottom_layers.value>UINT16_MAX)
        throw ExportError("FDG bottom count exceeds uint16 decoder");
}
void FDGArchive::export_print(const std::string fname,const SLAPrint &print,
                                    const ThumbnailsList &thumbnails,const std::string &) {
    validate_configuration(print);
    if(m_layers.empty()||m_layers.size()!=print.print_layers().size()||m_layers.size()>UINT32_MAX)
        throw ExportError("FDG requires a complete nonempty layer stack");
    const bool portrait=m_cfg.display_orientation.value==sladoPortrait;
    const uint32_t cols=portrait?m_cfg.display_pixels_y.value:m_cfg.display_pixels_x.value;
    const uint32_t rows=portrait?m_cfg.display_pixels_x.value:m_cfg.display_pixels_y.value;
    const double width=portrait?m_cfg.display_height.value:m_cfg.display_width.value;
    const double height=portrait?m_cfg.display_width.value:m_cfg.display_height.value;
    const uint32_t seed=m_seed;
    const std::string model=m_cfg.printer_model.value.empty()?"Unknown":m_cfg.printer_model.value;
    const auto small=encode_chitubox_rgb15(200,125,thumbnails);
    const auto large=encode_chitubox_rgb15(400,300,thumbnails);
    const uint32_t small_offset=header_size,large_offset=small_offset+32+uint32_t(small.size());
    const uint32_t model_offset=large_offset+32+uint32_t(large.size());
    const uint32_t table_offset=model_offset+uint32_t(model.size());
    struct Payload {const sla::EncodedRaster *mask;uint64_t address;bool write=true;};
    std::vector<Payload> payloads;
    std::map<std::array<unsigned,5>,std::vector<size_t>> hashes;
    uint64_t cursor=uint64_t(table_offset)+uint64_t(layer_size)*m_layers.size();
    double volume=0,previous_z=0;uint64_t last_pixels=0;
    const uint64_t expected=uint64_t(cols)*rows;
    const double area=width*height/expected;
    for(size_t i=0;i<m_layers.size();++i) {
        const auto &mask=m_layers[i];
        if(mask.size()>UINT32_MAX)throw ExportError("FDG layer exceeds its size field");
        const double z=sla::layer_z(print,i);
        if(!std::isfinite(z)||z<=previous_z||z>m_cfg.max_print_height.value+1e-5)
            throw ExportError("Invalid FDG layer Z");
        if(!i || mask.size()!=m_layers[i-1].size() || std::memcmp(mask.data(),m_layers[i-1].data(),mask.size())!=0)
            last_pixels=exposed_pixels(mask,expected);
        volume+=last_pixels*area*(z-previous_z)/1000.;previous_z=z;
        Payload payload{&mask,cursor};
        if(!seed) {
            boost::uuids::detail::sha1 hash;hash.process_bytes(mask.data(),mask.size());
            unsigned digest[5];hash.get_digest(digest);
            const std::array<unsigned,5> key{digest[0],digest[1],digest[2],digest[3],digest[4]};
            const auto found=hashes.find(key);
            if(found!=hashes.end())for(const size_t candidate:found->second) {
                const auto &old=payloads[candidate];
                if(old.mask->size()==mask.size()&&!std::memcmp(old.mask->data(),mask.data(),mask.size())) {
                    payload.address=old.address;payload.write=false;break;
                }
            }
            if(payload.write&&hashes.size()<1024)hashes[key].push_back(i);
        }
        if(payload.write) {
            if(cursor>uint64_t(std::numeric_limits<std::streamoff>::max())-mask.size())
                throw ExportError("FDG exceeds stream address limits");
            cursor+=mask.size();
        }
        payloads.push_back(payload);
    }
    if(table_offset<model_offset)throw ExportError("FDG table offset overflow");
    const auto &material=print.material_config();
    const double mass=volume*material.material_density.value;
    const double cost=material.bottle_volume.value>0?volume*material.bottle_cost.value/material.bottle_volume.value:0.;
    for(const auto value:{volume,mass,cost})
        if(!std::isfinite(float(value)))throw ExportError("FDG resin statistics overflow");
    std::vector<uint8_t> header(header_size,0);
    u32(header,0,0xBD3C7AC8);u32(header,4,2);
    const auto bottom=uint32_t(sla::bottom_layers(print,m_layers.size()));
    u32(header,8,uint32_t(m_layers.size()));u32(header,12,bottom);
    u32(header,16,m_cfg.display_mirror_x.value?1:0);u32(header,20,bottom);
    u32(header,24,cols);u32(header,28,rows);
    f32(header,32,print.default_object_config().layer_height.value);
    f32(header,36,material.exposure_time.value);f32(header,40,material.initial_exposure_time.value);
    u32(header,44,large_offset);u32(header,48,small_offset);u32(header,52,table_offset);
    u32(header,56,sla::lcd_print_time(print));u32(header,60,1);
    header[64]=uint8_t(sla::motion_parameter(print,"light_pwm",false));
    header[66]=uint8_t(sla::motion_parameter(print,"light_pwm",true));
    f32(header,76,sla::layer_z(print,m_layers.size()-1));
    f32(header,80,width);f32(header,84,height);f32(header,88,m_cfg.max_print_height.value);
    // One stored raster, with grayscale antialiasing (UVtools' full-gray level).
    u32(header,92,seed);u32(header,96,16);u32(header,100,0x4c);
    f32(header,104,float(volume));f32(header,108,float(mass));f32(header,112,float(cost));
    u32(header,116,model_offset);u32(header,120,uint32_t(model.size()));
    f32(header,124,light_off(print,true));f32(header,128,light_off(print,false));
    f32(header,136,sla::motion_parameter(print,"lift_height",true));f32(header,140,sla::motion_parameter(print,"lift_speed",true));
    f32(header,144,sla::motion_parameter(print,"lift_height",false));f32(header,148,sla::motion_parameter(print,"lift_speed",false));
    f32(header,152,sla::motion_parameter(print,"retract_speed",false));
    u32(header,184,uint32_t(std::time(nullptr)/60));u32(header,188,0x01060300);
    std::ofstream out(fname,std::ios::binary|std::ios::trunc);
    if(!out)throw ExportError("Cannot open FDG output");
    write(out,header);
    auto preview=[&](uint32_t address,uint32_t w,uint32_t h,const std::vector<uint8_t> &bytes) {
        std::vector<uint8_t> table(32,0);u32(table,0,w);u32(table,4,h);u32(table,8,address+32);u32(table,12,uint32_t(bytes.size()));
        write(out,table);write(out,bytes);
    };
    preview(small_offset,200,125,small);preview(large_offset,400,300,large);
    out.write(model.data(),model.size());
    for(size_t i=0;i<payloads.size();++i) {
        const auto &p=payloads[i];std::vector<uint8_t> table(layer_size,0);
        f32(table,0,sla::layer_z(print,i));f32(table,4,sla::layer_exposure(print,i));f32(table,8,light_off(print,i<bottom));
        u32(table,12,uint32_t(p.address));u32(table,16,uint32_t(p.mask->size()));u32(table,20,uint32_t(p.address>>32));u32(table,24,84);
        write(out,table);
    }
    for(size_t i=0;i<payloads.size();++i)if(payloads[i].write) {
        const auto &mask=*payloads[i].mask;
        if(seed) {
            const auto *data=static_cast<const uint8_t *>(mask.data());std::vector<uint8_t> encrypted(data,data+mask.size());
            crypt_mask(encrypted,seed,uint32_t(i));write(out,encrypted);
        } else out.write(static_cast<const char *>(mask.data()),mask.size());
    }
    out.flush();if(!out)throw ExportError("Cannot finish FDG output");
}

}
