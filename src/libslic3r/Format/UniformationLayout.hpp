// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader-side layout of the Uniformation GKone JXS format. Port of the
// UVtools JXSFile.cs decode path; the writer is UniformationArchive.cpp.
//
// A JXS is a ZIP with config.ini (key=value, "NumLayers"), control.json (the
// printer's action list) and one "<nnnnn>.png" per layer numbered from zero.
// Every layer is an 8-bit RGB PNG one third as wide as the screen: the three
// channels of a pixel carry three neighbouring gray samples of the mask
// (UVtools "Png24RgbAA"), so no colour conversion may be applied.
//
// The masks are always editable. The per-layer print parameters live in the
// action list of control.json (UVtools JXSFile.cs PrintParameterPerLayerModifiers)
// and are editable when that list has the exact shape our exporter writes:
// every layer block is re-rendered and compared. Z is stored only as relative
// moves whose footer depends on the final height, so it stays read-only here.
// config.ini and control.json keep every other value byte for byte; only the
// print time and the material estimate follow an edit. control.json may use any key order, indent
// or final line break (it is rewritten only when it dumps back to its own bytes); a control.json
// without an action list (UVtools' own JXS writer stores its settings there) has no per-layer program.
#ifndef SLIC3R_UNIFORMATION_LAYOUT_HPP
#define SLIC3R_UNIFORMATION_LAYOUT_HPP

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <optional>
#include <locale>

#include "MemoryZip.hpp"
#include "NativeParameterLayout.hpp"
#include "PngHeaderProbe.hpp"
#include "libslic3r/SLA/ColorMask.hpp"
#include "libslic3r/SLA/RasterBase.hpp"

namespace Slic3r {

class UniformationLayout {
public:
    static bool is_format(const std::string &format) { return format == "jxs"; }
    // The SL1 family also has a config.ini, but always next to prusaslicer.ini.
    static bool recognizes(const std::set<std::string> &names)
    {
        return names.count("config.ini") && names.count("control.json") && !names.count("prusaslicer.ini");
    }

    std::string format = "jxs";
    // columns is the full mask width (three times the PNG width).
    size_t columns = 0, rows = 0, count = 0;

