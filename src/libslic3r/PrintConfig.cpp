///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Lukáš Hejl @hejllukas, Tomáš Mészáros @tamasmeszaros, Oleksandra Iushchenko @YuSanka, Pavel Mikuš @Godrak, David Kocík @kocikdav, Enrico Turri @enricoturri1966, Filip Sykala @Jony01, Vojtěch Král @vojtechkral
///|/ Copyright (c) 2023 Pedro Lamas @PedroLamas
///|/ Copyright (c) 2023 Mimoja @Mimoja
///|/ Copyright (c) 2020 - 2021 Sergey Kovalev @RandoMan70
///|/ Copyright (c) 2021 Niall Sheridan @nsheridan
///|/ Copyright (c) 2021 Martin Budden
///|/ Copyright (c) 2021 Ilya @xorza
///|/ Copyright (c) 2020 Paul Arden @ardenpm
///|/ Copyright (c) 2020 rongith
///|/ Copyright (c) 2019 Spencer Owen @spuder
///|/ Copyright (c) 2019 Stephan Reichhelm @stephanr
///|/ Copyright (c) 2018 Martin Loidl @LoidlM
///|/ Copyright (c) SuperSlicer 2018 Remi Durand @supermerill
///|/ Copyright (c) 2016 - 2017 Joseph Lenox @lordofhyphens
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2016 Vanessa Ezekowitz @VanessaE
///|/ Copyright (c) 2015 Alexander Rössler @machinekoder
///|/ Copyright (c) 2014 Petr Ledvina @ledvinap
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PrintConfig.hpp"

#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/lexical_cast/bad_lexical_cast.hpp>
#include <boost/preprocessor/cat.hpp>
#include <cereal/cereal.hpp>
#include <set>
#include <cmath>
#include <optional>
#include <string_view>

#include "Config.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "libslic3r/Thumbnails/Thumbnails.hpp"
#include "libslic3r/SLA/SupportTreeStrategies.hpp"
#include "libslic3r/enum_bitmask.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {

static t_config_enum_names enum_names_from_keys_map(const t_config_enum_values &enum_keys_map)
{
    t_config_enum_names names;
    int cnt = 0;
    for (const auto& kvp : enum_keys_map)
        cnt = std::max(cnt, kvp.second);
    cnt += 1;
    names.assign(cnt, "");
    for (const auto& kvp : enum_keys_map)
        names[kvp.second] = kvp.first;
    return names;
}

