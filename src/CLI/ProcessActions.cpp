#include <algorithm>
#include "libslic3r/Utils.hpp"
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <cstring>
#include <iostream>
#include <math.h>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/iostream.hpp>
#include <boost/nowide/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/iostream.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/dll/runtime_symbol_info.hpp>

#include "libslic3r/libslic3r.h"
#if !SLIC3R_OPENGL_ES
#include <boost/algorithm/string/split.hpp>
#endif // !SLIC3R_OPENGL_ES
#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include <arrange-wrapper/ModelArrange.hpp>
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerConversionPlan.hpp"
#include "libslic3r/SLA/RepresentationAdjustment.hpp"
#include "libslic3r/Format/AMF.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Format/SL1.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerSuggestions.hpp"
#include "libslic3r/SLA/LayerIssues.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/SLA/LayerRecipe.hpp"
#include <atomic>
#include <csignal>
#include <set>
#include <cmath>
#include "libslic3r/MultipleBeds.hpp"
#include "libslic3r/BuildVolume.hpp"

#include "CLI/CLI.hpp"
#include "CLI/ProfilesSharingUtils.hpp"


namespace Slic3r::CLI {

static bool has_profile_sharing_action(const Data& cli)
{
    return cli.actions_config.has("query-printer-models") || cli.actions_config.has("query-print-material-profiles");
}

bool has_full_config_from_profiles(const Data& cli)
{
    const DynamicPrintConfig& input = cli.input_config;
    return !has_profile_sharing_action(cli) &&
           ((input.has("print-profile") && !input.opt_string("print-profile").empty()) ||
            (input.has("material-profile") && !input.option<ConfigOptionStrings>("material-profile")->values.empty()) ||
            (input.has("printer-profile") && !input.opt_string("printer-profile").empty()));
}

std::optional<int> process_profiles_sharing(const Data& cli)
{
    if (!has_profile_sharing_action(cli))
        return std::nullopt;

    std::string ret;

    if (cli.actions_config.has("query-printer-models")) {
        ret = Slic3r::get_json_printer_models(ptSLA);
    }
    else if (cli.actions_config.has("query-print-material-profiles")) {
        if (cli.input_config.has("printer-profile") && !cli.input_config.opt_string("printer-profile").empty()) {
            const std::string printer_profile = cli.input_config.opt_string("printer-profile");
            ret = Slic3r::get_json_print_material_profiles(printer_profile);
            if (ret.empty()) {
                boost::nowide::cerr << "query-print-material-profiles error: Printer profile '" << printer_profile <<
                                        "' wasn't found among installed printers." << std::endl <<
                                        "Or the request can be wrong." << std::endl;
                return 1;
            }
        }
        else {
            boost::nowide::cerr << "query-print-material-profiles error: This action requires set 'printer-profile' option" << std::endl;
            return 1;
        }
    }

    if (ret.empty()) {
        boost::nowide::cerr << "Wrong request" << std::endl;
        return 1;
    }

    // use --output when available

    if (cli.misc_config.has("output")) {
        std::string cmdline_param = cli.misc_config.opt_string("output");
        // if we were supplied a directory, use it and append our automatically generated filename
        boost::filesystem::path cmdline_path(cmdline_param);
        boost::filesystem::path proposed_path = boost::filesystem::path(Slic3r::resources_dir()) / "out.json";
        if (boost::filesystem::is_directory(cmdline_path))
            proposed_path = (cmdline_path / proposed_path.filename());
        else if (cmdline_path.extension().empty())
            proposed_path = cmdline_path.replace_extension("json");
        else
            proposed_path = cmdline_path;
        const std::string file = proposed_path.string();

        boost::nowide::ofstream c;
        c.open(file, std::ios::out | std::ios::trunc);
        c << ret << std::endl;
        c.close();
        if (!c) {
            boost::nowide::cerr << "Cannot write profile query output: " << file << std::endl;
            return 1;
        }

        boost::nowide::cout << "Output for your request is written into " << file << std::endl;
    }
    else 
        printf("%s", ret.c_str());

    return 0;
}

namespace IO {
    enum ExportFormat : int {
        OBJ,
        STL,
        // SVG, 
        TMF
    };
}

static std::string output_filepath(const Model& model, IO::ExportFormat format, const std::string& cmdline_param)
{
    std::string ext;
    switch (format) {
    case IO::OBJ: ext = ".obj"; break;
    case IO::STL: ext = ".stl"; break;
    case IO::TMF: ext = ".3mf"; break;
    default: assert(false); break;
    };
    auto proposed_path = boost::filesystem::path(model.propose_export_file_name_and_path(ext));
    // use --output when available
    if (!cmdline_param.empty()) {
        // if we were supplied a directory, use it and append our automatically generated filename
        boost::filesystem::path cmdline_path(cmdline_param);
        if (boost::filesystem::is_directory(cmdline_path))
            proposed_path = cmdline_path / proposed_path.filename();
        else
            proposed_path = cmdline_path;
    }
    return proposed_path.string();
}

static bool export_models(std::vector<Model>& models, IO::ExportFormat format, const std::string& cmdline_param)
{
    for (Model& model : models) {
        const std::string path = output_filepath(model, format, cmdline_param);
        bool success = false;
        switch (format) {
        case IO::OBJ: success = Slic3r::store_obj(path.c_str(), &model);          break;
        case IO::STL: success = Slic3r::store_stl(path.c_str(), &model, true);    break;
        case IO::TMF: success = Slic3r::store_3mf(path.c_str(), &model, nullptr, false); break;
        default: assert(false); break;
        }
        if (success)
            std::cout << "File exported to " << path << std::endl;
        else {
            std::cerr << "File export to " << path << " failed" << std::endl;
            return false;
        }
    }
    return true;
}



static void update_instances_outside_state(Model& model, const DynamicPrintConfig& config)
{
    Pointfs bed_shape = dynamic_cast<const ConfigOptionPoints*>(config.option("bed_shape"))->values;
    BuildVolume build_volume(bed_shape, config.opt_float("max_print_height"));
    s_multiple_beds.update_build_volume(BoundingBoxf(bed_shape));
    model.update_print_volume_state(build_volume);
}

bool process_actions(Data& cli, const DynamicPrintConfig& print_config, std::vector<Model>& models)
{
    DynamicPrintConfig& actions     = cli.actions_config;
    DynamicPrintConfig& transform   = cli.transform_config;

    // doesn't need any aditional input 

    if (actions.has("help")) {
        print_help();
    }
    if (actions.has("help_sla")) {
        print_help(true, ptSLA);
    }

    if (actions.has("info")) {
        if (models.empty()) {
            boost::nowide::cerr << "error: cannot show info for empty models." << std::endl;
            return false;
        }
        // --info works on unrepaired model
        for (Model& model : models) {
            model.add_default_instances();
            model.print_info();
        }
    }

    if (actions.has("save")) {
            print_config.save(actions.opt_string("save"));
    }

    if (models.empty() && (actions.has("export_stl") || actions.has("export_obj") || actions.has("export_3mf"))) {
        boost::nowide::cerr << "error: cannot export empty models." << std::endl;
        return false;
    }

    const std::string output = cli.misc_config.has("output") ? cli.misc_config.opt_string("output") : "";

    if (actions.has("export_stl")) {
        for (auto& model : models)
            model.add_default_instances();
        if (!export_models(models, IO::STL, output))
            return false;
    }
    if (actions.has("export_obj")) {
        for (auto& model : models)
            model.add_default_instances();
        if (!export_models(models, IO::OBJ, output))
            return false;
    }
    if (actions.has("export_3mf")) {
        if (!export_models(models, IO::TMF, output))
            return false;
    }

    if (actions.has("slice") || actions.has("export_sla")) {
        if (models.empty()) {
            boost::nowide::cerr << "error: cannot slice empty models" << std::endl;
            return false;
        }
        // The SLA-only path uses sla_print directly.

        const Vec2crd           gap{ s_multiple_beds.get_bed_gap() };
        arr2::ArrangeBed        bed = arr2::to_arrange_bed(get_bed_shape(print_config), gap);
        arr2::ArrangeSettings   arrange_cfg;
        arrange_cfg.set_distance_from_objects(min_object_distance(print_config));

        for (Model& model : models) {
            // If all objects have defined instances, their relative positions will be
            // honored when printing (they will be only centered, unless --dont-arrange
            // is supplied); if any object has no instances, it will get a default one
            // and all instances will be rearranged (unless --dont-arrange is supplied).
            if (!transform.has("dont_arrange") || !transform.opt_bool("dont_arrange")) {
                if (transform.has("center")) {
                    Vec2d c = transform.option<ConfigOptionPoint>("center")->value;
                    arrange_objects(model, arr2::InfiniteBed{ scaled(c) }, arrange_cfg);
                }
                else
                    arrange_objects(model, bed, arrange_cfg);
            }

            SLAPrint    sla_print;
            sla_print.set_status_callback( [](const PrintBase::SlicingStatus& s) {
                if (s.percent >= 0) { // FIXME: is this sufficient?
                    printf("%3d%s %s\n", s.percent, "% =>", s.text.c_str());
                    std::fflush(stdout);
                }
            });

            PrintBase* print = static_cast<PrintBase*>(&sla_print);

            // Approvals given on the command line (the GUI asks the same questions in dialogs). Default: strict.
            sla::ExportApprovalOptions approvals;
            approvals.accept_adjustments = cli.misc_config.has("sla_accept_adjustments") && cli.misc_config.opt_bool("sla_accept_adjustments");
            approvals.accept_conversion  = cli.misc_config.has("sla_accept_conversion")  && cli.misc_config.opt_bool("sla_accept_conversion");
            approvals.log = [](const std::string &line) { boost::nowide::cout << line << std::endl; };
            // Adjustments are planned for the format the job is actually written in (output extension), not for
            // the printer's own format: export_print_approved does that after slicing.
            const DynamicPrintConfig &job_config = print_config;

            update_instances_outside_state(model, job_config);
            MultipleBedsUtils::with_single_bed_model_sla(model, 0, [&print, &model, &job_config]()
            {
                print->apply(model, job_config);
            });

            std::string err = print->validate();
            if (!err.empty()) {
                boost::nowide::cerr << err << std::endl;
                return false;
            }

            std::string outfile = output;

            if (print->empty()) {
                boost::nowide::cerr << "Nothing to print for " << outfile << " . Either the print is empty or no object is fully inside the print volume." << std::endl;
                return false;
            }
            else
                try {
                std::string outfile_final;
                print->process();
                {
                    outfile = sla_print.output_filepath(outfile);
                    // We need to finalize the filename beforehand because the export function sets the filename inside the zip metadata
                    outfile_final = sla_print.print_statistics().finalize_output_path(outfile);
                    // export_print() publishes outfile_final itself (temp file + rename): no further rename here.
                    sla_print.export_print_approved(outfile_final, approvals);
                }
                outfile = outfile_final;
                boost::nowide::cout << "Slicing result exported to " << outfile << std::endl;
            }
            catch (const std::exception& ex) {
                boost::nowide::cerr << ex.what() << std::endl;
                return false;
            }

        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// --sla-detect-issues <layer archive|.sla-project> [--output issues.json] [...]
//
// Standalone action: it needs no printer/material profile, so it is handled
// before regular option parsing (see Run.cpp) and does not need a definition in
// PrintConfig.cpp. Output is JSON, units: pixels (inclusive bbox, 0-based layer
// indices) and millimetres.
// ---------------------------------------------------------------------------
namespace {

const char *issue_type_name(sla::LayerIssue::Type type)
{
    switch (type) {
    case sla::LayerIssue::Type::Island:        return "island";
    case sla::LayerIssue::Type::Overhang:      return "overhang";
    case sla::LayerIssue::Type::ResinTrap:     return "resin_trap";
    case sla::LayerIssue::Type::SuctionCup:    return "suction_cup";
    case sla::LayerIssue::Type::TouchingBound: return "touching_bound";
    case sla::LayerIssue::Type::PrintHeight:   return "print_height";
    case sla::LayerIssue::Type::EmptyLayer:    return "empty_layer";
    }
    return "unknown";
}

std::string json_escape(const std::string &text)
{
    std::string out;
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
        else out += char(c);
    }
    return out;
}

// bbox is inclusive in pixels; print-height / empty-layer issues have none.
std::string json_bbox(const BoundingBox &box)
{
    if (!box.defined) return "null";
    return "{\"x\":" + std::to_string(box.min.x()) + ",\"y\":" + std::to_string(box.min.y()) +
           ",\"width\":" + std::to_string(box.max.x() - box.min.x() + 1) +
           ",\"height\":" + std::to_string(box.max.y() - box.min.y() + 1) + "}";
}

// Z of the top of every layer, mm. Falls back to a constant pitch for formats
// that carry no per-layer Z (plain image stacks), which only affects the
// suction-cup height rule and the print-height check.
std::vector<double> layer_positions_z(const sla::LayerDocument &document, const std::vector<double> &project_z,
                                      double archive_layer_height_mm = 0.)
{
    const size_t count = document.layer_count();
    if (project_z.size() == count) return project_z;
    std::vector<double> z(count);
    // Archives without per-layer Z (SL1, image stacks) are a constant pitch; reading the pitch from
    // the archive matters: a wrong one scales every height rule (suction cup height, print height).
    double pitch = 0.05;
    if (const auto job = document.job_parameters(); job && job->layer_height > 0) pitch = job->layer_height;
    else if (archive_layer_height_mm > 0) pitch = archive_layer_height_mm;
    for (size_t i = 0; i < count; ++i) {
        const auto parameters = document.layer_parameters(i);
        const auto it = parameters.find(sla::LayerParameter::PositionZ);
        z[i] = it != parameters.end() ? it->second : pitch * double(i + 1);
    }
    return z;
}

// Text of one entry of a ZIP based archive; empty when the file is no ZIP or the entry is missing.
std::string read_zip_text(const std::string &path, const char *entry)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, path)) return {};
    std::string text;
    size_t size = 0;
    if (void *data = mz_zip_reader_extract_file_to_heap(&zip, entry, &size, 0)) {
        text.assign(static_cast<const char *>(data), size);
        mz_free(data);
    }
    close_zip_reader(&zip);
    return text;
}

// "key = value" lookup in an INI-like text (SL1 config.ini / prusaslicer.ini).
std::optional<double> ini_number(const std::string &text, const std::string &key)
{
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string name = line.substr(0, eq);
        name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); }), name.end());
        if (name != key) continue;
        char *end = nullptr;
        const double value = std::strtod(line.c_str() + eq + 1, &end);
        if (end != line.c_str() + eq + 1 && std::isfinite(value)) return value;
    }
    return std::nullopt;
}