    explicit UniformationLayout(const MemoryZip &zip) : m_zip(zip)
    {
        const size_t declared = declared_layers(zip);
        std::map<size_t, std::string> images;
        for (const auto &name : zip.ordered_names()) {
            if (name.find('/') != std::string::npos || !boost::algorithm::iends_with(name, ".png")) continue;
            const auto stem = name.substr(0, name.size() - 4);
            if (stem.empty() || stem.size() > 7 || !std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; }))
                continue;
            if (!images.emplace(size_t(std::stoul(stem)), name).second)
                throw RuntimeError("JXS has two layer images numbered " + std::to_string(std::stoul(stem)));
        }
        if (images.empty()) throw RuntimeError("JXS contains no layer images");
        count = declared ? declared : images.rbegin()->first + 1;
        if (!count || count > 100000) throw RuntimeError("JXS layer count is zero or exceeds the editor limit");
        if (images.rbegin()->first >= count)
            throw RuntimeError("JXS layer image " + std::to_string(images.rbegin()->first) + " is beyond its declared " +
                               std::to_string(count) + " layers");
        m_names.reserve(count);
        for (size_t layer = 0; layer < count; ++layer) {
            const auto found = images.find(layer);
            if (found == images.end()) throw RuntimeError("JXS layer image " + std::to_string(layer) + " is missing");
            m_names.push_back(found->second);
        }
        const auto first = header_of(zip.extract(m_names.front(), image_limit), 0);
        columns = first.width * 3; rows = first.height;
        if (uint64_t(columns) * rows > 128ULL * 1024 * 1024) throw RuntimeError("JXS raster exceeds the editor memory budget");
    }

    const std::string &layer_entry(size_t layer) const { return m_names.at(layer); }

    std::vector<uint8_t> decode(size_t layer) const
    {
        const std::string where = "JXS layer " + std::to_string(layer + 1);
        const auto bytes = m_zip.extract(layer_entry(layer), image_limit);
        const auto header = header_of(bytes, layer);
        if (header.width * 3 != columns || header.height != rows) throw RuntimeError(where + " differs in size from layer 1");
        png::ImageGreyscale image;
        // The shared helper returns the RGB bytes of the packed PNG in file
        // order after a B/R swap, which is undone here.
        if (!sla::decode_bgr_packed_mask({bytes.data(), bytes.size()}, columns, rows, image))
            throw RuntimeError(where + " is not a valid packed RGB mask");
        for (size_t i = 0; i + 2 < image.buf.size(); i += 3) std::swap(image.buf[i], image.buf[i + 2]);
        return std::move(image.buf);
    }

    std::vector<uint8_t> encode(const std::vector<uint8_t> &pixels) const
    {
        const auto encoded = sla::PNGRasterEncoder{}(pixels.data(), columns / 3, rows, 3);
        if (!encoded.size()) throw RuntimeError("Cannot encode JXS layer image");
        const auto *data = static_cast<const uint8_t *>(encoded.data());
        return {data, data + encoded.size()};
    }


    // ---- Per-layer parameters -------------------------------------------------------------
    static sla::LayerParameterCapabilities parameter_capabilities()
    {
        using F = sla::LayerParameter;
        auto result = native_parameters::capabilities({F::Exposure, F::LiftDistance, F::LiftSpeed, F::LiftDistance2, F::LiftSpeed2,
            F::WaitAfterLift, F::RetractSpeed, F::RetractDistance2, F::RetractSpeed2, F::WaitBefore, F::WaitAfter, F::LightPWM});
        result.insert(result.begin(), native_parameters::capability(F::PositionZ, false));
        return result;
    }
    // Exposures are written at 0.01 s, everything else at three decimals (waits as whole milliseconds).
    static double canonical(sla::LayerParameter field, double value)
    {
        using F = sla::LayerParameter;
        if (field == F::LightPWM) return value; // a fractional PWM is refused by the whole-number rule, not rounded
        return native_parameters::round_decimals(value, field == F::Exposure ? 100. : 1000.);
    }
    static void check_storable(const native_parameters::Row &row)
    {
        using F = sla::LayerParameter;
        auto v = [&](F field) { return native_parameters::value_of(row, field); };
        const double lift = v(F::LiftDistance), lift2 = v(F::LiftDistance2), retract2 = v(F::RetractDistance2), total = lift + lift2;
        if (!pos(lift) && pos(lift2))
            throw RuntimeError("JXS stores a second lift only together with a first lift");
        if (!pos(lift2) && v(F::LiftSpeed2) != 0)
            throw RuntimeError("JXS stores lift speed 2 only together with a positive lift distance 2");
        if (!pos(retract2) && v(F::RetractSpeed2) != 0)
            throw RuntimeError("JXS stores retract speed 2 only together with a positive retract distance 2");
        if (pos(total) && pos(retract2) && !pos(total - retract2))
            throw RuntimeError("JXS cannot tell a retract equal to the whole lift from a plain retract; keep retract distance 2 below the total lift");
        if (!pos(total - retract2) && v(F::RetractSpeed) != 0)
            throw RuntimeError("JXS stores the retract speed only together with a positive retract of the lift");
        if (!pos(total) && v(F::LiftSpeed) <= 0)
            throw RuntimeError("JXS stores a Z advance that needs a positive lift speed");
        if (!pos(total) && v(F::WaitAfterLift) != 0)
            throw RuntimeError("JXS stores the wait after lift only together with a lift");
    }

    native_parameters::Table parameter_rows() const { return parse_program().rows; }
    double height_limit() const { return 0.; }
    size_t bottom_layers() const
    {
        const auto value = ini_value("NumBottomLayers");
        return value ? size_t(std::max(0., std::strtod(value->c_str(), nullptr))) : 0;
    }

    std::map<std::string, std::vector<uint8_t>> parameter_replacements(const native_parameters::Table &table,
        const std::vector<native_parameters::MaskSummary> &old_masks, const std::vector<native_parameters::MaskSummary> &new_masks) const
    {
        using F = sla::LayerParameter;
        if (table.size() != count || old_masks.size() != count || new_masks.size() != count)
            throw RuntimeError("JXS parameter table has the wrong layer count");
        auto program = parse_program();
        bool rows_changed = false, masks_changed = false;
        for (size_t i = 0; i < count; ++i) {
            check_storable(table[i]);
            if (native_parameters::value_of(table[i], F::PositionZ) != native_parameters::value_of(program.rows[i], F::PositionZ))
                throw RuntimeError("The JXS layer positions are read-only");
            if (table[i] != program.rows[i]) rows_changed = true;
            if (old_masks[i] != new_masks[i]) masks_changed = true;
        }
        if (!rows_changed && !masks_changed) return {};
        std::map<std::string, std::vector<uint8_t>> result;
        auto control = program.control;
        if (rows_changed) {
            Json actions = Json::array();
            for (size_t i = 0; i < program.blocks.front().first; ++i) actions.push_back(program.actions[i]);
            for (size_t i = 0; i < count; ++i) {
                if (table[i] == program.rows[i])
                    for (size_t a = program.blocks[i].first; a < program.blocks[i].second; ++a) actions.push_back(program.actions[a]);
                else
                    for (auto &action : render_block(table[i], i, program.steps[i])) actions.push_back(std::move(action));
            }
            for (size_t i = program.blocks.back().second; i < program.actions.size(); ++i) actions.push_back(program.actions[i]);
            control["action_list"] = std::move(actions);
            const double old_total = native_parameters::total_seconds(program.rows), new_total = native_parameters::total_seconds(table);
            if (old_total != new_total && control.contains("total_time") && control["total_time"].is_number_unsigned())
                control["total_time"] = native_parameters::carried_time(control["total_time"].get<uint32_t>(), old_total, new_total);
        }
        const auto z = native_parameters::z_column(table);
        const double old_weight = native_parameters::thickness_weighted_area(old_masks, z),
                     new_weight = native_parameters::thickness_weighted_area(new_masks, z);
        std::optional<float> new_material;
        if (old_weight != new_weight) {
            if (old_weight <= 0) throw RuntimeError("The JXS material estimate cannot be recomputed from a source without lit pixels");
            if (!control.contains("used_material") || !control["used_material"].is_number())
                throw RuntimeError("JXS control.json lacks used_material");
            new_material = float(control["used_material"].get<double>() * (new_weight / old_weight));
            control["used_material"] = *new_material;
        }
        const auto serialized = native_parameters::json_dump(control, program.style);
        if (serialized != native_parameters::json_dump(program.control, program.style) || rows_changed)
            result["control.json"] = std::vector<uint8_t>(serialized.begin(), serialized.end());
        if (new_material) {
            auto ini = program.ini_text;
            const auto at = ini.find("UsedMaterial=");
            if (at != std::string::npos && (at == 0 || ini[at - 1] == '\n')) {
                const auto end = ini.find('\n', at);
                std::ostringstream out;
                out.imbue(std::locale::classic());
                out << std::setprecision(9) << *new_material;
                ini.replace(at + 13, (end == std::string::npos ? ini.size() : end) - at - 13, out.str());
                result["config.ini"] = std::vector<uint8_t>(ini.begin(), ini.end());
            }
        }
        return result;
    }