#define CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(NAME) \
    static t_config_enum_names s_keys_names_##NAME = enum_names_from_keys_map(s_keys_map_##NAME); \
    template<> const t_config_enum_values& ConfigOptionEnum<NAME>::get_enum_values() { return s_keys_map_##NAME; } \
    template<> const t_config_enum_names& ConfigOptionEnum<NAME>::get_enum_names() { return s_keys_names_##NAME; }

// "FFF" is deliberately absent: the technology was removed and the legacy value is rejected before the enum
// parser by ConfigBase::set_deserialize_raw() (FffConfigurationException).
static t_config_enum_values s_keys_map_PrinterTechnology {
    { "SLA",            ptSLA }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(PrinterTechnology)

static const t_config_enum_values s_keys_map_SlicingMode {
    { "regular",        int(SlicingMode::Regular) },
    { "even_odd",       int(SlicingMode::EvenOdd) },
    { "close_holes",    int(SlicingMode::CloseHoles) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SlicingMode)

static const t_config_enum_values s_keys_map_SLADisplayOrientation = {
    { "landscape",      sladoLandscape},
    { "portrait",       sladoPortrait}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SLADisplayOrientation)

static const t_config_enum_values s_keys_map_SLAArchiveHeaderMirror = {
    { "auto",           slahmAuto},
    { "none",           slahmNone},
    { "horizontal",     slahmHorizontal},
    { "vertical",       slahmVertical},
    { "both",           slahmBoth}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SLAArchiveHeaderMirror)

static const t_config_enum_values s_keys_map_SLAPillarConnectionMode = {
    {"zigzag",          int(SLAPillarConnectionMode::zigzag)},
    {"cross",           int(SLAPillarConnectionMode::cross)},
    {"dynamic",         int(SLAPillarConnectionMode::dynamic)}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SLAPillarConnectionMode)

static const t_config_enum_values s_keys_map_SLAMaterialSpeed = {
    {"slow",            slamsSlow},
    {"fast",            slamsFast},
    {"high_viscosity",  slamsHighViscosity}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SLAMaterialSpeed);

static inline const t_config_enum_values s_keys_map_SLASupportTreeType = {
    {"default", int(sla::SupportTreeType::Default)},
    {"branching",   int(sla::SupportTreeType::Branching)},
    // NOTE: "organic" was the branching-layout smoother for one day (wave 31a). That
    // algorithm is now "organic_branching"; "organic" is the full growth algorithm.
    {"organic",     int(sla::SupportTreeType::Organic)},
    {"organic_branching", int(sla::SupportTreeType::OrganicBranching)}
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SLASupportTreeType);

static const t_config_enum_values s_keys_map_GCodeThumbnailsFormat = {
    { "PNG", int(GCodeThumbnailsFormat::PNG) },
    { "JPG", int(GCodeThumbnailsFormat::JPG) },
    { "QOI", int(GCodeThumbnailsFormat::QOI) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(GCodeThumbnailsFormat)

static const t_config_enum_values s_keys_map_ForwardCompatibilitySubstitutionRule = {
    { "disable",        ForwardCompatibilitySubstitutionRule::Disable },
    { "enable",         ForwardCompatibilitySubstitutionRule::Enable },
    { "enable_silent",  ForwardCompatibilitySubstitutionRule::EnableSilent }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(ForwardCompatibilitySubstitutionRule)


static const t_config_enum_values s_keys_map_TowerSpeeds{
    { "layer1",  tsLayer1    },
    { "layer2",  tsLayer2    },
    { "layer3",  tsLayer3    },
    { "layer4",  tsLayer4    },
    { "layer5",  tsLayer5    },
    { "layer8",  tsLayer8    },
    { "layer11", tsLayer11   },
    { "layer14", tsLayer14   },
    { "layer18", tsLayer18   },
    { "layer22", tsLayer22   },
    { "layer24", tsLayer24   },
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(TowerSpeeds)

static const t_config_enum_values s_keys_map_TiltSpeeds{
    { "move120",    tsMove120    },
    { "layer200",   tsLayer200   },
    { "move300",    tsMove300    },
    { "layer400",   tsLayer400   },
    { "layer600",   tsLayer600   },
    { "layer800",   tsLayer800   },
    { "layer1000",  tsLayer1000  },
    { "layer1250",  tsLayer1250  },
    { "layer1500",  tsLayer1500  },
    { "layer1750",  tsLayer1750  },
    { "layer2000",  tsLayer2000  },
    { "layer2250",  tsLayer2250  },
    { "move5120",   tsMove5120   },
    { "move8000",   tsMove8000   },
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(TiltSpeeds)

static void assign_printer_technology_to_unknown(t_optiondef_map &options, PrinterTechnology printer_technology)
{
    for (std::pair<const t_config_option_key, ConfigOptionDef> &kvp : options)
        if (kvp.second.printer_technology == ptUnknown)
            kvp.second.printer_technology = printer_technology;
}

PrintConfigDef::PrintConfigDef()
{
    this->init_common_params();
    assign_printer_technology_to_unknown(this->options, ptAny);
    this->init_shared_params();
    // The preset bookkeeping options above are not tied to a technology: they keep the ptUnknown marker they had
    // while they lived among the FFF options (the CLI setup skips ptUnknown options). Remember them so the SLA
    // pass below does not claim them.
    std::vector<t_config_option_key> shared_keys;
    for (const auto &kvp : this->options)
        if (kvp.second.printer_technology == ptUnknown)
            shared_keys.push_back(kvp.first);
    this->init_sla_params();
    this->init_sla_tilt_params();
    assign_printer_technology_to_unknown(this->options, ptSLA);
    for (const t_config_option_key &key : shared_keys)
        this->options[key].printer_technology = ptUnknown;
    this->finalize();
}

void PrintConfigDef::init_common_params()
{
    ConfigOptionDef* def;

    def = this->add("printer_technology", coEnum);
    def->label = L("Printer technology");
    def->tooltip = L("Printer technology");
    def->set_enum<PrinterTechnology>({ "SLA" });
    def->set_default_value(new ConfigOptionEnum<PrinterTechnology>(ptSLA));

    def = this->add("bed_shape", coPoints);
    def->label = L("Bed shape");
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionPoints{ Vec2d(0, 0), Vec2d(200, 0), Vec2d(200, 200), Vec2d(0, 200) });

    def = this->add("bed_custom_texture", coString);
    def->label = L("Bed custom texture");
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("bed_custom_model", coString);
    def->label = L("Bed custom model");
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("elefant_foot_compensation", coFloat);
    def->label = L("Elephant foot compensation");
    def->category = L("Advanced");
    def->tooltip = L("The first layer will be shrunk in the XY plane by the configured value "
                     "to compensate for the 1st layer squish aka an Elephant Foot effect.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = this->add("thumbnails", coString);
    def->label = L("Thumbnails");
    def->tooltip = L("Picture sizes to be stored into .sl1 / .sl1s files, in the following format: \"XxY/EXT, XxY/EXT, ...\"\n"
                     "Currently supported extensions are PNG, QOI and JPG.");
    def->mode = comExpert;
    def->gui_type = ConfigOptionDef::GUIType::one_string;
    def->set_default_value(new ConfigOptionString());

    def = this->add("thumbnails_format", coEnum);
    def->label = L("Thumbnail format");
    def->tooltip = L("Thumbnail format: PNG for best quality, JPG for smallest size, QOI for low memory firmware");
    def->mode = comExpert;
    def->set_enum<GCodeThumbnailsFormat>({ "PNG", "JPG", "QOI" });
    def->set_default_value(new ConfigOptionEnum<GCodeThumbnailsFormat>(GCodeThumbnailsFormat::PNG));

    def = this->add("layer_height", coFloat);
    def->label = L("Layer height");
    def->category = L("Layers");
    def->tooltip = L("This setting controls the height (and thus the total number) of the slices/layers. "
                   "Thinner layers give better accuracy but take more time to print.");
    def->sidetext = L("mm");
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(0.05));

    def = this->add("max_print_height", coFloat);
    def->label = L("Max print height");
    def->tooltip = L("Maximum height available on the printer's Z axis.");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 1200;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(200.0));

    def = this->add("profile_vendor", coString);
    def->label = L("Profile vendor");
    def->tooltip = L("Name of profile vendor");
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("profile_version", coString);
    def->label = L("Profile version");
    def->tooltip = L("Version of profile");
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString(""));
}

// Technology-neutral preset bookkeeping options (compatibility / inheritance / printer identity) and the SLA options
// whose definitions historically lived next to the removed FFF options. Definitions are unchanged.
void PrintConfigDef::init_shared_params()
{
    ConfigOptionDef* def;

    def = this->add("compatible_printers", coStrings);
    def->label = L("Compatible printers");
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionStrings());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("compatible_printers_condition", coString);
    def->label = L("Compatible printers condition");
    def->tooltip = L("A boolean expression using the configuration values of an active printer profile. "
                   "If this expression evaluates to true, this profile is considered compatible "
                   "with the active printer profile.");
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("compatible_prints", coStrings);
    def->label = L("Compatible print profiles");
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionStrings());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("compatible_prints_condition", coString);
    def->label = L("Compatible print profiles condition");
    def->tooltip = L("A boolean expression using the configuration values of an active print profile. "
                   "If this expression evaluates to true, this profile is considered compatible "
                   "with the active print profile.");
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("inherits", coString);
    def->label = L("Inherits profile");
    def->tooltip = L("Name of the profile, from which this profile inherits.");
    def->full_width = true;
    def->height = 5;
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("output_filename_format", coString);
    def->label = L("Output filename format");
    def->tooltip = L("You can use all configuration options as variables inside this template. "
                   "For example: [layer_height], [fill_density] etc. You can also use [timestamp], "
                   "[year], [month], [day], [hour], [minute], [second], [version], "
                   "[input_filename_base], [default_output_extension].");
    def->full_width = true;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionString("[input_filename_base][default_output_extension]"));

    def = this->add("printer_model", coString);
    def->label = L("Printer type");
    def->tooltip = L("Type of the printer.");
    def->set_default_value(new ConfigOptionString());

    def = this->add("printer_notes", coString);
    def->label = L("Printer notes");
    def->tooltip = L("You can put your notes regarding the printer here.");
    def->multiline = true;
    def->full_width = true;
    def->height = 13;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("printer_settings_id", coString);
    def->set_default_value(new ConfigOptionString(""));
    def->cli = ConfigOptionDef::nocli;

    def = this->add("printer_variant", coString);
    def->label = L("Printer variant");
    def->tooltip = L("Name of the printer variant. For example, the printer variants may be differentiated by the screen or the resin tank.");
    def->set_default_value(new ConfigOptionString());

    def = this->add("printer_vendor", coString);
    def->label = L("Printer vendor");
    def->tooltip = L("Name of the printer vendor.");
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("slice_closing_radius", coFloat);
    def->label = L("Slice gap closing radius");
    def->category = L("Advanced");
    def->tooltip = L("Cracks smaller than 2x gap closing radius are being filled during the triangle mesh slicing. "
                     "The gap closing operation may reduce the final print resolution, therefore it is advisable to keep the value reasonably low.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.049));

    def = this->add("slicing_mode", coEnum);
    def->label = L("Slicing Mode");
    def->category = L("Advanced");
    def->tooltip = L("Use \"Even-odd\" for 3DLabPrint airplane models. Use \"Close holes\" to close all holes in the model.");
    def->set_enum<SlicingMode>({
        { "regular",        L("Regular") },
        { "even_odd",       L("Even-odd") },
        { "close_holes",    L("Close holes") }
    });
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionEnum<SlicingMode>(SlicingMode::Regular));

    def = this->add("sla_preset_metadata", coString);
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("compatible_printers_condition_cummulative", coStrings);
    def->set_default_value(new ConfigOptionStrings());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("compatible_prints_condition_cummulative", coStrings);
    def->set_default_value(new ConfigOptionStrings());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("inherits_cummulative", coStrings);
    def->set_default_value(new ConfigOptionStrings());
    def->cli = ConfigOptionDef::nocli;

}

void PrintConfigDef::init_sla_support_params(const std::string &prefix)
{
    ConfigOptionDef* def;

    def = this->add(prefix + "support_head_front_diameter", coFloat);
    def->label = L("Pinhead front diameter");
    def->category = L("Supports");
    def->tooltip = L("Diameter of the pointing side of the head");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.4));

    def = this->add(prefix + "support_head_penetration", coFloat);
    def->label = L("Head penetration");
    def->category = L("Supports");
    def->tooltip = L("How much the pinhead has to penetrate the model surface");
    def->sidetext = L("mm");
    def->mode = comAdvanced;
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(0.2));

    def = this->add(prefix + "support_head_width", coFloat);
    def->label = L("Pinhead width");
    def->category = L("Supports");
    def->tooltip = L("Width from the back sphere center to the front sphere center");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 20;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(1.0));

    def = this->add(prefix + "support_pillar_diameter", coFloat);
    def->label = L("Pillar diameter");
    def->category = L("Supports");
    def->tooltip = L("Diameter in mm of the support pillars");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 15;
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionFloat(1.0));

    def = this->add(prefix + "support_small_pillar_diameter_percent", coPercent);
    def->label = L("Small pillar diameter percent");
    def->category = L("Supports");
    def->tooltip = L("The percentage of smaller pillars compared to the normal pillar diameter "
                      "which are used in problematic areas where a normal pilla cannot fit.");
    def->sidetext = L("%");
    def->min = 1;
    def->max = 100;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionPercent(50));

    def = this->add(prefix + "support_max_bridges_on_pillar", coInt);
    def->label = L("Max bridges on a pillar");
    def->tooltip = L(
        "Maximum number of bridges that can be placed on a pillar. Bridges "
        "hold support point pinheads and connect to pillars as small branches.");
    def->min = 0;
    def->max = 50;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionInt(prefix == "branching" ? 2 : 3));

    def = this->add(prefix + "support_max_weight_on_model", coFloat);
    def->label = L("Max weight on model");
    def->category = L("Supports");
    def->tooltip  = L(
        "Maximum weight of sub-trees that terminate on the model instead of the print bed. The weight is the sum of the lenghts of all "
        "branches emanating from the endpoint.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(10.));

    def = this->add(prefix + "support_pillar_connection_mode", coEnum);
    def->label = L("Pillar connection mode");
    def->tooltip = L("Controls the bridge type between two neighboring pillars."
                            " Can be zig-zag, cross (double zig-zag) or dynamic which"
                            " will automatically switch between the first two depending"
                            " on the distance of the two pillars.");
    def->set_enum<SLAPillarConnectionMode>(
        ConfigOptionEnum<SLAPillarConnectionMode>::get_enum_names(),
        { L("Zig-Zag"), L("Cross"), L("Dynamic") });
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionEnum(SLAPillarConnectionMode::dynamic));

    def = this->add(prefix + "support_buildplate_only", coBool);
    def->label = L("Support on build plate only");
    def->category = L("Supports");
    def->tooltip = L("Only create support if it lies on a build plate. Don't create support on a print.");
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add(prefix + "support_pillar_widening_factor", coFloat);
    def->label = L("Pillar widening factor");
    def->category = L("Supports");

    def->tooltip  = 
        L("Merging bridges or pillars into another pillars can "
        "increase the radius. Zero means no increase, one means "
        "full increase. The exact amount of increase is unspecified and can "
        "change in the future.");

    def->min = 0;
    def->max = 1;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(0.5));

    def = this->add(prefix + "support_base_diameter", coFloat);
    def->label = L("Support base diameter");
    def->category = L("Supports");
    def->tooltip = L("Diameter in mm of the pillar base");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 30;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(4.0));

    def = this->add(prefix + "support_base_height", coFloat);
    def->label = L("Support base height");
    def->category = L("Supports");
    def->tooltip = L("The height of the pillar base cone");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(1.0));

    def = this->add(prefix + "support_base_safety_distance", coFloat);
    def->label = L("Support base safety distance");
    def->category = L("Supports");
    def->tooltip  = L(
        "The minimum distance of the pillar base from the model in mm. "
        "Makes sense in zero elevation mode where a gap according "
        "to this parameter is inserted between the model and the pad.");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 10;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(1));

    def = this->add(prefix + "support_critical_angle", coFloat);
    def->label = L("Critical angle");
    def->category = L("Supports");
    def->tooltip = L("The default angle for connecting support sticks and junctions.");
    def->sidetext = L("°");
                    def->min = 0;
    def->max = 90;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(45));

    def = this->add(prefix + "support_max_bridge_length", coFloat);
    def->label = L("Max bridge length");
    def->category = L("Supports");
    def->tooltip = L("The max length of a bridge");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvanced;

    double default_val = 15.0;
    if (prefix == "branching")
        default_val = 5.0;

    def->set_default_value(new ConfigOptionFloat(default_val));

    def = this->add(prefix + "support_max_pillar_link_distance", coFloat);
    def->label = L("Max pillar linking distance");
    def->category = L("Supports");
    def->tooltip = L("The max distance of two pillars to get linked with each other."
                               " A zero value will prohibit pillar cascading.");
    def->sidetext = L("mm");
    def->min = 0;   // 0 means no linking
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(10.0));

    def = this->add(prefix + "support_object_elevation", coFloat);
    def->label = L("Object elevation");
    def->category = L("Supports");
    def->tooltip = L("How much the supports should lift up the supported object. "
                      "If \"Pad around object\" is enabled, this value is ignored.");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 150; // This is the max height of print on SL1
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(5.0));
}

