// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader-side layout of the Anycubic ZIP family (pm4u, pm7, pm7m, pwsz).
// Port of UVtools.Core/FileFormats/AnycubicZipFile.cs DecodeInternally and
// AnycubicFile.DecodePW0/EncodePW0; the writer lives in AnycubicZipArchive.cpp.
//
// Container: anycubic_photon_resins.pwsp (machine JSON), layers_controller.conf
// (per-layer table), scene.slice, print_info.json, previews and one
// layer_images/layer_<n>.pw0Img per layer.
#ifndef SLIC3R_ANYCUBIC_ZIP_LAYOUT_HPP
#define SLIC3R_ANYCUBIC_ZIP_LAYOUT_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "MemoryZip.hpp"
#include "NativeParameterLayout.hpp"

namespace Slic3r {

class AnycubicZipLayout {
public:
    static constexpr const char *settings_name = "anycubic_photon_resins.pwsp";
    static constexpr const char *layers_name = "layers_controller.conf";

    static bool is_format(const std::string &format)
    {
        return format == "pm4u" || format == "pm7" || format == "pm7m" || format == "pwsz";
    }
    // Both manifests are mandatory in UVtools; neither is used by another family.
    static bool recognizes(const std::set<std::string> &names)
    {
        return names.count(settings_name) && names.count(layers_name);
    }
    // PW0 stores four-bit gray. A mask edit must land on a representable level,
    // otherwise the editor would show values the file cannot hold.
    static constexpr uint8_t quantize(uint8_t value) { return uint8_t((value >> 4) * 17); }

    std::string format;
    size_t columns = 0, rows = 0, count = 0;

    AnycubicZipLayout(const MemoryZip &zip, const std::string &extension) : m_zip(zip)
    {
        const auto settings = parse_json(zip.extract(settings_name, metadata_limit), settings_name);
        const auto &machine = member(settings, "machine_type", settings_name);
        columns = bounded_integer(member(machine, "res_x", settings_name), "res_x", 32768);
        rows = bounded_integer(member(machine, "res_y", settings_name), "res_y", 32768);
        if (uint64_t(columns) * rows > 128ULL * 1024 * 1024)
            throw RuntimeError("Anycubic ZIP raster exceeds the editor memory budget");

        // UVtools chooses the family from the file extension on save; the
        // manifest copy is only a fallback for a renamed file.
        std::string suffix;
        if (machine.contains("key_suffix") && machine["key_suffix"].is_string())
            suffix = machine["key_suffix"].get<std::string>();
        if (is_format(extension)) format = extension;
        else if (is_format(suffix)) format = suffix;
        else throw RuntimeError("Anycubic ZIP needs a .pm4u, .pm7, .pm7m or .pwsz extension");

        const auto layers = parse_json(zip.extract(layers_name, metadata_limit), layers_name);
        const auto &paras = member(layers, "paras", layers_name);
        if (!paras.is_array())
            throw RuntimeError("Anycubic ZIP layer table 'paras' is not a list");
        count = paras.size();
        if (!count || count > 100000)
            throw RuntimeError("Anycubic ZIP layer count is zero or exceeds the editor limit");
        // UVtools derives the count from the table length; a disagreeing copy
        // means the file was damaged or hand-edited inconsistently.
        if (layers.contains("count") && (!layers["count"].is_number_integer() || layers["count"].get<int64_t>() != int64_t(count)))
            throw RuntimeError("Anycubic ZIP layer count does not match its layer table");

        const uint64_t pixels = uint64_t(columns) * rows;
        m_names.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const std::string base = "layer_images/layer_" + std::to_string(i);
            if (zip.has(base + ".pwszImg"))
                // PWSZ is a vector contour description rasterised with OpenCV in
                // UVtools; there is no exact pixel mask to edit or write back.
                throw RuntimeError("Anycubic ZIP layer " + std::to_string(i) +
                                   " uses vector contours (pwszImg); only pw0Img layers can be edited");
            const std::string name = base + ".pw0Img";
            if (!zip.has(name))
                throw RuntimeError("Anycubic ZIP layer image " + std::to_string(i) + " is missing");
            // Worst case is two bytes per pixel (alternating black and white).
            if (zip.size_of(name) > 2 * pixels + 64)
                throw RuntimeError("Anycubic ZIP layer image " + std::to_string(i) + " is larger than its raster allows");
            m_names.push_back(name);
        }
    }