// SL1 (PrusaSlicer) stores only globals: no per-layer Z, waits or lifts. Zero values = absent.
struct Sl1Globals
{
    double layer_height_mm = 0, exposure_s = 0, bottom_exposure_s = 0, pixel_width_mm = 0, pixel_height_mm = 0;
    size_t bottom_layers = 0;
    bool   present = false;
};

Sl1Globals read_sl1_globals(const std::string &path)
{
    Sl1Globals g;
    const std::string config = read_zip_text(path, "config.ini");
    if (config.empty()) return g;
    g.present = true;
    g.layer_height_mm   = ini_number(config, "layerHeight").value_or(0.);
    g.exposure_s        = ini_number(config, "expTime").value_or(0.);
    g.bottom_exposure_s = ini_number(config, "expTimeFirst").value_or(0.);
    // UVtools SL1File maps BottomLayerCount to numFade.
    g.bottom_layers     = size_t(std::max(0., ini_number(config, "numFade").value_or(0.)));
    const std::string profile = read_zip_text(path, "prusaslicer.ini");
    const double width = ini_number(profile, "display_width").value_or(0.), height = ini_number(profile, "display_height").value_or(0.);
    const double px = ini_number(profile, "display_pixels_x").value_or(0.), py = ini_number(profile, "display_pixels_y").value_or(0.);
    if (width > 0 && height > 0 && px > 0 && py > 0) { g.pixel_width_mm = width / px; g.pixel_height_mm = height / py; }
    return g;
}

