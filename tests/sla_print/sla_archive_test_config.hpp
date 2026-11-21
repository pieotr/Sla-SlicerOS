#pragma once

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Format/AnycubicSLA.hpp"
#include "libslic3r/Format/NanoDLPSchema.hpp"
#include <string>

namespace Slic3r::Test {
// A small, valid fixture for each native writer, not production defaults.
// Legacy formats have fixed geometry, integer units or mandatory rest times;
// testing them with an unrelated SL1 preset fails before exercising their codec.
inline void configure_archive_fixture(SLAFullPrintConfig &config, const std::string &format)
{
    config.printer_technology.setInt(ptSLA);
    config.display_orientation.value = sladoLandscape;
    config.set("sla_archive_format", format);
    config.set("display_width", 100.); config.set("display_height", 60.);
    config.set("display_pixels_x", format == "cwsrgb" ? 201 : 200);
    config.set("display_pixels_y", 120);
    config.set("layer_height", .05); config.set("initial_layer_height", .05);
    config.set("exposure_time", 2.5); config.set("initial_exposure_time", 20.);
    if(format=="ctb_encrypted" || format=="ctb_encrypted_v4" || format=="ctb_v3" ||
       format=="ctb_v4" || format=="ctb_v5" || format=="ctb_gktwo") {
        config.set("display_mirror_x",true);config.set("display_mirror_y",false);
    }
    if(format=="nanodlp" || format=="nanodlp_v1" || format=="nanodlp_rgb" || format=="nanodlp_rgb_v1") {
        // Development fixture only: preserve the original FSS profile, never
        // install it as a default for a user's unrelated controller.
        config.set("nanodlp_device_profile",nanodlp::schema_defaults()["profile"].dump());
        config.set("display_mirror_x",true);config.set("display_mirror_y",false);
        config.set("sla_light_pwm",180);config.set("sla_bottom_light_pwm",180);
        config.set("sla_transition_layers",0);
    }
    if(format=="svgx") {
        config.set("display_mirror_x",false);config.set("display_mirror_y",true);
        config.set("sla_transition_layers",0);
        for(const bool bottom:{false,true}) {
            const std::string prefix=bottom?"sla_bottom_":"sla_";
            config.set(prefix+"lift_height",6.);config.set(prefix+"lift_speed",100.);config.set(prefix+"retract_speed",100.);
            for(const char *key:{"wait_before_cure","wait_after_cure","wait_after_lift","lift_height2","retract_height2"})
                config.set(prefix+key,0.);
            config.set(prefix+"light_pwm",255);
        }
    }
    if(format=="ctb_v1" || format=="cbt" || format=="cbddlp_v1" || format=="photon_v1") {
        config.set("sla_lift_height",.05);config.set("sla_bottom_lift_height",6.);
        for(const char *key:{"sla_lift_speed","sla_bottom_lift_speed","sla_retract_speed","sla_bottom_retract_speed"})
            config.set(key,100.);
    }
    if (const auto *machine = anycubic_legacy_geometry(format)) {
        config.set("display_width", machine->width); config.set("display_height", machine->height);
        config.set("max_print_height", machine->machine_z);
    }
    if (format == "cxdlpv1" || format == "cxdlp" || format == "cxdlpv2") {
        config.set("exposure_time", 3.);
        config.set("sla_wait_before_cure", 1.); config.set("sla_bottom_wait_before_cure", 1.);
        if (format != "cxdlpv1") {
            config.set("printer_model", std::string("CL-60"));
            config.set("sla_lift_speed", 120.); config.set("sla_bottom_lift_speed", 60.);
            config.set("sla_retract_speed", 180.); config.set("sla_bottom_retract_speed", 180.);
        }
    }
    if (format == "cxdlpv4") {
        // Required native identity for this software-only synthetic raster;
        // the shared dimensions do not certify a physical CL-60 profile.
        config.set("printer_model",std::string("CL-60"));
    }
    if (format == "n4" || format == "n7") {
        const bool n7 = format == "n7";
        config.set("display_width", n7 ? 192. : 68.04);
        config.set("display_height", n7 ? 120. : 120.96);
        config.set("display_pixels_x", n7 ? 2560 : 1440);
        config.set("display_pixels_y", n7 ? 1600 : 2560);
        config.set("max_print_height", n7 ? 300. : 135.);
        config.set("exposure_time", 3.); config.set("sla_transition_layers", 0);
        for (const char *key : {"sla_lift_speed", "sla_bottom_lift_speed", "sla_retract_speed", "sla_bottom_retract_speed"})
            config.set(key, 120.);
    }
    if (format == "lgs" || format == "lgs30" || format == "lgs120" || format == "lgs4k") {
        config.set("display_pixels_x",120);config.set("display_pixels_y",200);
        config.set("display_width",60.);config.set("display_height",100.);
        config.set("max_print_height",format=="lgs"?140.:format=="lgs30"?170.:format=="lgs120"?150.:190.);
        config.set("display_mirror_x",true);config.set("display_mirror_y",false);
        config.set("sla_transition_layers",0);
        config.set("sla_retract_speed",80.);config.set("sla_bottom_retract_speed",80.);
    }
    if (format == "gr1" || format == "mdlp") {
        config.set("display_mirror_x",false);config.set("display_mirror_y",false);
        config.set("sla_transition_layers",0);config.set("exposure_time",3.);
        if(format=="mdlp") {
            for(const char *key:{"sla_lift_speed","sla_bottom_lift_speed","sla_retract_speed","sla_bottom_retract_speed"})config.set(key,100.);
            config.set("sla_wait_before_cure",.8);config.set("sla_bottom_wait_before_cure",.8);
        } else {
            config.set("sla_lift_speed",120.);config.set("sla_bottom_lift_speed",60.);
            config.set("sla_retract_speed",120.);config.set("sla_bottom_retract_speed",120.);
            config.set("sla_bottom_lift_height",4.);
            config.set("sla_wait_before_cure",2.);config.set("sla_bottom_wait_before_cure",2.);
        }
    }
    if (format == "zcode") {
        config.set("display_mirror_x",false);config.set("display_mirror_y",true);
        config.set("sla_transition_layers",0);
    }
    if (format == "zcodex") {
        config.set("display_width",74.67);config.set("display_height",132.88);
        config.set("display_pixels_x",1440);config.set("display_pixels_y",2560);
        config.set("max_print_height",175.);config.set("display_mirror_x",true);config.set("display_mirror_y",false);
        config.set("sla_transition_layers",0);
        config.set("sla_lift_speed",120.);config.set("sla_bottom_lift_speed",120.);
        config.set("sla_retract_speed",100.);config.set("sla_bottom_retract_speed",100.);
    }
    if (format == "jxs") {
        config.set("display_width",221.4);config.set("display_height",129.6);
        config.set("display_pixels_x",4920);config.set("display_pixels_y",2880);config.set("max_print_height",245.);
        config.set("display_mirror_x",false);config.set("display_mirror_y",true);
    }
    if (format == "qdt") {
        config.set("display_width",400.);config.set("display_height",200.);
        config.set("display_pixels_x",800);config.set("display_pixels_y",400);config.set("max_print_height",400.);
        config.set("display_mirror_x",true);config.set("display_mirror_y",false);
        config.set("sla_bottom_layers",4);config.set("sla_transition_layers",0);
        config.set("exposure_time",3.);config.set("initial_exposure_time",30.);
        for(const char *key:{"sla_lift_speed","sla_bottom_lift_speed","sla_retract_speed","sla_bottom_retract_speed"})config.set(key,100.);
    }
    if (format == "photons") {
        config.set("display_width", 68.04); config.set("display_height", 120.96);
        config.set("display_pixels_x", 1440); config.set("display_pixels_y", 2560);
        config.set("max_print_height", 165.);
        config.set("display_mirror_x", true); config.set("display_mirror_y", false);
        config.set("sla_transition_layers", 0);
        for (const char *key : {"sla_lift_speed", "sla_bottom_lift_speed", "sla_retract_speed", "sla_bottom_retract_speed"})
            config.set(key, 120.);
    }

}
}