    const std::string &layer_entry(size_t layer) const { return m_names.at(layer); }

    std::vector<uint8_t> decode(size_t layer) const
    {
        const auto rle = m_zip.extract(layer_entry(layer), 2 * uint64_t(columns) * rows + 64);
        return decode_pw0(rle, columns * rows, layer);
    }

    // DecodePW0: the high nibble is the gray level (0 and 15 are followed by a
    // second byte, giving a 12-bit run), the low nibble the repeat.
    static std::vector<uint8_t> decode_pw0(const std::vector<uint8_t> &rle, size_t pixel_count, size_t layer)
    {
        const std::string where = "Anycubic ZIP layer " + std::to_string(layer);
        std::vector<uint8_t> pixels(pixel_count, 0);
        size_t position = 0;
        for (size_t i = 0; i < rle.size(); ++i) {
            const unsigned code = rle[i] >> 4;
            size_t repeat = rle[i] & 0x0f;
            uint8_t color;
            if (code == 0 || code == 15) {
                color = code ? 255 : 0;
                ++i;
                // UVtools treats a dangling run byte as "fill the remainder".
                if (i >= rle.size()) repeat = pixel_count - position;
                else repeat = (repeat << 8) + rle[i];
            } else
                color = uint8_t(code * 17);
            if (repeat > pixel_count - position)
                throw RuntimeError(where + " runs past the end of the raster");
            std::fill_n(pixels.begin() + position, repeat, color);
            position += repeat;
            if (position == pixel_count)
                break; // UVtools ignores anything after a complete image.
        }
        if (position != pixel_count)
            throw RuntimeError(where + " ends short: " + std::to_string(position) + " of " + std::to_string(pixel_count) + " pixels");
        return pixels;
    }

    std::vector<uint8_t> encode(const std::vector<uint8_t> &pixels) const { return encode_pw0(pixels); }

    // EncodePW0. Black/white runs take two bytes (max 4095), the other levels one (max 15).
    static std::vector<uint8_t> encode_pw0(const std::vector<uint8_t> &pixels)
    {
        std::vector<uint8_t> out;
        for (size_t first = 0; first < pixels.size();) {
            const uint8_t color = pixels[first] >> 4;
            size_t end = first + 1;
            while (end < pixels.size() && (pixels[end] >> 4) == color)
                ++end;
            for (size_t left = end - first; left;) {
                const bool wide = color == 0 || color == 15;
                const size_t count = std::min(left, size_t(wide ? 4095 : 15));
                if (wide) {
                    out.push_back(uint8_t((color << 4) | (count >> 8)));
                    out.push_back(uint8_t(count));
                } else
                    out.push_back(uint8_t((color << 4) | count));
                left -= count;
            }
            first = end;
        }
        return out;
    }