void PrintConfigDef::init_sla_params()
{
    ConfigOptionDef* def;

    // SLA Printer settings

    def = this->add("display_width", coFloat);
    def->label = L("Display width");
    def->tooltip = L("Width of the display");
    def->min = 1;
    def->set_default_value(new ConfigOptionFloat(120.));

    def = this->add("display_height", coFloat);
    def->label = L("Display height");
    def->tooltip = L("Height of the display");
    def->min = 1;
    def->set_default_value(new ConfigOptionFloat(68.));

    def = this->add("display_pixels_x", coInt);
    def->full_label = L("Number of pixels in");
    def->label = ("X");
    def->tooltip = L("Number of pixels in X");
    // Match the SLA geometry contract: every positive resolution is valid.
    // wxSpinCtrl otherwise displays/clamps small loaded profiles to 100 while
    // the engine continues using their original raster dimensions.
    def->min = 1;
    def->set_default_value(new ConfigOptionInt(2560));

    def = this->add("display_pixels_y", coInt);
    def->label = ("Y");
    def->tooltip = L("Number of pixels in Y");
    def->min = 1;
    def->set_default_value(new ConfigOptionInt(1440));

    def = this->add("display_mirror_x", coBool);
    def->full_label = L("Display horizontal mirroring");
    def->label = L("Mirror horizontally");
    def->tooltip = L("Enable horizontal mirroring of output images");
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionBool(true));

    def = this->add("display_mirror_y", coBool);
    def->full_label = L("Display vertical mirroring");
    def->label = L("Mirror vertically");
    def->tooltip = L("Enable vertical mirroring of output images");
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("display_orientation", coEnum);
    def->label = L("Display orientation");
    def->tooltip = L("Set the actual LCD display orientation inside the SLA printer."
                     " Portrait mode will flip the meaning of display width and height parameters"
                     " and the output images will be rotated by 90 degrees.");
    def->set_enum<SLADisplayOrientation>({
        { "landscape",  L("Landscape") },
        { "portrait",   L("Portrait") }
    });
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionEnum<SLADisplayOrientation>(sladoPortrait));

    def = this->add("fast_tilt_time", coFloat);
    def->label = L("Fast");
    def->full_label = L("Fast tilt");
    def->tooltip = L("Time of the fast tilt");
    def->sidetext = L("s");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(5.));

    def = this->add("slow_tilt_time", coFloat);
    def->label = L("Slow");
    def->full_label = L("Slow tilt");
    def->tooltip = L("Time of the slow tilt");
    def->sidetext = L("s");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(8.));

    def = this->add("high_viscosity_tilt_time", coFloat);
    def->label = L("High viscosity");
    def->full_label = L("Tilt for high viscosity resin");
    def->tooltip = L("Time of the super slow tilt");
    def->sidetext = L("s");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(10.));

    def = this->add("area_fill", coFloat);
    def->label = L("Area fill threshold");
    def->tooltip = L("The value is expressed as a percentage of the bed area. If the area of a particular layer "
                     "is smaller than 'area_fill', then 'Below area fill threshold' parameters are used to determine the "
                     "layer separation (tearing) procedure. Otherwise 'Above area fill threshold' parameters are used.");
    def->sidetext = L("%");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(35.));

    def = this->add("relative_correction", coFloats);
    def->label = L("Printer scaling correction");
    def->full_label = L("Printer scaling correction");
    def->tooltip  = L("Printer scaling correction");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloats( { 1., 1.} ));

    def = this->add("relative_correction_x", coFloat);
    def->label = L("Printer scaling correction in X axis");
    def->full_label = L("Printer scaling X axis correction");
    def->tooltip  = L("Printer scaling correction in X axis");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("relative_correction_y", coFloat);
    def->label = L("Printer scaling correction in Y axis");
    def->full_label = L("Printer scaling Y axis correction");
    def->tooltip  = L("Printer scaling correction in Y axis");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("relative_correction_z", coFloat);
    def->label = L("Printer scaling correction in Z axis");
    def->full_label = L("Printer scaling Z axis correction");
    def->tooltip  = L("Printer scaling correction in Z axis");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("absolute_correction", coFloat);
    def->label = L("Printer absolute correction");
    def->full_label = L("Printer absolute correction");
    def->tooltip  = L("Will inflate or deflate the sliced 2D polygons according "
                      "to the sign of the correction.");
    def->sidetext = L("mm");
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(0.0));
    
    def = this->add("elefant_foot_min_width", coFloat);
    def->label = L("Elephant foot minimum width");
    def->category = L("Advanced");
    def->tooltip = L("Minimum width of features to maintain when doing elephant foot compensation.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.2));

    def = this->add("zcorrection_layers", coInt);
    def->label = L("Z compensation");
    def->category = L("Advanced");
    def->tooltip = L("Number of layers to Z correct to avoid cross layer bleed");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionInt(0));

    def = this->add("gamma_correction", coFloat);
    def->label = L("Printer gamma correction");
    def->full_label = L("Printer gamma correction");
    def->tooltip  = L("This will apply a gamma correction to the rasterized 2D "
                      "polygons. A gamma value of zero means thresholding with "
                      "the threshold in the middle. This behaviour eliminates "
                      "antialiasing without losing holes in polygons.");
    def->min = 0;
    def->max = 1;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(1.0));


    // SLA Material settings.

    def = this->add("material_colour", coString);
    def->label = L("Color");
    def->tooltip = L("This is only used in the Slic3r interface as a visual help.");
    def->gui_type = ConfigOptionDef::GUIType::color;
    def->set_default_value(new ConfigOptionString("#29B2B2"));

    def = this->add("material_type", coString);
    def->label = L("SLA material type");
    def->tooltip = L("SLA material type");
    def->gui_flags = "show_value";
    def->set_enum_values(ConfigOptionDef::GUIType::select_open,
        { "Tough", "Flexible", "Casting", "Dental", "Heat-resistant" });
    def->set_default_value(new ConfigOptionString("Tough"));

    def = this->add("initial_layer_height", coFloat);
    def->label = L("Initial layer height");
    def->tooltip = L("Initial layer height");
    def->sidetext = L("mm");
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(0.05));

    def = this->add("bottle_volume", coFloat);
    def->label = L("Bottle volume");
    def->tooltip = L("Bottle volume");
    def->sidetext = L("ml");
    def->min = 50;
    def->set_default_value(new ConfigOptionFloat(1000.0));

    def = this->add("bottle_weight", coFloat);
    def->label = L("Bottle weight");
    def->tooltip = L("Bottle weight");
    def->sidetext = L("kg");
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(1.0));

    def = this->add("material_density", coFloat);
    def->label = L("Density");
    def->tooltip = L("Density");
    def->sidetext = L("g/ml");
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(1.0));

    def = this->add("bottle_cost", coFloat);
    def->label = L("Cost");
    def->tooltip = L("Cost");
    def->sidetext = L("money/bottle");
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(0.0));

    def = this->add("faded_layers", coInt);
    def->label = L("Faded layers");
    def->tooltip = L("Number of the layers needed for the exposure time fade from initial exposure time to the exposure time");
    def->min = 3;
    def->max = 20;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionInt(10));

    def = this->add("min_exposure_time", coFloat);
    def->label = L("Minimum exposure time");
    def->tooltip = L("Minimum exposure time");
    def->sidetext = L("s");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("max_exposure_time", coFloat);
    def->label = L("Maximum exposure time");
    def->tooltip = L("Maximum exposure time");
    def->sidetext = L("s");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(100));

    def = this->add("exposure_time", coFloat);
    def->label = L("Exposure time");
    def->tooltip = L("Exposure time");
    def->sidetext = L("s");
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(10));

    def = this->add("min_initial_exposure_time", coFloat);
    def->label = L("Minimum initial exposure time");
    def->tooltip = L("Minimum initial exposure time");
    def->sidetext = L("s");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("max_initial_exposure_time", coFloat);
    def->label = L("Maximum initial exposure time");
    def->tooltip = L("Maximum initial exposure time");
    def->sidetext = L("s");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(150));

    def = this->add("initial_exposure_time", coFloat);
    def->label = L("Initial exposure time");
    def->tooltip = L("Initial exposure time");
    def->sidetext = L("s");
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(15));

    def = this->add("material_correction", coFloats);
    def->full_label = L("Correction for expansion");
    def->tooltip  = L("Correction for expansion");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloats( { 1., 1., 1. } ));

    def = this->add("material_correction_x", coFloat);
    def->full_label = L("Correction for expansion in X axis");
    def->tooltip  = L("Correction for expansion in X axis");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("material_correction_y", coFloat);
    def->full_label = L("Correction for expansion in Y axis");
    def->tooltip  = L("Correction for expansion in Y axis");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("material_correction_z", coFloat);
    def->full_label = L("Correction for expansion in Z axis");
    def->tooltip  = L("Correction for expansion in Z axis");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = this->add("material_notes", coString);
    def->label = L("SLA print material notes");
    def->tooltip = L("You can put your notes regarding the SLA print material here.");
    def->multiline = true;
    def->full_width = true;
    def->height = 13;
    // TODO currently notes are the only way to pass data
    // for non-PrusaResearch printers. We therefore need to always show them 
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("material_vendor", coString);
    def->set_default_value(new ConfigOptionString(L("(Unknown)")));
    def->cli = ConfigOptionDef::nocli;

    def = this->add("default_sla_material_profile", coString);
    def->label = L("Default SLA material profile");
    def->tooltip = L("Default print profile associated with the current printer profile. "
                   "On selection of the current printer profile, this print profile will be activated.");
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("sla_material_settings_id", coString);
    def->set_default_value(new ConfigOptionString(""));
    def->cli = ConfigOptionDef::nocli;

    def = this->add("default_sla_print_profile", coString);
    def->label = L("Default SLA material profile");
    def->tooltip = L("Default print profile associated with the current printer profile. "
                   "On selection of the current printer profile, this print profile will be activated.");
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = this->add("sla_print_settings_id", coString);
    def->set_default_value(new ConfigOptionString(""));
    def->cli = ConfigOptionDef::nocli;

    def = this->add("supports_enable", coBool);
    def->label = L("Generate supports");
    def->category = L("Supports");
    def->tooltip = L("Generate supports for the models");
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionBool(true));

    def = this->add("support_tree_type", coEnum);
    def->label = L("Support tree type");
    def->tooltip = L("Support tree building strategy");
    def->set_enum<sla::SupportTreeType>(
        ConfigOptionEnum<sla::SupportTreeType>::get_enum_names(),
        { L("Default"),
    // TRN One of the "Support tree type"s on SLAPrintSettings : Supports
            L("Branching (experimental)"),
    // TRN One of the "Support tree type"s on SLAPrintSettings : Supports
            L("Organic (experimental)"),
    // TRN One of the "Support tree type"s on SLAPrintSettings : Supports
            L("Organic (branching layout)") });
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionEnum(sla::SupportTreeType::Default));

    init_sla_support_params("");
    init_sla_support_params("branching");

    def = this->add("support_enforcers_only", coBool);
    def->label = L("Support only in enforced regions");
    def->category = L("Supports");
    def->tooltip = L("Only create support if it lies in a support enforcer.");
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("support_points_density_relative", coInt);
    def->label = L("Support points density");
    def->category = L("Supports");
    def->tooltip = L("This is a relative measure of support points density.");
    def->sidetext = L("%");
    def->min = 0;
    def->set_default_value(new ConfigOptionInt(100));

    def = this->add("pad_enable", coBool);
    def->label = L("Use pad");
    def->category = L("Pad");
    def->tooltip = L("Add a pad underneath the supported model");
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionBool(true));

    def = this->add("pad_wall_thickness", coFloat);
    def->label = L("Pad wall thickness");
    def->category = L("Pad");
     def->tooltip = L("The thickness of the pad and its optional cavity walls.");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 30;
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionFloat(2.0));

    def = this->add("pad_wall_height", coFloat);
    def->label = L("Pad wall height");
    def->tooltip = L("Defines the pad cavity depth. Set to zero to disable the cavity. "
                     "Be careful when enabling this feature, as some resins may "
                     "produce an extreme suction effect inside the cavity, "
                     "which makes peeling the print off the vat foil difficult.");
    def->category = L("Pad");
