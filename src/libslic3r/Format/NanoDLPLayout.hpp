// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader-side layout of the NanoDLP plate ZIP (nanodlp, nanodlp_v1,
// nanodlp_rgb, nanodlp_rgb_v1). Port of the UVtools NanoDLPFile.cs decode path;
// the writer lives in NanoDLPArchive.cpp.
//
// Container: meta.json (FormatVersion), slicer.json (PWidth/PHeight in pixels),
// plate.json (LayersCount), profile.json, optional info.json (per-layer
// statistics), optional 3d.png preview and one top-level "<n>.png" per layer,
// numbered from one. Layer PNGs come in three shapes:
//   Gray    8-bit gray PNG, width == PWidth
//   Packed  8-bit RGB PNG of PWidth/3 columns; each RGB pixel carries three
//           consecutive mask pixels in file byte order (UVtools "Png24RgbAA")
//   FullRgb 8-bit RGB PNG of PWidth columns with R == G == B
//
// Print parameters: masks-only by design. NanoDLPFile.cs: the only per-layer data (info.json NanoDLPInfoItem) are area/bounding statistics;
// exposure, waits and lifts live in profile.json globals, so NanoDLP has no per-layer parameters.
#ifndef SLIC3R_NANODLP_LAYOUT_HPP
#define SLIC3R_NANODLP_LAYOUT_HPP

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <nlohmann/json.hpp>

#include "MemoryZip.hpp"
#include "PngHeaderProbe.hpp"
#include "libslic3r/SLA/ColorMask.hpp"
#include "libslic3r/SLA/RasterBase.hpp"

namespace Slic3r {

class NanoDLPLayout {
public:
    enum class Variant { Gray, Packed, FullRgb };

    // UVtools requires the meta, plate and profile manifests.
    static bool recognizes(const std::set<std::string> &names)
    {
        return names.count("meta.json") && names.count("plate.json") && names.count("profile.json");
    }
    static bool is_format(const std::string &format)
    {
        return format == "nanodlp" || format == "nanodlp_v1" || format == "nanodlp_rgb" || format == "nanodlp_rgb_v1";
    }
    // The registry extensions overlap as suffixes (".rgb.nanodlp" also ends in
    // ".nanodlp"), so the longest matching suffix decides which family a path names.
    static bool extension_matches(const std::string &format, const std::string &path)
    {
        static const std::pair<const char *, const char *> suffixes[] = {
            {"nanodlp", ".nanodlp"}, {"nanodlp_v1", ".v1.nanodlp"},
            {"nanodlp_rgb", ".rgb.nanodlp"}, {"nanodlp_rgb_v1", ".v1.rgb.nanodlp"}};
        const char *best = nullptr; size_t best_length = 0;
        for (const auto &[id, suffix] : suffixes)
            if (boost::algorithm::iends_with(path, suffix) && std::strlen(suffix) > best_length) {
                best = id; best_length = std::strlen(suffix);
            }
        return best && format == best;
    }

    std::string format;
    // columns is the full mask width in pixels (PWidth), also for packed files.
    size_t columns = 0, rows = 0, count = 0;
    Variant variant = Variant::Gray;
    unsigned version = 2;

    explicit NanoDLPLayout(const MemoryZip &zip) : m_zip(zip)
    {
        const auto meta = parse_json(zip, "meta.json", true);
        if (meta.contains("FormatVersion")) {
            const auto &value = meta["FormatVersion"];
            if (!value.is_number_integer() || (value.get<int64_t>() != 1 && value.get<int64_t>() != 2))
                throw RuntimeError("NanoDLP FormatVersion must be 1 or 2");
            version = unsigned(value.get<int64_t>());
        }
        size_t declared_count = 0;
        const auto plate = parse_json(zip, "plate.json", true);
        if (plate.contains("LayersCount")) declared_count = whole_number(plate["LayersCount"], "plate.json LayersCount", 1000000);

        // slicer.json is optional for UVtools; it is what tells a packed mono
        // file (PWidth == 3 * PNG width) from a full-width RGB one.
        size_t slicer_width = 0, slicer_height = 0;
        if (zip.has("slicer.json")) {
            const auto slicer = parse_json(zip, "slicer.json", false);
            if (slicer.contains("PWidth")) slicer_width = whole_number(slicer["PWidth"], "slicer.json PWidth", 1000000);
            if (slicer.contains("PHeight")) slicer_height = whole_number(slicer["PHeight"], "slicer.json PHeight", 1000000);
        }

        std::map<size_t, std::string> images;
        for (const auto &name : zip.ordered_names()) {
            if (name.find('/') != std::string::npos || !boost::algorithm::iends_with(name, ".png")) continue;
            const auto stem = name.substr(0, name.size() - 4);
            if (stem.empty() || stem.size() > 7 || !std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; }))
                continue; // 3d.png preview and other assets
            if (!images.emplace(size_t(std::stoul(stem)), name).second)
                throw RuntimeError("NanoDLP ZIP has two layer images numbered " + std::to_string(std::stoul(stem)));
        }
        if (images.empty())
            throw RuntimeError("NanoDLP ZIP contains no layer images");
        count = declared_count ? declared_count : images.rbegin()->first;
        if (!count || count > 100000)
            throw RuntimeError("NanoDLP layer count is zero or exceeds the editor limit");
        if (images.rbegin()->first > count)
            throw RuntimeError("NanoDLP layer image " + std::to_string(images.rbegin()->first) + " is beyond its declared " +
                               std::to_string(count) + " layers");
        m_names.reserve(count);
        for (size_t layer = 1; layer <= count; ++layer) {
            const auto found = images.find(layer);
            if (found == images.end())
                throw RuntimeError("NanoDLP layer image " + std::to_string(layer) + " is missing");
            m_names.push_back(found->second);
        }

