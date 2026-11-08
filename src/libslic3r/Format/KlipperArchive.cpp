// Native port of the local UVtools KlipperFile/GCodeBuilder contract.
// These commands control an MSLA exposure cycle; no FFF emitter is used.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "KlipperArchive.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <locale>
#include <sstream>
#include <nlohmann/json.hpp>

namespace Slic3r {
namespace {
bool precise(double value, double scale) {
    return std::isfinite(value) && std::abs(value - std::round(value * scale) / scale) < 1e-6;
}
double layer_z(const SLAPrint &print, size_t index) {
    // Keep the slicer's fixed-point Z in double precision. Converting through
    // float loses more than the validation tolerance on ordinary tall models
    // and can reject a perfectly representable 0.001 mm position.
    return double(print.print_layers()[index].level()) * SCALING_FACTOR;
}
void check_text(const std::string &value) {
    // KlipperFile's header reader splits at colons; preserve text faithfully
    // instead of silently truncating a user-provided name or adding commands.
    if (value.find_first_of(":\r\n") != std::string::npos)
        throw ExportError("Klipper machine/material names cannot contain a colon or a line break");
}
std::string material_name(const SLAPrint &print) {
    // Preset IDs are kept in the placeholder context, not the filtered
    // full_print_config used for slicing. Preserve the selected preset name.
    const auto &config = print.placeholder_parser().config();
    return config.has("material_preset") ? config.opt_string("material_preset") : std::string{};
}
struct MaskStats { uint64_t pixels = 0; size_t left = 0, top = 0, right = 0, bottom = 0; };
MaskStats mask_stats(const sla::EncodedRaster &mask, size_t columns, size_t rows) {
    png::ImageGreyscale image;
    if (!png::decode_color_png({mask.data(), mask.size()}, image) || image.cols != columns || image.rows != rows)
        throw ExportError("Klipper mask dimensions differ from the printer screen");
    MaskStats result;
    result.left = columns; result.top = rows;
    for (size_t y = 0; y < rows; ++y) for (size_t x = 0; x < columns; ++x)
        if (image.buf[y * columns + x]) {
            ++result.pixels;
            result.left = std::min(result.left, x); result.top = std::min(result.top, y);
            result.right = std::max(result.right, x + 1); result.bottom = std::max(result.bottom, y + 1);
        }
    if (!result.pixels) result.left = result.top = 0;
    return result;
}
void merge(MaskStats &total, const MaskStats &layer) {
    if (!layer.pixels) return;
    if (!total.pixels) { total = layer; return; }
    total.pixels += layer.pixels;
    total.left = std::min(total.left, layer.left); total.top = std::min(total.top, layer.top);
    total.right = std::max(total.right, layer.right); total.bottom = std::max(total.bottom, layer.bottom);
}
}
std::unique_ptr<sla::RasterBase> KlipperArchive::create_raster() const {
    return sla::create_printer_raster(m_cfg);
}
sla::RasterEncoder KlipperArchive::get_encoder() const {
    return [rgb = m_rgb](const void *ptr, size_t width, size_t height, size_t components) {
        if (!ptr || components != 1 || !width || !height || width > 32768 || height > 32768 ||
            uint64_t(width) * height > 128ULL * 1024 * 1024)
            throw ExportError("Invalid Klipper grayscale raster");
        if (!rgb) return sla::PNGRasterEncoder{}(ptr, width, height, 1);
        // Original .rgb.zip uses full-width PNG24, not packed RGB AA.
        const auto *gray = static_cast<const uint8_t *>(ptr);
        std::vector<uint8_t> pixels(width * height * 3);
        for (size_t i = 0; i < width * height; ++i)
            pixels[3 * i] = pixels[3 * i + 1] = pixels[3 * i + 2] = gray[i];
        return sla::PNGRasterEncoder{}(pixels.data(), width, height, 3);
    };
}
void KlipperArchive::validate_configuration(const SLAPrint &print) {
    const auto &cfg = print.printer_config();
    if (cfg.display_pixels_x.value < 1 || cfg.display_pixels_y.value < 1 ||
        cfg.display_pixels_x.value > 32768 || cfg.display_pixels_y.value > 32768 ||
        uint64_t(cfg.display_pixels_x.value) * cfg.display_pixels_y.value > (128ULL << 20) ||
        cfg.display_width.value <= 0 || cfg.display_height.value <= 0 || cfg.max_print_height.value <= 0 ||
        !precise(cfg.display_width.value, 100) || !precise(cfg.display_height.value, 100) ||
        !precise(cfg.max_print_height.value, 100))
        throw ExportError("Klipper requires positive finite screen geometry at 0.01 mm precision");
    const auto &mat = print.material_config();
    if (mat.sla_bottom_layers.value < 0 || mat.sla_bottom_layers.value > UINT16_MAX ||
        mat.sla_transition_layers.value < 0 || mat.sla_transition_layers.value > UINT16_MAX)
        throw ExportError("Klipper bottom/transition counts exceed 16-bit metadata");
    for (double value : {mat.initial_exposure_time.value, mat.exposure_time.value})
        if (value <= 0 || !precise(value, 100) || value * 1000 > UINT32_MAX)
            throw ExportError("Klipper exposure must be positive, at 0.01 s precision and within 32-bit milliseconds");
    for (double value : {mat.initial_layer_height.value, print.default_object_config().layer_height.value})
        if (value <= 0 || !precise(value, 1000))
            throw ExportError("Klipper layer heights require positive values at 0.001 mm precision");
    for (bool bottom : {false, true}) {
        for (const char *key : {"lift_height", "lift_height2", "lift_speed", "lift_speed2", "retract_height2",
                               "retract_speed", "retract_speed2", "wait_before_cure", "wait_after_cure", "wait_after_lift"}) {
            const double value = sla::motion_parameter(print, key, bottom);
            if (value < 0 || !precise(value, 100) || value * 1000 > UINT32_MAX)
                throw ExportError(std::string("Klipper requires nonnegative finite ") + key + " at 0.01 precision");
        }
        auto p = [&](const char *key) { return sla::motion_parameter(print, key, bottom); };
        if (p("lift_height") <= 0 || p("lift_speed") <= 0 ||
            (p("lift_height2") > 0 && p("lift_speed2") <= 0) || p("retract_speed") <= 0 ||
            (p("retract_height2") > 0 && p("retract_speed2") <= 0) ||
            p("retract_height2") >= p("lift_height") + p("lift_height2"))
            throw ExportError("Klipper requires a positive first lift and retract stage and positive enabled speeds");
        // The original parser cannot retain an exposure duration with PWM=0.
        if (p("light_pwm") <= 0 || p("light_pwm") > 255)
            throw ExportError("Klipper requires UV PWM 1–255 for a representable timed exposure");
    }
    if (!std::isfinite(mat.material_density.value) || mat.material_density.value <= 0 ||
        !std::isfinite(mat.bottle_volume.value) || mat.bottle_volume.value <= 0 ||
        !std::isfinite(mat.bottle_cost.value) || mat.bottle_cost.value < 0)
        throw ExportError("Klipper requires valid resin density, bottle volume and cost");
    check_text(cfg.printer_model.value);
    check_text(material_name(print));
}
void KlipperArchive::export_print(const std::string filename, const SLAPrint &print,
                                  const ThumbnailsList &thumbnails, const std::string &projectname) {
    validate_configuration(print);
    if (m_layers.empty() || m_layers.size() != print.print_layers().size() || m_layers.size() > UINT32_MAX)
        throw ExportError("Klipper requires a nonempty complete layer stack");
    const bool portrait = m_cfg.display_orientation.value == sladoPortrait;
    const size_t columns = portrait ? m_cfg.display_pixels_y.value : m_cfg.display_pixels_x.value;
    const size_t rows = portrait ? m_cfg.display_pixels_x.value : m_cfg.display_pixels_y.value;
    const double width = portrait ? m_cfg.display_height.value : m_cfg.display_width.value;
    const double height = portrait ? m_cfg.display_width.value : m_cfg.display_height.value;
    const double pixel_x = width / columns, pixel_y = height / rows;
    const auto bottom_count = sla::bottom_layers(print, m_layers.size());
    const auto &mat = print.material_config();
    std::vector<MaskStats> statistics;
    statistics.reserve(m_layers.size());
    MaskStats bounds;
    double previous_z = 0, volume = 0, time = 0;
    for (size_t i = 0; i < m_layers.size(); ++i) {
        const double z = layer_z(print, i);
        if (z <= previous_z || z > m_cfg.max_print_height.value || !precise(z, 1000))
            throw ExportError("Klipper requires increasing Z within machine height at 0.001 mm precision");
        const bool bottom = i < bottom_count;
        if (z + sla::motion_parameter(print, "lift_height", bottom) +
                sla::motion_parameter(print, "lift_height2", bottom) > m_cfg.max_print_height.value)
            throw ExportError("Klipper lift would exceed the printer's maximum Z");
        const bool repeated = i && m_layers[i].size() == m_layers[i - 1].size() &&
            std::memcmp(m_layers[i].data(), m_layers[i - 1].data(), m_layers[i].size()) == 0;
        statistics.push_back(repeated ? statistics.back() : mask_stats(m_layers[i], columns, rows));
        merge(bounds, statistics.back());
        volume += statistics.back().pixels * pixel_x * pixel_y * (z - previous_z);
        time += sla::lcd_layer_time(print, i);
        previous_z = z;
    }
    const double ml = volume / 1000;
    using Json = nlohmann::json;
    Json header{{"FileName", projectname.empty() ? "job" : projectname}, {"MachineName", m_cfg.printer_model.value},
        {"MaterialName", material_name(print)},
        {"PrintTime", time}, {"Volume", volume}, {"MaterialMl", ml},
        {"MaterialGrams", ml * mat.material_density.value}, {"Price", ml * mat.bottle_cost.value / mat.bottle_volume.value},
        {"ResolutionX", columns}, {"ResolutionY", rows}, {"MachineX", width}, {"DisplayHeight", height},
        {"MachineZ", m_cfg.max_print_height.value}, {"LayerHeight", print.default_object_config().layer_height.value},
        {"LayerCount", m_layers.size()}, {"BottomLayerCount", bottom_count},
        {"TransitionLayerCount", std::min<size_t>(mat.sla_transition_layers.value, m_layers.size() - bottom_count)},
        {"Mirror", (m_cfg.display_mirror_x.value ? 1 : 0) + (m_cfg.display_mirror_y.value ? 2 : 0)}, {"AntiAliasing", 1},
        {"BottomExposureTime", mat.initial_exposure_time.value}, {"ExposureTime", mat.exposure_time.value}};
    for (bool bottom : {false, true}) {
        const std::string prefix = bottom ? "Bottom" : "";
        for (auto [name, key] : {std::pair{"WaitTimeBeforeCure", "wait_before_cure"}, {"WaitTimeAfterCure", "wait_after_cure"},
            {"WaitTimeAfterLift", "wait_after_lift"}, {"LiftHeight", "lift_height"}, {"LiftHeight2", "lift_height2"},
            {"LiftSpeed", "lift_speed"}, {"LiftSpeed2", "lift_speed2"}, {"RetractHeight2", "retract_height2"},
            {"RetractSpeed", "retract_speed"}, {"RetractSpeed2", "retract_speed2"}, {"LightPWM", "light_pwm"}})
            if (std::strcmp(key, "light_pwm") == 0)
                header[prefix + name] = int(sla::motion_parameter(print, key, bottom));
            else
                header[prefix + name] = sla::motion_parameter(print, key, bottom);
    }
    std::ostringstream script;
    script.imbue(std::locale::classic()); script << std::fixed << std::setprecision(6);
    for (const auto &field : header.items()) {
        script << ';' << field.key() << ':';
        if (field.value().is_string()) { const auto value = field.value().get<std::string>(); check_text(value); script << value; }
        else if (field.value().is_number_unsigned()) script << field.value().get<uint64_t>();
        else if (field.value().is_number_integer()) script << field.value().get<int64_t>();
        else script << field.value().get<double>();
        script << '\n';
    }
    auto rectangle = [&](const MaskStats &box) {
        script << box.left * pixel_x << ',' << box.top * pixel_y << ','
               << (box.right - box.left) * pixel_x << ',' << (box.bottom - box.top) * pixel_y;
    };
    auto wait = [&](double seconds) {
        // Valid Klipper numeric zero. Original UVtools treats a leading '0'
        // as a synchronization delay; .0 preserves a real zero before-cure
        // field so a later after-cure wait is classified correctly.
        script << "G4 P";
        if (seconds == 0) script << ".0";
        else script << uint32_t(std::llround(seconds * 1000));
        script << '\n';
    };
    script << "MSLA_DISPLAY_VALIDATE RESOLUTION=" << columns << ',' << rows << " PIXEL=" << pixel_x << ',' << pixel_y << '\n'
           << "SET_PRINT_STATS_INFO TOTAL_LAYER=" << m_layers.size() << " LAYER=1 MATERIAL_TOTAL=" << ml << '\n'
           << "PRINT_START LAYERS=" << m_layers.size() << " HEIGHT=" << previous_z << " BOUNDS=";
    rectangle(bounds); script << " VOLUME=" << volume << " MATERIALML=" << ml << "\nG90\nM1401\n";
    previous_z = 0;
    for (size_t i = 0; i < m_layers.size(); ++i) {
        const double z = layer_z(print, i), area = statistics[i].pixels * pixel_x * pixel_y;
        const bool bottom = i < bottom_count;
        auto p = [&](const char *key) { return double(sla::motion_parameter(print, key, bottom)); };
        const double lift1 = p("lift_height"), lift2 = p("lift_height2"), retract2 = p("retract_height2");
        const double layer_volume = area * (z - previous_z);
        script << ";LAYER_START: " << i << "\nSET_PRINT_STATS_INFO CURRENT_LAYER=" << i + 1
               << "\nBEFORE_LAYER_CHANGE LAYER=" << i + 1 << " HEIGHT=" << z << " PIXELS=" << statistics[i].pixels << " BOUNDS=";
        rectangle(statistics[i]);
        script << " AREA=" << area << " VOLUME=" << layer_volume << " MATERIALML=" << layer_volume / 1000
               << "\nM1451 F\"" << i + 1 << ".png\"\nG1 Z" << z + lift1 << " F" << p("lift_speed") << '\n';
        if (lift2 > 0) script << "G1 Z" << z + lift1 + lift2 << " F" << p("lift_speed2") << '\n';
        script << "M400\n"; wait(p("wait_after_lift"));
        script << "G1 Z" << z + retract2 << " F" << p("retract_speed") << '\n';
        if (retract2 > 0) script << "G1 Z" << z << " F" << p("retract_speed2") << '\n';
        script << "M400\n"; wait(p("wait_before_cure"));
        // UVtools rounds each derived transition exposure to 0.01 seconds.
        const double exposure = std::round(double(sla::layer_exposure(print, i)) * 100) / 100;
        script << "M1400 S" << int(p("light_pwm")) << " P" << uint32_t(std::llround(exposure * 1000)) << '\n';
        wait(p("wait_after_cure"));
        script << "M1450\nSET_PRINT_STATS_INFO CONSUME_MATERIAL=" << layer_volume / 1000
               << "\nAFTER_LAYER_CHANGE\n;LAYER_END\n";
        previous_z = z;
    }
    script << "M1401\nM1450\nPRINT_END\n";
    // Direct writer calls and scene/editor export share atomic publication.
    struct Staging { boost::filesystem::path path; ~Staging() { boost::system::error_code e; if (!path.empty()) boost::filesystem::remove(path, e); } }
        staging{boost::filesystem::path(filename).parent_path() / boost::filesystem::unique_path(".klipper-%%%%-%%%%-%%%%-%%%%.tmp")};
    {
        Zipper zip(staging.path.string(), Zipper::NO_COMPRESSION);
        // The stream API skips empty entries; the binary overload writes the
        // zero-byte marker that the original CanProcess probe requires.
        zip.add_entry(".klipper", "", 0);
        zip.add_entry("job.gcode"); zip << script.str();
        for (size_t i = 0; i < m_layers.size(); ++i)
            zip.add_entry(std::to_string(i + 1) + ".png", m_layers[i].data(), m_layers[i].size());
        for (size_t i = 0; i < std::min<size_t>(2, thumbnails.size()); ++i) {
            const auto &thumb = thumbnails[i]; if (!thumb.is_valid()) continue;
            std::vector<uint8_t> pixels(thumb.pixels.size());
            for (size_t y = 0; y < thumb.height; ++y)
                std::memcpy(pixels.data() + y * thumb.width * 4,
                            thumb.pixels.data() + (thumb.height - y - 1) * thumb.width * 4, thumb.width * 4);
            const auto png = sla::PNGRasterEncoder{}(pixels.data(), thumb.width, thumb.height, 4);
            zip.add_entry(i ? "preview_cropping.png" : "preview.png", png.data(), png.size());
        }
        zip.finalize();
    }
    if (const auto error = rename_file(staging.path.string(), filename))
        throw ExportError("Cannot replace Klipper ZIP: " + error.message());
    staging.path.clear();
}
}