bool parse_double_arg(const std::string &name, const char *text, double &out)
{
    char *end = nullptr;
    out = std::strtod(text, &end);
    if (end == text || *end != 0 || !std::isfinite(out)) {
        boost::nowide::cerr << "error: " << name << " expects a number, got '" << text << "'" << std::endl;
        return false;
    }
    return true;
}

} // namespace

std::optional<int> process_sla_detect_issues(int argc, char **argv)
{
    std::string input, output;
    bool        requested = false;
    std::set<std::string> disabled;
    sla::IssueDetectionConfig config;
    double machine_height_mm = 0.;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--sla-detect-issues") { requested = true; if (i + 1 < argc && argv[i + 1][0] != '-') input = argv[++i]; }
        else if (arg.rfind("--sla-detect-issues=", 0) == 0) { requested = true; input = arg.substr(20); }
    }
    if (!requested) return std::nullopt;

    // Remaining options; values are UVtools defaults unless overridden here.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--sla-detect-issues") { if (i + 1 < argc && argv[i + 1][0] != '-') ++i; continue; }
        if (arg.rfind("--sla-detect-issues=", 0) == 0) continue;
        auto value = [&](double &out) {
            return i + 1 < argc && parse_double_arg(arg, argv[++i], out);
        };
        double v = 0.;
        if (arg == "--output" || arg == "-o") { if (i + 1 >= argc) { boost::nowide::cerr << "error: --output needs a file" << std::endl; return 1; } output = argv[++i]; }
        else if (arg == "--disable") {
            if (i + 1 >= argc) { boost::nowide::cerr << "error: --disable needs a list" << std::endl; return 1; }
            std::string list = argv[++i];
            size_t pos = 0;
            while (pos <= list.size()) {
                const size_t comma = std::min(list.find(',', pos), list.size());
                disabled.insert(list.substr(pos, comma - pos)); pos = comma + 1;
            }
        }
        else if (arg == "--island-required-area") { if (!value(v)) return 1; config.island.required_area_to_process_check_px = uint16_t(std::clamp(v, 0., 65535.)); }
        else if (arg == "--island-support-percent") { if (!value(v)) return 1; config.island.required_pixels_to_support_multiplier = v / 100.; }
        else if (arg == "--overhang-erode") { if (!value(v)) return 1; config.overhang.erode_iterations = uint8_t(std::clamp(v, 0., 255.)); }
        else if (arg == "--resin-trap-required-area") { if (!value(v)) return 1; config.resin_trap.required_area_to_process_check_px = uint8_t(std::clamp(v, 0., 255.)); }
        else if (arg == "--suction-cup-height") { if (!value(v)) return 1; config.resin_trap.required_height_to_consider_suction_cup_mm = v; }
        else if (arg == "--touching-margin") { if (!value(v)) return 1; const auto m = uint8_t(std::clamp(v, 0., 255.)); config.touching_bound.margin_left_px = config.touching_bound.margin_top_px = config.touching_bound.margin_right_px = config.touching_bound.margin_bottom_px = m; }
        else if (arg == "--machine-height") { if (!value(machine_height_mm)) return 1; }
        else { boost::nowide::cerr << "error: unknown option for --sla-detect-issues: " << arg << std::endl; return 1; }
    }
    if (input.empty()) {
        boost::nowide::cerr << "error: --sla-detect-issues needs a layer archive or .sla-project file\n"
                               "options: --output FILE, --disable island,overhang,resin_trap,suction_cup,touching_bound,print_height,empty_layer,\n"
                               "  --island-required-area PX, --island-support-percent P, --overhang-erode PX,\n"
                               "  --resin-trap-required-area PX, --suction-cup-height MM, --touching-margin PX, --machine-height MM" << std::endl;
        return 1;
    }
    config.island.enabled         = !disabled.count("island");
    config.overhang.enabled       = !disabled.count("overhang");
    config.resin_trap.enabled     = !disabled.count("resin_trap");
    config.resin_trap.detect_suction_cups = !disabled.count("suction_cup");
    config.touching_bound.enabled = !disabled.count("touching_bound");
    config.print_height.enabled   = !disabled.count("print_height");
    config.print_height.machine_height_mm = machine_height_mm;
    config.empty_layer.enabled    = !disabled.count("empty_layer");

    try {
        sla::LayerDocument document;
        std::vector<double> project_z;
        const sla::LayerProgress load_progress = [](size_t, size_t) { return true; };
        if (boost::algorithm::iends_with(input, ".sla-project")) {
            auto project = sla::read_project_layers(input, load_progress);
            if (!project) throw RuntimeError("Not a readable layer project: " + input);
            document  = std::move(project->document);
            project_z = std::move(project->positions_z);
        } else
            document = sla::LayerDocument::load_archive(input, load_progress);

        const std::vector<double> z = layer_positions_z(document, project_z, read_sl1_globals(input).layer_height_mm);
        // Each mask is decoded on demand by the detector's worker threads.
        const sla::LayerMaskProvider provider = [&document](size_t index) {
            auto image = document.image(index);
            sla::LayerMask mask;
            mask.width = image.cols; mask.height = image.rows; mask.data = std::move(image.buf);
            return mask;
        };
        const auto issues = sla::detect_layer_issues(provider, z, config);

        std::string json = "{\n  \"file\": \"" + json_escape(input) + "\",\n  \"layer_count\": " +
                           std::to_string(document.layer_count()) + ",\n  \"issue_count\": " +
                           std::to_string(issues.size()) + ",\n  \"issues\": [";
        for (size_t n = 0; n < issues.size(); ++n) {
            const auto &issue = issues[n];
            json += std::string(n ? "," : "") + "\n    {\"type\": \"" + issue_type_name(issue.type) +
                    "\", \"first_layer\": " + std::to_string(issue.first_layer) +
                    ", \"last_layer\": " + std::to_string(issue.last_layer) +
                    ", \"pixel_count\": " + std::to_string(issue.pixel_count) +
                    ", \"bbox\": " + json_bbox(issue.bbox) + ", \"parts\": [";
            for (size_t k = 0; k < issue.parts.size(); ++k) {
                const auto &part = issue.parts[k];
                json += std::string(k ? ", " : "") + "{\"layer\": " + std::to_string(part.layer) +
                        ", \"pixel_count\": " + std::to_string(part.pixel_count) + ", \"bbox\": " + json_bbox(part.bbox) + "}";
            }
            json += "]}";
        }
        json += issues.empty() ? "]\n}\n" : "\n  ]\n}\n";

        if (output.empty())
            boost::nowide::cout << json;
        else {
            boost::nowide::ofstream file(output, std::ios::binary);
            file << json;
            if (!file.good()) throw RuntimeError("Cannot write " + output);
        }
        boost::nowide::cerr << issues.size() << " issue(s) in " << document.layer_count() << " layers" << std::endl;
        return 0;
    } catch (const std::exception &error) {
        boost::nowide::cerr << "error: " << error.what() << std::endl;
        return 1;
    }
}