//     def->tooltip = L("");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 30;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(0.));
    
    def = this->add("pad_brim_size", coFloat);
    def->label = L("Pad brim size");
    def->tooltip = L("How far should the pad extend around the contained geometry");
    def->category = L("Pad");
    //     def->tooltip = L("");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 30;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(1.6));

    def = this->add("pad_max_merge_distance", coFloat);
    def->label = L("Max merge distance");
    def->category = L("Pad");
     def->tooltip = L("Some objects can get along with a few smaller pads "
                      "instead of a single big one. This parameter defines "
                      "how far the center of two smaller pads should be. If they"
                      "are closer, they will get merged into one pad.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(50.0));

    // This is disabled on the UI. I hope it will never be enabled.
//    def = this->add("pad_edge_radius", coFloat);
//    def->label = L("Pad edge radius");
//    def->category = L("Pad");
////     def->tooltip = L("");
//    def->sidetext = L("mm");
//    def->min = 0;
//    def->mode = comAdvanced;
//    def->set_default_value(new ConfigOptionFloat(1.0));

    def = this->add("pad_wall_slope", coFloat);
    def->label = L("Pad wall slope");
    def->category = L("Pad");
    def->tooltip = L("The slope of the pad wall relative to the bed plane. "
                     "90 degrees means straight walls.");
    def->sidetext = L("°");
    def->min = 45;
    def->max = 90;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(90.0));

    def = this->add("pad_around_object", coBool);
    def->label = L("Pad around object");
    def->category = L("Pad");
    def->tooltip = L("Create pad around object and ignore the support elevation");
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionBool(false));
    
    def = this->add("pad_around_object_everywhere", coBool);
    def->label = L("Pad around object everywhere");
    def->category = L("Pad");
    def->tooltip = L("Force pad around object everywhere");
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("pad_object_gap", coFloat);
    def->label = L("Pad object gap");
    def->category = L("Pad");
    def->tooltip  = L("The gap between the object bottom and the generated "
                      "pad in zero elevation mode.");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 10;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(1));

    def = this->add("pad_object_connector_stride", coFloat);
    def->label = L("Pad object connector stride");
    def->category = L("Pad");
    def->tooltip = L("Distance between two connector sticks which connect the object and the generated pad.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(10));

    def = this->add("pad_object_connector_width", coFloat);
    def->label = L("Pad object connector width");
    def->category = L("Pad");
    def->tooltip  = L("Width of the connector sticks which connect the object and the generated pad.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(0.5));

    def = this->add("pad_object_connector_penetration", coFloat);
    def->label = L("Pad object connector penetration");
    def->category = L("Pad");
    def->tooltip  = L(
        "How much should the tiny connectors penetrate into the model body.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(0.3));
    
    def = this->add("hollowing_enable", coBool);
    def->label = L("Enable hollowing");
    def->category = L("Hollowing");
    def->tooltip = L("Hollow out a model to have an empty interior");
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionBool(false));
    
    def = this->add("hollowing_min_thickness", coFloat);
    def->label = L("Wall thickness");
    def->category = L("Hollowing");
    def->tooltip  = L("Minimum wall thickness of a hollowed model.");
    def->sidetext = L("mm");
    def->min = 1;
    def->max = 10;
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionFloat(3.));
    
    def = this->add("hollowing_quality", coFloat);
    def->label = L("Accuracy");
    def->category = L("Hollowing");
    def->tooltip  = L("Performance vs accuracy of calculation. Lower values may produce unwanted artifacts.");
    def->min = 0;
    def->max = 1;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(0.5));
    
    def = this->add("hollowing_closing_distance", coFloat);
    def->label = L("Closing distance");
    def->category = L("Hollowing");
    def->tooltip  = L(
        "Hollowing is done in two steps: first, an imaginary interior is "
        "calculated deeper (offset plus the closing distance) in the object and "
        "then it's inflated back to the specified offset. A greater closing "
        "distance makes the interior more rounded. At zero, the interior will "
        "resemble the exterior the most.");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 10;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(2.0));

    def = this->add("material_print_speed", coEnum);
    def->label = L("Print speed");
    def->tooltip = L(
        "A slower printing profile might be necessary when using materials with higher viscosity "
        "or with some hollowed parts. It slows down the tilt movement and adds a delay before exposure.");
    def->set_enum<SLAMaterialSpeed>({
        { "slow",           L("Slow") },
        { "fast",           L("Fast") },
        { "high_viscosity", L("High viscosity") }
    });
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionEnum<SLAMaterialSpeed>(slamsFast));

    def = this->add("sla_archive_format", coString);
    def->label = L("Format of the output SLA archive");
    def->tooltip = L("Output format stored in this printer preset. Save the printer preset to keep your selection. "
                     "Select the version supported by your printer firmware; some formats require additional printer settings.");
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionString("SL1"));

    def = this->add("sla_anycubic_version", coString);
    def->label = L("Anycubic file version");
    def->tooltip = L("Version saved in this printer preset. Automatic uses the format default. Choose a version supported by your firmware. "
                     "Versions 1/515 have limited motion and infer geometry from the extension (PWC dimensions decode as zero); versions 516/517/518 store a machine table. "
                     "An incompatible imported value remains visible and must be corrected before slicing or export.");
    def->gui_type = ConfigOptionDef::GUIType::select_open;
    def->set_enum_values(std::vector<std::string>{"auto", "1", "515", "516", "517", "518"},
                         std::vector<std::string>{L("Automatic (format default)"), "1", "515", "516", "517", "518"});
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionString("auto"));

    def = this->add("sla_cbddlp_aa", coInt);
    def->label = L("CBDDLP / Photon AA planes");
    def->tooltip = L("Number of binary exposure planes in CBDDLP/Photon v2, saved in the printer preset. "
                     "UVtools supports 1 to 16. Powers of two (1, 2, 4, 8, 16) preserve full-white pixels on repeated encoding; "
                     "other counts quantize full white below 255. Version 1 always uses one plane. "
                     "This controls the file codec, separately from raster smoothing.");
    def->min = 1; def->max = 16; def->mode = comSimple;
    def->set_default_value(new ConfigOptionInt(8));

    def = this->add("sla_archive_header_mirror", coEnum);
    def->label = L("Mirror flag in file header");
    def->full_label = L("Mirror flag written into the output file header");
    def->tooltip = L("Only the mirror/flip flag stored in the header of the output file (e.g. CTB, CXDLP v4, Chitubox ZIP, OSF, OSLA, "
                     "CWS, GOO, VDT, Klipper, NanoDLP). The layer images themselves are NOT changed; use the display mirroring options above for that. "
                     "Automatic keeps the value this slicer has always written for each format. Change it only if your printer shows the print "
                     "mirrored or double-mirrored. Verify with a small asymmetric test print. Formats with a single yes/no flag store "
                     "any mirroring as 'yes' (NanoDLP: horizontal only).");
    def->set_enum<SLAArchiveHeaderMirror>({
        { "auto",       L("Automatic (format default)") },
        { "none",       L("No mirror") },
        { "horizontal", L("Horizontal") },
        { "vertical",   L("Vertical") },
        { "both",       L("Horizontal and vertical") }
    });
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionEnum<SLAArchiveHeaderMirror>(slahmAuto));

    def = this->add("nanodlp_device_profile", coString);
    def->label = L("NanoDLP controller profile (JSON)");
    def->tooltip = L("Controller profile saved inside this printer preset. Load profile.json from your NanoDLP controller or paste its JSON here. "
                     "The controller's motion templates determine lift/retract speeds and hardware commands. Generic SLA speeds do not rewrite those templates. "
                     "A valid profile requires ShieldBeforeLayer and ShieldAfterLayer. Empty profiles cannot be exported.");
    def->multiline = true;
    def->full_width = true;
    def->height = 12;
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("sla_phz_encryption_key", coString);
    def->label = L("PHZ encryption seed");
    def->tooltip = L("PHZ v2 only. 0 disables encryption. Enter an unsigned 32-bit decimal number or hexadecimal with 0x prefix. Saved with this printer preset.");
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionString("0"));

    for (const auto &setting : sla::machine_settings) {
        def = this->add(setting.key, setting.integer ? coInt : coFloat);
        def->label = setting.label;
        def->tooltip = "Generic LCD exposure and motion setting. Exporters must explicitly support this parameter.";
        def->sidetext = setting.unit;
        def->min = 0;
        if (std::string(setting.key).find("light_pwm") != std::string::npos) def->max = 255;
        def->mode = comSimple;
        if (setting.integer) def->set_default_value(new ConfigOptionInt(int(setting.initial)));
        else def->set_default_value(new ConfigOptionFloat(setting.initial));
    }

    def = this->add("sla_output_precision", coFloat);
    def->label = L("SLA output precision");
    def->tooltip = L("Minimum resolution in nanometers");
    def->sidetext = L("mm");
    def->min = float(SCALING_FACTOR);
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloat(0.001));

    // Declare retract values for material profile, overriding the print and printer profiles.
    for (const char* opt_key : {
        // float
        "support_head_front_diameter", "branchingsupport_head_front_diameter", 
        "support_head_penetration", "branchingsupport_head_penetration", 
        "support_head_width", "branchingsupport_head_width",
        "support_pillar_diameter", "branchingsupport_pillar_diameter",
        "elefant_foot_compensation", "absolute_correction",
        // int
        "support_points_density_relative"
        }) {
        auto it_opt = options.find(opt_key);
        assert(it_opt != options.end());
        def = this->add_nullable(std::string("material_ow_") + opt_key, it_opt->second.type);
        def->label = it_opt->second.label;
        def->full_label = it_opt->second.full_label;
        def->tooltip = it_opt->second.tooltip;
        def->sidetext = it_opt->second.sidetext;
        def->min  = it_opt->second.min;
        def->max  = it_opt->second.max;
        def->mode = it_opt->second.mode;
        switch (def->type) {
        case coFloat: def->set_default_value(new ConfigOptionFloatNullable{ it_opt->second.default_value->getFloat() }); break;
        case coInt:   def->set_default_value(new ConfigOptionIntNullable{ it_opt->second.default_value->getInt() }); break;
        default: assert(false);
        }
    }
}

