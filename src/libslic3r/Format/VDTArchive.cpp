// Native port of the local UVtools VDTFile.cs JSON schema, AGPLv3 or later.
#include "VoxeldanceArchive.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include "libslic3r/Zipper.hpp"
#include <cstring>
#include <ctime>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

namespace Slic3r {
namespace {
// UVtools Layer setters round heights to 0.001 mm and motion/waits to
// two decimal places. Reject values that its decoder would silently alter.
bool representable(float value, double scale) { return value == float(std::round(double(value) * scale) / scale); }
using Json = nlohmann::json;
// Map app units directly: speeds are mm/min, distances mm, waits seconds.
Json parameters(const SLAPrint &print, bool bottom) {
    Json result{{"light_off_delay", 0}};
    for (const auto &key : {"wait_before_cure", "wait_after_cure", "wait_after_lift"})
        result[std::string("wait_time_") + std::string(key).substr(5)] = sla::motion_parameter(print, key, bottom);
    for (const auto &pair : {std::pair{"lift_distance", "lift_height"},
                             {"lift_speed", "lift_speed"},
                             {"lift_distance2", "lift_height2"},
                             {"lift_speed2", "lift_speed2"},
                             {"retract_speed", "retract_speed"},
                             {"retract_distance2", "retract_height2"},
                             {"retract_speed2", "retract_speed2"}})
        result[pair.first] = sla::motion_parameter(print, pair.second, bottom);
    return result;
}
} // namespace
void VoxeldanceTangoArchive::validate_configuration(const SLAPrint &print) {
    const auto &cfg = print.printer_config();
    if (cfg.display_pixels_x.value < 1 || cfg.display_pixels_y.value < 1 || cfg.display_pixels_x.value > 32768 ||
        cfg.display_pixels_y.value > 32768 ||
        uint64_t(cfg.display_pixels_x.value) * cfg.display_pixels_y.value > (uint64_t(128) << 20) ||
        !std::isfinite(cfg.display_width.value) || !std::isfinite(cfg.display_height.value) ||
        cfg.display_width.value <= 0 || cfg.display_height.value <= 0 || cfg.max_print_height.value <= 0)
        throw ExportError("VDT requires positive screen geometry within raster limits");
    const auto &material = print.material_config();
    if (material.sla_bottom_layers.value < 0 || material.sla_bottom_layers.value > UINT16_MAX ||
        material.sla_transition_layers.value < 0 || material.sla_transition_layers.value > UINT16_MAX)
        throw ExportError("VDT bottom/transition counts exceed uint16");
    for (const auto value : {material.exposure_time.value, material.initial_exposure_time.value,
                             print.default_object_config().layer_height.value, material.initial_layer_height.value,
                             material.material_density.value})
        if (!std::isfinite(value) || value <= 0)
            throw ExportError("VDT requires finite positive heights, exposures and resin density");
    if (!representable(float(material.exposure_time.value), 100) ||
        !representable(float(material.initial_exposure_time.value), 100))
        throw ExportError("VDT configured exposures require at most two decimal places");
    for (const auto value : {material.bottle_cost.value, material.bottle_volume.value})
        if (!std::isfinite(value) || value < 0)
            throw ExportError("VDT requires finite nonnegative resin bottle cost/volume");
    if (!std::isfinite(cfg.max_print_height.value))
        throw ExportError("VDT requires finite machine height");
    for (bool bottom : {false, true}) {
        const auto motion = parameters(print, bottom);
        for (const auto &item : motion.items())
            if (!std::isfinite(item.value().get<float>()) || item.value().get<float>() < 0 ||
                !representable(item.value().get<float>(), 100))
                throw ExportError("VDT requires finite nonnegative motion/waits with at most two decimal places");
        const float lift = sla::motion_parameter(print, "lift_height", bottom);
        const float lift2 = sla::motion_parameter(print, "lift_height2", bottom);
        const float retract2 = sla::motion_parameter(print, "retract_height2", bottom);
        if (retract2 > lift + lift2)
            throw ExportError("VDT secondary retract exceeds total lift height");
        const auto pwm = sla::motion_parameter(print, "light_pwm", bottom);
        if (pwm < 0 || pwm > 255)
            throw ExportError("VDT UV PWM exceeds uint8");
        for (const auto &move : {std::pair{lift, "lift_speed"},
                                 {lift2, "lift_speed2"},
                                 {lift + lift2 - retract2, "retract_speed"},
                                 {retract2, "retract_speed2"}})
            if (move.first > 0 && sla::motion_parameter(print, move.second, bottom) <= 0)
                throw ExportError("VDT requires positive speed for every active move");
    }
}
void VoxeldanceTangoArchive::export_print(const std::string fname, const SLAPrint &print, const ThumbnailsList &thumbnails,
                                     const std::string &projectname) {
    validate_configuration(print);
    if (m_layers.empty() || m_layers.size() != print.print_layers().size() || m_layers.size() > UINT32_MAX)
        throw ExportError("VDT requires a complete nonempty layer stack");
    const bool portrait = m_cfg.display_orientation.value == sladoPortrait;
    const int width = portrait ? m_cfg.display_pixels_y.value : m_cfg.display_pixels_x.value;
    const int height = portrait ? m_cfg.display_pixels_x.value : m_cfg.display_pixels_y.value;
    const double width_mm = portrait ? m_cfg.display_height.value : m_cfg.display_width.value;
    const double height_mm = portrait ? m_cfg.display_width.value : m_cfg.display_height.value;
    const auto &material = print.material_config();
    Json settings = parameters(print, false);
    settings["exposure_time"] = float(material.exposure_time.value);
    const auto bottom = parameters(print, true);
    for (const auto &item : bottom.items())
        settings["bottom_" + item.key()] = item.value();
    settings["bottom_exposure_time"] = float(material.initial_exposure_time.value);
    settings["layer_thickness"] = float(print.default_object_config().layer_height.value);
    settings["bottom_layers"] = sla::bottom_layers(print, m_layers.size());
    settings["transition_layer"] = material.sla_transition_layers.value;
    Json layers = Json::array();
    double previous_z = 0, volume = 0;
    size_t previous_pixels = 0;
    for (size_t i = 0; i < m_layers.size(); ++i) {
        const float z = sla::layer_z(print, i);
        if (!std::isfinite(z) || z <= previous_z || z > m_cfg.max_print_height.value || !representable(z, 1000))
            throw ExportError("VDT requires increasing layer Z within machine height at 0.001 mm precision");
        const auto &mask = m_layers[i];
        size_t pixels = previous_pixels;
        const bool repeated = i && mask.size() == m_layers[i - 1].size() &&
                              std::memcmp(mask.data(), m_layers[i - 1].data(), mask.size()) == 0;
        // Repeated masks need no PNG decompression or full-screen statistics
        // scan. Reuse pixel count, while applying this layer's own Z thickness.
        if (!repeated) {
            png::ImageGreyscale image;
            if (!png::decode_png(png::ReadBuf{mask.data(), mask.size()}, image) || image.cols != size_t(width) ||
                image.rows != size_t(height))
                throw ExportError("VDT PNG dimensions differ from printer screen");
            // UVtools measures occupied pixels (all nonzero grayscale values).
            pixels = std::count_if(image.buf.begin(), image.buf.end(), [](uint8_t p) { return p != 0; });
        }
        previous_pixels = pixels;
        volume += pixels * width_mm / width * height_mm / height * (z - previous_z) / 1000.;
        previous_z = z;
        const bool is_bottom = i < sla::bottom_layers(print, m_layers.size());
        auto layer = parameters(print, is_bottom);
        layer["height"] = z;
        // Transition interpolation can produce more digits than UVtools can
        // represent. Apply its native two-decimal rounding to generated values.
        layer["exposure_time"] = std::nearbyint(sla::layer_exposure(print, i) * 100.f) / 100.f;
        layer["light_pwm"] = int(sla::motion_parameter(print, "light_pwm", is_bottom));
        layers.push_back(std::move(layer));
    }
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream timestamp;
    timestamp << std::put_time(&utc, "%Y-%m-%d %H:%M:%SZ");
    const double density = material.material_density.value;
    const double cost =
        material.bottle_volume.value > 0 ? material.bottle_cost.value / material.bottle_volume.value : 0.;
    Json manifest{{"application_name", "Sla-SlicerOS"},
                  {"application_version", "1"},
                  {"create_datetime", timestamp.str()},
                  {"file_version", 1},
                  {"project_name", projectname},
                  {"machine",
                   {{"name", m_cfg.printer_model.value},
                    {"type", "Default"},
                    {"lcd_width", float(width_mm)},
                    {"lcd_height", float(height_mm)},
                    {"z_height", float(m_cfg.max_print_height.value)},
                    {"resolution_x", width},
                    {"resolution_y", height},
                    {"x_mirror", m_cfg.display_mirror_x.value},
                    {"y_mirror", m_cfg.display_mirror_y.value},
                    {"notes", ""},
                    {"uvtools_convert_to", ""}}},
                  {"advanced_parameters",
                   {{"antialasing_level", 16},
                    {"grey_level", 0},
                    {"image_blur_level", 0},
                    {"bottom_light_pwm", int(sla::motion_parameter(print, "light_pwm", true))},
                    {"light_pwm", int(sla::motion_parameter(print, "light_pwm", false))},
                    {"beam_compensation", 0}}},
                  {"resin", {{"name", ""}, {"cost", float(volume * cost)}, {"density", float(density)}, {"notes", ""}}},
                  {"print", settings},
                  {"layers", layers},
                  {"print_statistics",
                   {{"price", float(volume * cost)},
                    {"price_currency", "EUR"},
                    {"estimated_time", sla::lcd_print_time(print)},
                    {"volume", float(volume)},
                    {"weight", float(volume * density)}}}};
    // nlohmann serializes NaN/Inf as null. Never let overflow of a float
    // field produce a superficially valid manifest with missing parameters.
    const auto finite_json = [](const auto &self, const Json &value) -> bool {
        if (value.is_number_float()) return std::isfinite(value.get<double>());
        if (value.is_object() || value.is_array())
            for (const auto &child : value) if (!self(self, child)) return false;
        return true;
    };
    if (!finite_json(finite_json, manifest))
        throw ExportError("VDT manifest contains a nonfinite numeric field");
    const auto serialized = manifest.dump(2); // Reject non-serializable values before opening destination.
    Zipper zip(fname);
    zip.add_entry("manifest.json");
    zip << serialized;
    for (size_t i = 0; i < m_layers.size(); ++i)
        zip.add_entry(std::to_string(i) + ".png", m_layers[i].data(), m_layers[i].size());
    // The supplied renderer image is the FLT view; do not mislabel it as five
    // different camera views. Missing orthographic views remain a UI task.
    for (const auto &thumbnail : thumbnails) {
        if (!thumbnail.is_valid() || thumbnail.width > 4096 || thumbnail.height > 4096)
            continue;
        for (size_t size : {size_t(200), size_t(48)}) {
            std::vector<uint8_t> pixels(size * size * 4, 0);
            for (size_t i = 3; i < pixels.size(); i += 4)
                pixels[i] = 255;
            const double scale = std::min(double(size) / thumbnail.width, double(size) / thumbnail.height);
            const size_t target_width = std::max(size_t(1), size_t(std::round(thumbnail.width * scale)));
            const size_t target_height = std::max(size_t(1), size_t(std::round(thumbnail.height * scale)));
            const size_t offset_x = (size - target_width) / 2, offset_y = (size - target_height) / 2;
            for (size_t y = 0; y < target_height; ++y)
                for (size_t x = 0; x < target_width; ++x) {
                    const size_t sx = x * thumbnail.width / target_width;
                    const size_t sy = thumbnail.height - 1 - y * thumbnail.height / target_height;
                    std::copy_n(thumbnail.pixels.data() + (sy * thumbnail.width + sx) * 4, 4,
                                pixels.data() + ((y + offset_y) * size + x + offset_x) * 4);
                }
            const auto png = sla::PNGRasterEncoder{}(pixels.data(), size, size, 4);
            zip.add_entry(size == 200 ? "Preview_FLT.png" : "Preview_FLT_48.png", png.data(), png.size());
        }
        break;
    }
    zip.finalize();
}
} // namespace Slic3r