// ---------------------------------------------------------------------------
// --sla-suggestions <layer archive|.sla-project> [--output suggestions.json]
//                   [--only a,b] [--disable a,b]
//
// UVtools "Suggestions" (print parameter advice). Evaluation only: nothing is
// applied or written back. Ids: bottom_layer_count, layer_height,
// transition_layer_count, wait_time_before_cure, wait_time_after_cure,
// model_position. Units: seconds, mm, pixels (0-based layers, inclusive bbox).
// ---------------------------------------------------------------------------
namespace {

const char *layer_parameter_name(sla::LayerParameter field)
{
    switch (field) {
    case sla::LayerParameter::Exposure:   return "exposure_s";
    case sla::LayerParameter::WaitBefore: return "wait_before_s";
    case sla::LayerParameter::WaitAfter:  return "wait_after_s";
    default:                              return "other";
    }
}

// "bottom_layer_count" etc. for --only / --disable.
std::string suggestion_cli_name(const std::string &id)
{
    std::string out;
    for (const char c : id) {
        if (std::isupper(static_cast<unsigned char>(c)) && !out.empty()) out += '_';
        out += char(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string json_number(double value)
{
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.6g", std::fabs(value) < 1e-12 ? 0. : value);
    return buffer;
}

std::string json_pair(const std::optional<std::pair<size_t, size_t>> &value)
{
    return value ? "{\"before\": " + std::to_string(value->first) + ", \"after\": " + std::to_string(value->second) + "}" : "null";
}

// Builds the value-type job description from whatever the archive stores. A value the container
// does not hold stays absent (empty array / capability false) instead of being invented.
sla::SuggestionJob build_suggestion_job(const sla::LayerDocument &document, const std::vector<double> &project_z,
                                        const std::string &path)
{
    sla::SuggestionJob job;
    const size_t count = document.layer_count();
    const Sl1Globals sl1 = read_sl1_globals(path);
    job.z_mm = layer_positions_z(document, project_z, sl1.layer_height_mm);

    const auto job_parameters = document.job_parameters();
    const auto geometry       = document.raster_geometry();
    job.layer_height_mm = job_parameters && job_parameters->layer_height > 0 ? job_parameters->layer_height : sl1.layer_height_mm;
    if (job.layer_height_mm <= 0 && count > 0) job.layer_height_mm = job.z_mm.front();
    job.bottom_layer_count = job_parameters ? job_parameters->bottom_layers : sl1.bottom_layers;
    job.transition_layer_count = job_parameters ? job_parameters->transition_layers : 0;
    job.can_set_bottom_layer_count = job_parameters.has_value() || sl1.present;

    // Per-layer values exist only when every row carries the field.
    const auto capabilities = document.parameter_capabilities();
    auto editable = [&](sla::LayerParameter field) {
        return std::any_of(capabilities.begin(), capabilities.end(), [&](const auto &c) { return c.field == field && c.editable; });
    };
    std::vector<sla::LayerParameters> rows(count);
    bool has_exposure = count > 0, has_before = count > 0, has_after = count > 0;
    for (size_t i = 0; i < count; ++i) {
        rows[i] = document.layer_parameters(i);
        has_exposure &= rows[i].count(sla::LayerParameter::Exposure) > 0;
        has_before   &= rows[i].count(sla::LayerParameter::WaitBefore) > 0;
        has_after    &= rows[i].count(sla::LayerParameter::WaitAfter) > 0;
    }
    auto column = [&](sla::LayerParameter field) {
        std::vector<double> values(count);
        for (size_t i = 0; i < count; ++i) values[i] = rows[i].at(field);
        return values;
    };
    if (has_exposure) {
        job.exposure_s = column(sla::LayerParameter::Exposure);
        job.can_set_exposure = editable(sla::LayerParameter::Exposure);
    } else if (sl1.present && sl1.exposure_s > 0) {
        // SL1 holds two exposures; the layers derive from them, so they are not individually editable.
        job.exposure_s.resize(count);
        for (size_t i = 0; i < count; ++i) job.exposure_s[i] = i < sl1.bottom_layers ? sl1.bottom_exposure_s : sl1.exposure_s;
        job.can_set_exposure = false;
    } else
        job.can_set_exposure = false;
    if (has_before && editable(sla::LayerParameter::WaitBefore)) job.wait_before_s = column(sla::LayerParameter::WaitBefore);
    if (has_after && editable(sla::LayerParameter::WaitAfter))   job.wait_after_s  = column(sla::LayerParameter::WaitAfter);

    // Global exposures: the format's defaults, else the first bottom / last normal layer.
    auto exposure_of = [](const std::map<sla::LayerParameter, double> &m) { const auto it = m.find(sla::LayerParameter::Exposure); return it == m.end() ? 0. : it->second; };
    job.bottom_exposure_s = job_parameters ? exposure_of(job_parameters->bottom) : sl1.bottom_exposure_s;
    job.normal_exposure_s = job_parameters ? exposure_of(job_parameters->normal) : sl1.exposure_s;
    if (!job.exposure_s.empty()) {
        if (job.bottom_exposure_s <= 0) job.bottom_exposure_s = job.exposure_s.front();
        if (job.normal_exposure_s <= 0) job.normal_exposure_s = job.exposure_s.back();
    }

    if (count > 0) {
        const auto first = document.image(0);
        job.resolution_x = size_t(first.cols);
        job.resolution_y = size_t(first.rows);
    }
    if (geometry && geometry->columns && geometry->rows) {
        job.pixel_width_mm  = geometry->width_mm / double(geometry->columns);
        job.pixel_height_mm = geometry->height_mm / double(geometry->rows);
    } else {
        job.pixel_width_mm = sl1.pixel_width_mm;
        job.pixel_height_mm = sl1.pixel_height_mm;
    }

    // Each mask is decoded on demand by the evaluators' worker threads.
    job.stats = sla::layer_stats_from_masks([&document](size_t index) {
        auto image = document.image(index);
        sla::LayerMask mask;
        mask.width = size_t(image.cols); mask.height = size_t(image.rows); mask.data = std::move(image.buf);
        return mask;
    });
    return job;
}

std::string suggestions_to_json(const std::string &input, size_t layer_count, const std::vector<sla::SuggestionResult> &results)
{
    auto status_name = [](sla::SuggestionStatus s) {
        return s == sla::SuggestionStatus::Applied ? "applied" : s == sla::SuggestionStatus::NotApplied ? "not_applied" : "unavailable";
    };
    std::string json = "{\n  \"file\": \"" + json_escape(input) + "\",\n  \"layer_count\": " + std::to_string(layer_count) +
                       ",\n  \"suggestions\": [";
    for (size_t n = 0; n < results.size(); ++n) {
        const auto &r = results[n];
        json += std::string(n ? "," : "") + "\n    {\"id\": \"" + suggestion_cli_name(r.id) + "\", \"title\": \"" + json_escape(r.title) +
                "\", \"status\": \"" + status_name(r.status) + "\", \"informational_only\": " + (r.informational_only ? "true" : "false") +
                ",\n     \"message\": \"" + json_escape(r.message) + "\", \"description\": \"" + json_escape(r.description) +
                "\",\n     \"confirmation\": \"" + json_escape(r.confirmation) + "\", \"information_url\": \"" + json_escape(r.information_url) +
                "\", \"unavailable_reason\": \"" + json_escape(r.unavailable_reason) +
                "\",\n     \"bottom_layer_count\": " + json_pair(r.bottom_layer_count) +
                ", \"transition_layer_count\": " + json_pair(r.transition_layer_count) +
                ", \"prepend_blank_first_layer\": " + (r.prepend_blank_first_layer ? "true" : "false") + ", \"model_move\": ";
        if (r.model_move)
            json += "{\"dx_px\": " + std::to_string(r.model_move->dx_px) + ", \"dy_px\": " + std::to_string(r.model_move->dy_px) +
                    ", \"anchor\": \"" + r.model_move->anchor + "\", \"before\": " + json_bbox(r.model_move->before) +
                    ", \"after\": " + json_bbox(r.model_move->after) + "}";
        else
            json += "null";
        json += ", \"layer_changes\": [";
        for (size_t k = 0; k < r.layer_changes.size(); ++k) {
            const auto &c = r.layer_changes[k];
            json += std::string(k ? ", " : "") + "{\"layer\": " + std::to_string(c.layer) + ", \"field\": \"" + layer_parameter_name(c.field) +
                    "\", \"before\": " + json_number(c.before) + ", \"after\": " + json_number(c.after) + "}";
        }
        json += "]}";
    }
    json += results.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return json;
}

} // namespace

std::optional<int> process_sla_suggestions(int argc, char **argv)
{
    std::string input, output;
    bool        requested = false;
    std::set<std::string> only, disabled;
    auto split_list = [](const std::string &list, std::set<std::string> &target) {
        size_t pos = 0;
        while (pos <= list.size()) {
            const size_t comma = std::min(list.find(',', pos), list.size());
            target.insert(list.substr(pos, comma - pos)); pos = comma + 1;
        }
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--sla-suggestions") { requested = true; if (i + 1 < argc && argv[i + 1][0] != '-') input = argv[++i]; }
        else if (arg.rfind("--sla-suggestions=", 0) == 0) { requested = true; input = arg.substr(18); }
    }
    if (!requested) return std::nullopt;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--sla-suggestions") { if (i + 1 < argc && argv[i + 1][0] != '-') ++i; continue; }
        if (arg.rfind("--sla-suggestions=", 0) == 0) continue;
        if ((arg == "--output" || arg == "-o") && i + 1 < argc) output = argv[++i];
        else if (arg == "--only" && i + 1 < argc) split_list(argv[++i], only);
        else if (arg == "--disable" && i + 1 < argc) split_list(argv[++i], disabled);
        else { boost::nowide::cerr << "error: unknown option for --sla-suggestions: " << arg << std::endl; return 1; }
    }
    if (input.empty()) {
        boost::nowide::cerr << "error: --sla-suggestions needs a layer archive or .sla-project file\n"
                               "options: --output FILE, --only ID,..., --disable ID,...\n"
                               "ids: bottom_layer_count, layer_height, transition_layer_count, wait_time_before_cure,\n"
                               "     wait_time_after_cure, model_position (evaluation only, nothing is applied)" << std::endl;
        return 1;
    }

    try {
        sla::LayerDocument document;
        std::vector<double> project_z;
        const sla::LayerProgress load_progress = [](size_t, size_t) { return true; };
        if (boost::algorithm::iends_with(input, ".sla-project")) {
            auto project = sla::read_project_layers(input, load_progress);
            if (!project) throw RuntimeError("Not a readable layer project: " + input);
            document  = std::move(project->document);
            project_z = std::move(project->positions_z);
        } else
            document = sla::LayerDocument::load_archive(input, load_progress);

        const sla::SuggestionJob job = build_suggestion_job(document, project_z, input);
        sla::SuggestionConfig config;
        auto wanted = [&](const char *id) { return (only.empty() || only.count(id)) && !disabled.count(id); };
        config.bottom_layer_count.enabled     = wanted("bottom_layer_count");
        config.layer_height.enabled           = wanted("layer_height");
        config.transition_layer_count.enabled = wanted("transition_layer_count");
        config.wait_time_before_cure.enabled  = wanted("wait_time_before_cure");
        config.wait_time_after_cure.enabled   = wanted("wait_time_after_cure");
        config.model_position.enabled         = wanted("model_position");

        const auto results = sla::evaluate_suggestions(job, config);
        const std::string json = suggestions_to_json(input, document.layer_count(), results);
        if (output.empty())
            boost::nowide::cout << json;
        else {
            boost::nowide::ofstream file(output, std::ios::binary);
            file << json;
            if (!file.good()) throw RuntimeError("Cannot write " + output);
        }
        const size_t pending = size_t(std::count_if(results.begin(), results.end(),
            [](const auto &r) { return r.status == sla::SuggestionStatus::NotApplied; }));
        boost::nowide::cerr << results.size() << " suggestion(s), " << pending << " not applied" << std::endl;
        return 0;
    } catch (const std::exception &error) {
        boost::nowide::cerr << "error: " << error.what() << std::endl;
        return 1;
    }
}

// ---------------------------------------------------------------------------
// --sla-run-recipe recipe.json [--input FILE] [--output FILE] [--dry-run] [--report FILE] [--sla-accept-adjustments]
//
// Local automation without the GUI: runs a declarative layer recipe (see doc/AUTOMATION.md and
// libslic3r/SLA/LayerRecipe.hpp). Standalone like the actions above: no profiles or models are loaded.
// Exit code: 0 success, 1 recipe or step error, 130 cancelled (Ctrl+C). Outputs are published only
// after every step succeeded, so a failure or Ctrl+C leaves all output files untouched.
// A JSON report (per step status) goes to --report FILE or stdout; a summary goes to stderr.
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_recipe_cancel{false};
extern "C" void recipe_sigint_handler(int) { g_recipe_cancel = true; }
} // namespace

std::optional<int> process_sla_run_recipe(int argc, char **argv)
{
    std::string recipe_path, input, output, report;
    bool        requested = false, dry_run = false, accept_adjustments = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](std::string &out) {
            if (i + 1 >= argc) { boost::nowide::cerr << "error: " << arg << " needs a value" << std::endl; return false; }
            out = argv[++i];
            return true;
        };
        if (arg == "--sla-run-recipe") { requested = true; if (i + 1 < argc && argv[i + 1][0] != '-') recipe_path = argv[++i]; }
        else if (arg.rfind("--sla-run-recipe=", 0) == 0) { requested = true; recipe_path = arg.substr(17); }
        else if (arg == "--input" || arg == "-i") { if (!value(input)) return 1; }
        else if (arg == "--output" || arg == "-o") { if (!value(output)) return 1; }
        else if (arg == "--report") { if (!value(report)) return 1; }
        else if (arg == "--dry-run") dry_run = true;
        else if (arg == "--sla-accept-adjustments") accept_adjustments = true;
    }
    if (!requested) return std::nullopt;

    // Only options of this action are accepted next to it; anything else is most likely a typo.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--sla-run-recipe") { if (i + 1 < argc && argv[i + 1][0] != '-') ++i; continue; }
        if (arg.rfind("--sla-run-recipe=", 0) == 0 || arg == "--dry-run" || arg == "--sla-accept-adjustments") continue;
        if (arg == "--input" || arg == "-i" || arg == "--output" || arg == "-o" || arg == "--report") { ++i; continue; }
        boost::nowide::cerr << "error: unknown option for --sla-run-recipe: " << arg << std::endl;
        return 1;
    }
    if (recipe_path.empty()) {
        boost::nowide::cerr << "error: --sla-run-recipe needs a recipe file (JSON)\n"
                               "options: --input FILE (replaces the recipe's open path), --output FILE (replaces the path of the last\n"
                               "  save/convert step), --dry-run (check everything, write nothing), --report FILE (JSON report, default stdout),\n"
                               "  --sla-accept-adjustments (convert steps write the nearest storable values instead of refusing)\n"
                               "operations:";
        for (const auto &op : sla::recipe::operation_catalogue()) boost::nowide::cerr << "\n  " << op.name << " - " << op.summary;
        boost::nowide::cerr << std::endl;
        return 1;
    }

    try {
        const sla::recipe::Recipe recipe = sla::recipe::load_recipe(recipe_path);
        sla::recipe::RunOptions   options;
        // --input / --output are relative to the current folder (not to the recipe's folder).
        if (!input.empty())  options.input  = boost::filesystem::absolute(input).string();
        if (!output.empty()) options.output = boost::filesystem::absolute(output).string();
        options.dry_run = dry_run;
        options.accept_adjustments = accept_adjustments;
        g_recipe_cancel = false;
        std::signal(SIGINT, recipe_sigint_handler);
        options.progress = [](size_t, size_t) { return !g_recipe_cancel.load(); };

        const sla::recipe::RunResult result = sla::recipe::run_recipe(recipe, std::move(options));
        std::signal(SIGINT, SIG_DFL);

        const std::string json = sla::recipe::run_result_to_json(result);
        if (report.empty())
            boost::nowide::cout << json;
        else {
            boost::nowide::ofstream file(report, std::ios::binary);
            file << json;
            if (!file.good()) boost::nowide::cerr << "error: cannot write " << report << std::endl;
        }
        const char *names[] = {"done", "planned", "FAILED", "cancelled", "not run"};
        for (const auto &step : result.steps)
            boost::nowide::cerr << "[" << step.index + 1 << "/" << result.steps.size() << "] " << step.op
                                << (step.label.empty() ? "" : " (" + step.label + ")") << ": " << names[int(step.status)]
                                << (step.message.empty() ? "" : " - " + step.message) << std::endl;
        if (result.cancelled) { boost::nowide::cerr << "cancelled; no output was written" << std::endl; return 130; }
        if (!result.ok) { boost::nowide::cerr << "error: " << result.error << "; no output was written" << std::endl; return 1; }
        boost::nowide::cerr << (result.dry_run ? "dry run OK: nothing was written" : "recipe finished") << std::endl;
        return 0;
    } catch (const std::exception &error) {
        boost::nowide::cerr << "error: " << error.what() << std::endl;
        return 1;
    }
}

}