// SLA Materials "sub-presets" settings
void PrintConfigDef::init_sla_tilt_params()
{
    ConfigOptionDef* def;

    def = this->add("delay_before_exposure", coFloats);
    def->full_label = L("Delay before exposure");
    def->tooltip = L("Delay before exposure after previous layer separation.");
    def->sidetext = L("s");
    def->min = 0;
    def->max = 30;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloats({ 3., 3.}));

    def = this->add("delay_after_exposure", coFloats);
    def->full_label = L("Delay after exposure");
    def->tooltip = L("Delay after exposure before layer separation.");
    def->sidetext = L("s");
    def->min = 0;
    def->max = 30;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloats({ 0., 0.}));

    def = this->add("tower_hop_height", coFloats);
    def->full_label = L("Tower hop height");
    def->tooltip = L("The height of the tower raise.");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 100;
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionFloats({ 0., 0.}));

    def = this->add("tower_speed", coEnums); 
    def->full_label = L("Tower speed");
    def->tooltip = L("Tower speed used for tower raise.");
    def->mode = comExpert;
    def->sidetext = L("mm/s");
    def->set_enum<TowerSpeeds>({
        { "layer1",  "1"  },
        { "layer2",  "2"  },
        { "layer3",  "3"  },
        { "layer4",  "4"  },
        { "layer5",  "5"  },
        { "layer8",  "8"  },
        { "layer11", "11" },
        { "layer14", "14" },
        { "layer18", "18" },
        { "layer22", "22" },
        { "layer24", "24" },
    });
    def->set_default_value(new ConfigOptionEnums<TowerSpeeds>({ tsLayer22, tsLayer22 }));

    const std::initializer_list<std::pair<std::string_view, std::string_view>> tilt_speeds_il = {
        { "move120",    "120"   },
        { "layer200",   "200"   },
        { "move300",    "300"   },
        { "layer400",   "400"   },
        { "layer600",   "600"   },
        { "layer800",   "800"   },
        { "layer1000",  "1000"  },
        { "layer1250",  "1250"  },
        { "layer1500",  "1500"  },
        { "layer1750",  "1750"  },
        { "layer2000",  "2000"  },
        { "layer2250",  "2250"  },
        { "move5120",   "5120"  },
        { "move8000",   "8000"  },
    };

    def = this->add("tilt_down_initial_speed", coEnums); 
    def->full_label = L("Tilt down initial speed");
    def->tooltip = L("Tilt speed used for an initial portion of tilt down move.");
    def->mode = comExpert;
    def->sidetext = L("μ-steps/s");
    def->set_enum<TiltSpeeds>(tilt_speeds_il);
    def->set_default_value(new ConfigOptionEnums<TiltSpeeds>({ tsLayer1750, tsLayer1750 }));

    def = this->add("tilt_down_finish_speed", coEnums); 
    def->full_label = L("Tilt down finish speed");
    def->tooltip = L("Tilt speed used for the rest of the tilt down move.");
    def->mode = comExpert;
    def->sidetext = L("μ-steps/s");
    def->set_enum<TiltSpeeds>(tilt_speeds_il);
    def->set_default_value(new ConfigOptionEnums<TiltSpeeds>({ tsLayer1750, tsLayer1750 }));

    def = this->add("tilt_up_initial_speed", coEnums); 
    def->full_label = L("Tilt up initial speed");
    def->tooltip = L("Tilt speed used for an initial portion of tilt up move.");
    def->mode = comExpert;
    def->sidetext = L("μ-steps/s");
    def->set_enum<TiltSpeeds>(tilt_speeds_il);
    def->set_default_value(new ConfigOptionEnums<TiltSpeeds>({ tsMove8000, tsMove8000 }));

    def = this->add("tilt_up_finish_speed", coEnums); 
    def->full_label = L("Tilt up finish speed");
    def->tooltip = L("Tilt speed used for the rest of the tilt-up.");
    def->mode = comExpert;
    def->sidetext = L("μ-steps/s");
    def->set_enum<TiltSpeeds>(tilt_speeds_il);
    def->set_default_value(new ConfigOptionEnums<TiltSpeeds>({ tsLayer1750, tsLayer1750 }));

    def = this->add("use_tilt", coBools);
    def->full_label = L("Use tilt");
    def->tooltip = L("If enabled, tilt is used for layer separation. Otherwise, all the parameters below are ignored.");
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionBools({ true, true }));

    def = this->add("tilt_down_offset_steps", coInts);
    def->full_label = L("Tilt down offset steps");
    def->tooltip = L("Number of steps to move down from the calibrated (horizontal) position with 'tilt_down_initial_speed'.");
    def->sidetext = L("μ-steps");
    def->min = 0;
    def->max = 10000;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionInts({ 0, 0 }));

    def = this->add("tilt_down_offset_delay", coFloats);
    def->full_label = L("Tilt down offset delay");
    def->tooltip = L("Delay after the tilt reaches 'tilt_down_offset_steps' position.");
    def->sidetext = L("s");
    def->min = 0;
    def->max = 20;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloats({ 0., 0. }));

    def = this->add("tilt_down_cycles", coInts);
    def->full_label = L("Tilt down cycles");
    def->tooltip = L("Number of cycles to split the rest of the tilt down move.");
    def->min = 0;
    def->max = 10;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionInts({ 1, 1 }));

    def = this->add("tilt_down_delay", coFloats);
    def->full_label = L("Tilt down delay");
    def->tooltip = L("The delay between tilt-down cycles.");
    def->sidetext = L("s");
    def->min = 0;
    def->max = 20;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloats({ 0., 0. }));

    def = this->add("tilt_up_offset_steps", coInts);
    def->full_label = L("Tilt up offset steps");
    def->tooltip = L("Move tilt up to calibrated (horizontal) position minus this offset.");
    def->sidetext = L("μ-steps");
    def->min = 0;
    def->max = 10000;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionInts({ 1200, 1200 }));

    def = this->add("tilt_up_offset_delay", coFloats);
    def->full_label = L("Tilt up offset delay");
    def->tooltip = L("Delay after the tilt reaches 'tilt_up_offset_steps' position.");
    def->sidetext = L("s");
    def->min = 0;
    def->max = 20;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloats({ 0., 0. }));

    def = this->add("tilt_up_cycles", coInts);
    def->full_label = L("Tilt up cycles");
    def->tooltip = L("Number of cycles to split the rest of the tilt-up.");
    def->min = 0;
    def->max = 10;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionInts({ 1, 1 }));

    def = this->add("tilt_up_delay", coFloats);
    def->full_label = L("Tilt up delay");
    def->tooltip = L("The delay between tilt-up cycles.");
    def->sidetext = L("s");
    def->min = 0;
    def->max = 20;
    def->mode = comExpert;
    def->set_default_value(new ConfigOptionFloats({ 0., 0. }));

}


