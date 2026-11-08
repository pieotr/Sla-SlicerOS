#include "libslic3r/SLA/PrinterRaster.hpp"
///|/ Copyright (c) Prusa Research 2020 - 2023 Tomáš Mészáros @tamasmeszaros, Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "SL1.hpp"
#include "libslic3r/SLA/SL1FreshExportPlan.hpp"
#include "libslic3r/SLA/SL1FreshMaterialProfile.hpp"

#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>

#include <sstream>
#include <iomanip>
#include <locale>
#include <cmath>

#include "libslic3r/Time.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/MTUtils.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "libslic3r/miniz_extension.hpp" // IWYU pragma: keep
#include <LocalesUtils.hpp>
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Utils/JsonUtils.hpp"

#include "SLAArchiveReader.hpp"
#include "SLAArchiveFormatRegistry.hpp"
#include "ZipperArchiveImport.hpp"

#include "libslic3r/MarchingSquares.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Execution/ExecutionTBB.hpp"

#include "libslic3r/SLA/RasterBase.hpp"


#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/algorithm/string.hpp>

namespace Slic3r {

using ConfMap = std::map<std::string, std::string>;

namespace {

std::string to_ini(const ConfMap &m)
{
    std::string ret;
    for (auto &param : m)
        ret += param.first + " = " + param.second + "\n";

    return ret;
}

static std::string get_key(const std::string& opt_key)
{
    static const std::set<std::string> ms_opts = {
      "delay_before_exposure"
    , "delay_after_exposure"
    , "tilt_down_offset_delay"
    , "tilt_up_offset_delay"
    , "tilt_down_delay"
    , "tilt_up_delay"
    };
    
    static const std::set<std::string> nm_opts = {
       "tower_hop_height"
    };
    
    static const std::set<std::string> speed_opts = {
      "tower_speed"
    , "tilt_down_initial_speed"
    , "tilt_down_finish_speed"
    , "tilt_up_initial_speed"
    , "tilt_up_finish_speed"
    };

    if (ms_opts.find(opt_key) != ms_opts.end())
        return opt_key + "_ms";

    if (nm_opts.find(opt_key) != nm_opts.end())
        return opt_key + "_nm";

    if (speed_opts.find(opt_key) != speed_opts.end())
        return boost::replace_all_copy(opt_key, "_speed", "_profile");

    return opt_key;
}

namespace pt = boost::property_tree;

std::string to_json(const SLAPrint& print, const ConfMap &m)
{
    auto& cfg = print.full_print_config();

    pt::ptree below_node;
    pt::ptree above_node;

    const t_config_enum_names& tilt_enum_names  = ConfigOptionEnum< TiltSpeeds>::get_enum_names();
    const t_config_enum_names& tower_enum_names = ConfigOptionEnum<TowerSpeeds>::get_enum_names();

    for (const std::string& opt_key : tilt_options()) {
        const ConfigOption* opt = cfg.option(opt_key);
        assert(opt != nullptr);

        switch (opt->type()) {
        case coFloats: {
            auto values = static_cast<const ConfigOptionFloats*>(opt);
            double koef = opt_key == "tower_hop_height" ? 1000000. : 1000.; // export in nm (instead of mm), resp. in ms (instead of s)
            below_node.put<double>(get_key(opt_key), int(koef * values->get_at(0)));
            above_node.put<double>(get_key(opt_key), int(koef * values->get_at(1)));
        }
        break;
        case coInts: {
            auto values = static_cast<const ConfigOptionInts*>(opt);
            below_node.put<int>(get_key(opt_key), values->get_at(0));
            above_node.put<int>(get_key(opt_key), values->get_at(1));
        }
        break;
        case coBools: {
            auto values = static_cast<const ConfigOptionBools*>(opt);
            below_node.put<bool>(get_key(opt_key), values->get_at(0));
            above_node.put<bool>(get_key(opt_key), values->get_at(1));
        }
        break;
        case coEnums: {
            const t_config_enum_names& enum_names = opt_key == "tower_speed" ? tower_enum_names : tilt_enum_names;
            auto values = static_cast<const ConfigOptionEnums<TiltSpeeds>*>(opt);
            below_node.put(get_key(opt_key), enum_names[values->get_at(0)]);
            above_node.put(get_key(opt_key), enum_names[values->get_at(1)]);
        }
        break;
        case coNone:
        default:
            break;
        }
    }

    pt::ptree profile_node;
    profile_node.put("area_fill", cfg.option("area_fill")->serialize());
    profile_node.add_child("below_area_fill", below_node);
    profile_node.add_child("above_area_fill", above_node);

    pt::ptree root;
    // params from config.ini
    for (auto& param : m)
        root.put(param.first, param.second );

    root.put("version", "1");
    root.add_child("exposure_profile", profile_node);

    // Boost confirms its implementation has no 100% conformance to JSON standard. 
    // In the boost libraries, boost will always serialize each value as string and parse all values to a string equivalent.
    // so, post-prosess output
    return write_json_with_post_process(root);
}

std::string get_cfg_value(const DynamicPrintConfig &cfg, const std::string &key)
{
    std::string ret;
    
    if (cfg.has(key)) {
        auto opt = cfg.option(key);
        if (opt) ret = opt->serialize();
    }
    
    return ret;    
}

void fill_iniconf(ConfMap &m, const SLAPrint &print, const sla::SL1FreshExportPlan *fresh)
{
    CNumericLocalesSetter locales_setter; // for to_string
    auto &cfg = print.full_print_config();
    m["layerHeight"]    = get_cfg_value(cfg, "layer_height");
    m["expTime"]        = get_cfg_value(cfg, "exposure_time");
    m["expTimeFirst"]   = get_cfg_value(cfg, "initial_exposure_time");
    const std::string mps = get_cfg_value(cfg, "material_print_speed");
    m["expUserProfile"] = mps == "slow" ? "1" : mps == "fast" ? "0" : "2";
    m["materialName"]   = get_cfg_value(cfg, "sla_material_settings_id");
    m["printerModel"]   = get_cfg_value(cfg, "printer_model");
    m["printerVariant"] = get_cfg_value(cfg, "printer_variant");
    m["printerProfile"] = get_cfg_value(cfg, "printer_settings_id");
    m["printProfile"]   = get_cfg_value(cfg, "sla_print_settings_id");
    m["fileCreationTimestamp"] = Utils::utc_timestamp();
    m["prusaSlicerVersion"]    = SLIC3R_BUILD_ID;
    
    // UVtools maps NumFade to bottom-layer count. Use the canonical LCD count,
    // rather than the legacy Prusa fade setting (a different exposure policy).
    const auto num_fade = sla::bottom_layers(print, print.print_layers().size());
    
    m["numFade"] = std::to_string(num_fade);
    if (!fresh) {
        const auto &stats = print.print_statistics();
        const double used_material = (stats.objects_used_material + stats.support_used_material) / 1000.;
        m["usedMaterial"] = std::to_string(used_material);
        m["numSlow"] = std::to_string(stats.slow_layers_count);
        m["numFast"] = std::to_string(stats.fast_layers_count);
        m["printTime"] = std::to_string(stats.estimated_print_time);
    }
    // Fresh exports below use complete format-owned metrics and therefore do
    // not ask the internal encoding context for unrelated job statistics.

    bool hollow_en = false;
    auto it = print.objects().begin();
    while (!hollow_en && it != print.objects().end())
        hollow_en = (*it++)->config().hollowing_enable;

    m["hollow"] = hollow_en ? "1" : "0";
    
    m["action"] = "print";
    if (fresh) {
        auto decimal = [](double value) {
            std::ostringstream text; text.imbue(std::locale::classic());
            text << std::setprecision(17) << value; return text.str();
        };
        using F = sla::LayerParameter;
        m["layerHeight"] = decimal(std::round(fresh->globals.layer_height * 1000.) / 1000.);
        m["expTime"] = decimal(fresh->globals.normal.at(F::Exposure));
        m["expTimeFirst"] = decimal(fresh->globals.bottom.at(F::Exposure));
        m["numFade"] = std::to_string(fresh->globals.bottom_layers);
        m["numSlow"] = std::to_string(fresh->globals.bottom_layers);
        m["numFast"] = std::to_string(fresh->rows.size() - fresh->globals.bottom_layers);
        m["usedMaterial"] = decimal(fresh->material_ml);
        m["printTime"] = decimal(fresh->expected_print_time);
    }

}

void fill_slicerconf(ConfMap &m, const SLAPrint &print, const sla::SL1FreshExportPlan *fresh)
{
    using namespace std::literals::string_view_literals;
    
    // Sorted list of config keys, which shall not be stored into the ini.
    static constexpr auto banned_keys = { 
		"compatible_printers"sv,
        "compatible_prints"sv,
        //FIXME The print host keys should not be exported to full_print_config anymore. The following keys may likely be removed.
        "print_host"sv,
        "printhost_apikey"sv,
        "printhost_cafile"sv
    };
    
    assert(std::is_sorted(banned_keys.begin(), banned_keys.end()));
    auto is_banned = [](const std::string &key) {
        return std::binary_search(banned_keys.begin(), banned_keys.end(), key);
    };

    auto is_tilt_param = [](const std::string& key) -> bool {
        const auto& keys = tilt_options();
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    };
    
    auto &cfg = print.full_print_config();
    for (const std::string &key : cfg.keys())
        if (! is_banned(key) && !is_tilt_param(key) && ! cfg.option(key)->is_nil())
            m[key] = cfg.opt_serialize(key);

    // External consumers use raster-axis geometry. The config.ini marker lets
    // our reader recover the original scene-axis printer profile unambiguously.
    if (cfg.option("display_orientation")->getInt() == sla::RasterBase::roPortrait) {
        std::swap(m["display_width"], m["display_height"]);
        std::swap(m["display_pixels_x"], m["display_pixels_y"]);
    }

    // UVtools' SL1 reader carries LCD parameters in escaped notes. Preserve
    // unrelated user notes, removing previous values of these exact keywords
    // before adding one authoritative value per parameter.
    const std::pair<const char *, const char *> fields[] = {
        {"WaitTimeBeforeCure", "wait_before_cure"}, {"WaitTimeAfterCure", "wait_after_cure"},
        {"LiftHeight", "lift_height"}, {"LiftSpeed", "lift_speed"}, {"LiftHeight2", "lift_height2"},
        {"LiftSpeed2", "lift_speed2"}, {"WaitTimeAfterLift", "wait_after_lift"},
        {"RetractSpeed", "retract_speed"}, {"RetractHeight2", "retract_height2"},
        {"RetractSpeed2", "retract_speed2"}, {"LightPWM", "light_pwm"}
    };
    std::vector<std::string> keywords{"TransitionLayerCount"};
    if (fresh) { keywords.push_back("LightOffDelay"); keywords.push_back("BottomLightOffDelay"); }
    for (const auto &field : fields) {
        keywords.push_back(field.first); keywords.push_back(std::string("Bottom")+field.first);
    }
    for (const char *key : {"material_notes", "printer_notes"}) {
        auto notes = m[key];
        boost::replace_all(notes, "\\r\\n", "\\n"); boost::replace_all(notes, "\\r", "\\n");
        std::string retained;
        for (size_t start = 0; start < notes.size();) {
            const auto end = notes.find("\\n", start);
            const auto line = notes.substr(start, end == std::string::npos ? end : end-start);
            auto trimmed = boost::trim_copy(line);
            const bool owned = std::any_of(keywords.begin(), keywords.end(), [&](const auto &keyword) {
                return boost::istarts_with(trimmed, keyword+"_");
            });
            if (!owned) { if (!retained.empty()) retained += "\\n"; retained += line; }
            if (end == std::string::npos) break;
            start = end+2;
        }
        m[key] = retained;
    }
    std::ostringstream notes; notes.imbue(std::locale::classic()); notes << std::fixed << std::setprecision(9);
    if (!m["printer_notes"].empty()) notes << m["printer_notes"] << "\\n";
    notes << "TransitionLayerCount_" << (fresh ? fresh->globals.transition_layers : print.material_config().sla_transition_layers.value);
    for (const bool bottom : {false, true}) for (const auto &field : fields) {
        notes << "\\n" << (bottom ? "Bottom" : "") << field.first << '_';
        static const std::map<std::string, sla::LayerParameter> mapping{
            {"wait_before_cure",sla::LayerParameter::WaitBefore},{"wait_after_cure",sla::LayerParameter::WaitAfter},
            {"wait_after_lift",sla::LayerParameter::WaitAfterLift},{"lift_height",sla::LayerParameter::LiftDistance},
            {"lift_height2",sla::LayerParameter::LiftDistance2},{"lift_speed",sla::LayerParameter::LiftSpeed},
            {"lift_speed2",sla::LayerParameter::LiftSpeed2},{"retract_speed",sla::LayerParameter::RetractSpeed},
            {"retract_height2",sla::LayerParameter::RetractDistance2},{"retract_speed2",sla::LayerParameter::RetractSpeed2},
            {"light_pwm",sla::LayerParameter::LightPWM}};
        const float value = fresh ? float((bottom ? fresh->globals.bottom : fresh->globals.normal).at(mapping.at(field.second))) :
            sla::motion_parameter(print, field.second, bottom);
        if (std::string(field.second) == "light_pwm") notes << int(value);
        else notes << value;
    }
    if (fresh) {
        notes << "\\nBottomLightOffDelay_" << fresh->globals.bottom.at(sla::LayerParameter::LightOff);
        notes << "\\nLightOffDelay_" << fresh->globals.normal.at(sla::LayerParameter::LightOff);
        std::ostringstream height; height.imbue(std::locale::classic());
        height << std::setprecision(17) << std::round(fresh->initial_layer_height * 1000.) / 1000.;
        m["initial_layer_height"] = height.str();
        auto decimal = [](double value) {
            std::ostringstream text; text.imbue(std::locale::classic());
            text << std::setprecision(17) << value; return text.str();
        };
        m["layer_height"] = decimal(std::round(fresh->globals.layer_height * 1000.) / 1000.);
        m["exposure_time"] = decimal(fresh->globals.normal.at(sla::LayerParameter::Exposure));
        m["initial_exposure_time"] = decimal(fresh->globals.bottom.at(sla::LayerParameter::Exposure));
        m["display_width"] = decimal(fresh->geometry.width_mm);
        m["display_height"] = decimal(fresh->geometry.height_mm);
        m["display_pixels_x"] = std::to_string(fresh->geometry.columns);
        m["display_pixels_y"] = std::to_string(fresh->geometry.rows);
        const auto material = sla::sl1_fresh_material_profile(*fresh,print.material_config());
        const char *keys[]={"material_density","bottle_cost","bottle_volume","bottle_weight"};
        for (size_t i=0;i<material.size();++i) m[keys[i]]=decimal(material[i]);

    }
    m["printer_notes"] = notes.str();

}

} // namespace

std::unique_ptr<sla::RasterBase> SL1Archive::create_raster() const
{
    return sla::create_printer_raster(m_cfg);
}

sla::RasterEncoder SL1Archive::get_encoder() const
{
    return sla::PNGRasterEncoder{};
}

static void write_thumbnail(Zipper &zipper, const ThumbnailData &data)
{
    size_t png_size = 0;

    void  *png_data = tdefl_write_image_to_png_file_in_memory_ex(
         (const void *) data.pixels.data(), data.width, data.height, 4,
         &png_size, MZ_DEFAULT_LEVEL, 1);

    if (png_data != nullptr) {
        zipper.add_entry("thumbnail/thumbnail" + std::to_string(data.width) +
                             "x" + std::to_string(data.height) + ".png",
                         static_cast<const std::uint8_t *>(png_data),
                         png_size);

        mz_free(png_data);
    }
}

void SL1Archive::export_print(Zipper               &zipper,
                              const SLAPrint       &print,
                              const ThumbnailsList &thumbnails,
                              const std::string    &prjname,
                              const sla::SL1FreshExportPlan *fresh,
                              const sla::LayerDocument *validated_document)
{
    print.throw_if_canceled();
    const auto &levels = print.print_layers();
    if (sla::bottom_layers(print, levels.size()) > UINT16_MAX || print.material_config().sla_transition_layers.value > UINT16_MAX)
        throw ExportError("SL1 bottom/transition count exceeds its 16-bit metadata fields");
    for (size_t i = 0; !fresh && i < levels.size(); ++i) {
        const double expected = print.material_config().initial_layer_height.value + i * print.default_object_config().layer_height.value;
        if (std::abs(double(levels[i].level()) * SCALING_FACTOR - expected) > SCALING_FACTOR * 2)
            throw ExportError("SL1 cannot store variable layer heights after the first layer; choose UVJ or another format with per-layer Z");
    }
    std::string project =
        prjname.empty() ?
            boost::filesystem::path(zipper.get_filename()).stem().string() :
            prjname;

    ConfMap iniconf, slicerconf;
    fill_iniconf(iniconf, print, fresh);

    iniconf["jobDir"] = project;
    iniconf["slaSlicerProfileGeometry"] = "raster-v1";

    fill_slicerconf(slicerconf, print, fresh);

    try {
        zipper.add_entry("config.ini");
        zipper << to_ini(iniconf);
        zipper.add_entry("prusaslicer.ini");
        zipper << to_ini(slicerconf);

        zipper.add_entry("config.json");
        zipper << to_json(print, iniconf);

        if (validated_document) {
            // PNG bytes belong to the exact planned document. The synchronous
            // ZIP call consumes each borrowed view before the next checkpoint;
            // neither the writer nor a global cache retains that view.
            for (size_t i = 0; i < validated_document->layer_count(); ++i) {
                print.throw_if_canceled();
                const auto png = validated_document->encoded_layer(i);
                const auto imgname = project + string_printf("%.5d", int(i)) + ".png";
                zipper.add_entry(imgname.c_str(), png.buf, png.sz);
            }
        } else {
            size_t i = 0;
            for (const sla::EncodedRaster &rst : m_layers) {
                // A frozen export context checks its request-owned Cancel flag.
                print.throw_if_canceled();
                std::string imgname = project + string_printf("%.5d", i++) + "." + rst.extension();
                zipper.add_entry(imgname.c_str(), rst.data(), rst.size());
            }
        }

        std::set<std::pair<unsigned,unsigned>> thumbnail_sizes;
        for (const ThumbnailData& data : thumbnails) {
            print.throw_if_canceled();
            if (data.is_valid() && thumbnail_sizes.emplace(data.width,data.height).second)
                write_thumbnail(zipper, data);
        }
        print.throw_if_canceled();
        zipper.finalize();
    } catch(std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << e.what();
        // Rethrow the exception
        throw;
    }
}

void SL1Archive::export_fresh_document(const std::string &fname, const SLAPrint &print,
    const ThumbnailsList &thumbnails, const std::string &projectname,
    const sla::SL1FreshExportPlan &fresh, const sla::LayerDocument &document)
{
    print.throw_if_canceled();
    if (fresh.rows.empty() || fresh.rows.size() != print.print_layers().size() ||
        fresh.rows.size() != document.layer_count() || document.mask_plane_count() != 1)
        throw ExportError("SL1 fresh plan does not match its immutable PNG document");
    sla::sl1_fresh_material_profile(fresh, print.material_config());
    // Keep the existing aggregate encoder limit, although this branch allocates
    // no second complete set of PNG buffers. Do not rely on archive type or
    // mere IHDR validity: the caller has completed sl1_fresh_export_plan.
    constexpr uint64_t encoded_budget = uint64_t(1) << 30;
    uint64_t total = 0;
    for (size_t i = 0; i < document.layer_count(); ++i) {
        print.throw_if_canceled();
        const auto png = document.encoded_layer(i);
        if (!png.sz || png.sz > encoded_budget - total)
            throw ExportError("Encoded layers exceed the 1 GiB memory budget");
        total += png.sz;
    }
    print.throw_if_canceled();
    Zipper zipper(fname, Zipper::FAST_COMPRESSION);
    export_print(zipper, print, thumbnails, projectname, &fresh, &document);
}

void SL1Archive::export_fresh_print(const std::string &fname, const SLAPrint &print,
    const ThumbnailsList &thumbnails, const std::string &projectname, const sla::SL1FreshExportPlan &fresh)
{
    if (fresh.rows.size() != print.print_layers().size() || fresh.rows.size() != m_layers.size())
        throw ExportError("SL1 fresh plan does not match encoded layers");
    sla::sl1_fresh_material_profile(fresh,print.material_config());
    Zipper zipper(fname, Zipper::FAST_COMPRESSION);
    export_print(zipper, print, thumbnails, projectname, &fresh);
}

void SL1Archive::export_print(const std::string     fname,
                              const SLAPrint       &print,
                              const ThumbnailsList &thumbnails,
                              const std::string    &prjname)
{
    // Keep the previous export intact if validation or ZIP writing fails.
    // A short adjacent name also supports long destination filenames.
    struct StagingFile {
        boost::filesystem::path path;
        ~StagingFile() {
            boost::system::error_code error;
            if (!path.empty()) boost::filesystem::remove(path, error);
        }
    } staging{boost::filesystem::path(fname).parent_path() /
              boost::filesystem::unique_path(".sl1-%%%%-%%%%-%%%%-%%%%.tmp")};
    {
        Zipper zipper{staging.path.string(), Zipper::FAST_COMPRESSION};
        // JobDir and image names must use the real filename, never staging.
        const std::string project = prjname.empty() ? boost::filesystem::path(fname).stem().string() : prjname;
        export_print(zipper, print, thumbnails, project);
    }
    if (const auto error = rename_file(staging.path.string(), fname))
        throw ExportError("Cannot replace SL1/SL1S output: " + error.message());
    staging.path.clear();
}

} // namespace Slic3r

