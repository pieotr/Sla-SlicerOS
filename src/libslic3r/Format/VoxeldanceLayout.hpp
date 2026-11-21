// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader-side layout of the Voxeldance Tango VDT format. Port of the UVtools
// VDTFile.cs decode path; the writer is VDTArchive.cpp.
//
// A VDT is a ZIP with manifest.json (printer, resin and print settings plus a
// "layers" array with one record per layer), Preview_*.png thumbnails and one
// 8-bit grayscale "<n>.png" per layer numbered from zero. The number of layers
// is the length of the manifest array (UVtools Init(Layers.Length)).
//
// The layer images and the per-layer records of the manifest are edited
// (all 14 fields of UVtools' VDTLayer). When masks or parameters change, the
// manifest statistics (volume, weight, price, estimated time) follow them; the
// previews and every other member are carried over byte for byte.
#ifndef SLIC3R_VOXELDANCE_LAYOUT_HPP
#define SLIC3R_VOXELDANCE_LAYOUT_HPP

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <nlohmann/json.hpp>

#include "MemoryZip.hpp"
#include "NativeParameterLayout.hpp"
#include "PngHeaderProbe.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/SLA/RasterBase.hpp"

namespace Slic3r {

class VoxeldanceLayout {
public:
    static bool is_format(const std::string &format) { return format == "vdt"; }
    // No other supported family writes a manifest.json next to zero-based PNGs.
    static bool recognizes(const std::set<std::string> &names) { return names.count("manifest.json") && names.count("0.png"); }

    std::string format = "vdt";
    size_t columns = 0, rows = 0, count = 0;