        const auto first = read_header(zip.extract(m_names.front(), image_limit), 1);
        rows = first.height;
        if (first.color_type == 0) {
            variant = Variant::Gray;
            columns = first.width;
        } else if (slicer_width && slicer_width == first.width * 3) {
            variant = Variant::Packed;
            columns = slicer_width;
        } else {
            variant = Variant::FullRgb;
            columns = first.width;
        }
        if ((slicer_width && slicer_width != columns) || (slicer_height && slicer_height != rows))
            throw RuntimeError("NanoDLP layer images are " + std::to_string(columns) + "x" + std::to_string(rows) +
                               " but slicer.json declares " + std::to_string(slicer_width) + "x" + std::to_string(slicer_height));
        if (uint64_t(columns) * rows > 128ULL * 1024 * 1024)
            throw RuntimeError("NanoDLP raster exceeds the editor memory budget");
        format = std::string(variant == Variant::FullRgb ? "nanodlp_rgb" : "nanodlp") + (version == 1 ? "_v1" : "");
    }

    const std::string &layer_entry(size_t layer) const { return m_names.at(layer); }

    std::vector<uint8_t> decode(size_t layer) const
    {
        const std::string where = "NanoDLP layer " + std::to_string(layer + 1);
        const auto bytes = m_zip.extract(layer_entry(layer), image_limit);
        const auto header = read_header(bytes, layer + 1);
        const bool wanted_gray = variant == Variant::Gray;
        const size_t wanted_width = variant == Variant::Packed ? columns / 3 : columns;
        if (header.width != wanted_width || header.height != rows || (header.color_type == 0) != wanted_gray)
            throw RuntimeError(where + " differs in size or colour type from layer 1");
        png::ImageGreyscale image;
        if (variant == Variant::Packed) {
            // The shared helper reads BGR (CWS); NanoDLP keeps file byte order,
            // so swapping R and B again restores it exactly.
            if (!sla::decode_bgr_packed_mask({bytes.data(), bytes.size()}, columns, rows, image))
                throw RuntimeError(where + " is not a valid packed RGB mask");
            for (size_t i = 0; i + 2 < image.buf.size(); i += 3)
                std::swap(image.buf[i], image.buf[i + 2]);
        } else if (!png::decode_color_png({bytes.data(), bytes.size()}, image) || image.cols != columns || image.rows != rows)
            throw RuntimeError(where + " is not a valid PNG");
        return std::move(image.buf);
    }

    // Keeps the shape the file already uses so the printer firmware sees the
    // same layer format as before the edit.
    std::vector<uint8_t> encode(const std::vector<uint8_t> &pixels) const
    {
        sla::EncodedRaster encoded;
        if (variant == Variant::Gray)
            encoded = sla::PNGRasterEncoder{}(pixels.data(), columns, rows, 1);
        else if (variant == Variant::Packed)
            encoded = sla::PNGRasterEncoder{}(pixels.data(), columns / 3, rows, 3);
        else {
            std::vector<uint8_t> color(pixels.size() * 3);
            for (size_t i = 0; i < pixels.size(); ++i)
                color[3 * i] = color[3 * i + 1] = color[3 * i + 2] = pixels[i];
            encoded = sla::PNGRasterEncoder{}(color.data(), columns, rows, 3);
        }
        if (!encoded.size()) throw RuntimeError("Cannot encode NanoDLP layer image");
        const auto *data = static_cast<const uint8_t *>(encoded.data());
        return {data, data + encoded.size()};
    }

private:
    static constexpr uint64_t metadata_limit = 32ULL * 1024 * 1024;
    static constexpr uint64_t image_limit = 256ULL * 1024 * 1024;
    const MemoryZip &m_zip;
    std::vector<std::string> m_names;

    static nlohmann::json parse_json(const MemoryZip &zip, const char *name, bool required)
    {
        if (!zip.has(name)) {
            if (required) throw RuntimeError(std::string("NanoDLP ZIP is missing ") + name);
            return nlohmann::json::object();
        }
        try {
            auto json = nlohmann::json::parse(zip.extract(name, metadata_limit));
            if (!json.is_object()) throw RuntimeError(std::string("NanoDLP ") + name + " is not a JSON object");
            return json;
        } catch (const nlohmann::json::exception &error) {
            throw RuntimeError(std::string("NanoDLP ") + name + " is not valid JSON: " + error.what());
        }
    }
    static size_t whole_number(const nlohmann::json &value, const char *what, uint64_t maximum)
    {
        if (!value.is_number() || value.get<double>() < 0 || value.get<double>() > double(maximum) ||
            value.get<double>() != double(uint64_t(value.get<double>())))
            throw RuntimeError(std::string("NanoDLP ") + what + " is not a whole number within range");
        return size_t(value.get<double>());
    }
    static PngHeaderInfo read_header(const std::vector<uint8_t> &png, size_t layer)
    {
        const std::string where = "NanoDLP layer " + std::to_string(layer);
        const auto header = read_png_header(png, where);
        if (header.bit_depth != 8 || (header.color_type != 0 && header.color_type != 2))
            throw RuntimeError(where + " must be an 8-bit grayscale or RGB PNG");
        return header;
    }
};

} // namespace Slic3r

#endif