// Keys of the removed FFF/FDM schema (and obsolete keys of older versions) are not defined any more: they are
// cleared below, so old presets, 3MF projects and bundles that still carry them load without any notice.
void PrintConfigDef::handle_legacy(t_config_option_key &opt_key, std::string &value)
{
    if(opt_key=="sla_archive_format" && boost::iequals(value,"cbt"))
        value="ctb_v1"; // Historical typo; never an actual .cbt printer format.
    // handle legacy options
    if (opt_key == "bed_size" && !value.empty()) {
        opt_key = "bed_shape";
        ConfigOptionPoint p;
        p.deserialize(value, ForwardCompatibilitySubstitutionRule::Disable);
        std::ostringstream oss;
        oss << "0x0," << p.value(0) << "x0," << p.value(0) << "x" << p.value(1) << ",0x" << p.value(1);
        value = oss.str();
    }

    if (! print_config_def.has(opt_key)) {
        opt_key = {};
        return;
    }
}

// Called after a config is loaded as a whole.
// Perform composite conversions, for example merging multiple keys into one key.
// Don't convert single options here, implement such conversion in PrintConfigDef::handle_legacy() instead.
void PrintConfigDef::handle_legacy_composite(DynamicPrintConfig &config)
{
    if (config.has("thumbnails")) {
        std::string extention;
        if (config.has("thumbnails_format")) {
            if (const ConfigOptionDef* opt = config.def()->get("thumbnails_format")) {
                auto label = opt->enum_def->enum_to_label(config.option("thumbnails_format")->getInt());
                if (label.has_value())
                    extention = *label;
            }
        }

        std::string thumbnails_str = config.opt_string("thumbnails");
        auto [thumbnails_list, errors] = GCodeThumbnails::make_and_check_thumbnail_list(thumbnails_str, extention);

        if (errors != enum_bitmask<ThumbnailError>()) {
            std::string error_str = "\n" + format("Invalid value provided for parameter %1%: %2%", "thumbnails", thumbnails_str);
            error_str += GCodeThumbnails::get_error_string(errors);
            throw BadOptionValueException(error_str);
        }

        if (!thumbnails_list.empty()) {
            const auto& extentions = ConfigOptionEnum<GCodeThumbnailsFormat>::get_enum_names();
            thumbnails_str.clear();
            for (const auto& [ext, size] : thumbnails_list)
                thumbnails_str += format("%1%x%2%/%3%, ", size.x(), size.y(), extentions[int(ext)]);
            thumbnails_str.resize(thumbnails_str.length() - 2);

            config.set_key_value("thumbnails", new ConfigOptionString(thumbnails_str));
        }
    }
}

const PrintConfigDef print_config_def;

DynamicPrintConfig DynamicPrintConfig::full_print_config()
{
    DynamicPrintConfig config;
    config.apply(SLAFullPrintConfig::defaults());
    return config;
}

DynamicPrintConfig::DynamicPrintConfig(const StaticPrintConfig& rhs) : DynamicConfig(rhs, rhs.keys_ref())
{
}

DynamicPrintConfig* DynamicPrintConfig::new_from_defaults_keys(const std::vector<std::string> &keys)
{
    auto *out = new DynamicPrintConfig();
    out->apply_only(SLAFullPrintConfig::defaults(), keys);
    return out;
}

double min_object_distance(const ConfigBase &cfg)
{
    // Only SLA exists; a configuration without a technology keeps the historical "no extra distance".
    const ConfigOptionEnum<PrinterTechnology> *opt_printer_technology = cfg.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
    return (opt_printer_technology != nullptr && opt_printer_technology->value == ptSLA) ? 6. : 0.;
}

// Default values containe option pair of values (Below and Above) for each titl modes 
// (Slow, Fast, HighViscosity and NoTilt) -> used for SL1S and other vendors printers

const std::map<std::string, ConfigOptionFloats> tilt_options_floats_defs =
{
    {"delay_before_exposure",    ConfigOptionFloats({ 3., 3., 0., 1., 3.5, 3.5, 0., 0. }) } ,
    {"delay_after_exposure",     ConfigOptionFloats({ 0., 0., 0., 0., 0., 0., 0., 0. }) } ,
    {"tilt_down_offset_delay",   ConfigOptionFloats({ 0., 0., 0., 0., 0., 0., 0., 0. }) } ,
    {"tilt_down_delay",          ConfigOptionFloats({ 0., 0., 0., 0., 0., 0., 0., 0. }) } ,
    {"tilt_up_offset_delay",     ConfigOptionFloats({ 0., 0., 0., 0., 0., 0., 0., 0. }) } ,
    {"tilt_up_delay",            ConfigOptionFloats({ 0., 0., 0., 0., 0., 0., 0., 0. }) } ,
    {"tower_hop_height",         ConfigOptionFloats({ 0., 0., 0., 0., 5., 5., 0., 0. }) } ,
};

const std::map<std::string, ConfigOptionInts> tilt_options_ints_defs =
{
    {"tilt_down_offset_steps",   ConfigOptionInts({ 0, 0, 0, 0, 2200, 2200, 0, 0 }) } ,
    {"tilt_down_cycles",         ConfigOptionInts({ 1, 1, 1, 1, 1, 1, 0, 0 }) } ,
    {"tilt_up_offset_steps",     ConfigOptionInts({ 1200, 1200, 600, 600, 2200, 2200, 0, 0 }) } ,
    {"tilt_up_cycles",           ConfigOptionInts({ 1, 1, 1, 1, 1, 1, 0, 0 }) } ,
};

const std::map<std::string, ConfigOptionBools> tilt_options_bools_defs =
{
    {"use_tilt",                    ConfigOptionBools({ true, true, true, true, true, true, false, false })} ,
};

const std::map<std::string, ConfigOptionEnums<TowerSpeeds>> tower_tilt_options_enums_defs =
{
    {"tower_speed",               ConfigOptionEnums<TowerSpeeds>({ tsLayer22, tsLayer22, tsLayer22, tsLayer22, tsLayer2, tsLayer2, tsLayer1, tsLayer1 })} ,
};

const std::map<std::string, ConfigOptionEnums<TiltSpeeds>> tilt_options_enums_defs =
{
    {"tilt_down_initial_speed",   ConfigOptionEnums<TiltSpeeds>({ tsLayer1750, tsLayer1750, tsLayer1750, tsLayer1750, tsLayer800, tsLayer800, tsMove120, tsMove120 }) } ,
    {"tilt_down_finish_speed",    ConfigOptionEnums<TiltSpeeds>({ tsLayer1750, tsLayer1750, tsMove8000, tsLayer1750, tsLayer1750, tsLayer1750, tsMove120, tsMove120 }) } ,
    {"tilt_up_initial_speed",     ConfigOptionEnums<TiltSpeeds>({ tsMove8000, tsMove8000, tsMove8000, tsMove8000, tsLayer1750, tsLayer1750, tsMove120, tsMove120 }) } ,
    {"tilt_up_finish_speed",      ConfigOptionEnums<TiltSpeeds>({ tsLayer1750, tsLayer1750, tsLayer1750, tsLayer1750, tsLayer800, tsLayer800, tsMove120, tsMove120 }) } ,
};

// Default values containe option pair of values (Below and Above) for each titl modes 
// (Slow, Fast, HighViscosity and NoTilt) -> used for SL1 printer

const std::map<std::string, ConfigOptionFloats> tilt_options_floats_sl1_defs =
{
    {"delay_before_exposure",    ConfigOptionFloats({ 3., 3., 0., 1., 3.5, 3.5, 0., 0. }) } ,
    {"delay_after_exposure",     ConfigOptionFloats({ 0., 0., 0., 0., 0., 0., 0., 0. }) } ,
    {"tilt_down_offset_delay",   ConfigOptionFloats({ 1., 1., 0., 0., 0., 0., 0., 0. }) } ,
    {"tilt_down_delay",          ConfigOptionFloats({ 0., 0., 0., 0., 0., 0., 0., 0. }) } ,
    {"tilt_up_offset_delay",     ConfigOptionFloats({ 0., 0., 0., 0., 1., 1., 0., 0. }) } ,
    {"tilt_up_delay",            ConfigOptionFloats({ 0., 0., 0., 0., 0., 0., 0., 0. }) } ,
    {"tower_hop_height",         ConfigOptionFloats({ 0., 0., 0., 0., 5., 5., 0., 0. }) } ,
};

const std::map<std::string, ConfigOptionInts> tilt_options_ints_sl1_defs =
{
    {"tilt_down_offset_steps",   ConfigOptionInts({ 650, 650, 0, 0, 2200, 2200, 0, 0 }) } ,
    {"tilt_down_cycles",         ConfigOptionInts({ 1, 1, 1, 1, 1, 1, 0, 0 }) } ,
    {"tilt_up_offset_steps",     ConfigOptionInts({ 400, 400, 400, 400, 2200, 2200, 0, 0 }) } ,
    {"tilt_up_cycles",           ConfigOptionInts({ 1, 1, 1, 1, 1, 1, 0, 0 }) } ,
};

const std::map<std::string, ConfigOptionBools> tilt_options_bools_sl1_defs =
{
    {"use_tilt",                    ConfigOptionBools({ true, true, true, true, true, true, false, false })} ,
};


const std::map<std::string, ConfigOptionEnums<TowerSpeeds>> tower_tilt_options_enums_sl1_defs =
{
    {"tower_speed",               ConfigOptionEnums<TowerSpeeds>({ tsLayer22, tsLayer22, tsLayer22, tsLayer22, tsLayer2, tsLayer2, tsLayer1, tsLayer1 })} ,
};

