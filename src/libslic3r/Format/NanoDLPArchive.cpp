///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "NanoDLPArchive.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
#include "NanoDLPPacket.hpp"
#include "NanoDLPSchema.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include <ctime>
#include "libslic3r/Zipper.hpp"
#include <algorithm>
#include <map>
#include <climits>

namespace Slic3r {

std::unique_ptr<sla::RasterBase> NanoDLPArchive::create_raster() const
{
    return sla::create_printer_raster(m_cfg);
}

sla::RasterEncoder NanoDLPArchive::get_encoder() const
{
    return [rgb=m_rgb](const void *ptr, size_t width, size_t height, size_t components) {
        return nanodlp::encode_packet(ptr, width, height, components, rgb?nanodlp::ImageMode::RGB:nanodlp::ImageMode::Mono);
    };
}

nlohmann::json NanoDLPArchive::prepare_metadata(const SLAPrint &print,const std::string &projectname) const
{
    if(m_layers.empty() || m_layers.size()!=print.print_layers().size() || m_layers.size()>UINT32_MAX)
        throw ExportError("NanoDLP requires a complete nonempty layer stack");
    const bool portrait=m_cfg.display_orientation.value==sladoPortrait;
    const int width=portrait?m_cfg.display_pixels_y.value:m_cfg.display_pixels_x.value;
    const int height=portrait?m_cfg.display_pixels_x.value:m_cfg.display_pixels_y.value;
    const double width_mm=portrait?m_cfg.display_height.value:m_cfg.display_width.value;
    const double height_mm=portrait?m_cfg.display_width.value:m_cfg.display_height.value;
    if(width<=0 || height<=0 || width>32768 || height>32768 || uint64_t(width)*height>128ULL*1024*1024 ||
        !std::isfinite(width_mm) || !std::isfinite(height_mm) || width_mm<=0 || height_mm<=0 ||
        !std::isfinite(m_cfg.max_print_height.value) || m_cfg.max_print_height.value<=0)
        throw ExportError("Invalid NanoDLP screen or machine dimensions");
    const auto &material=print.material_config();
    const double normal_height=print.default_object_config().layer_height.value;
    if(!std::isfinite(normal_height) || normal_height<=0 ||
        !std::isfinite(material.initial_layer_height.value) || material.initial_layer_height.value<=0)
        throw ExportError("Invalid NanoDLP layer height");
    if(material.sla_bottom_layers.value<0 || material.sla_bottom_layers.value>UINT16_MAX ||
        material.sla_transition_layers.value<0 || material.sla_transition_layers.value>UINT16_MAX)
        throw ExportError("NanoDLP bottom/transition counts exceed schema limits");
    auto root=nanodlp::schema_defaults();
    auto &meta=root["meta"], &slicer=root["slicer"], &plate=root["plate"], &profile=root["profile"];
    meta["Program"]="Sla-SlicerOS";meta["Version"]="1";
    // Reference defaults contain host-specific UVtools provenance. A native
    // file must identify the actual encoder and never advertise that runtime.
#ifdef _WIN32
    meta["OS"]="windows";
#elif defined(__APPLE__)
    meta["OS"]="macos";
#else
    meta["OS"]="linux";
#endif
    meta["Arch"]=sizeof(void*)==8?"64-bit":"32-bit";
    const auto now=std::time(nullptr);
    if(now<0)throw ExportError("Cannot determine NanoDLP metadata timestamp");
    plate["CreatedDate"]=plate["Updated"]=profile["Updated"]=uint64_t(now);
    plate["Path"]=projectname;profile["Title"]=m_cfg.printer_model.value;
    profile["Desc"]=projectname;
    // These device-specific defaults cannot be treated as configured motion.
    // Metadata preparation intentionally leaves templates empty; export_print
    // remains blocked until exposure/movement scripts are implemented.
    profile["ShieldBeforeLayer"]="";profile["ShieldAfterLayer"]="";
    const auto bottom=sla::bottom_layers(print,m_layers.size());
    slicer["PWidth"]=width;slicer["PHeight"]=height;
    slicer["XOffset"]=width/2;slicer["YOffset"]=height/2;
    // Do not copy UVtools' four-decimal pitch setter: it changes LCD dimensions.
    slicer["XPixelSize"]=width_mm/width;slicer["YPixelSize"]=height_mm/height;
    slicer["Thickness"]=normal_height*1000.;profile["Depth"]=normal_height*1000.;
    slicer["SupportDepth"]=material.initial_layer_height.value*1000.;
    profile["SupportDepth"]=material.initial_layer_height.value*1000.;
    slicer["SupportLayerNumber"]=bottom;profile["SupportLayerNumber"]=bottom;
    slicer["LayerCount"]=m_layers.size();plate["LayersCount"]=m_layers.size();
    slicer["AntiAlias"]=profile["AntiAlias"]=16;
    slicer["DisplayController"]=m_rgb?0:1;
    // ImageMirror has only a horizontal flag. Vertical reflection and rotated
    // metadata need the complete device contract, not an invented flag value.
    slicer["ImageMirror"]=m_cfg.display_mirror_x.value?1:0;
    profile["TransitionalLayer"]=material.sla_transition_layers.value;
    for(bool is_bottom:{false,true}) {
        const std::string prefix=is_bottom?"Support":"";
        for(const auto &pair:{std::pair{"WaitBeforePrint","wait_before_cure"},
            {"WaitAfterPrint","wait_after_cure"},{"TopWait","wait_after_lift"},{"WaitHeight","lift_height"}}) {
            const float value=sla::motion_parameter(print,pair.second,is_bottom);
            if(!std::isfinite(value) || value<0)throw ExportError("Invalid NanoDLP parameter");
            profile[prefix+pair.first]=value;
        }
        const auto exposure=is_bottom?material.initial_exposure_time.value:material.exposure_time.value;
        if(!std::isfinite(exposure) || exposure<0)throw ExportError("Invalid NanoDLP exposure");
        profile[is_bottom?"SupportCureTime":"CureTime"]=exposure;
    }
    const auto pwm=sla::motion_parameter(print,"light_pwm",false);
    if(pwm<0 || pwm>255)throw ExportError("Invalid NanoDLP UV PWM");
    profile["CustomValues"]["UvPwmValue"]=std::to_string(double(pwm)/255.);
    nlohmann::json info=nlohmann::json::array();
    double previous_z=0,volume=0,total_solid_area=0;
    nanodlp::ContourStats union_box;union_box.min_x=uint32_t(width);union_box.min_y=uint32_t(height);
    bool occupied=false;
    nanodlp::PacketView cached;
    for(size_t i=0;i<m_layers.size();++i) {
        if(!i || m_layers[i].size()!=m_layers[i-1].size() ||
            std::memcmp(m_layers[i].data(),m_layers[i-1].data(),m_layers[i].size())!=0)
            cached=nanodlp::inspect_packet(m_layers[i]);
        if(cached.metadata["width"]!=width || cached.metadata["height"]!=height || cached.metadata["rgb"]!=m_rgb)
            throw ExportError("NanoDLP packet differs from configured screen or image mode");
        const auto stats=cached.statistics();
        const double z=sla::layer_z(print,i);
        if(!std::isfinite(z) || z<=previous_z || z>m_cfg.max_print_height.value)
            throw ExportError("NanoDLP requires increasing Z within machine height");
        volume+=stats.occupied_pixels*(width_mm/width)*(height_mm/height)*(z-previous_z)/1000.;
        previous_z=z;
        const auto layer_info=nanodlp::layer_info(stats,float(width_mm/width)*float(height_mm/height));
        total_solid_area+=layer_info["TotalSolidArea"].get<double>();
        info.push_back(layer_info);
        if(stats.occupied_pixels) {
            occupied=true;
            union_box.min_x=std::min(union_box.min_x,stats.min_x);union_box.min_y=std::min(union_box.min_y,stats.min_y);
            union_box.max_x=std::max(union_box.max_x,stats.max_x);union_box.max_y=std::max(union_box.max_y,stats.max_y);
        }
    }
    if(!occupied)union_box.min_x=union_box.min_y=0;
    for(const auto &pair:{std::pair{"XMin",double(union_box.min_x)*width_mm/width-width_mm/2},
        {"XMax",double(union_box.max_x)*width_mm/width-width_mm/2},
        {"YMin",double(union_box.min_y)*height_mm/height-height_mm/2},
        {"YMax",double(union_box.max_y)*height_mm/height-height_mm/2},
        {"ZMin",0.},{"ZMax",previous_z}})
        slicer["Boundary"][pair.first]=plate[pair.first]=std::round(pair.second*10000.)/10000.;
    if(!std::isfinite(volume))throw ExportError("NanoDLP resin volume overflow");
    // NanoDLP TotalSolidArea is mm². The local UVtools changelog v5.2.1
    // (#1069) explicitly reverts ml here, but its current setter still stores
    // ml. Keep that original encoder discrepancy visible instead of copying it.
    plate["TotalSolidArea"]=total_solid_area;
    // Internal application statistics are not a NanoDLP manifest. A future
    // writer must not mistake this object for another required ZIP entry.
    root["native_statistics"]={{"material_milliliters",volume}};
    root["info"]=std::move(info);
    const auto finite_json=[](const auto &self,const nlohmann::json &value)->bool {
        if(value.is_number_float())return std::isfinite(value.get<double>());
        if(value.is_array() || value.is_object())
            for(const auto &child:value)if(!self(self,child))return false;
        return true;
    };
    if(!finite_json(finite_json,root))throw ExportError("NanoDLP metadata contains a nonfinite field");
    // Validate UTF-8/serialization before a future writer opens its destination.
    root.dump();
    return root;
}

namespace {
// Template/profile validation proves framing and preservation, not the
// controller's execution semantics. Never silently insert a hardware script.
void validate_device_profile(const nlohmann::json &profile)
{
    if (!profile.is_object())
        throw ExportError("NanoDLP requires a device profile object");
    const auto schema = nanodlp::schema_defaults()["profile"];
    for (auto it = profile.begin(); it != profile.end(); ++it) {
        const auto expected = schema.find(it.key());
        if (expected == schema.end())
            throw ExportError("Unknown NanoDLP device profile field: " + it.key());
        const auto &value = it.value();
        if ((expected->is_string() && !value.is_string()) ||
            (expected->is_number() && (!value.is_number() || !std::isfinite(value.get<double>()))) ||
            (expected->is_object() && !value.is_object()))
            throw ExportError("Invalid NanoDLP device profile field: " + it.key());
        if (value.is_number()) {
            const double number = value.get<double>();
            if (!std::isfinite(float(number)))
                throw ExportError("NanoDLP profile number exceeds float range: " + it.key());
            // Actual CLR wire types from NanoDLPProfileManifest. JSON permits
            // fractions/negative values that its unsigned reader cannot accept.
            static const std::map<std::string, uint64_t> unsigned_limits = {
                {"ResinID", UINT32_MAX}, {"ProfileID", UINT32_MAX}, {"ZStepWait", UINT32_MAX},
                {"DimSkip", UINT32_MAX}, {"ElephantWall", UINT32_MAX}, {"ElephantLayers", UINT32_MAX},
                {"HatchingWall", UINT32_MAX}, {"HatchingGap", UINT32_MAX}, {"HatchingOuterWall", UINT32_MAX},
                {"HatchingTopCap", UINT32_MAX}, {"HatchingBottomCap", UINT32_MAX},
                {"SlowSectionStepWait", UINT32_MAX}, {"JumpPerLayer", UINT32_MAX},
                {"TransitionalLayer", UINT16_MAX}, {"SupportLayerNumber", UINT16_MAX},
                {"AdaptSlicing", UINT8_MAX}, {"PixelDiming", UINT8_MAX}, {"HatchingType", UINT8_MAX},
                {"ElephantMidExposure", UINT8_MAX}, {"ElephantType", UINT8_MAX}, {"AntiAlias", UINT8_MAX},
                {"AntiAlias3D", UINT8_MAX}, {"AntiAlias3DMin", UINT8_MAX}, {"ImageRotate", UINT8_MAX},
                {"IgnoreMask", UINT8_MAX}
            };
            const auto limit = unsigned_limits.find(it.key());
            if (limit != unsigned_limits.end() &&
                (!value.is_number_integer() || number < 0 || number > double(limit->second)))
                throw ExportError("Invalid unsigned NanoDLP profile field: " + it.key());
            if ((it.key() == "Type" || it.key() == "DimWall") &&
                (!value.is_number_integer() || number < INT32_MIN || number > INT32_MAX))
                throw ExportError("Invalid signed NanoDLP profile field: " + it.key());
        }
        if (it.key() == "Updated" &&
            (!value.is_number_integer() ||
             (value.is_number_unsigned() && value.get<uint64_t>() > uint64_t(INT64_MAX))))
            throw ExportError("Invalid NanoDLP device timestamp");
        if (value.is_string()) {
            const auto &text = value.get_ref<const std::string &>();
            if (text.size() > 1024 * 1024 || text.find('\0') != std::string::npos)
                throw ExportError("Invalid NanoDLP device template length or NUL");
        }
    }
    for (const char *key : {"ShieldBeforeLayer", "ShieldAfterLayer"}) {
        const auto it = profile.find(key);
        if (it == profile.end() || !it->is_string() ||
            it->get_ref<const std::string &>().find_first_not_of(" \t\r\n") == std::string::npos)
            throw ExportError(std::string("NanoDLP device profile requires ") + key);
    }
    if (const auto it = profile.find("CustomValues"); it != profile.end())
        for (auto value = it->begin(); value != it->end(); ++value)
            if (!value.value().is_string() || value.value().get_ref<const std::string &>().size() > 65536)
                throw ExportError("NanoDLP device custom values must be bounded strings");
    // Includes UTF-8 validation before opening the destination.
    if (profile.dump().size() > 4 * 1024 * 1024)
        throw ExportError("NanoDLP device profile exceeds metadata limit");
}
}

namespace {
void validate_print_parameters(const SLAPrint &print, const SLAPrinterConfig &cfg, bool check_layers)
{
    if (cfg.display_pixels_x.value <= 0 || cfg.display_pixels_y.value <= 0 ||
        cfg.display_pixels_x.value > 32768 || cfg.display_pixels_y.value > 32768 ||
        uint64_t(cfg.display_pixels_x.value) * cfg.display_pixels_y.value > (uint64_t(128) << 20) ||
        !std::isfinite(cfg.display_width.value) || !std::isfinite(cfg.display_height.value) ||
        cfg.display_width.value <= 0 || cfg.display_height.value <= 0 ||
        !std::isfinite(cfg.max_print_height.value) || cfg.max_print_height.value <= 0)
        throw ExportError("Invalid NanoDLP display or machine dimensions");
    for (int count : {print.material_config().sla_bottom_layers.value, print.material_config().sla_transition_layers.value})
        if (count < 0 || count > UINT16_MAX)
            throw ExportError("NanoDLP bottom/transition counts exceed uint16");

    // The public schema does not carry per-layer Z or independent PWM. Its
    // generic decoder reconstructs a uniform stack from the normal height.
    // Refuse values that would be silently changed even with valid scripts.
    const double layer_height = print.default_object_config().layer_height.value;
    const double initial_height = print.material_config().initial_layer_height.value;
    if (cfg.display_mirror_y.value || !std::isfinite(initial_height) ||
        std::abs(initial_height - layer_height) > 1e-7 ||
        !std::isfinite(layer_height) || layer_height <= 0 ||
        std::abs(std::round(layer_height * 1000) / 1000 - layer_height) > 1e-7)
        throw ExportError("NanoDLP requires uniform 0.001 mm layer height and no vertical mirror");
    if (sla::motion_parameter(print, "light_pwm", true) != sla::motion_parameter(print, "light_pwm", false))
        throw ExportError("NanoDLP stores one shared bottom/normal PWM");
    if (check_layers) for (size_t index = 0; index < print.print_layers().size(); ++index)
        if (std::abs(double(print.print_layers()[index].level()) * SCALING_FACTOR - layer_height * (index + 1)) > 1e-5)
            throw ExportError("NanoDLP cannot preserve variable per-layer Z in this schema");
    for (bool bottom : {false, true}) {
        for (const char *key : {"wait_before_cure", "wait_after_cure", "wait_after_lift", "lift_height"}) {
            const double value = sla::motion_parameter(print, key, bottom);
            if (!std::isfinite(value) || value < 0 || std::abs(std::round(value * 100) / 100 - value) > 1e-6)
                throw ExportError(std::string("NanoDLP requires 0.01 precision for ") + key);
        }
        const double exposure = bottom ? print.material_config().initial_exposure_time.value :
                                         print.material_config().exposure_time.value;
        if (!std::isfinite(exposure) || exposure < 0 || std::abs(std::round(exposure * 100) / 100 - exposure) > 1e-6)
            throw ExportError("NanoDLP requires 0.01 second exposure precision");
    }
}
}

nlohmann::json NanoDLPArchive::parse_device_profile(const std::string &json)
{
    if (json.empty())
        throw ExportError("NanoDLP requires a controller profile. Load profile.json in Printer Settings > NanoDLP controller and save the printer preset.");
    if (json.size() > 4 * 1024 * 1024)
        throw ExportError("NanoDLP controller profile exceeds 4 MiB");
    try {
        auto profile = nlohmann::json::parse(json);
        validate_device_profile(profile);
        return profile;
    } catch (const ExportError &) {
        throw;
    } catch (const std::exception &error) {
        throw ExportError(std::string("Invalid NanoDLP controller profile JSON: ") + error.what());
    }
}

void NanoDLPArchive::validate_configuration(const SLAPrint &print)
{
    parse_device_profile(print.printer_config().nanodlp_device_profile.value);
    validate_print_parameters(print, print.printer_config(), false);
}

void NanoDLPArchive::export_with_profile(const std::string &filename, const SLAPrint &print,
                                       const ThumbnailsList &thumbnails,
                                       const nlohmann::json &device_profile, unsigned version,
                                       const std::string &projectname) const
{
    if (version != 1 && version != 2)
        throw ExportError("NanoDLP supports container versions 1 and 2");
    validate_device_profile(device_profile);
    validate_print_parameters(print, m_cfg, true);
    auto root = prepare_metadata(print, projectname);
    auto &slicer = root["slicer"];
    root["meta"]["FormatVersion"] = version;
    // Device scripts/custom settings are preserved; canonical print fields
    // then take precedence over stale geometry/exposure in a copied profile.
    auto profile = nanodlp::schema_defaults()["profile"];
    profile.update(device_profile);
    const auto &prepared = root["profile"];
    for (const char *key : {"Title", "Desc", "Depth", "SupportDepth", "Updated", "AntiAlias",
         "SupportLayerNumber", "TransitionalLayer", "CureTime", "SupportCureTime",
         "WaitBeforePrint", "SupportWaitBeforePrint", "WaitAfterPrint", "SupportWaitAfterPrint",
         "TopWait", "SupportTopWait", "WaitHeight", "SupportWaitHeight"})
        profile[key] = prepared[key];
    profile["CustomValues"]["UvPwmValue"] = prepared["CustomValues"]["UvPwmValue"];
    root["profile"] = std::move(profile);

    // View validation retains pointers into immutable encoded masks; no
    // second PNG decode, contour extraction, or whole-file raster buffer.
    std::vector<nanodlp::PacketView> images;
    images.reserve(m_layers.size());
    for (const auto &layer : m_layers)
        images.push_back(nanodlp::inspect_packet(layer));

    sla::EncodedRaster preview;
    // Source renderer uses bottom-up RGBA. NanoDLP preview is top-down RGB
    // with alpha composited over black, fitted to the original 607x487 frame.
    for (const auto &thumbnail : thumbnails) {
        if (!thumbnail.is_valid() || thumbnail.pixels.size() != size_t(thumbnail.width) * thumbnail.height * 4)
            continue;
        constexpr size_t width = 607, height = 487;
        std::vector<uint8_t> rgb(width * height * 3, 0);
        const double scale = std::min(double(width) / thumbnail.width, double(height) / thumbnail.height);
        const size_t columns = std::min(width, std::max(size_t(1), size_t(std::round(thumbnail.width * scale))));
        const size_t rows = std::min(height, std::max(size_t(1), size_t(std::round(thumbnail.height * scale))));
        for (size_t y = 0; y < rows; ++y)
            for (size_t x = 0; x < columns; ++x) {
                const size_t from = ((thumbnail.height - 1 - y * thumbnail.height / rows) * thumbnail.width +
                                     x * thumbnail.width / columns) * 4;
                const size_t to = ((y + (height - rows) / 2) * width + x + (width - columns) / 2) * 3;
                for (size_t channel = 0; channel < 3; ++channel)
                    rgb[to + channel] = uint8_t((unsigned(thumbnail.pixels[from + channel]) *
                                               thumbnail.pixels[from + 3] + 127) / 255);
            }
        preview = sla::PNGRasterEncoder{}(rgb.data(), width, height, 3);
        slicer["PreviewWidth"] = width;
        slicer["PreviewHeight"] = height;
        break;
    }
    // Serialize every manifest before touching the destination. Internal
    // native_statistics and NDP1 packet headers never enter the public ZIP.
    std::vector<std::pair<std::string, std::string>> manifests;
    for (const char *key : {"meta", "slicer", "plate", "profile", "info"})
        manifests.emplace_back(std::string(key) + ".json", root[key].dump(2));
    Zipper zip(filename);
    for (const auto &manifest : manifests) {
        zip.add_entry(manifest.first);
        zip << manifest.second;
    }
    if (preview.size())
        zip.add_entry("3d.png", preview.data(), preview.size());
    for (size_t index = 0; index < images.size(); ++index)
        zip.add_entry(std::to_string(index + 1) + ".png", images[index].png, images[index].png_size);
    zip.finalize();
}

void NanoDLPArchive::export_print(const std::string     fname,
                                   const SLAPrint       &print,
                                   const ThumbnailsList &thumbnails,
                                   const std::string    &projectname)
{
    const auto profile = parse_device_profile(m_cfg.nanodlp_device_profile.value);
    export_with_profile(fname, print, thumbnails, profile, m_version, projectname);
}

}