    explicit VoxeldanceLayout(const MemoryZip &zip) : m_zip(zip)
    {
        count = declared_layers(zip);
        if (!count || count > 100000) throw RuntimeError("VDT layer count is zero or exceeds the editor limit");
        std::map<size_t, std::string> images;
        for (const auto &name : zip.ordered_names()) {
            if (name.find('/') != std::string::npos || !boost::algorithm::iends_with(name, ".png")) continue;
            const auto stem = name.substr(0, name.size() - 4);
            if (stem.empty() || stem.size() > 7 || !std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; }))
                continue; // Preview_*.png
            if (!images.emplace(size_t(std::stoul(stem)), name).second)
                throw RuntimeError("VDT has two layer images numbered " + std::to_string(std::stoul(stem)));
        }
        m_names.reserve(count);
        for (size_t layer = 0; layer < count; ++layer) {
            const auto found = images.find(layer);
            if (found == images.end()) throw RuntimeError("VDT layer image " + std::to_string(layer) + " is missing");
            m_names.push_back(found->second);
        }
        const auto first = header_of(zip.extract(m_names.front(), image_limit), 0);
        columns = first.width; rows = first.height;
    }

    const std::string &layer_entry(size_t layer) const { return m_names.at(layer); }

    std::vector<uint8_t> decode(size_t layer) const
    {
        const std::string where = "VDT layer " + std::to_string(layer + 1);
        const auto bytes = m_zip.extract(layer_entry(layer), image_limit);
        const auto header = header_of(bytes, layer);
        if (header.width != columns || header.height != rows) throw RuntimeError(where + " differs in size from layer 1");
        png::ImageGreyscale image;
        if (!png::decode_color_png({bytes.data(), bytes.size()}, image) || image.cols != columns || image.rows != rows)
            throw RuntimeError(where + " is not a valid PNG");
        return std::move(image.buf);
    }

    std::vector<uint8_t> encode(const std::vector<uint8_t> &pixels) const
    {
        const auto encoded = sla::PNGRasterEncoder{}(pixels.data(), columns, rows, 1);
        if (!encoded.size()) throw RuntimeError("Cannot encode VDT layer image");
        const auto *data = static_cast<const uint8_t *>(encoded.data());
        return {data, data + encoded.size()};
    }


    // ---- Per-layer parameters -------------------------------------------------------------
    // JSON key of each stored field (UVtools VDTLayer).
    static const std::vector<std::pair<sla::LayerParameter, const char *>> &field_keys()
    {
        using F = sla::LayerParameter;
        static const std::vector<std::pair<F, const char *>> keys = {
            {F::PositionZ, "height"}, {F::LightOff, "light_off_delay"}, {F::WaitBefore, "wait_time_before_cure"},
            {F::Exposure, "exposure_time"}, {F::WaitAfter, "wait_time_after_cure"}, {F::LiftDistance, "lift_distance"},
            {F::LiftSpeed, "lift_speed"}, {F::LiftDistance2, "lift_distance2"}, {F::LiftSpeed2, "lift_speed2"},
            {F::WaitAfterLift, "wait_time_after_lift"}, {F::RetractSpeed, "retract_speed"},
            {F::RetractDistance2, "retract_distance2"}, {F::RetractSpeed2, "retract_speed2"}, {F::LightPWM, "light_pwm"}};
        return keys;
    }
    static sla::LayerParameterCapabilities parameter_capabilities()
    {
        sla::LayerParameterCapabilities result;
        for (const auto &[field, key] : field_keys()) result.push_back(native_parameters::capability(field));
        return result;
    }
    // UVtools rounds heights to 0.001 mm and everything else to 0.01 on load, so
    // those are the values a saved edit has to be able to read back.
    static double canonical(sla::LayerParameter field, double value)
    {
        using F = sla::LayerParameter;
        // PWM stays as given: a fractional value must be refused by the whole-number rule, not rounded.
        if (field == F::LightPWM) return value;
        return native_parameters::as_float(native_parameters::round_decimals(value, field == F::PositionZ ? 1000. : 100.));
    }
    native_parameters::Row parameter_row(size_t layer) const
    {
        const auto &entry = manifest_layers().at(layer);
        if (!entry.is_object()) throw RuntimeError("VDT layer " + std::to_string(layer + 1) + " record is not an object");
        native_parameters::Row row;
        for (const auto &[field, key] : field_keys()) {
            if (!entry.contains(key) || !entry[key].is_number())
                throw RuntimeError("VDT layer " + std::to_string(layer + 1) + " has no numeric '" + key + "'");
            const double value = native_parameters::as_float(entry[key].get<double>());
            if (!std::isfinite(value) || value < 0) throw RuntimeError("VDT layer " + std::to_string(layer + 1) + " has an invalid '" + key + "'");
            row[field] = value;
        }
        return row;
    }
    native_parameters::Table parameter_rows() const
    {
        native_parameters::Table rows;
        rows.reserve(count);
        for (size_t i = 0; i < count; ++i) rows.push_back(parameter_row(i));
        return rows;
    }
    double height_limit() const
    {
        const auto json = manifest();
        if (json.contains("machine") && json["machine"].is_object() && json["machine"].contains("z_height") &&
            json["machine"]["z_height"].is_number())
            return json["machine"]["z_height"].get<double>();
        return 0.;
    }
    size_t bottom_layers() const
    {
        const auto json = manifest();
        if (json.contains("print") && json["print"].is_object() && json["print"].contains("bottom_layers") &&
            json["print"]["bottom_layers"].is_number_unsigned())
            return json["print"]["bottom_layers"].get<size_t>();
        return 0;
    }
    // New manifest bytes, or an empty map when neither a parameter nor the lit
    // area changed. `old_lit` / `new_lit` hold the lit-pixel count of every layer.
    std::map<std::string, std::vector<uint8_t>> parameter_replacements(const native_parameters::Table &table,
        const std::vector<native_parameters::MaskSummary> &old_masks,
        const std::vector<native_parameters::MaskSummary> &new_masks) const
    {
        const auto old_lit = native_parameters::lit_counts(old_masks), new_lit = native_parameters::lit_counts(new_masks);
        using F = sla::LayerParameter;
        if (table.size() != count || old_lit.size() != count || new_lit.size() != count)
            throw RuntimeError("VDT parameter table has the wrong layer count");
        auto json = manifest();
        auto &layers = json["layers"];
        bool changed = false;
        double old_seconds = 0, new_seconds = 0, old_weighted = 0, new_weighted = 0, old_z = 0, new_z = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto original = parameter_row(i);
            old_seconds += native_parameters::layer_seconds(original);
            new_seconds += native_parameters::layer_seconds(table[i]);
            const double z_before = original.at(F::PositionZ), z_after = table[i].at(F::PositionZ);
            old_weighted += old_lit[i] * (z_before - old_z); new_weighted += new_lit[i] * (z_after - new_z);
            old_z = z_before; new_z = z_after;
            for (const auto &[field, key] : field_keys())
                if (original.at(field) != table[i].at(field)) {
                    // The exporter stores the float; PWM is a JSON integer.
                    if (field == F::LightPWM) layers[i][key] = int(table[i].at(field));
                    else layers[i][key] = table[i].at(field);
                    changed = true;
                }
            if (old_lit[i] != new_lit[i]) changed = true;
        }
        if (!changed) return {};
        if (json.contains("print_statistics") && json["print_statistics"].is_object()) {
            auto &stats = json["print_statistics"];
            // Ratios only: the display size and resin data may be absent or in other units.
            if (old_weighted > 0 && new_weighted != old_weighted)
                for (const char *key : {"volume", "weight", "price"})
                    if (stats.contains(key) && stats[key].is_number())
                        stats[key] = native_parameters::as_float(stats[key].get<double>() * new_weighted / old_weighted);
            if (stats.contains("estimated_time") && stats["estimated_time"].is_number())
                stats["estimated_time"] = native_parameters::carried_time(
                    uint32_t(std::llround(std::max(0., stats["estimated_time"].get<double>()))), old_seconds, new_seconds);
        }
        if (old_weighted > 0 && new_weighted != old_weighted && json.contains("resin") && json["resin"].is_object() &&
            json["resin"].contains("cost") && json["resin"]["cost"].is_number())
            json["resin"]["cost"] = native_parameters::as_float(json["resin"]["cost"].get<double>() * new_weighted / old_weighted);
        const auto text = json.dump(2);
        return {{"manifest.json", std::vector<uint8_t>(text.begin(), text.end())}};
    }