const std::map<std::string, ConfigOptionEnums<TiltSpeeds>> tilt_options_enums_sl1_defs =
{
    {"tilt_down_initial_speed",   ConfigOptionEnums<TiltSpeeds>({ tsLayer400, tsLayer400, tsLayer400, tsLayer400, tsLayer600, tsLayer600, tsMove120, tsMove120 }) } ,
    {"tilt_down_finish_speed",    ConfigOptionEnums<TiltSpeeds>({ tsLayer1500, tsLayer1500, tsLayer1750, tsLayer1500, tsLayer1500, tsLayer1500, tsMove120, tsMove120 }) } ,
    {"tilt_up_initial_speed",     ConfigOptionEnums<TiltSpeeds>({ tsMove5120, tsMove5120, tsMove5120, tsMove5120, tsLayer1500, tsLayer1500, tsMove120, tsMove120 }) } ,
    {"tilt_up_finish_speed",      ConfigOptionEnums<TiltSpeeds>({ tsLayer400, tsLayer400, tsLayer400, tsLayer400, tsLayer600, tsLayer600, tsMove120, tsMove120 }) } ,
};

void  handle_legacy_sla(DynamicPrintConfig &config)
{
    for (std::string corr : {"relative_correction", "material_correction"}) {
        if (config.has(corr)) {
            if (std::string corr_x = corr + "_x"; !config.has(corr_x)) {
                auto* opt = config.opt<ConfigOptionFloat>(corr_x, true);
                opt->value = config.opt<ConfigOptionFloats>(corr)->values[0];
            }

            if (std::string corr_y = corr + "_y"; !config.has(corr_y)) {
                auto* opt = config.opt<ConfigOptionFloat>(corr_y, true);
                opt->value = config.opt<ConfigOptionFloats>(corr)->values[0];
            }

            if (std::string corr_z = corr + "_z"; !config.has(corr_z)) {
                auto* opt = config.opt<ConfigOptionFloat>(corr_z, true);
                opt->value = config.opt<ConfigOptionFloats>(corr)->values[1];
            }
        }
    }

    // Load default tilt options in config in respect to the print speed, if config is loaded from old PS

    if (config.has("material_print_speed") &&
        !config.has("tilt_down_offset_delay") // Config from old PS doesn't contain any of tilt options, so check it
        ) {
        int tilt_mode = config.option("material_print_speed")->getInt();
        const bool is_sl1_model = config.opt_string("printer_model") == "SL1";
        update_tilts_by_mode(config, tilt_mode, is_sl1_model);
    }
}

void update_tilts_by_mode(DynamicPrintConfig& config, int tilt_mode, bool is_sl1_model)
{
    const std::map<std::string, ConfigOptionFloats> floats_defs = is_sl1_model ? tilt_options_floats_sl1_defs : tilt_options_floats_defs;
    const std::map<std::string, ConfigOptionInts>   ints_defs   = is_sl1_model ? tilt_options_ints_sl1_defs : tilt_options_ints_defs;
    const std::map<std::string, ConfigOptionBools>  bools_defs  = is_sl1_model ? tilt_options_bools_sl1_defs : tilt_options_bools_defs;
    const std::map<std::string, ConfigOptionEnums<TowerSpeeds>>   tower_enums_defs = is_sl1_model ? tower_tilt_options_enums_sl1_defs : tower_tilt_options_enums_defs;
    const std::map<std::string, ConfigOptionEnums<TiltSpeeds>>    tilt_enums_defs  = is_sl1_model ? tilt_options_enums_sl1_defs : tilt_options_enums_defs;

    for (const std::string& opt_key : tilt_options()) {
        switch (config.def()->get(opt_key)->type) {
        case coFloats: {
            ConfigOptionFloats values = floats_defs.at(opt_key);
            double val1 = values.get_at(2 * tilt_mode);
            double val2 = values.get_at(2 * tilt_mode + 1);
            config.set_key_value(opt_key, new ConfigOptionFloats({ val1, val2 }));
        }
            break;
        case coInts: {
            auto values = ints_defs.at(opt_key);
            int val1 = values.get_at(2 * tilt_mode);
            int val2 = values.get_at(2 * tilt_mode + 1);
            config.set_key_value(opt_key, new ConfigOptionInts({ val1, val2 }));
        }
            break;
        case coBools: {
            auto values = bools_defs.at(opt_key);
            bool val1 = values.get_at(2 * tilt_mode);
            bool val2 = values.get_at(2 * tilt_mode + 1);
            config.set_key_value(opt_key, new ConfigOptionBools({ val1, val2 }));
        }
            break;
        case coEnums: {
            int val1, val2;
            if (opt_key == "tower_speed") {
                auto values = tower_enums_defs.at(opt_key);
                val1 = values.get_at(2 * tilt_mode);
                val2 = values.get_at(2 * tilt_mode + 1);
            }
            else {
                auto values = tilt_enums_defs.at(opt_key);
                val1 = values.get_at(2 * tilt_mode);
                val2 = values.get_at(2 * tilt_mode + 1);
            }
            config.set_key_value(opt_key, new ConfigOptionEnumsGeneric({ val1, val2 }));
        }
            break;
        case coNone:
        default:
            break;
        }
    }
}

std::string DynamicPrintConfig::validate()
{
    // Full print config is initialized from the defaults.
    //FIXME no validation on SLA data? (the FFF validation through FullPrintConfig is gone with ptFFF)
    return std::string();
}

// Declare and initialize static caches of StaticPrintConfig derived classes.
#define PRINT_CONFIG_CACHE_ELEMENT_DEFINITION(r, data, CLASS_NAME) StaticPrintConfig::StaticCache<class Slic3r::CLASS_NAME> BOOST_PP_CAT(CLASS_NAME::s_cache_, CLASS_NAME);
#define PRINT_CONFIG_CACHE_ELEMENT_INITIALIZATION(r, data, CLASS_NAME) Slic3r::CLASS_NAME::initialize_cache();
#define PRINT_CONFIG_CACHE_INITIALIZE(CLASSES_SEQ) \
    BOOST_PP_SEQ_FOR_EACH(PRINT_CONFIG_CACHE_ELEMENT_DEFINITION, _, BOOST_PP_TUPLE_TO_SEQ(CLASSES_SEQ)) \
    int print_config_static_initializer() { \
        /* Putting a trace here to avoid the compiler to optimize out this function. */ \
        /*BOOST_LOG_TRIVIAL(trace) << "Initializing StaticPrintConfigs";*/ \
        /* Tamas: alternative solution through a static volatile int. Boost log pollutes stdout and prevents tests from generating clean output */ \
        static volatile int ret = 1; \
        BOOST_PP_SEQ_FOR_EACH(PRINT_CONFIG_CACHE_ELEMENT_INITIALIZATION, _, BOOST_PP_TUPLE_TO_SEQ(CLASSES_SEQ)) \
        return ret; \
    }
PRINT_CONFIG_CACHE_INITIALIZE((
    SLAMaterialConfig, SLAPrintConfig, SLAPrintObjectConfig, SLAPrinterConfig, SLAFullPrintConfig))
static int print_config_static_initialized = print_config_static_initializer();

CLIInputConfigDef::CLIInputConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("load", coStrings);
    def->label = L("Load config file");
    def->tooltip = L("Load configuration from the specified file. It can be used more than once to load options from multiple files.");

    def = this->add("printer-profile", coString);
    def->label = ("Printer preset name");
    def->tooltip = ("Name of the printer preset used for slicing.");
    def->set_default_value(new ConfigOptionString());

    def = this->add("print-profile", coString);
    def->label = ("Print preset name");
    def->tooltip = ("Name of the print preset used for slicing.");
    def->set_default_value(new ConfigOptionString());

    def = this->add("material-profile", coStrings);
    def->label = ("Material preset name(s)");
    def->tooltip = "Name(s) of the resin material preset(s) used for SLA slicing.";
    def->set_default_value(new ConfigOptionStrings());
}