    // ---- Per-layer parameters -------------------------------------------------------------
    // layers_controller.conf stores exposure, lift distance, lift speed (mm/s)
    // and the layer thickness per layer; UVtools adds the thicknesses up to get
    // Z. Retract and the second lift phase belong to the resin stage settings
    // (global), so only these four fields are per-layer.
    static sla::LayerParameterCapabilities parameter_capabilities()
    {
        using F = sla::LayerParameter;
        return native_parameters::capabilities({F::PositionZ, F::Exposure, F::LiftDistance, F::LiftSpeed});
    }
    // What the file can hold: Z at 0.001 mm (the precision UVtools reads), the
    // others as 32-bit floats, the speed through its mm/s field.
    static double canonical(sla::LayerParameter field, double value)
    {
        using F = sla::LayerParameter;
        if (field == F::PositionZ) return native_parameters::round_decimals(value, 1000.);
        if (field == F::LiftSpeed) return native_parameters::as_float(value / 60.) * 60.;
        return native_parameters::as_float(value);
    }
    native_parameters::Table parameter_rows() const
    {
        using F = sla::LayerParameter;
        native_parameters::Table rows;
        rows.reserve(count);
        double z = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto &entry = paras().at(i);
            auto number = [&](const char *key) {
                if (!entry.is_object() || !entry.contains(key) || !entry[key].is_number())
                    throw RuntimeError("Anycubic ZIP layer " + std::to_string(i) + " has no numeric '" + key + "'");
                const double value = entry[key].get<double>();
                if (!std::isfinite(value) || value < 0)
                    throw RuntimeError("Anycubic ZIP layer " + std::to_string(i) + " has an invalid '" + key + "'");
                return value;
            };
            // UVtools adds the thicknesses up; the running sum is rounded to the 0.001 mm it keeps.
            z += number("layer_thickness");
            rows.push_back({{F::PositionZ, canonical(F::PositionZ, z)}, {F::Exposure, native_parameters::as_float(number("exposure_time"))},
                            {F::LiftDistance, native_parameters::as_float(number("zup_height"))},
                            {F::LiftSpeed, native_parameters::as_float(number("zup_speed")) * 60.}});
        }
        return rows;
    }
    // True when the scene file agrees with the layer table: only then is a Z
    // edit written to both places consistently.
    bool scene_matches(const native_parameters::Table &rows) const
    {
        if (!m_zip.has(scene_name)) return false;
        const auto scene = m_zip.extract(scene_name, 1ULL << 30);
        if (scene.size() != scene_defs + 64 * count + 4 || std::memcmp(scene.data(), "ANYCUBIC-PWSZ", 13) != 0 ||
            std::memcmp(scene.data() + 388, "<---", 4) != 0 || le32(scene, 100) != count || le32(scene, 392) != count ||
            std::memcmp(scene.data() + scene.size() - 4, "--->", 4) != 0)
            return false;
        for (size_t i = 0; i < count; ++i)
            if (std::fabs(lef(scene, scene_defs + 64 * i) - rows[i].at(sla::LayerParameter::PositionZ)) > 0.0015) return false;
        return true;
    }
    // Machine height (print_zsize) from the machine manifest; 0 when absent.
    double height_limit() const
    {
        const auto settings = json_of(settings_name);
        if (settings.contains("machine_type") && settings["machine_type"].contains("print_zsize") &&
            settings["machine_type"]["print_zsize"].is_number())
            return settings["machine_type"]["print_zsize"].get<double>();
        return 0.;
    }
    size_t bottom_layers() const
    {
        try {
            const auto settings = json_of(settings_name);
            return size_t(settings.at("machine_extern").at("user_resins").at(0).at("slicepara").at("bott_layers").get<int64_t>());
        } catch (const nlohmann::json::exception &) {
            return 0;
        }
    }
    // Changed members after a parameter or mask edit; empty when nothing changed.
    std::map<std::string, std::vector<uint8_t>> parameter_replacements(const native_parameters::Table &table,
        const std::vector<native_parameters::MaskSummary> &old_masks,
        const std::vector<native_parameters::MaskSummary> &new_masks) const
    {
        const auto old_lit = native_parameters::lit_counts(old_masks), new_lit = native_parameters::lit_counts(new_masks);
        using F = sla::LayerParameter;
        if (table.size() != count || old_lit.size() != count || new_lit.size() != count)
            throw RuntimeError("Anycubic ZIP parameter table has the wrong layer count");
        const auto original = parameter_rows();
        auto layers = json_of(layers_name);
        bool rows_changed = false, z_changed = false;
        double old_weighted = 0, new_weighted = 0, old_z = 0, new_z = 0;
        for (size_t i = 0; i < count; ++i) {
            old_weighted += old_lit[i] * (original[i].at(F::PositionZ) - old_z);
            new_weighted += new_lit[i] * (table[i].at(F::PositionZ) - new_z);
            old_z = original[i].at(F::PositionZ); new_z = table[i].at(F::PositionZ);
            auto &row = layers["paras"][i];
            if (original[i].at(F::Exposure) != table[i].at(F::Exposure)) { row["exposure_time"] = table[i].at(F::Exposure); rows_changed = true; }
            if (original[i].at(F::LiftDistance) != table[i].at(F::LiftDistance)) { row["zup_height"] = table[i].at(F::LiftDistance); rows_changed = true; }
            if (original[i].at(F::LiftSpeed) != table[i].at(F::LiftSpeed)) { row["zup_speed"] = table[i].at(F::LiftSpeed) / 60.; rows_changed = true; }
            const bool z_here = original[i].at(F::PositionZ) != table[i].at(F::PositionZ);
            const bool z_before = i && original[i - 1].at(F::PositionZ) != table[i - 1].at(F::PositionZ);
            if (z_here || z_before) {
                const double previous = i ? table[i - 1].at(F::PositionZ) : 0.;
                row["layer_minheight"] = previous;
                row["layer_thickness"] = table[i].at(F::PositionZ) - previous;
                rows_changed = z_changed = true;
            }
        }
        const bool lit_changed = old_lit != new_lit;
        if (!rows_changed && !lit_changed) return {};
        std::map<std::string, std::vector<uint8_t>> result;
        auto put = [&](const char *name, const std::string &text) { result[name] = std::vector<uint8_t>(text.begin(), text.end()); };
        if (rows_changed) put(layers_name, layers.dump(2));
        if (z_changed) {
            if (!scene_matches(original)) throw RuntimeError("Anycubic ZIP scene.slice does not match its layer table; Z cannot be edited");
            auto scene = m_zip.extract(scene_name, 1ULL << 30);
            auto putf = [&](size_t at, double value) {
                const float number = float(value);
                std::memcpy(scene.data() + at, &number, 4); // the file is little endian like the hosts we build for
            };
            for (size_t i = 0; i < count; ++i) putf(scene_defs + 64 * i, table[i].at(F::PositionZ));
            putf(124, table.back().at(F::PositionZ)); // ZMax
            result[scene_name] = std::move(scene);
        }
        // Print info: volume and cost follow the thickness-weighted lit area; the time follows the table.
        if (m_zip.has(info_name)) {
            auto info = json_of(info_name);
            if (old_weighted > 0 && new_weighted != old_weighted)
                for (const char *key : {"volume", "cost"})
                    if (info.contains(key) && info[key].is_number())
                        info[key] = info[key].get<double>() * new_weighted / old_weighted;
            if (rows_changed && info.contains("print_time") && info["print_time"].is_number()) {
                const auto model = time_model();
                if (model) {
                    const double before = model(original), after = model(table);
                    const double stored = info["print_time"].get<double>();
                    // Our exporter stores round2(model); another writer keeps its own overhead.
                    const bool ours = std::fabs(stored - native_parameters::round_decimals(before, 100.)) < 0.0051;
                    info["print_time"] = ours ? native_parameters::round_decimals(after, 100.) : std::max(0., stored + (after - before));
                }
            }
            put(info_name, info.dump(2));
        }
        return result;
    }

