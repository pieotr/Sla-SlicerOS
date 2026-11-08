///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ZCodeArchive.hpp"
#include "ZCodeRSA.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Zipper.hpp"
#include <cstring>
#include <iomanip>
#include <locale>
#include <nlohmann/json.hpp>
#include <sstream>

namespace Slic3r {

// Native ports of local UVtools ZCodeFile.cs and ZCodexFile.cs (AGPLv3).
// Device motion strings below are SLA protocols, never FFF toolpaths.
namespace {
using Json = nlohmann::json;
bool precise(float v, double scale) { return std::isfinite(v) && v == float(std::round(double(v) * scale) / scale); }
uint32_t milliseconds(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds * 1000 > UINT32_MAX)
        throw ExportError("ZCode/ZCodex delay exceeds uint32 milliseconds");
    return uint32_t(std::llround(seconds * 1000));
}
std::string number(double v) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(3) << v;
    std::string s = out.str();
    while (s.back() == '0')
        s.pop_back();
    if (s.back() == '.')
        s.pop_back();
    return s;
}
} // namespace
// Native ZCode XML/RSA protocol, from local UVtools ZCodeFile.cs (AGPLv3).
std::unique_ptr<sla::RasterBase> ZCodeArchive::create_raster() const { return sla::create_printer_raster(m_cfg); }
sla::RasterEncoder ZCodeArchive::get_encoder() const { return sla::PNGRasterEncoder{}; }
void ZCodeArchive::validate_configuration(const SLAPrint &print) {
    const auto &cfg = print.printer_config();
    if (cfg.display_pixels_x.value < 1 || cfg.display_pixels_y.value < 1 || cfg.display_pixels_x.value > 32768 ||
        cfg.display_pixels_y.value > 32768 ||
        uint64_t(cfg.display_pixels_x.value) * cfg.display_pixels_y.value > (uint64_t(128) << 20) ||
        !std::isfinite(cfg.display_width.value) || !std::isfinite(cfg.display_height.value) ||
        cfg.display_width.value <= 0 || cfg.display_height.value <= 0 || !std::isfinite(cfg.max_print_height.value) ||
        cfg.max_print_height.value <= 0)
        throw ExportError("ZCode requires positive finite screen geometry inside raster limits");
    if (!precise(float(cfg.max_print_height.value), 1000))
        throw ExportError("ZCode machine Z requires 0.001 mm precision to keep the final raise within bounds");
    const bool portrait = cfg.display_orientation.value == sladoPortrait;
    const double pitch = portrait ? cfg.display_height.value / cfg.display_pixels_y.value
                                  : cfg.display_width.value / cfg.display_pixels_x.value;
    const float pixel_size = float(pitch * 1000);
    const auto decoded_mm = [&](int pixels) {
        return std::nearbyint((float(pixels) * pixel_size / 1000.f) * 100.f) / 100.f;
    };
    // Nominal screen dimensions are rounded to 0.01 mm. A small pitch
    // difference caused by that rounding is valid when the shared stored
    // pixel size reconstructs both declared dimensions exactly in UVtools.
    if (!precise(float(cfg.display_width.value), 100) || !precise(float(cfg.display_height.value), 100) ||
        decoded_mm(cfg.display_pixels_x.value) != float(cfg.display_width.value) ||
        decoded_mm(cfg.display_pixels_y.value) != float(cfg.display_height.value))
        throw ExportError("ZCode shared pixel size must reproduce both screen dimensions at 0.01 mm precision");
    if (cfg.display_mirror_x.value || !cfg.display_mirror_y.value)
        throw ExportError("ZCode requires vertical mirroring without horizontal mirroring");
    const auto &mat = print.material_config();
    if (mat.sla_bottom_layers.value < 0 || mat.sla_bottom_layers.value > UINT16_MAX ||
        mat.sla_transition_layers.value < 0)
        throw ExportError("ZCode bottom/transition counts exceed schema limits");
    if (print.default_object_config().layer_height.value <= 0 ||
        !precise(float(print.default_object_config().layer_height.value), 1000) || mat.exposure_time.value <= 0 ||
        mat.initial_exposure_time.value <= 0 || !precise(float(mat.exposure_time.value), 100) ||
        !precise(float(mat.initial_exposure_time.value), 100))
        throw ExportError("ZCode requires positive height at 0.001 mm and exposure at 0.01 s precision");
    milliseconds(mat.exposure_time.value);
    milliseconds(mat.initial_exposure_time.value);
    for (bool bottom : {false, true}) {
        for (const char *key : {"lift_height", "lift_height2", "retract_height2", "wait_before_cure", "wait_after_cure",
                                "wait_after_lift"}) {
            const float value = sla::motion_parameter(print, key, bottom);
            if (value < 0 || !precise(value, 100))
                throw ExportError("ZCode distances/rests require nonnegative two-decimal precision");
        }
        for (const char *key : {"lift_speed", "lift_speed2", "retract_speed", "retract_speed2"}) {
            const float value = sla::motion_parameter(print, key, bottom);
            if (value < 0 || !precise(value, 10))
                throw ExportError("ZCode speeds require nonnegative 0.1 mm/min precision (cm/min wire units)");
        }
        if (sla::motion_parameter(print, "retract_height2", bottom) >
            sla::motion_parameter(print, "lift_height", bottom) + sla::motion_parameter(print, "lift_height2", bottom))
            throw ExportError("ZCode retract exceeds total lift");
        const float lift = sla::motion_parameter(print, "lift_height", bottom);
        const float lift2 = sla::motion_parameter(print, "lift_height2", bottom);
        const float retract2 = sla::motion_parameter(print, "retract_height2", bottom);
        for (const auto &stage :
             {std::pair{lift, "lift_speed"}, std::pair{lift2, "lift_speed2"},
              std::pair{lift + lift2 - retract2, "retract_speed"}, std::pair{retract2, "retract_speed2"}})
            if (stage.first > 0 && sla::motion_parameter(print, stage.second, bottom) <= 0)
                throw ExportError("ZCode active movement requires a positive speed");
        if (lift + lift2 == 0 && sla::motion_parameter(print, "lift_speed", bottom) <= 0)
            throw ExportError("ZCode Z advance requires a positive lift speed");
        const float pwm = sla::motion_parameter(print, "light_pwm", bottom);
        if (pwm <= 0 || pwm > 255)
            throw ExportError("ZCode cannot preserve nonzero configured exposure/rest with PWM=0; requires PWM 1..255");
    }
    for (double value : {mat.material_density.value, mat.bottle_volume.value, mat.bottle_cost.value})
        if (!std::isfinite(value) || value < 0)
            throw ExportError("ZCode resin statistics require finite nonnegative settings");
}
void ZCodeArchive::export_print(const std::string fname, const SLAPrint &print, const ThumbnailsList &thumbnails,
                                const std::string &projectname) {
    validate_configuration(print);
    if (m_layers.empty() || m_layers.size() != print.print_layers().size() || m_layers.size() > UINT32_MAX)
        throw ExportError("ZCode requires a complete nonempty layer stack");
    const bool portrait = m_cfg.display_orientation.value == sladoPortrait;
    const size_t width = portrait ? m_cfg.display_pixels_y.value : m_cfg.display_pixels_x.value;
    const size_t height = portrait ? m_cfg.display_pixels_x.value : m_cfg.display_pixels_y.value;
    const double width_mm = portrait ? m_cfg.display_height.value : m_cfg.display_width.value;
    const double height_mm = portrait ? m_cfg.display_width.value : m_cfg.display_height.value;
    std::vector<std::string> lines = {"G21;", "G90;", "M106 S0;", "M17;", "G28 Z0;"};
    auto move = [&](double z, double speed, const char *command = "G0") {
        lines.push_back(std::string(command) + " Z" + number(std::round(z * 1000) / 1000) + " F" + number(speed / 10) +
                        ";");
    };
    auto wait = [&](double seconds) {
        const auto ms = milliseconds(seconds);
        lines.push_back(ms ? "G4 P" + std::to_string(ms) + ";" : "G4 P.0;");
    };
    auto power = [](int pwm) {
        return (pwm * 300 + 254) / 255;
    }; // Inverse decoder truncation, preserving every requested PWM.
    double volume = 0, last_z = 0;
    size_t previous_pixels = 0;
    const auto bottom = sla::bottom_layers(print, m_layers.size());
    for (size_t i = 0; i < m_layers.size(); ++i) {
        const float z = sla::layer_z(print, i);
        auto p = [&](const char *key) { return double(sla::motion_parameter(print, key, i < bottom)); };
        const double lift = p("lift_height"), lift2 = p("lift_height2"), total = lift + lift2,
                     retract2 = p("retract_height2");
        if (!precise(z, 1000) || z <= last_z || z + total > m_cfg.max_print_height.value)
            throw ExportError("ZCode requires increasing Z at 0.001 mm within machine motion limits");
        const auto &mask = m_layers[i];
        size_t pixels = previous_pixels;
        if (!(i && mask.size() == m_layers[i - 1].size() &&
              std::memcmp(mask.data(), m_layers[i - 1].data(), mask.size()) == 0)) {
            png::ImageGreyscale image;
            if (!png::decode_png(png::ReadBuf{mask.data(), mask.size()}, image) || image.cols != width ||
                image.rows != height)
                throw ExportError("ZCode PNG dimensions differ from printer screen");
            pixels = std::count_if(image.buf.begin(), image.buf.end(), [](uint8_t p) { return p != 0; });
        }
        previous_pixels = pixels;
        volume += pixels * width_mm / width * height_mm / height * (z - last_z) / 1000.;
        lines.push_back("M6054 \"" + std::to_string(i + 1) + ".png\";");
        if (lift > 0)
            move(z + lift, p("lift_speed"));
        if (lift2 > 0)
            move(z + total, p("lift_speed2"));
        if (total == 0)
            move(z, p("lift_speed"));
        if (p("wait_after_lift") > 0)
            wait(p("wait_after_lift"));
        if (total - retract2 > 0)
            move(z + retract2, p("retract_speed"));
        if (retract2 > 0)
            move(z, p("retract_speed2"));
        wait(p("wait_before_cure"));
        lines.push_back("M106 S" + std::to_string(power(int(p("light_pwm")))) + ";");
        wait(std::nearbyint(sla::layer_exposure(print, i) * 100.f) / 100.f);
        lines.push_back("M106 S0;");
        if (p("wait_after_cure") > 0)
            wait(p("wait_after_cure"));
        last_z = z;
    }
    lines.push_back("M106 S0;");
    double maximum_speed = 0;
    for (bool b : {false, true})
        for (const char *key : {"lift_speed", "lift_speed2", "retract_speed", "retract_speed2"})
            maximum_speed = std::max(maximum_speed, double(sla::motion_parameter(print, key, b)));
    if (last_z < m_cfg.max_print_height.value) {
        const double first = std::min(
            m_cfg.max_print_height.value,
            last_z + std::max(4., double(sla::motion_parameter(print, "lift_height", m_layers.size() - 1 < bottom))));
        const double normal_speed = sla::motion_parameter(print, "lift_speed", false);
        move(first, normal_speed > 0 ? normal_speed : maximum_speed);
        if (first < m_cfg.max_print_height.value)
            move(m_cfg.max_print_height.value, maximum_speed, "G1");
    }
    lines.push_back("M18;");
    const auto cipher = sla::zcode::encrypt_lines(lines); // Validate/encrypt before opening destination.
    auto escape = [](const std::string &text) {
        std::string out;
        for (char c : text)
            switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '\"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out += c;
            }
        return out;
    };
    const auto &mat = print.material_config();
    const float weight = float(volume * mat.material_density.value);
    const float cost =
        float(mat.bottle_volume.value > 0 ? volume * mat.bottle_cost.value / mat.bottle_volume.value : 0);
    if (!std::isfinite(float(volume)) || !std::isfinite(weight) || !std::isfinite(cost))
        throw ExportError("ZCode statistics overflow");
    std::ostringstream xml;
    xml.imbue(std::locale::classic());
    xml << std::setprecision(9);
    xml << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Print><Device z=\"" << m_cfg.max_print_height.value
        << "\" height=\"" << height << "\" width=\"" << width << "\" type=\""
        << escape(m_cfg.printer_model.value.empty() ? "IBEE" : m_cfg.printer_model.value) << "\" pixel_size=\""
        << float(width_mm / width * 1000) << "\"/><Profile name=\"Sla-SlicerOS\"><Slice bottom_layers=\"" << bottom
        << "\" exposure_bottom=\"" << milliseconds(mat.initial_exposure_time.value) << "\" exposure=\""
        << milliseconds(mat.exposure_time.value) << "\" height_bottom=\""
        << sla::motion_parameter(print, "lift_height", true) << "\" speed_bottom=\""
        << sla::motion_parameter(print, "lift_speed", true) << "\" height=\""
        << sla::motion_parameter(print, "lift_height", false) << "\" speed=\""
        << sla::motion_parameter(print, "lift_speed", false) << "\" cooldown_bottom=\""
        << milliseconds(sla::motion_parameter(print, "wait_before_cure", true)) << "\" cooldown=\""
        << milliseconds(sla::motion_parameter(print, "wait_before_cure", false)) << "\" thickness=\""
        << print.default_object_config().layer_height.value
        << "\" anti_aliasing_level=\"16\" anti_aliasing_grey_level=\"0\" led_power=\""
        << power(int(sla::motion_parameter(print, "light_pwm", false))) << "\"/></Profile><Job><name>"
        << escape(projectname) << "</name><previewImage>preview.png</previewImage><layers>" << m_layers.size()
        << "</layers><time>" << sla::lcd_print_time(print) << "</time><volumn>" << float(volume)
        << "</volumn><thickness>" << print.default_object_config().layer_height.value << "</thickness><price>" << cost
        << "</price><weight>" << weight << "</weight></Job></Print>";
    Zipper zip(fname);
    zip.add_entry("task.xml");
    zip << xml.str();
    zip.add_entry("lcd.gcode");
    zip << cipher;
    for (size_t i = 0; i < m_layers.size(); ++i)
        zip.add_entry(std::to_string(i + 1) + ".png", m_layers[i].data(), m_layers[i].size());
    for (const auto &thumb : thumbnails) {
        if (!thumb.is_valid() || thumb.width > 4096 || thumb.height > 4096)
            continue;
        std::vector<uint8_t> rgba(640 * 480 * 4, 0);
        for (size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 255;
        const double scale = std::min(640. / thumb.width, 480. / thumb.height);
        const size_t w = std::max(size_t(1), size_t(std::round(thumb.width * scale))),
                     h = std::max(size_t(1), size_t(std::round(thumb.height * scale)));
        for (size_t y = 0; y < h; ++y)
            for (size_t x = 0; x < w; ++x)
                std::copy_n(thumb.pixels.data() +
                                ((thumb.height - 1 - y * thumb.height / h) * thumb.width + x * thumb.width / w) * 4,
                            4, rgba.data() + ((y + (480 - h) / 2) * 640 + x + (640 - w) / 2) * 4);
        const auto png = sla::PNGRasterEncoder{}(rgba.data(), 640, 480, 4);
        zip.add_entry("preview.png", png.data(), png.size());
        break;
    }
    zip.finalize();
}
std::unique_ptr<sla::RasterBase> ZCodexArchive::create_raster() const { return sla::create_printer_raster(m_cfg); }
sla::RasterEncoder ZCodexArchive::get_encoder() const { return sla::PNGRasterEncoder{}; }
void ZCodexArchive::validate_configuration(const SLAPrint &print) {
    const auto &cfg = print.printer_config();
    const bool portrait = cfg.display_orientation.value == sladoPortrait;
    if ((portrait ? cfg.display_pixels_y.value : cfg.display_pixels_x.value) != 1440 ||
        (portrait ? cfg.display_pixels_x.value : cfg.display_pixels_y.value) != 2560 ||
        (portrait ? cfg.display_height.value : cfg.display_width.value) != 74.67 ||
        (portrait ? cfg.display_width.value : cfg.display_height.value) != 132.88 || !cfg.display_mirror_x.value ||
        cfg.display_mirror_y.value)
        throw ExportError("ZCodex requires Inkspire geometry: 1440x2560, "
                          "74.67x132.88 mm, horizontal mirroring");
    if (!std::isfinite(cfg.max_print_height.value) || cfg.max_print_height.value <= 40)
        throw ExportError("ZCodex requires finite machine height allowing 40 mm final raise");
    const auto &mat = print.material_config();
    if (mat.sla_transition_layers.value != 0)
        throw ExportError("ZCodex has no per-layer exposure/transition field");
    if (mat.sla_bottom_layers.value < 0 || mat.sla_bottom_layers.value > UINT16_MAX)
        throw ExportError("ZCodex bottom count exceeds uint16");
    if (mat.exposure_time.value <= 0 || mat.initial_exposure_time.value <= 0 ||
        !precise(float(mat.exposure_time.value), 100) || !precise(float(mat.initial_exposure_time.value), 100) ||
        print.default_object_config().layer_height.value <= 0 ||
        !precise(float(print.default_object_config().layer_height.value), 1000))
        throw ExportError("ZCodex requires positive exposure at 0.01 s and height "
                          "at 0.001 mm precision");
    milliseconds(mat.exposure_time.value);
    milliseconds(mat.initial_exposure_time.value);
    sla::require_single_stage_motion(print, "ZCodex", true);
    // Metadata stores one shared motion/rest setting. Do not silently discard
    // separate bottom values even though firmware commands allow per-layer PWM.
    for (const char *key : {"lift_height", "lift_speed", "retract_speed", "wait_before_cure"}) {
        const float v = sla::motion_parameter(print, key, false);
        if (v != sla::motion_parameter(print, key, true))
            throw ExportError(std::string("ZCodex requires shared bottom/normal ") + key);
        if (v < 0 || !precise(v, 100))
            throw ExportError("ZCodex motion/rest values require nonnegative "
                              "two-decimal precision");
    }
    // The original decoder's speed regex reads integers only; a decimal feed
    // would be truncated without reporting an error.
    for (const char *key : {"lift_speed", "retract_speed"}) {
        const float v = sla::motion_parameter(print, key, false);
        if (v <= 0 || !precise(v, 1))
            throw ExportError("ZCodex requires positive integer mm/min speeds for "
                              "decoder fidelity");
    }
    milliseconds(sla::motion_parameter(print, "wait_before_cure", false));
    for (bool bottom : {false, true}) {
        const float pwm = sla::motion_parameter(print, "light_pwm", bottom);
        if (pwm <= 0 || pwm > 255)
            throw ExportError("ZCodex PWM exceeds uint8");
    }
    if (!std::isfinite(mat.material_density.value) || mat.material_density.value <= 0)
        throw ExportError("ZCodex requires finite positive material density");
}
void ZCodexArchive::export_print(const std::string fname, const SLAPrint &print, const ThumbnailsList &thumbnails,
                                 const std::string &projectname) {
    validate_configuration(print);
    if (m_layers.empty() || m_layers.size() != print.print_layers().size() || m_layers.size() > UINT32_MAX)
        throw ExportError("ZCodex requires a complete nonempty layer stack");
    const auto &mat = print.material_config();
    const float lift = sla::motion_parameter(print, "lift_height", false);
    const float lift_speed = sla::motion_parameter(print, "lift_speed", false);
    const float retract_speed = sla::motion_parameter(print, "retract_speed", false);
    std::ostringstream protocol;
    protocol.imbue(std::locale::classic());
    double previous_z = 0, volume = 0;
    size_t previous_pixels = 0;
    Json layer_stats = Json::array();
    for (size_t i = 0; i < m_layers.size(); ++i) {
        const float z = sla::layer_z(print, i);
        const double dz = z - previous_z;
        if (!precise(z, 1000) || dz <= 0 || z + lift > m_cfg.max_print_height.value || (lift > 0 && dz >= lift))
            throw ExportError("ZCodex requires increasing Z inside machine height "
                              "and thickness below active lift");
        const auto &mask = m_layers[i];
        size_t pixels = previous_pixels;
        if (!(i && mask.size() == m_layers[i - 1].size() &&
              std::memcmp(mask.data(), m_layers[i - 1].data(), mask.size()) == 0)) {
            png::ImageGreyscale image;
            if (!png::decode_png(png::ReadBuf{mask.data(), mask.size()}, image) || image.cols != 1440 ||
                image.rows != 2560)
                throw ExportError("ZCodex PNG dimensions differ from Inkspire screen");
            pixels = std::count_if(image.buf.begin(), image.buf.end(), [](uint8_t p) { return p != 0; });
        }
        previous_pixels = pixels;
        const double layer_volume = pixels * (74.67 / 1440) * (132.88 / 2560) * dz / 1000.;
        volume += layer_volume;
        layer_stats.push_back(Json{{"Layer", i}, {"UsedMaterialVolume", float(layer_volume)}});
        protocol << "<Slice> " << i << '\n';
        if (lift > 0) {
            protocol << "G1 Z" << number(lift) << " F" << number(lift_speed) << '\n';
            protocol << "G1 Z-" << number(std::round((lift - dz) * 1000) / 1000) << " F" << number(retract_speed)
                     << '\n';
        } else
            protocol << "G1 Z" << number(std::round(dz * 1000) / 1000) << " F" << number(lift_speed) << '\n';
        protocol << "<Delay_blank>\nM106 S"
                 << int(sla::motion_parameter(print, "light_pwm", i < sla::bottom_layers(print, m_layers.size())))
                 << "\n<Delay_model>\nM106 S0\n";
        previous_z = z;
    }
    if (previous_z + 40 > m_cfg.max_print_height.value)
        throw ExportError("ZCodex final 40 mm raise exceeds machine height");
    protocol << "G1 Z40.0 F" << number(lift_speed) << "\nM18\n";
    const auto time = sla::lcd_print_time(print);
    const auto bottom = sla::bottom_layers(print, m_layers.size());
    const uint32_t before = milliseconds(sla::motion_parameter(print, "wait_before_cure", false));
    const uint32_t exposure = milliseconds(mat.exposure_time.value),
                   bottom_exposure = milliseconds(mat.initial_exposure_time.value);
    const float weight = float(std::round(volume * mat.material_density.value * 1000) / 1000);
    if (!std::isfinite(float(volume)) || !std::isfinite(weight))
        throw ExportError("ZCodex material statistics overflow");
    Json resin{{"Guid", "07452AC2-7494-4576-BA60-BFEA8815F917"},
               {"Material", ""},
               {"MaterialId", nullptr},
               {"LayerThickness", float(print.default_object_config().layer_height.value)},
               {"PrintTime", time},
               {"LayerTime", exposure},
               {"BottomLayersTime", bottom_exposure},
               {"AdditionalSupportLayerTime", 0},
               {"BottomLayersNumber", bottom},
               {"BlankingLayerTime", before},
               {"TotalMaterialVolumeUsed", float(volume)},
               {"TotalMaterialWeightUsed", weight},
               {"TotalLayersCount", m_layers.size()},
               {"DisableSettingsChanges", false},
               {"Layers", layer_stats}};
    Json user{{"MaxLayer", m_layers.size() - 1},
              {"PrintTime", std::to_string((time / 3600) % 24) + "h " + std::to_string((time / 60) % 60) + "m"},
              {"MaterialVolume", float(volume)},
              {"IsAdvanced", 1},
              {"Printer", "Zortrax Inkspire"},
              {"MaterialType", ""},
              {"MaterialId", 0},
              {"LayerThickness", number(print.default_object_config().layer_height.value) + " mm"},
              {"RaftEnabled", 0},
              {"RaftHeight", 0},
              {"RaftOffset", 0},
              {"ModelLiftEnabled", 0},
              {"ModelLiftHeight", 0},
              {"CrossSupportEnabled", 0},
              {"LayerExposureTime", exposure},
              {"ExposureOffTime", before},
              {"BottomLayerExposureTime", bottom_exposure},
              {"BottomLayersCount", bottom},
              {"SupportAdditionalExposureEnabled", 0},
              {"SupportAdditionalExposureTime", 0},
              {"ZLiftDistance", lift},
              {"ZLiftRetractRate", retract_speed},
              {"ZLiftFeedRate", lift_speed},
              {"AntiAliasing", 16},
              {"XCorrection", 0},
              {"YCorrection", 0},
              {"HollowEnabled", 0},
              {"HollowThickness", 0},
              {"InfillDensity", 0}};
    // These legacy wire fields are mandatory schema defaults, not exposed
    // FFF settings. They cannot be removed from an Inkspire-compatible archive.
    Json metadata{
        {"ZCodexVersion", "2.0.0.0"},
        {"SoftwareVersion", "2.12.2.0"},
        {"MinFirmwareVersion", "20013"},
        {"PrinterModelEnumId", 40},
        {"PrinterName", "Inkspire"},
        {"Materials",
         Json::array({Json{{"ExtruderType", "MAIN"}, {"Id", 0}, {"Name", ""}, {"Usage", 0}, {"Temperature", 0}}})},
        {"HeatbedTemperature", 0},
        {"ChamberTemperature", 0},
        {"CommandCount", 0},
        {"PrintTime", time},
        {"NozzleDiameter", 0},
        {"PrintBoundingBox", nullptr},
        {"Pauses", nullptr},
        {"MaterialUsages", nullptr}};
    const auto resin_text = resin.dump(2), user_text = user.dump(2), metadata_text = metadata.dump(2);
    Zipper zip(fname);
    zip.add_entry("ResinMetadata");
    zip << resin_text;
    zip.add_entry("UserSettingsData");
    zip << user_text;
    zip.add_entry("ZCodeMetadata");
    zip << metadata_text;
    zip.add_entry("ResinGCodeData");
    zip << protocol.str();
    for (size_t i = 0; i < m_layers.size(); ++i) {
        std::ostringstream name;
        name << "ResinSlicesData/Slice" << std::setw(5) << std::setfill('0') << i << ".png";
        zip.add_entry(name.str(), m_layers[i].data(), m_layers[i].size());
    }
    // Renderer FLT image, with aspect ratio preserved and a black letterbox.
    for (const auto &thumb : thumbnails) {
        if (!thumb.is_valid() || thumb.width > 4096 || thumb.height > 4096)
            continue;
        std::vector<uint8_t> rgba(320 * 180 * 4, 0);
        for (size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 255;
        const double scale = std::min(320. / thumb.width, 180. / thumb.height);
        const size_t w = std::max(size_t(1), size_t(std::round(thumb.width * scale)));
        const size_t h = std::max(size_t(1), size_t(std::round(thumb.height * scale)));
        for (size_t y = 0; y < h; ++y)
            for (size_t x = 0; x < w; ++x) {
                const auto *src = thumb.pixels.data() +
                                  ((thumb.height - 1 - y * thumb.height / h) * thumb.width + x * thumb.width / w) * 4;
                std::copy_n(src, 4, rgba.data() + ((y + (180 - h) / 2) * 320 + x + (320 - w) / 2) * 4);
            }
        const auto preview = sla::PNGRasterEncoder{}(rgba.data(), 320, 180, 4);
        zip.add_entry("Preview.png", preview.data(), preview.size());
        break;
    }
    zip.finalize();
}
} // namespace Slic3r