private:
    static constexpr uint64_t metadata_limit = 32ULL * 1024 * 1024;
    static constexpr uint64_t image_limit = 256ULL * 1024 * 1024;
    const MemoryZip &m_zip;
    std::vector<std::string> m_names;

    static PngHeaderInfo header_of(const std::vector<uint8_t> &png, size_t layer)
    {
        const std::string where = "JXS layer " + std::to_string(layer + 1);
        const auto header = read_png_header(png, where);
        if (header.bit_depth != 8 || header.color_type != 2) throw RuntimeError(where + " must be an 8-bit RGB PNG");
        return header;
    }

    // "NumLayers=N" in config.ini; 0 when absent (the images then decide).
    static size_t declared_layers(const MemoryZip &zip)
    {
        const auto bytes = zip.extract("config.ini", metadata_limit);
        std::istringstream lines(std::string(bytes.begin(), bytes.end()));
        std::string line;
        while (std::getline(lines, line)) {
            const auto equals = line.find('=');
            if (equals == std::string::npos || boost::algorithm::trim_copy(line.substr(0, equals)) != "NumLayers") continue;
            const auto value = boost::algorithm::trim_copy(line.substr(equals + 1));
            if (value.empty() || value.size() > 9 || !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }))
                throw RuntimeError("JXS config.ini NumLayers is not a whole number");
            return size_t(std::stoul(value));
        }
        return 0;
    }

    // ---- Parameter parsing and rendering (private) ----------------------------------------
    using Json = nlohmann::ordered_json;
    static bool pos(double value) { return native_parameters::round_decimals(value, 1000.) > 0; }
    static std::string n3(double value) { return native_parameters::trimmed_number(value); }
    static double r3(double value) { return native_parameters::round_decimals(value, 1000.); }

    std::optional<std::string> ini_value(const char *key) const
    {
        const auto bytes = m_zip.extract("config.ini", metadata_limit);
        std::istringstream lines(std::string(bytes.begin(), bytes.end()));
        std::string line;
        while (std::getline(lines, line)) {
            const auto equals = line.find('=');
            if (equals != std::string::npos && boost::algorithm::trim_copy(line.substr(0, equals)) == key)
                return boost::algorithm::trim_copy(line.substr(equals + 1));
        }
        return std::nullopt;
    }

    // UVtools LightOffDelayC.CalculateSecondsLiftOnly, in single precision like the exporter.
    static float synchronization(float height, float speed, float height2, float speed2)
    {
        const float extra = .75f;
        float seconds = extra;
        for (const auto &stage : {std::pair{height, speed}, std::pair{height2, speed2}})
            if (stage.first > 0) {
                if (stage.second <= 0) throw RuntimeError("JXS active movement requires a positive speed");
                seconds += std::nearbyint((stage.first / (stage.second / 60.f) + extra) * 100.f) / 100.f;
            }
        return seconds;
    }
    static Json command(const std::string &text) { return Json::array({"gcode", text}); }
    static void delay(std::vector<Json> &out, double seconds)
    {
        const auto ms = std::llround(seconds * 1000);
        if (ms > 0) out.push_back(Json::array({"delay", uint32_t(ms)}));
    }
    static void sync(std::vector<Json> &out, double seconds)
    {
        const auto ms = std::llround(seconds * 1000);
        if (ms > 0) out.push_back(Json::array({"delay", uint32_t(ms)}));
        else out.push_back(command("G4 P00"));
    }
    static std::vector<Json> render_block(const native_parameters::Row &row, size_t layer, double step)
    {
        using F = sla::LayerParameter;
        auto v = [&](F field) { return native_parameters::value_of(row, field); };
        const double lift = v(F::LiftDistance), lift2 = v(F::LiftDistance2), retract2 = v(F::RetractDistance2), total = lift + lift2;
        std::vector<Json> out;
        auto move = [&](double delta, double speed) {
            if (!pos(std::abs(delta))) return;
            out.push_back(command("G0 Z" + n3(delta) + " F" + n3(speed)));
        };
        std::ostringstream name;
        name << std::setw(5) << std::setfill('0') << layer << ".png";
        out.push_back(Json::array({"layerno", layer + 1}));
        out.push_back(Json::array({"slice", "{PWD}/" + name.str()}));
        if (pos(lift)) {
            move(step + lift, v(F::LiftSpeed));
            if (pos(lift2)) move(lift2, v(F::LiftSpeed2));
        } else
            move(step + lift2, pos(lift2) ? v(F::LiftSpeed2) : v(F::LiftSpeed));
        sync(out, pos(total) ? double(synchronization(float(lift), float(v(F::LiftSpeed)), float(lift2), float(v(F::LiftSpeed2)))) : 0.);
        if (pos(total)) delay(out, v(F::WaitAfterLift));
        move(-(total - retract2), v(F::RetractSpeed));
        move(-retract2, v(F::RetractSpeed2));
        sync(out, pos(total) ? double(synchronization(float(total - retract2), float(v(F::RetractSpeed)), float(retract2), float(v(F::RetractSpeed2)))) : 0.);
        if (pos(v(F::WaitBefore))) delay(out, v(F::WaitBefore));
        else out.push_back(command("G4 P.0"));
        out.push_back(command("M106 S" + std::to_string(int(v(F::LightPWM)))));
        delay(out, std::nearbyint(v(F::Exposure) * 100.) / 100.);
        out.push_back(command("M106 S0"));
        out.push_back(Json::array({"slice", "<BLANK>"}));
        delay(out, v(F::WaitAfter));
        return out;
    }

    struct Program {
        Json control;
        native_parameters::JsonStyle style;        // the file's own indent and line ending
        std::string ini_text;
        std::vector<Json> actions;
        std::vector<std::pair<size_t, size_t>> blocks;
        std::vector<double> steps;                 // thickness of each layer (relative Z move)
        native_parameters::Table rows;
    };
    static bool is_key(const Json &action, const char *key)
    {
        return action.is_array() && action.size() == 2 && action[0].is_string() && action[0].get<std::string>() == key;
    }
    static bool is_gcode(const Json &action, const std::string &text)
    {
        return is_key(action, "gcode") && action[1].is_string() && action[1].get<std::string>() == text;
    }
    static bool is_delay(const Json &action, double &seconds)
    {
        if (!is_key(action, "delay") || !action[1].is_number_integer()) return false;
        seconds = double(action[1].get<int64_t>()) / 1000.;
        return true;
    }
    // "G0 Z<delta> F<speed>" with the delta's sign.
    static bool is_move(const Json &action, double &delta, double &speed)
    {
        if (!is_key(action, "gcode") || !action[1].is_string()) return false;
        const auto text = action[1].get<std::string>();
        if (text.compare(0, 4, "G0 Z") != 0) return false;
        char *rest = nullptr;
        delta = std::strtod(text.c_str() + 4, &rest);
        if (rest == text.c_str() + 4 || std::strncmp(rest, " F", 2) != 0) return false;
        speed = std::strtod(rest + 2, &rest);
        return rest && !*rest;
    }

    Program parse_program() const
    {
        using F = sla::LayerParameter;
        const std::string where = "JXS control.json is not in the exporter's layout: ";
        Program program;
        const auto bytes = m_zip.extract("control.json", metadata_limit);
        const std::string text(bytes.begin(), bytes.end());
        try { program.control = Json::parse(text); }
        catch (const Json::exception &error) { throw RuntimeError(std::string("JXS control.json is not valid JSON: ") + error.what()); }
        if (!program.control.is_object()) throw RuntimeError(where + "it is not a JSON object");
        const auto style = native_parameters::json_style(program.control, text);
        if (!style) throw RuntimeError(where + "its layout cannot be rewritten without reformatting it");
        program.style = *style;
        if (!program.control.contains("action_list") || !program.control["action_list"].is_array())
            throw RuntimeError(where + "it has no action list");
        const auto ini = m_zip.extract("config.ini", metadata_limit);
        program.ini_text.assign(ini.begin(), ini.end());
        for (const auto &action : program.control["action_list"]) program.actions.push_back(action);
        const auto &a = program.actions;
        size_t at = 0;
        auto need = [&](bool ok, const std::string &what) { if (!ok) throw RuntimeError(where + what); };
        for (const char *line : {"G21", "G91", "M106 S0"}) { need(at < a.size() && is_gcode(a[at], line), "its header differs"); ++at; }
        need(at < a.size() && is_key(a[at], "slice") && a[at][1] == "<BLANK>", "its header differs"); ++at;
        for (const char *line : {"M17", "G28 Z0"}) { need(at < a.size() && is_gcode(a[at], line), "its header differs"); ++at; }
        double z = 0;
        for (size_t layer = 0; layer < count; ++layer) {
            const std::string name = "layer " + std::to_string(layer + 1) + " ";
            const size_t begin = at;
            need(at + 1 < a.size() && is_key(a[at], "layerno") && a[at][1].is_number_integer() && a[at][1].get<size_t>() == layer + 1, name + "does not start where expected");
            at += 2; // layerno and slice (the rendering below proves the slice name)
            std::vector<std::pair<double, double>> ups, downs;
            double delta = 0, speed = 0, seconds = 0;
            while (at < a.size() && is_move(a[at], delta, speed) && delta > 0) { ups.push_back({delta, speed}); ++at; }
            need(!ups.empty() && ups.size() <= 2, name + "has no lift move");
            native_parameters::Row row;
            row[F::WaitAfterLift] = 0.;
            if (at < a.size() && is_gcode(a[at], "G4 P00")) { // no lift: a plain advance
                ++at;
                need(ups.size() == 1, name + "has an unexpected advance");
            } else {
                need(at < a.size() && is_delay(a[at], seconds), name + "lacks its lift synchronization");
                ++at;
                if (at < a.size() && is_delay(a[at], seconds)) { row[F::WaitAfterLift] = seconds; ++at; }
                while (at < a.size() && is_move(a[at], delta, speed) && delta < 0) { downs.push_back({-delta, speed}); ++at; }
                need(!downs.empty() && downs.size() <= 2, name + "has no retract move");
                need(at < a.size() && is_delay(a[at], seconds), name + "lacks its retract synchronization");
                ++at;
            }
            if (downs.empty()) need(at < a.size() && is_gcode(a[at++], "G4 P00"), name + "lacks its retract synchronization");
            double wait_before = 0;
            if (at < a.size() && is_delay(a[at], wait_before)) ++at;
            else { need(at < a.size() && is_gcode(a[at], "G4 P.0"), name + "lacks its pre-cure wait"); ++at; wait_before = 0; }
            need(at < a.size() && is_key(a[at], "gcode") && a[at][1].is_string() && a[at][1].get<std::string>().compare(0, 6, "M106 S") == 0, name + "lacks its light-on");
            char *rest = nullptr;
            const auto light = a[at][1].get<std::string>();
            const long pwm = std::strtol(light.c_str() + 6, &rest, 10);
            need(rest && !*rest && rest != light.c_str() + 6 && pwm >= 0 && pwm <= 255, name + "has an invalid PWM");
            ++at;
            double exposure = 0;
            if (at < a.size() && is_delay(a[at], seconds)) { exposure = seconds; ++at; }
            need(at + 1 < a.size() && is_gcode(a[at], "M106 S0") && is_key(a[at + 1], "slice") && a[at + 1][1] == "<BLANK>", name + "lacks its light-off");
            at += 2;
            double wait_after = 0;
            if (at < a.size() && is_delay(a[at], seconds)) { wait_after = seconds; ++at; }
            // Values (see render_block for the order).
            double total = 0, lift, lift2 = 0, step;
            for (const auto &down : downs) total += down.first;
            if (downs.empty()) { lift = 0; step = ups[0].first; }
            else {
                lift2 = ups.size() == 2 ? ups[1].first : 0.;
                lift = total - lift2;
                step = ups[0].first - lift;
            }
            row[F::LiftDistance] = r3(lift); row[F::LiftSpeed] = ups[0].second;
            row[F::LiftDistance2] = r3(lift2); row[F::LiftSpeed2] = ups.size() == 2 ? ups[1].second : 0.;
            row[F::RetractDistance2] = downs.size() == 2 ? r3(downs[1].first) : 0.;
            row[F::RetractSpeed] = downs.empty() ? 0. : downs[0].second;
            row[F::RetractSpeed2] = downs.size() == 2 ? downs[1].second : 0.;
            row[F::WaitBefore] = wait_before; row[F::Exposure] = exposure; row[F::WaitAfter] = wait_after;
            row[F::LightPWM] = double(pwm);
            step = r3(step);
            need(step > 0 && std::isfinite(step), name + "has a nonpositive thickness");
            z = r3(z + step);
            row[F::PositionZ] = z;
            for (const auto &[field, value] : row)
                need(std::isfinite(value) && value >= 0, name + "has a negative value");
            const auto expected = render_block(row, layer, step);
            need(expected.size() == at - begin && std::equal(expected.begin(), expected.end(), a.begin() + begin), name + "differs from the exporter's rendering");
            program.blocks.push_back({begin, at});
            program.steps.push_back(step);
            program.rows.push_back(std::move(row));
        }
        need(at < a.size() && is_gcode(a[at], "M106 S0"), "its tail is missing");
        return program;
    }
};

} // namespace Slic3r

#endif