CLIActionsConfigDef::CLIActionsConfigDef()
{
    ConfigOptionDef* def;

    // doesn't need any aditional input 

    def = this->add("help", coBool);
    def->label = L("Help");
    def->tooltip = L("Show this help.");
    def->cli = "help|h";
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("help_sla", coBool);
    def->label = L("Help (SLA options)");
    def->tooltip = L("Show the full list of SLA print configuration options.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("query-printer-models", coBool);
    def->label = ("Get list of printer models");
    def->tooltip = "Get installed SLA printer models as JSON. Use --output to save the result and --datadir to select a configuration folder.";

    // needs a --printer-profile input

    def = this->add("query-print-material-profiles", coBool);
    def->cli = "query-print-material-profiles";
    def->label = "Get print and resin material profiles";
    def->tooltip = "Get print and resin material profiles for the selected --printer-profile as JSON.";

    // needs a configuration input

    def = this->add("save", coString);
    def->label = L("Save config file");
    def->tooltip = L("Save configuration to the specified file.");
    def->set_default_value(new ConfigOptionString());

    // needs a model to process this actions

    def = this->add("info", coBool);
    def->label = L("Output Model Info");
    def->tooltip = L("Write information about the model to the console.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("export_obj", coBool);
    def->label = L("Export OBJ");
    def->tooltip = L("Export the model(s) as OBJ.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("export_stl", coBool);
    def->label = L("Export STL");
    def->tooltip = L("Export the model(s) as STL.");
    def->set_default_value(new ConfigOptionBool(false));

    // needs model and configuration

    def = this->add("export_3mf", coBool);
    def->label = L("Export 3MF");
    def->tooltip = L("Export the model(s) as 3MF.");
    def->set_default_value(new ConfigOptionBool(false));

    //! slice/export_sla is the same action
    //! May be merged into one action "slice_and_export"

    def = this->add("slice", coBool);
    def->label = L("Slice");
    def->tooltip = L("Slice the model and export the selected SLA archive format.");
    def->cli = "slice|s";
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("export_sla", coBool);
    def->label = L("Export SLA");
    def->tooltip = L("Slice the model and export SLA printing layers as PNG.");
    def->cli = "export-sla|sla";
    def->set_default_value(new ConfigOptionBool(false));

}

CLITransformConfigDef::CLITransformConfigDef()
{
    ConfigOptionDef* def;

    // Transform options:
    def = this->add("align_xy", coPoint);
    def->label = L("Align XY");
    def->tooltip = L("Align the model to the given point.");
    def->set_default_value(new ConfigOptionPoint(Vec2d(100,100)));

    def = this->add("cut", coFloat);
    def->label = L("Cut");
    def->tooltip = L("Cut model at the given Z.");
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("center", coPoint);
    def->label = L("Center");
    def->tooltip = L("Center the print around the given center.");
    def->set_default_value(new ConfigOptionPoint(Vec2d(100,100)));

    def = this->add("dont_arrange", coBool);
    def->label = L("Don't arrange");
    def->tooltip = L("Do not rearrange the given models before merging and keep their original XY coordinates.");

    def = this->add("ensure_on_bed", coBool);
    def->label = L("Ensure on bed");
    def->tooltip = L("Lift the object above the bed when it is partially below. Enabled by default, use --no-ensure-on-bed to disable.");
    def->set_default_value(new ConfigOptionBool(true));

    def = this->add("duplicate", coInt);
    def->label = L("Duplicate");
    def->tooltip =L("Multiply copies by this factor.");
    def->min = 1;

    def = this->add("duplicate_grid", coPoint);
    def->label = L("Duplicate by grid");
    def->tooltip = L("Multiply copies by creating a grid.");

    def = this->add("merge", coBool);
    def->label = L("Merge");
    def->tooltip = L("Arrange the supplied models in a plate and merge them in a single model in order to perform actions once.");
    def->cli = "merge|m";

    def = this->add("rotate", coFloat);
    def->label = L("Rotate");
    def->tooltip = L("Rotation angle around the Z axis in degrees.");
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("rotate_x", coFloat);
    def->label = L("Rotate around X");
    def->tooltip = L("Rotation angle around the X axis in degrees.");
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("rotate_y", coFloat);
    def->label = L("Rotate around Y");
    def->tooltip = L("Rotation angle around the Y axis in degrees.");
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("scale", coFloatOrPercent);
    def->label = L("Scale");
    def->tooltip = L("Scaling factor or percentage.");
    def->set_default_value(new ConfigOptionFloatOrPercent(1, false));

    def = this->add("split", coBool);
    def->label = L("Split");
    def->tooltip = L("Detect unconnected parts in the given model(s) and split them into separate objects.");

    def = this->add("scale_to_fit", coPoint3);
    def->label = L("Scale to Fit");
    def->tooltip = L("Scale to fit the given volume.");
    def->set_default_value(new ConfigOptionPoint3(Vec3d(0, 0, 0)));
}

CLIMiscConfigDef::CLIMiscConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("ignore_nonexistent_config", coBool);
    def->label = L("Ignore non-existent config files");
    def->tooltip = L("Do not fail if a file supplied to --load does not exist.");

    def = this->add("config_compatibility", coEnum);
    def->label = L("Forward-compatibility rule when loading configurations from config files and project files (3MF, AMF).");
    def->tooltip = L("This version of PrusaSlicer may not understand configurations produced by the newest PrusaSlicer versions. "
        "For example, newer PrusaSlicer may extend the list of supported firmware flavors. One may decide to "
        "bail out or to substitute an unknown value with a default silently or verbosely.");
    def->set_enum<ForwardCompatibilitySubstitutionRule>({
        { "disable",        L("Bail out on unknown configuration values") },
        { "enable",         L("Enable reading unknown configuration values by verbosely substituting them with defaults.") },
        { "enable_silent",  L("Enable reading unknown configuration values by silently substituting them with defaults.") }
        });
    def->set_default_value(new ConfigOptionEnum<ForwardCompatibilitySubstitutionRule>(ForwardCompatibilitySubstitutionRule::Enable));

    def = this->add("output", coString);
    def->label = L("Output File");
    def->tooltip = L("The file where the output will be written (if not specified, it will be based on the input file).");
    def->cli = "output|o";

    def = this->add("datadir", coString);
    def->label = L("Data directory");
    def->tooltip = L("Load and store settings at the given directory. This is useful for maintaining different profiles or including configurations from a network storage.");

    def = this->add("threads", coInt);
    def->label = L("Maximum number of threads");
    def->tooltip = L("Sets the maximum number of threads the slicing process will use. If not defined, it will be decided automatically.");
    def->min = 1;

    def = this->add("loglevel", coInt);
    def->label = L("Logging level");
    def->tooltip = L("Sets logging sensitivity. 0:fatal, 1:error, 2:warning, 3:info, 4:debug, 5:trace\n"
        "For example. loglevel=2 logs fatal, error and warning level messages.");
    def->min = 0;

#ifdef SLIC3R_GUI
    def = this->add("opengl-aa", coBool);
    def->label = L("Automatic OpenGL antialiasing samples number selection");
    def->tooltip = L("Automatically select the highest number of samples for OpenGL antialiasing.");
    def->cli = "opengl-aa";
    def->set_default_value(new ConfigOptionBool(false));

#if !SLIC3R_OPENGL_ES
    def = this->add("opengl-version", coString);
    def->label = L("OpenGL version");
    def->tooltip = L("Select a specific version of OpenGL");
    def->cli = "opengl-version";
    def->set_default_value(new ConfigOptionString());

    def = this->add("opengl-compatibility", coBool);
    def->label = L("OpenGL compatibility profile");
    def->tooltip = L("Enable OpenGL compatibility profile");
    def->cli = "opengl-compatibility";
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("opengl-debug", coBool);
    def->label = L("OpenGL debug output");
    def->tooltip = L("Activate OpenGL debug output on graphic cards which support it (OpenGL 4.3 or higher)");
    def->cli = "opengl-debug";
    def->set_default_value(new ConfigOptionBool(false));
#endif // !SLIC3R_OPENGL_ES

    def = this->add("single_instance", coBool);
    def->label = L("Single instance mode");
    def->tooltip = L("If enabled, the command line arguments are sent to an existing instance of GUI PrusaSlicer, "
                     "or an existing PrusaSlicer window is activated. "
                     "Overrides the \"single_instance\" configuration value from application preferences.");

    def = this->add("delete-after-load", coString);
    def->label = L("Delete files after loading");
    def->tooltip = L("Delete files after loading.");

#if (defined(_MSC_VER) || defined(__MINGW32__)) && defined(SLIC3R_GUI)
    def = this->add("sw_renderer", coBool);
    def->label = L("Render with a software renderer");
    def->tooltip = L("Render with a software renderer. The bundled MESA software renderer is loaded instead of the default OpenGL driver.");
    def->min = 0;
#endif // _MSC_VER

    // for internal use only
    def = this->add("webdev", coBool);
    def->label = "Enable webdev tools"; // Not translated on purpose - for internal use only.
    def->tooltip = "Enable webdev tools";

    def = this->add("single_instance_on_url", coBool);
    def->label = "Single instance mode for prusaslicer url"; // Not translated on purpose - for internal use only.
    def->tooltip = "Works as single_instance but only if prusaslicer url is present.";

#endif // SLIC3R_GUI
}

const CLIInputConfigDef             cli_input_config_def;
const CLIActionsConfigDef           cli_actions_config_def;
const CLITransformConfigDef         cli_transform_config_def;
const CLIMiscConfigDef              cli_misc_config_def;

uint64_t ModelConfig::s_last_timestamp = 1;

static Points to_points(const std::vector<Vec2d> &dpts)
{
    Points pts; pts.reserve(dpts.size());
    for (auto &v : dpts)
        pts.emplace_back( coord_t(scale_(v.x())), coord_t(scale_(v.y())) );
    return pts;    
}

Points get_bed_shape(const DynamicPrintConfig &config)
{
    const auto *bed_shape_opt = config.opt<ConfigOptionPoints>("bed_shape");
    if (!bed_shape_opt) {
        
        // Here, it is certain that the bed shape is missing, so an infinite one
        // has to be used, but still, the center of bed can be queried
        if (auto center_opt = config.opt<ConfigOptionPoint>("center"))
            return { scaled(center_opt->value) };
        
        return {};
    }
    
    return to_points(bed_shape_opt->values);
}

Points get_bed_shape(const SLAPrinterConfig &cfg) { return to_points(cfg.bed_shape.values); }

std::string get_sla_suptree_prefix(const DynamicPrintConfig &config)
{
    const auto *suptreetype = config.option<ConfigOptionEnum<sla::SupportTreeType>>("support_tree_type");
    std::string slatree = "";
    if (suptreetype) {
        auto ttype = static_cast<sla::SupportTreeType>(suptreetype->getInt());
        switch (ttype) {
        // The organic tree reuses the whole branching option family.
        case sla::SupportTreeType::Branching:
        case sla::SupportTreeType::Organic:
        case sla::SupportTreeType::OrganicBranching: slatree = "branching"; break;
        default:
            ;
        }
    }

    return slatree;
}

static bool is_XL_printer(const std::string& printer_notes)
{
    return boost::algorithm::contains(printer_notes, "PRINTER_VENDOR_PRUSA3D")
        && boost::algorithm::contains(printer_notes, "PRINTER_MODEL_XL");
}

bool is_XL_printer(const DynamicPrintConfig &cfg)
{
    auto *printer_notes = cfg.opt<ConfigOptionString>("printer_notes");
    return printer_notes && is_XL_printer(printer_notes->value);
}

} // namespace Slic3r

#include <cereal/types/polymorphic.hpp> // IWYU pragma: keep

CEREAL_REGISTER_TYPE(Slic3r::DynamicPrintConfig)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::DynamicConfig, Slic3r::DynamicPrintConfig)
