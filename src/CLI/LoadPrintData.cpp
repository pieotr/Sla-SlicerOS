#include <cstdio>
#include <string>
#include <cstring>
#include <iostream>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/iostream.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Config.hpp"
#include "libslic3r/SLA/PrinterGeometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/FileReader.hpp"

#include "CLI/CLI.hpp"
#include "CLI/ProfilesSharingUtils.hpp"

namespace Slic3r::CLI {

PrinterTechnology get_printer_technology(const DynamicConfig &config)
{
    const ConfigOptionEnum<PrinterTechnology> *opt = config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
    return (opt == nullptr) ? ptUnknown : opt->value;
}

// may be "validate_and_apply_printer_technology" will be better? 
static bool can_apply_printer_technology(PrinterTechnology& printer_technology, const PrinterTechnology& other_printer_technology)
{
    if (printer_technology == ptUnknown) {
        printer_technology = other_printer_technology;
        return true;
    }

    bool invalid_other_pt = printer_technology != other_printer_technology && other_printer_technology != ptUnknown;

    if (invalid_other_pt)
        boost::nowide::cerr << "Mixing configurations for FFF and SLA technologies" << std::endl;

    return !invalid_other_pt;
}

static void print_config_substitutions(const ConfigSubstitutions& config_substitutions, const std::string& file)
{
    if (config_substitutions.empty())
        return;
    boost::nowide::cout << "The following configuration values were substituted when loading \" << file << \":\n";
    for (const ConfigSubstitution& subst : config_substitutions)
        boost::nowide::cout << "\tkey = \"" << subst.opt_def->opt_key << "\"\t loaded = \"" << subst.old_value << "\tsubstituted = \"" << subst.new_value->serialize() << "\"\n";
}

static bool load_print_config(DynamicPrintConfig &print_config, PrinterTechnology& printer_technology, const Data& cli)
{
    // first of all load configuration from "--load" if any

    if (cli.input_config.has("load")) {

        const std::vector<std::string>& load_configs = cli.input_config.option<ConfigOptionStrings>("load")->values;
        ForwardCompatibilitySubstitutionRule config_substitution_rule = cli.misc_config.option<ConfigOptionEnum<ForwardCompatibilitySubstitutionRule>>("config_compatibility")->value;

        // load config files supplied via --load
        for (auto const& file : load_configs) {
            if (!boost::filesystem::exists(file)) {
                if (cli.misc_config.has("ignore_nonexistent_config") && cli.misc_config.opt_bool("ignore_nonexistent_config")) {
                    continue;
                }
                else {
                    boost::nowide::cerr << "No such file: " << file << std::endl;
                    return false;
                }
            }
            DynamicPrintConfig  config;
            ConfigSubstitutions config_substitutions;
            try {
                config_substitutions = config.load(file, config_substitution_rule);
            }
            catch (std::exception& ex) {
                boost::nowide::cerr << "Error while reading config file \"" << file << "\": " << ex.what() << std::endl;
                return false;
            }

            if (!can_apply_printer_technology(printer_technology, get_printer_technology(config)))
                return false;

            print_config_substitutions(config_substitutions, file);

            print_config.apply(config);
        }
    }

    // than apply other options from full print config if any is provided by prifiles set

    if (has_full_config_from_profiles(cli)) {
        DynamicPrintConfig  config;
        // load config from profiles set
        std::string errors = Slic3r::load_full_print_config(cli.input_config.opt_string("print-profile"),
                                                            cli.input_config.option<ConfigOptionStrings>("material-profile")->values,
                                                            cli.input_config.opt_string("printer-profile"),
                                                            config, printer_technology);
        if (!errors.empty()) {
            boost::nowide::cerr << "Error while loading config from profiles: " << errors << std::endl;
            return false;
        }

        if (!can_apply_printer_technology(printer_technology, get_printer_technology(config)))
            return false;


        // config is applied with print_config loaded before
        config += std::move(print_config);
        print_config = std::move(config);
    }

    return true;
}

static bool process_input_files(std::vector<Model>& models, DynamicPrintConfig& print_config, PrinterTechnology& printer_technology, Data& cli)
{
    for (const std::string& file : cli.input_files) {
        if (file.find("://") != std::string::npos) {
            boost::nowide::cerr << "Only local input files are supported: " << file << std::endl;
            return false;
        }
        {
            std::string lower = boost::algorithm::to_lower_copy(file);
            if (boost::algorithm::ends_with(lower, ".gcode") || boost::algorithm::ends_with(lower, ".gco") ||
                boost::algorithm::ends_with(lower, ".g") || boost::algorithm::ends_with(lower, ".bgcode")) {
                boost::nowide::cerr << "G-code files are not supported: this application is an SLA/MSLA-only slicer (" << file << ")" << std::endl;
                return false;
            }
        }
        if (!boost::filesystem::exists(file)) {
            boost::nowide::cerr << "No such file: " << file << std::endl;
            return false;
        }

        Model model;
        try {
            if (has_full_config_from_profiles(cli) || !FileReader::is_project_file(file)) {
                // we have full banch of options from profiles set
                // so, just load a geometry
                model = FileReader::load_model(file);
                if (model.loaded_from_fff_project)
                    boost::nowide::cerr << "Warning: " << file << ": This project was created for an FFF printer; only the geometry was loaded." << std::endl;
            }
            else {
                // load model and configuration from the file
                DynamicPrintConfig          config;
                ConfigSubstitutionContext   config_substitutions_ctxt(cli.misc_config.option<ConfigOptionEnum<ForwardCompatibilitySubstitutionRule>>("config_compatibility")->value);
                boost::optional<Semver>     prusaslicer_generator_version;

                //FIXME should we check the version here? // | Model::LoadAttribute::CheckVersion ?
                model = FileReader::load_model_with_config(file, &config, &config_substitutions_ctxt, prusaslicer_generator_version, FileReader::LoadAttribute::AddDefaultInstances);

                if (model.loaded_from_fff_project || config_substitutions_ctxt.legacy_fff_technology) {
                    // The FFF technology only exists as a raw "printer_technology = FFF" string of the embedded config.
                    boost::nowide::cerr << "FFF/FDM configurations are not supported: this application is an SLA/MSLA-only slicer and requires an SLA printer configuration" << std::endl;
                    return false;
                }
                if (!can_apply_printer_technology(printer_technology, get_printer_technology(config)))
                    return false;

                print_config_substitutions(config_substitutions_ctxt.substitutions, file);

                // config is applied with print_config loaded before
                config += std::move(print_config);
                print_config = std::move(config);
            }

            // If model for slicing is loaded from 3mf file, then its geometry has to be used and arrange couldn't be apply for this model.
            if (FileReader::is_project_file(file) &&
                (!cli.transform_config.has("dont_arrange") || !cli.transform_config.opt_bool("dont_arrange"))) {
                //So, check a state of "dont_arrange" parameter and set it to true, if its value is false.
                cli.transform_config.set_key_value("dont_arrange", new ConfigOptionBool(true));
            }
        }
        catch (std::exception& e) {
            boost::nowide::cerr << file << ": " << e.what() << std::endl;
            return false;
        }
        if (model.objects.empty()) {
            boost::nowide::cerr << "Error: file is empty: " << file << std::endl;
            continue;
        }
        models.push_back(model);
    }

    return true;
}

static bool finalize_print_config(DynamicPrintConfig& print_config, PrinterTechnology& printer_technology, const Data& cli)
{
    // Apply command line options to a more specific DynamicPrintConfig which provides normalize()
    // (command line options override --load files or congiguration which is loaded prom profiles)
    print_config.apply(cli.overrides_config, true);
    // Normalizing after importing the 3MFs / AMFs
    if (!can_apply_printer_technology(printer_technology, get_printer_technology(print_config)))
        return false;
    printer_technology = ptSLA;

    if (printer_technology == ptUnknown)
        printer_technology = ptSLA;
    print_config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology", true)->value = printer_technology;

    SLAFullPrintConfig sla_print_config;

    // Synchronize the default parameters and the ones received on the command line.
    {
        assert(printer_technology == ptSLA);
        sla_print_config.output_filename_format.value = "[input_filename_base][default_output_extension]";

        // The default bed shape should reflect the default display parameters
        // and not the fff defaults.
        double w = sla_print_config.display_width.getFloat();
        double h = sla_print_config.display_height.getFloat();
        sla_print_config.bed_shape.values = { Vec2d(0, 0), Vec2d(w, 0), Vec2d(w, h), Vec2d(0, h) };

        sla_print_config.apply(print_config, true);
        print_config.apply(sla_print_config, true);
        // Scene axes retain the physical LCD dimensions in either raster orientation.
        sla::synchronize_bed_shape(print_config);
    }

    // validate print configuration
    std::string validity = print_config.validate();
    if (!validity.empty()) {
        boost::nowide::cerr << "Error: The composite configation is not valid: " << validity << std::endl;
        return false;
    }

    return true;
}

bool load_print_data(std::vector<Model>& models, 
                     DynamicPrintConfig& print_config, 
                     PrinterTechnology& printer_technology, 
                     Data& cli)
{
    if (!load_print_config(print_config, printer_technology, cli))
        return false;

    if (!process_input_files(models, print_config, printer_technology, cli))
        return false;

    if (!finalize_print_config(print_config, printer_technology, cli))
        return false;

    return true;
}

}