private:
    static constexpr uint64_t metadata_limit = 32ULL * 1024 * 1024;
    static constexpr uint64_t image_limit = 256ULL * 1024 * 1024;
    const MemoryZip &m_zip;
    std::vector<std::string> m_names;

    static PngHeaderInfo header_of(const std::vector<uint8_t> &png, size_t layer)
    {
        const std::string where = "VDT layer " + std::to_string(layer + 1);
        const auto header = read_png_header(png, where);
        // The exporter writes 8-bit gray; another shape could not be written back unchanged.
        if (header.bit_depth != 8 || header.color_type != 0) throw RuntimeError(where + " must be an 8-bit grayscale PNG");
        return header;
    }

    static nlohmann::json parse_manifest(const MemoryZip &zip)
    {
        nlohmann::json json;
        try {
            json = nlohmann::json::parse(zip.extract("manifest.json", metadata_limit));
        } catch (const nlohmann::json::exception &error) {
            throw RuntimeError(std::string("VDT manifest.json is not valid JSON: ") + error.what());
        }
        if (!json.is_object() || !json.contains("layers") || !json["layers"].is_array())
            throw RuntimeError("VDT manifest.json has no layers array");
        return json;
    }
    nlohmann::json manifest() const { return parse_manifest(m_zip); }
    // Parsed once per layout object: reading every row would otherwise re-parse the manifest per layer.
    const nlohmann::json &manifest_layers() const
    {
        if (!m_layers_cache) m_layers_cache = std::make_shared<nlohmann::json>(parse_manifest(m_zip)["layers"]);
        return *m_layers_cache;
    }
    static size_t declared_layers(const MemoryZip &zip) { return parse_manifest(zip)["layers"].size(); }
    mutable std::shared_ptr<nlohmann::json> m_layers_cache;
};

} // namespace Slic3r

#endif
