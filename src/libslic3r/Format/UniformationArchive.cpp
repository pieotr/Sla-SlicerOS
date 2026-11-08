// Native Uniformation JXS port from local UVtools JXSFile.cs (AGPLv3).
// JSON contains the printer's SLA motion/exposure protocol. It is never
// exposed as an FFF toolpath or sent to an external process.
#include "UniformationArchive.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
#include "libslic3r/Zipper.hpp"
#include <cstring>
#include <iomanip>
#include <locale>
#include <nlohmann/json.hpp>
#include <sstream>

namespace Slic3r {
namespace {
using Json = nlohmann::json;
constexpr size_t cols = 4920, rows = 2880;
uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t pixel_count(const sla::EncodedRaster &mask) {
    const auto *p = static_cast<const uint8_t *>(mask.data());
    if (mask.size() < 41 || std::memcmp(p + 8, "\x89PNG\r\n\x1a\n", 8) || be32(p + 16) != 13 ||
        std::memcmp(p + 20, "IHDR", 4) || be32(p + 24) != cols / 3 || be32(p + 28) != rows || p[32] != 8 || p[33] != 2)
        throw ExportError("Invalid JXS packed RGB raster");
    uint64_t count = 0;
    for (size_t i = 0; i < 8; ++i)
        count = (count << 8) | p[i];
    if (count > cols * rows)
        throw ExportError("JXS occupied pixel count exceeds screen size");
    return count;
}
uint32_t milliseconds(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds * 1000 > UINT32_MAX)
        throw ExportError("JXS delay exceeds uint32 milliseconds");
    return uint32_t(std::llround(seconds * 1000));
}
std::string number(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(3) << value;
    std::string text = out.str();
    while (text.back() == '0')
        text.pop_back();
    if (text.back() == '.')
        text.pop_back();
    return text;
}
// UVtools LightOffDelayC.CalculateSecondsLiftOnly uses a 0.75 s margin
// before the sequence and inside each active stage, not a time multiplier.
float synchronization(float height, float speed, float height2 = 0, float speed2 = 0, float extra = .75f) {
    float seconds = extra;
    for (const auto &stage : {std::pair{height, speed}, std::pair{height2, speed2}})
        if (stage.first > 0) {
            if (stage.second <= 0)
                throw ExportError("JXS active movement requires a positive speed");
            seconds += std::nearbyint((stage.first / (stage.second / 60.f) + extra) * 100.f) / 100.f;
        }
    return seconds;
}
bool precise(float value, double scale) {
    return std::isfinite(value) && value == float(std::round(double(value) * scale) / scale);
}
} // namespace
std::unique_ptr<sla::RasterBase> UniformationGKoneArchive::create_raster() const {
    return sla::create_printer_raster(m_cfg);
}
sla::RasterEncoder UniformationGKoneArchive::get_encoder() const {
    return [](const void *ptr, size_t width, size_t height, size_t components) {
        if (!ptr || width != cols || height != rows || components != 1)
            throw ExportError("JXS requires a 4920x2880 grayscale source");
        const auto *pixels = static_cast<const uint8_t *>(ptr);
        const uint64_t count = std::count_if(pixels, pixels + width * height, [](uint8_t p) { return p != 0; });
        // RGB channels retain three adjacent 8-bit grayscale samples, without
        // color/luminance conversion. UVtools reshapes them back to one channel.
        const auto png = sla::PNGRasterEncoder{}(ptr, width / 3, height, 3);
        std::vector<uint8_t> packet;
        packet.reserve(8 + png.size());
        for (int shift = 56; shift >= 0; shift -= 8)
            packet.push_back(uint8_t(count >> shift));
        const auto *bytes = static_cast<const uint8_t *>(png.data());
        packet.insert(packet.end(), bytes, bytes + png.size());
        return sla::EncodedRaster(std::move(packet), "jxs");
    };
}
void UniformationGKoneArchive::validate_configuration(const SLAPrint &print) {
    const auto &cfg = print.printer_config();
    const bool portrait = cfg.display_orientation.value == sladoPortrait;
    if ((portrait ? cfg.display_pixels_y.value : cfg.display_pixels_x.value) != int(cols) ||
        (portrait ? cfg.display_pixels_x.value : cfg.display_pixels_y.value) != int(rows) ||
        (portrait ? cfg.display_height.value : cfg.display_width.value) != 221.4 ||
        (portrait ? cfg.display_width.value : cfg.display_height.value) != 129.6 || cfg.max_print_height.value != 245)
        throw ExportError("JXS requires Uniformation GKone: 4920x2880, 221.4x129.6 mm, Z=245 mm");
    if (cfg.display_mirror_x.value || !cfg.display_mirror_y.value)
        throw ExportError("JXS requires vertical mirroring and no horizontal mirroring");
    const auto &mat = print.material_config();
    if (mat.sla_bottom_layers.value < 0 || mat.sla_bottom_layers.value > UINT16_MAX ||
        mat.sla_transition_layers.value < 0)
        throw ExportError("JXS bottom/transition count is outside schema limits");
    if (!precise(float(print.default_object_config().layer_height.value), 1000) ||
        print.default_object_config().layer_height.value <= 0 || !precise(float(mat.exposure_time.value), 100) ||
        !precise(float(mat.initial_exposure_time.value), 100) || mat.exposure_time.value <= 0 ||
        mat.initial_exposure_time.value <= 0)
        throw ExportError("JXS requires positive height at 0.001 mm precision and exposures at 0.01 s precision");
    milliseconds(mat.exposure_time.value);
    milliseconds(mat.initial_exposure_time.value);
    for (bool bottom : {false, true}) {
        for (const char *key :
             {"lift_height", "lift_height2", "lift_speed", "lift_speed2", "retract_height2", "retract_speed",
              "retract_speed2", "wait_before_cure", "wait_after_cure", "wait_after_lift"}) {
            const float v = sla::motion_parameter(print, key, bottom);
            if (v < 0 || !precise(v, 100))
                throw ExportError("JXS motion/rest parameters require finite nonnegative values at two decimals");
        }
        const float total =
            sla::motion_parameter(print, "lift_height", bottom) + sla::motion_parameter(print, "lift_height2", bottom);
        if (sla::motion_parameter(print, "retract_height2", bottom) > total)
            throw ExportError("JXS retract exceeds total lift");
        const float pwm = sla::motion_parameter(print, "light_pwm", bottom);
        if (pwm < 0 || pwm > 255)
            throw ExportError("JXS PWM exceeds uint8");
    }
}
void UniformationGKoneArchive::export_print(const std::string fname, const SLAPrint &print, const ThumbnailsList &,
                                            const std::string &projectname) {
    validate_configuration(print);
    if (m_layers.empty() || m_layers.size() != print.print_layers().size() || m_layers.size() > UINT32_MAX)
        throw ExportError("JXS requires a complete nonempty layer stack");
    Json actions = Json::array();
    auto command = [&](const std::string &text) { actions.push_back(Json::array({"gcode", text})); };
    auto delay = [&](double seconds) {
        const auto ms = milliseconds(seconds);
        if (ms)
            actions.push_back(Json::array({"delay", ms}));
    };
    auto move = [&](double delta, double speed) {
        if (delta == 0)
            return;
        if (!std::isfinite(speed) || speed <= 0)
            throw ExportError("JXS active movement requires a positive speed");
        command("G0 Z" + number(delta) + " F" + number(speed));
    };
    // Leading-zero G4 is the protocol's synchronization wait, excluded from
    // exposure/rest by UVtools. Keep even zero markers to disambiguate waits.
    auto sync = [&](double seconds) {
        const auto ms = milliseconds(seconds);
        if (ms)
            actions.push_back(Json::array({"delay", ms}));
        else
            command("G4 P00");
    };
    for (const char *text : {"G21", "G91", "M106 S0"})
        command(text);
    actions.push_back(Json::array({"slice", "<BLANK>"}));
    command("M17");
    command("G28 Z0");
    double last_z = 0, volume = 0;
    const auto bottom = sla::bottom_layers(print, m_layers.size());
    for (size_t i = 0; i < m_layers.size(); ++i) {
        const double z = sla::layer_z(print, i);
        const uint64_t pixels = pixel_count(m_layers[i]);
        if (!precise(float(z), 1000) || z <= last_z || z > 245)
            throw ExportError("JXS requires increasing Z at 0.001 mm precision");
        volume += pixels * .045 * .045 * (z - last_z) / 1000.;
        auto p = [&](const char *key) { return double(sla::motion_parameter(print, key, i < bottom)); };
        const double lift = p("lift_height"), lift2 = p("lift_height2"), retract2 = p("retract_height2"),
                     total = lift + lift2;
        if (z + total > 245)
            throw ExportError("JXS lift movement exceeds GKone machine height");
        std::ostringstream name;
        name << std::setw(5) << std::setfill('0') << i << ".png";
        actions.push_back(Json::array({"layerno", i + 1}));
        actions.push_back(Json::array({"slice", "{PWD}/" + name.str()}));
        if (lift > 0) {
            move(z - last_z + lift, p("lift_speed"));
            if (lift2 > 0)
                move(lift2, p("lift_speed2"));
        } else
            move(z - last_z + lift2, lift2 > 0 ? p("lift_speed2") : p("lift_speed"));
        sync(total > 0 ? synchronization(float(lift), float(p("lift_speed")), float(lift2), float(p("lift_speed2")))
                       : 0);
        if (total > 0)
            delay(p("wait_after_lift"));
        move(-(total - retract2), p("retract_speed"));
        move(-retract2, p("retract_speed2"));
        sync(total > 0 ? synchronization(float(total - retract2), float(p("retract_speed")), float(retract2),
                                         float(p("retract_speed2")))
                       : 0);
        if (p("wait_before_cure") > 0)
            delay(p("wait_before_cure"));
        else
            command("G4 P.0"); // Zero rest, without the leading-zero sync marker.
        command("M106 S" + std::to_string(int(p("light_pwm"))));
        delay(std::nearbyint(sla::layer_exposure(print, i) * 100.f) / 100.f);
        command("M106 S0");
        actions.push_back(Json::array({"slice", "<BLANK>"}));
        delay(p("wait_after_cure"));
        last_z = z;
    }
    command("M106 S0");
    actions.push_back(Json::array({"slice", "<BLANK>"}));
    double maximum_speed = 0;
    for (bool is_bottom : {false, true})
        for (const char *key : {"lift_speed", "lift_speed2", "retract_speed", "retract_speed2"})
            maximum_speed = std::max(maximum_speed, double(sla::motion_parameter(print, key, is_bottom)));
    const double normal_speed = sla::motion_parameter(print, "lift_speed", false);
    const double park_speed = normal_speed > 0 ? normal_speed : maximum_speed;
    const double first_raise = std::min(
        245 - last_z, std::max(4., double(sla::motion_parameter(print, "lift_height", m_layers.size() - 1 < bottom))));
    move(first_raise, park_speed);
    if (first_raise > 0)
        sync(synchronization(float(sla::motion_parameter(print, "lift_height", m_layers.size() - 1 < bottom)),
                             float(sla::motion_parameter(print, "lift_speed", m_layers.size() - 1 < bottom)),
                             float(sla::motion_parameter(print, "lift_height2", m_layers.size() - 1 < bottom)),
                             float(sla::motion_parameter(print, "lift_speed2", m_layers.size() - 1 < bottom))));
    const double final_raise = 245 - last_z - first_raise;
    move(final_raise, maximum_speed);
    if (final_raise > 0)
        sync(synchronization(float(final_raise + first_raise), float(maximum_speed), 0, 0, 0));
    const std::string footer_move = final_raise > 0 ? "G0 Z" + number(final_raise) + " F" + number(maximum_speed)
                                                    : "G0 Z" + number(first_raise) + " F" + number(park_speed);
    command("M18");
    const auto &mat = print.material_config();
    Json control{{"total_time", sla::lcd_print_time(print)},
                 {"total_slices", m_layers.size()},
                 {"used_material", float(volume)},
                 {"action_list", actions}};
    const auto serialized = control.dump(2);
    std::ostringstream ini;
    ini.imbue(std::locale::classic());
    ini << std::setprecision(9);
    auto field = [&](const char *key, const auto &value) { ini << key << '=' << value << '\n'; };
    field("Action", "print");
    field("FirstLayerTime", milliseconds(mat.initial_exposure_time.value));
    field("LayerTime", milliseconds(mat.exposure_time.value));
    field("NumFade", mat.sla_transition_layers.value);
    field("NumLayers", m_layers.size());
    field("UsedMaterial", float(volume));
    field("layerHeight", print.default_object_config().layer_height.value);
    std::string job_name = projectname.empty() ? "sla_job" : projectname;
    for (char &c : job_name)
        if (c == '\r' || c == '\n')
            c = ' ';
    field("nJobName", job_name);
    field("NumBottomLayers", bottom);
    field("LiftDistance1", sla::motion_parameter(print, "lift_height", false));
    field("LiftFeedrate1", sla::motion_parameter(print, "lift_speed", false));
    field("LiftDistance2", sla::motion_parameter(print, "lift_height2", false));
    field("LiftFeedrate2", sla::motion_parameter(print, "lift_speed2", false));
    field("BottomLiftFeedrate", sla::motion_parameter(print, "lift_speed", true));
    field("RetractFeedrate", sla::motion_parameter(print, "retract_speed", false));
    field("ZMoveTimeCompensation", milliseconds(sla::motion_parameter(print, "wait_before_cure", false)));
    field("ZMoveTimeCompensationBottom", milliseconds(sla::motion_parameter(print, "wait_before_cure", true)));
    field("OnLightCode", "M106 S255");
    field("OffLightCode", "M106 S0");
    field("GcodeHeader", "G21|G91|M106 S0|M17|G28 Z0");
    field("GcodeFooter", footer_move);
    // JXS uses this prefix as a parser compatibility marker. Without it,
    // UVtools reapplies bottom rest to transition layers after reading the
    // correct per-layer actions. State compatibility and actual provenance.
    field("ApplicationName", "UVtools-compatible native Sla-SlicerOS");
    Zipper zip(fname);
    zip.add_entry("config.ini");
    zip << ini.str();
    zip.add_entry("control.json");
    zip << serialized;
    for (size_t i = 0; i < m_layers.size(); ++i) {
        std::ostringstream name;
        name << std::setw(5) << std::setfill('0') << i << ".png";
        zip.add_entry(name.str(), static_cast<const uint8_t *>(m_layers[i].data()) + 8, m_layers[i].size() - 8);
    }
    zip.finalize();
}
} // namespace Slic3r
