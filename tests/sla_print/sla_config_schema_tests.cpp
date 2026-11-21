// W9b-2: dump of the configuration schema used by tests/sla_print/verify_sla_schema_keys.py.
// The test always checks that every key of the SLA static configuration classes is defined in
// print_config_def; when SLA_SCHEMA_DUMP is set it additionally writes the list of all defined keys
// (<prefix>.all) and of the SLA class keys (<prefix>.sla), one key per line.
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

TEST_CASE("SLA configuration classes only use keys defined in print_config_def", "[sla_config_schema]")
{
    std::set<std::string> all;
    for (const auto &kv : print_config_def.options)
        all.insert(kv.first);

    std::set<std::string> sla;
    auto add = [&sla](const auto &cfg) {
        for (const std::string &k : cfg.keys())
            sla.insert(k);
    };
    add(SLAFullPrintConfig());
    add(SLAPrintConfig());
    add(SLAPrintObjectConfig());
    add(SLAMaterialConfig());
    add(SLAPrinterConfig());

    for (const std::string &k : sla)
        CHECK(all.count(k) == 1);

    // Every key of the SLA preset option lists must be a defined option too.
    for (const auto *list : { &Preset::sla_print_options(), &Preset::sla_material_options(), &Preset::sla_printer_options() })
        for (const std::string &k : *list) {
            INFO(k);
            CHECK(all.count(k) == 1);
            sla.insert(k);
        }

    if (const char *prefix = std::getenv("SLA_SCHEMA_DUMP")) {
        std::ofstream a(std::string(prefix) + ".all"), s(std::string(prefix) + ".sla");
        for (const std::string &k : all) a << k << '\n';
        for (const std::string &k : sla) s << k << '\n';
        // Full signature of every option, to prove that a kept definition did not change.
        std::ofstream f(std::string(prefix) + ".full");
        for (const auto &kv : print_config_def.options) {
            const ConfigOptionDef &d = kv.second;
            f << kv.first << '\t' << int(d.type) << '\t' << int(d.printer_technology) << '\t' << int(d.mode) << '\t'
              << d.label << '\t' << d.full_label << '\t' << d.category << '\t' << d.tooltip << '\t' << d.sidetext << '\t'
              << d.cli << '\t' << d.ratio_over << '\t' << d.gui_flags << '\t' << int(d.gui_type) << '\t'
              << d.multiline << d.full_width << d.is_code << d.readonly << '\t' << d.height << '\t' << d.width << '\t'
              << d.min << '\t' << d.max << '\t' << d.max_literal << '\t'
              << (d.default_value ? d.default_value->serialize() : std::string("<none>")) << '\t';
            if (d.enum_def)
                for (size_t i = 0; i < d.enum_def->values().size(); ++i)
                    f << d.enum_def->value(int(i)) << '|' << d.enum_def->label(int(i)) << ',';
            for (const std::string &al : d.aliases) f << " alias:" << al;
            for (const std::string &sc : d.shortcut) f << " short:" << sc;
            f << '\n';
        }
    }
}

TEST_CASE("Removed FFF keys in an SLA configuration are ignored silently", "[sla_config_schema][sla_fff_keys_ignored]")
{
    // Values that would be rejected or substituted if the options still existed (unknown enum value, bad number).
    const std::string ini =
        "printer_technology = SLA\n"
        "layer_height = 0.05\n"
        "gcode_flavor = no_such_flavor\n"
        "fill_pattern = gyroid\n"
        "fill_density = 15%\n"
        "nozzle_diameter = 0.4,0.4\n"
        "filament_diameter = abc\n"
        "extruder = 2\n"
        "perimeters = 3\n"
        "wipe_tower_x = 170\n"
        "extrusion_width_ratio = 0.5\n"
        "draft_shield = 1\n"
        "support_material_pattern = pillars\n"
        "gcode_label_objects = 1\n"
        "wiping_volumes_matrix = 1,2,3,4\n"
        "colorprint_heights = 1,2\n"
        "start_gcode = G28\n"
        "exposure_time = 7\n";
    DynamicPrintConfig cfg;
    REQUIRE_NOTHROW(cfg.load_from_ini_string(ini, ForwardCompatibilitySubstitutionRule::Disable));
    CHECK(cfg.opt_float("layer_height") == 0.05);
    CHECK(cfg.opt_float("exposure_time") == 7.);
    for (const char *key : {"gcode_flavor", "fill_pattern", "fill_density", "nozzle_diameter", "filament_diameter", "extruder", "perimeters",
                            "wipe_tower_x", "extrusion_width_ratio", "draft_shield", "support_material_pattern",
                            "gcode_label_objects", "wiping_volumes_matrix", "colorprint_heights", "start_gcode"}) {
        INFO(key);
        CHECK_FALSE(cfg.has(key));
    }
    // Neither a substitution nor an error is reported for them.
    ConfigSubstitutions subs;
    REQUIRE_NOTHROW(subs = cfg.load_from_ini_string(ini, ForwardCompatibilitySubstitutionRule::Enable));
    CHECK(subs.empty());
    REQUIRE_NOTHROW(subs = cfg.load_from_ini_string(ini, ForwardCompatibilitySubstitutionRule::EnableSilent));
    CHECK(subs.empty());
}