// /////////////////////////////////////////////////////////////////////////////
// Reader implementation
// /////////////////////////////////////////////////////////////////////////////

namespace marchsq {

template<> struct _RasterTraits<Slic3r::png::ImageGreyscale> {
    using Rst = Slic3r::png::ImageGreyscale;

       // The type of pixel cell in the raster
    using ValueType = uint8_t;

       // Value at a given position
    static uint8_t get(const Rst &rst, size_t row, size_t col)
    {
        return rst.get(row, col);
    }

       // Number of rows and cols of the raster
    static size_t rows(const Rst &rst) { return rst.rows; }
    static size_t cols(const Rst &rst) { return rst.cols; }
};

} // namespace marchsq

namespace Slic3r {

template<class Fn> static void foreach_vertex(ExPolygon &poly, Fn &&fn)
{
    for (auto &p : poly.contour.points) fn(p);
    for (auto &h : poly.holes)
        for (auto &p : h.points) fn(p);
}

void invert_raster_trafo(ExPolygons &                  expolys,
                         const sla::RasterBase::Trafo &trafo,
                         coord_t                       width,
                         coord_t                       height)
{
    if (trafo.flipXY) std::swap(height, width);

    for (auto &expoly : expolys) {
        if (trafo.mirror_y)
            foreach_vertex(expoly, [height](Point &p) {p.y() = height - p.y(); });

        if (trafo.mirror_x)
            foreach_vertex(expoly, [width](Point &p) {p.x() = width - p.x(); });

        expoly.translate(-trafo.center_x, -trafo.center_y);

        if (trafo.flipXY)
            foreach_vertex(expoly, [](Point &p) { std::swap(p.x(), p.y()); });

        if ((trafo.mirror_x + trafo.mirror_y + trafo.flipXY) % 2) {
            expoly.contour.reverse();
            for (auto &h : expoly.holes) h.reverse();
        }
    }
}

RasterParams get_raster_params(const DynamicPrintConfig &cfg)
{
    auto *opt_disp_cols = cfg.option<ConfigOptionInt>("display_pixels_x");
    auto *opt_disp_rows = cfg.option<ConfigOptionInt>("display_pixels_y");
    auto *opt_disp_w    = cfg.option<ConfigOptionFloat>("display_width");
    auto *opt_disp_h    = cfg.option<ConfigOptionFloat>("display_height");
    auto *opt_mirror_x  = cfg.option<ConfigOptionBool>("display_mirror_x");
    auto *opt_mirror_y  = cfg.option<ConfigOptionBool>("display_mirror_y");
    auto *opt_orient    = cfg.option<ConfigOptionEnum<SLADisplayOrientation>>("display_orientation");

    if (!opt_disp_cols || !opt_disp_rows || !opt_disp_w || !opt_disp_h ||
        !opt_mirror_x || !opt_mirror_y || !opt_orient)
        throw MissingProfileError("Invalid SL1 / SL1S file");

    if (opt_disp_cols->value <= 0 || opt_disp_rows->value <= 0 ||
        !std::isfinite(opt_disp_w->value) || !std::isfinite(opt_disp_h->value) ||
        opt_disp_w->value <= 0. || opt_disp_h->value <= 0.)
        throw MissingProfileError("Invalid SL1 display dimensions or resolution");

    RasterParams rstp;

    rstp.px_w = opt_disp_w->value / opt_disp_cols->value;
    rstp.px_h = opt_disp_h->value / opt_disp_rows->value;
    if (opt_orient->value == sladoPortrait)
        std::swap(rstp.px_w, rstp.px_h);

    rstp.trafo = sla::RasterBase::Trafo{opt_orient->value == sladoLandscape ?
                                            sla::RasterBase::roLandscape :
                                            sla::RasterBase::roPortrait,
                                        {opt_mirror_x->value, opt_mirror_y->value}};

    rstp.height = scaled(opt_disp_h->value);
    rstp.width  = scaled(opt_disp_w->value);

    return rstp;
}

namespace {

ExPolygons rings_to_expolygons(const std::vector<marchsq::Ring> &rings,
                               double px_w, double px_h)
{
    auto polys = reserve_vector<ExPolygon>(rings.size());

    for (const marchsq::Ring &ring : rings) {
        Polygon poly; Points &pts = poly.points;
        pts.reserve(ring.size());

        for (const marchsq::Coord &crd : ring)
            pts.emplace_back(scaled(crd.c * px_w), scaled(crd.r * px_h));

        polys.emplace_back(poly);
    }

    // TODO: Is a union necessary?
    return union_ex(polys);
}

std::vector<ExPolygons> extract_slices_from_sla_archive(
    ZipperArchive           &arch,
    const RasterParams      &rstp,
    const marchsq::Coord    &win,
    std::function<bool(int)> progr)
{
    std::vector<ExPolygons> slices(arch.entries.size());

    struct Status
    {
        double                                 incr, val, prev;
        bool                                   stop  = false;
        execution::SpinningMutex<ExecutionTBB> mutex = {};
    } st{100. / slices.size(), 0., 0.};

    execution::for_each(
        ex_tbb, size_t(0), arch.entries.size(),
        [&arch, &slices, &st, &rstp, &win, progr](size_t i) {
            // Status indication guarded with the spinlock
            {
                std::lock_guard lck(st.mutex);
                if (st.stop) return;

                st.val += st.incr;
                double curr = std::round(st.val);
                if (curr > st.prev) {
                    st.prev = curr;
                    st.stop = !progr(int(curr));
                }
            }

            png::ImageGreyscale img;
            png::ReadBuf        rb{arch.entries[i].buf.data(),
                            arch.entries[i].buf.size()};
            if (!png::decode_png(rb, img)) return;

            constexpr uint8_t isoval = 128;
            auto              rings = marchsq::execute(img, isoval, win);
            ExPolygons        expolys = rings_to_expolygons(rings, rstp.px_w,
                                                            rstp.px_h);

            // Invert the raster transformations indicated in the profile metadata
            invert_raster_trafo(expolys, rstp.trafo, rstp.width, rstp.height);

            slices[i] = std::move(expolys);
        },
        execution::max_concurrency(ex_tbb));

    if (st.stop) slices = {};

    return slices;
}

} // namespace

ConfigSubstitutions SL1Reader::read(std::vector<ExPolygons> &slices,
                                    DynamicPrintConfig      &profile_out)
{
    Vec2i windowsize;

    switch(m_quality)
    {
    case SLAImportQuality::Fast: windowsize = {8, 8}; break;
    case SLAImportQuality::Balanced: windowsize = {4, 4}; break;
    default:
    case SLAImportQuality::Accurate:
        windowsize = {2, 2}; break;
    };

    // Ensure minimum window size for marching squares
    windowsize.x() = std::max(2, windowsize.x());
    windowsize.y() = std::max(2, windowsize.y());

    std::vector<std::string> includes = { "ini", "png"};
    std::vector<std::string> excludes = { "thumbnail" };
    ZipperArchive arch = read_zipper_archive(m_fname, includes, excludes);
    auto [profile_use, config_substitutions] = extract_profile(arch, profile_out);

    RasterParams   rstp = get_raster_params(profile_use);
    marchsq::Coord win  = {windowsize.y(), windowsize.x()};
    slices = extract_slices_from_sla_archive(arch, rstp, win, m_progr);

    return std::move(config_substitutions);
}

ConfigSubstitutions SL1Reader::read(DynamicPrintConfig &out)
{
    ZipperArchive arch = read_zipper_archive(m_fname, {"ini"}, {"png", "thumbnail"});
    return extract_profile(arch, out).second;
}

} // namespace Slic3r