private:
    static constexpr const char *scene_name = "scene.slice";
    static constexpr const char *info_name = "print_info.json";
    static constexpr size_t scene_defs = 396;
    static uint32_t le32(const std::vector<uint8_t> &bytes, size_t at)
    {
        return uint32_t(bytes.at(at)) | (uint32_t(bytes.at(at + 1)) << 8) | (uint32_t(bytes.at(at + 2)) << 16) | (uint32_t(bytes.at(at + 3)) << 24);
    }
    static float lef(const std::vector<uint8_t> &bytes, size_t at)
    {
        const uint32_t bits = le32(bytes, at);
        float value;
        std::memcpy(&value, &bits, 4);
        return value;
    }
    nlohmann::json json_of(const char *name) const { return parse_json(m_zip.extract(name, metadata_limit), name); }
    const nlohmann::json &paras() const
    {
        if (!m_paras) m_paras = std::make_shared<nlohmann::json>(json_of(layers_name)["paras"]);
        return *m_paras;
    }
    mutable std::shared_ptr<nlohmann::json> m_paras;
    // Layer time as the exporter plans it (AnycubicZipArchive.cpp plan_zip_parameters):
    // 300 s of fixed overhead, then per layer exposure + pre-cure wait + max(motor, wait),
    // where the motor covers the row's lift plus the resin's second stage and retract.
    // Returns an empty function when the resin settings are not in the exporter's shape.
    std::function<double(const native_parameters::Table &)> time_model() const
    {
        using F = sla::LayerParameter;
        try {
            const auto settings = json_of(settings_name);
            const auto &resins = settings.at("machine_extern").at("user_resins");
            if (!resins.is_array() || resins.empty()) return {};
            const auto &resin = resins.at(0);
            const double wait = resin.at("slicepara").at("off_time").get<double>();
            const size_t bottom = size_t(resin.at("slicepara").at("bott_layers").get<int64_t>());
            const auto &stages = resin.at("slice_extpara").at("multi_state_paras");
            struct Stage { double lift2, speed2, retract; };
            auto stage = [&](const char *first, const char *second) {
                return Stage{stages.at(second).at("height").get<double>(), stages.at(second).at("up_speed").get<double>() * 60.,
                             stages.at(first).at("down_speed").get<double>() * 60.};
            };
            const Stage bottom_stage = stage("bott_0", "bott_1"), normal_stage = stage("normal_0", "normal_1");
            return [=](const native_parameters::Table &table) {
                double total = 300.;
                for (size_t i = 0; i < table.size(); ++i) {
                    const auto &s = i < bottom ? bottom_stage : normal_stage;
                    const double lift = table[i].at(F::LiftDistance), speed = table[i].at(F::LiftSpeed);
                    double motor = 0;
                    auto phase = [&](double distance, double rate) { if (distance > 0 && rate > 0) motor += distance * 60. / rate; };
                    phase(lift, speed); phase(s.lift2, s.speed2); phase(lift + s.lift2, s.retract);
                    motor = native_parameters::round_decimals(motor, 100.);
                    total += table[i].at(F::Exposure) + wait + std::max(motor, wait);
                }
                return total;
            };
        } catch (const nlohmann::json::exception &) {
            return {};
        }
    }

    static constexpr uint64_t metadata_limit = 32ULL * 1024 * 1024;
    const MemoryZip &m_zip;
    std::vector<std::string> m_names;

    static nlohmann::json parse_json(const std::vector<uint8_t> &bytes, const char *name)
    {
        try {
            return nlohmann::json::parse(bytes.begin(), bytes.end());
        } catch (const nlohmann::json::exception &error) {
            throw RuntimeError(std::string("Anycubic ZIP ") + name + " is not valid JSON: " + error.what());
        }
    }
    static const nlohmann::json &member(const nlohmann::json &object, const char *key, const char *where)
    {
        if (!object.is_object() || !object.contains(key))
            throw RuntimeError(std::string("Anycubic ZIP ") + where + " has no '" + key + "'");
        return object[key];
    }
    static size_t bounded_integer(const nlohmann::json &value, const char *key, int64_t maximum)
    {
        if (!value.is_number_integer() || value.get<int64_t>() <= 0 || value.get<int64_t>() > maximum)
            throw RuntimeError(std::string("Anycubic ZIP '") + key + "' is not a positive integer within the editor limit");
        return size_t(value.get<int64_t>());
    }
};

} // namespace Slic3r

#endif
