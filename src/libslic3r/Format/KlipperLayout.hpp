// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader-side layout of the Klipper MSLA ZIP (klipper = grayscale PNG,
// klipper_rgb = full-width PNG24). Port of the UVtools KlipperFile.cs decode
// path (CanProcess, header/G-code scan, "<n>.png" layers numbered from one);
// the writer lives in KlipperArchive.cpp.
//
// Container: an empty ".klipper" marker, one G-code script (header comments
// ";Key:value" plus MSLA_DISPLAY_VALIDATE/PRINT_START commands), previews and
// one top-level "<n>.png" per layer.
#ifndef SLIC3R_KLIPPER_LAYOUT_HPP
#define SLIC3R_KLIPPER_LAYOUT_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>

#include "MemoryZip.hpp"
#include "NativeParameterLayout.hpp"
#include "PngHeaderProbe.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/SLA/RasterBase.hpp"

namespace Slic3r {

class KlipperLayout {
public:
    static constexpr const char *marker_name = ".klipper";

    // The same probe as UVtools: a ".klipper" marker and a G-code file.
    static bool recognizes(const std::set<std::string> &names)
    {
        bool gcode = false;
        for (const auto &name : names)
            if (boost::algorithm::iends_with(name, ".gcode")) gcode = true;
        return names.count(marker_name) && gcode;
    }
    static bool is_format(const std::string &format) { return format == "klipper" || format == "klipper_rgb"; }

    std::string format;
    size_t columns = 0, rows = 0, count = 0;
    bool rgb = false;

    explicit KlipperLayout(const MemoryZip &zip) : m_zip(zip)
    {
        std::vector<std::string> scripts;
        std::map<size_t, std::string> images;
        for (const auto &name : zip.ordered_names()) {
            if (boost::algorithm::iends_with(name, ".gcode")) scripts.push_back(name);
            if (name.find('/') != std::string::npos || !boost::algorithm::iends_with(name, ".png")) continue;
            const auto stem = name.substr(0, name.size() - 4);
            if (stem.empty() || stem.size() > 7 || !std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; }))
                continue; // preview.png, preview_cropping.png and other assets
            if (!images.emplace(size_t(std::stoul(stem)), name).second)
                throw RuntimeError("Klipper ZIP has two layer images numbered " + std::to_string(std::stoul(stem)));
        }
        if (scripts.size() != 1)
            throw RuntimeError(scripts.empty() ? "Klipper ZIP contains no G-code file" : "Klipper ZIP contains several G-code files");
        if (images.empty())
            throw RuntimeError("Klipper ZIP contains no layer images");

        m_script = scripts.front();
        const auto header = scan_script(zip.extract(scripts.front(), script_limit));
        count = header.layers ? header.layers : images.rbegin()->first;
        if (!count || count > 100000)
            throw RuntimeError("Klipper ZIP layer count is zero or exceeds the editor limit");
        // Layers are one-based and must form an unbroken run.
        if (images.rbegin()->first > count)
            throw RuntimeError("Klipper ZIP layer image " + std::to_string(images.rbegin()->first) + " is beyond its declared " +
                               std::to_string(count) + " layers");
        m_names.reserve(count);
        for (size_t layer = 1; layer <= count; ++layer) {
            const auto found = images.find(layer);
            if (found == images.end())
                throw RuntimeError("Klipper ZIP layer image " + std::to_string(layer) + " is missing");
            m_names.push_back(found->second);
        }

        // The first layer decides the raster and colour type; every other
        // layer must agree (checked again while decoding).
        const auto first = zip.extract(m_names.front(), image_limit);
        const auto ihdr = read_ihdr(first, 1);
        columns = ihdr.width; rows = ihdr.height; m_color_type = ihdr.color_type;
        rgb = ihdr.color_type == 2;
        format = rgb ? "klipper_rgb" : "klipper";
        if (header.columns && (header.columns != columns || header.rows != rows)) {
            // UVtools reads RGB AA files as packed (width = ResolutionX / 3).
            if (rgb && header.columns == columns * 3 && header.rows == rows)
                throw RuntimeError("Klipper packed RGB anti-aliased layers are not editable");
            throw RuntimeError("Klipper ZIP layer images are " + std::to_string(columns) + "x" + std::to_string(rows) +
                               " but its G-code declares " + std::to_string(header.columns) + "x" + std::to_string(header.rows));
        }
    }

    const std::string &layer_entry(size_t layer) const { return m_names.at(layer); }

    std::vector<uint8_t> decode(size_t layer) const
    {
        const auto bytes = m_zip.extract(layer_entry(layer), image_limit);
        const auto ihdr = read_ihdr(bytes, layer + 1);
        if (ihdr.width != columns || ihdr.height != rows || ihdr.color_type != m_color_type)
            throw RuntimeError("Klipper ZIP layer " + std::to_string(layer + 1) + " differs in size or colour type from layer 1");
        png::ImageGreyscale image;
        if (!png::decode_color_png({bytes.data(), bytes.size()}, image) || image.cols != columns || image.rows != rows)
            throw RuntimeError("Klipper ZIP layer " + std::to_string(layer + 1) + " is not a valid PNG");
        return std::move(image.buf);
    }

    // Same bytes the writer emits: gray PNG, or gray replicated into PNG24.
    std::vector<uint8_t> encode(const std::vector<uint8_t> &pixels) const
    {
        sla::EncodedRaster encoded;
        if (!rgb) encoded = sla::PNGRasterEncoder{}(pixels.data(), columns, rows, 1);
        else {
            std::vector<uint8_t> color(pixels.size() * 3);
            for (size_t i = 0; i < pixels.size(); ++i)
                color[3 * i] = color[3 * i + 1] = color[3 * i + 2] = pixels[i];
            encoded = sla::PNGRasterEncoder{}(color.data(), columns, rows, 3);
        }
        if (!encoded.size()) throw RuntimeError("Cannot encode Klipper layer image");
        const auto *data = static_cast<const uint8_t *>(encoded.data());
        return {data, data + encoded.size()};
    }


    // ---- Per-layer parameters -------------------------------------------------------------
    // The exporter's script (KlipperArchive.cpp) writes each layer as a block of
    // moves, waits and one M1400 exposure command. Everything in that block is
    // an editable field. Only a script in exactly that shape is editable: the
    // parser re-renders every block's motion lines and refuses any difference.
    static sla::LayerParameterCapabilities parameter_capabilities()
    {
        using F = sla::LayerParameter;
        return native_parameters::capabilities({F::PositionZ, F::Exposure, F::LiftDistance, F::LiftSpeed, F::LiftDistance2,
            F::LiftSpeed2, F::WaitAfterLift, F::RetractSpeed, F::RetractDistance2, F::RetractSpeed2, F::WaitBefore, F::WaitAfter,
            F::LightPWM});
    }
    // The script prints positions with six decimals and times as whole milliseconds.
    static double canonical(sla::LayerParameter field, double value)
    {
        using F = sla::LayerParameter;
        if (field == F::LightPWM) return value; // a fractional PWM is refused by the whole-number rule, not rounded
        if (field == F::Exposure || field == F::WaitBefore || field == F::WaitAfter || field == F::WaitAfterLift)
            return native_parameters::round_decimals(value, 1000.);
        return native_parameters::round_decimals(value, 1e6);
    }
    // A phase's speed exists in the script only while its distance does.
    static void check_storable(const native_parameters::Row &row)
    {
        using F = sla::LayerParameter;
        if (native_parameters::value_of(row, F::LiftDistance2) <= 0 && native_parameters::value_of(row, F::LiftSpeed2) != 0)
            throw RuntimeError("Klipper stores lift speed 2 only together with a positive lift distance 2");
        if (native_parameters::value_of(row, F::RetractDistance2) <= 0 && native_parameters::value_of(row, F::RetractSpeed2) != 0)
            throw RuntimeError("Klipper stores retract speed 2 only together with a positive retract distance 2");
    }
    native_parameters::Table parameter_rows() const { return parse_script().rows; }
    double height_limit() const
    {
        const auto parsed = parse_script();
        const auto found = parsed.comment_line.find("MachineZ");
        return found == parsed.comment_line.end() ? 0. : comment_number(parsed.lines[found->second]);
    }
    size_t bottom_layers() const
    {
        const auto parsed = parse_script();
        const auto found = parsed.comment_line.find("BottomLayerCount");
        return found == parsed.comment_line.end() ? 0 : size_t(std::max(0., comment_number(parsed.lines[found->second])));
    }
    // The script with `table` and the lit areas applied: per-layer motion, Z,
    // pixel/box/area statistics, and the totals (volume, material, price,
    // print time) that depend on them. Empty when nothing changed.
    std::map<std::string, std::vector<uint8_t>> parameter_replacements(const native_parameters::Table &table,
        const std::vector<native_parameters::MaskSummary> &old_masks,
        const std::vector<native_parameters::MaskSummary> &new_masks) const
    {
        using F = sla::LayerParameter;
        auto parsed = parse_script();
        if (table.size() != count || old_masks.size() != count || new_masks.size() != count)
            throw RuntimeError("Klipper parameter table has the wrong layer count");
        const double px = parsed.pixel_x, py = parsed.pixel_y;
        const auto &lines = parsed.lines;
        std::vector<std::vector<std::string>> blocks(count);
        double old_volume = 0, new_volume = 0, old_seconds = 0, new_seconds = 0, old_z = 0, new_z = 0;
        bool changed = false, masks_changed = false;
        native_parameters::MaskSummary box;
        std::vector<LayerStat> stats = parsed.stats;
        for (size_t i = 0; i < count; ++i) {
            const auto &before = parsed.rows[i];
            const auto &row = table[i];
            check_storable(row);
            auto &stat = stats[i];
            const bool mask_here = old_masks[i] != new_masks[i];
            if (mask_here) {
                const auto &m = new_masks[i];
                stat.pixels = m.lit;
                stat.x = m.left * px; stat.y = m.top * py; stat.w = (m.right - m.left) * px; stat.h = (m.bottom - m.top) * py;
                stat.area = double(m.lit) * px * py;
                masks_changed = true;
            }
            const double thickness_old = before.at(F::PositionZ) - old_z, thickness_new = row.at(F::PositionZ) - new_z;
            old_volume += parsed.stats[i].area * thickness_old;
            new_volume += stat.area * thickness_new;
            old_seconds += native_parameters::layer_seconds(before);
            new_seconds += native_parameters::layer_seconds(row);
            const bool motion_here = before != row;
            const bool volume_here = mask_here || thickness_old != thickness_new;
            const auto [begin, end] = parsed.blocks[i];
            std::vector<std::string> block(lines.begin() + begin, lines.begin() + end);
            if (motion_here || volume_here) {
                changed = true;
                std::string &stats_line = block[2];
                if (motion_here && before.at(F::PositionZ) != row.at(F::PositionZ)) replace_token(stats_line, "HEIGHT=", number(row.at(F::PositionZ)));
                if (mask_here) {
                    replace_token(stats_line, "PIXELS=", std::to_string(stat.pixels));
                    replace_token(stats_line, "BOUNDS=", number(stat.x) + "," + number(stat.y) + "," + number(stat.w) + "," + number(stat.h));
                    replace_token(stats_line, "AREA=", number(stat.area));
                }
                if (volume_here) {
                    const double layer_volume = stat.area * thickness_new;
                    replace_token(stats_line, "VOLUME=", number(layer_volume));
                    replace_token(stats_line, "MATERIALML=", number(layer_volume / 1000));
                    replace_token(block[parsed.consume_offset[i]], "CONSUME_MATERIAL=", number(layer_volume / 1000));
                }
                if (motion_here) {
                    const auto motion = motion_lines(row);
                    block.erase(block.begin() + parsed.motion_begin[i], block.begin() + parsed.motion_end[i]);
                    block.insert(block.begin() + parsed.motion_begin[i], motion.begin(), motion.end());
                }
            }
            blocks[i] = std::move(block);
            old_z = before.at(F::PositionZ); new_z = row.at(F::PositionZ);
            if (new_masks[i].lit) {
                const auto &m = new_masks[i];
                if (!box.lit) box = m;
                else {
                    box.left = std::min(box.left, m.left); box.top = std::min(box.top, m.top);
                    box.right = std::max(box.right, m.right); box.bottom = std::max(box.bottom, m.bottom);
                    box.lit += m.lit;
                }
            }
        }
        if (!changed) return {};
        // Rebuild the text: head lines (patched), blocks, tail lines.
        std::vector<std::string> out(lines.begin(), lines.begin() + parsed.blocks.front().first);
        const double ratio = old_volume > 0 ? new_volume / old_volume : 1.;
        auto scale_comment = [&](const char *key) {
            const auto found = parsed.comment_line.find(key);
            if (found == parsed.comment_line.end()) return;
            out[found->second] = ";" + std::string(key) + ":" + number(comment_number(out[found->second]) * ratio);
        };
        if (ratio != 1.) for (const char *key : {"Volume", "MaterialMl", "MaterialGrams", "Price"}) scale_comment(key);
        if (const auto found = parsed.comment_line.find("PrintTime"); found != parsed.comment_line.end())
            out[found->second] = ";PrintTime:" + number(std::max(0., comment_number(out[found->second]) + (new_seconds - old_seconds)));
        if (parsed.stats_line != npos && ratio != 1.)
            replace_token(out[parsed.stats_line], "MATERIAL_TOTAL=", number(token_number(out[parsed.stats_line], "MATERIAL_TOTAL=") * ratio));
        if (parsed.start_line != npos) {
            auto &start = out[parsed.start_line];
            replace_token(start, "HEIGHT=", number(table.back().at(F::PositionZ)));
            if (masks_changed && box.lit)
                replace_token(start, "BOUNDS=", number(box.left * px) + "," + number(box.top * py) + "," +
                    number((box.right - box.left) * px) + "," + number((box.bottom - box.top) * py));
            if (ratio != 1.) {
                replace_token(start, "VOLUME=", number(token_number(start, "VOLUME=") * ratio));
                replace_token(start, "MATERIALML=", number(token_number(start, "MATERIALML=") * ratio));
            }
        }
        for (const auto &block : blocks) out.insert(out.end(), block.begin(), block.end());
        out.insert(out.end(), lines.begin() + parsed.blocks.back().second, lines.end());
        std::string text;
        for (size_t i = 0; i < out.size(); ++i) { text += out[i]; if (i + 1 < out.size()) text += '\n'; }
        return {{m_script, std::vector<uint8_t>(text.begin(), text.end())}};
    }

private:
    static constexpr size_t npos = size_t(-1);
    std::string m_script;

    struct LayerStat { uint64_t pixels = 0; double x = 0, y = 0, w = 0, h = 0, area = 0; };
    struct Parsed {
        std::vector<std::string> lines;
        std::vector<std::pair<size_t, size_t>> blocks;              // [begin, end) line range of each layer
        std::vector<size_t> motion_begin, motion_end, consume_offset; // relative to the block
        native_parameters::Table rows;
        std::vector<LayerStat> stats;
        std::map<std::string, size_t> comment_line;                  // header ";Key:value"
        size_t stats_line = npos, start_line = npos;
        double pixel_x = 0, pixel_y = 0;
    };

    // Six decimals, like the exporter's `std::fixed << setprecision(6)`.
    static std::string number(double value)
    {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::fixed << std::setprecision(6) << value;
        return out.str();
    }
    static std::string wait_text(double seconds)
    {
        return seconds == 0 ? std::string(".0") : std::to_string(uint32_t(std::llround(seconds * 1000)));
    }
    static double comment_number(const std::string &line)
    {
        return std::strtod(line.c_str() + line.find(':') + 1, nullptr);
    }
    static std::string token(const std::string &line, const char *key)
    {
        const auto at = line.find(key);
        if (at == std::string::npos) throw RuntimeError(std::string("Klipper script line lacks ") + key);
        const auto start = at + std::strlen(key);
        const auto end = line.find(' ', start);
        return line.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
    static double token_number(const std::string &line, const char *key) { return std::strtod(token(line, key).c_str(), nullptr); }
    static void replace_token(std::string &line, const char *key, const std::string &value)
    {
        const auto at = line.find(key);
        if (at == std::string::npos) throw RuntimeError(std::string("Klipper script line lacks ") + key);
        const auto start = at + std::strlen(key);
        const auto end = line.find(' ', start);
        line.replace(start, end == std::string::npos ? std::string::npos : end - start, value);
    }
    // The moves, waits and exposure command of one layer, in the exporter's order.
    static std::vector<std::string> motion_lines(const native_parameters::Row &row)
    {
        using F = sla::LayerParameter;
        auto v = [&](F field) { return native_parameters::value_of(row, field); };
        const double z = v(F::PositionZ), lift = v(F::LiftDistance), lift2 = v(F::LiftDistance2), retract2 = v(F::RetractDistance2);
        std::vector<std::string> out;
        out.push_back("G1 Z" + number(z + lift) + " F" + number(v(F::LiftSpeed)));
        if (lift2 > 0) out.push_back("G1 Z" + number(z + lift + lift2) + " F" + number(v(F::LiftSpeed2)));
        out.push_back("M400");
        out.push_back("G4 P" + wait_text(v(F::WaitAfterLift)));
        out.push_back("G1 Z" + number(z + retract2) + " F" + number(v(F::RetractSpeed)));
        if (retract2 > 0) out.push_back("G1 Z" + number(z) + " F" + number(v(F::RetractSpeed2)));
        out.push_back("M400");
        out.push_back("G4 P" + wait_text(v(F::WaitBefore)));
        out.push_back("M1400 S" + std::to_string(int(v(F::LightPWM))) + " P" + std::to_string(uint32_t(std::llround(v(F::Exposure) * 1000))));
        out.push_back("G4 P" + wait_text(v(F::WaitAfter)));
        return out;
    }
    // A "G1 Z<z> F<speed>" line.
    static bool parse_move(const std::string &line, double &z, double &speed)
    {
        if (!boost::algorithm::starts_with(line, "G1 Z")) return false;
        char *rest = nullptr;
        z = std::strtod(line.c_str() + 4, &rest);
        if (!rest || std::strncmp(rest, " F", 2) != 0) return false;
        speed = std::strtod(rest + 2, &rest);
        return rest && !*rest;
    }
    static bool parse_wait(const std::string &line, double &seconds)
    {
        if (!boost::algorithm::starts_with(line, "G4 P")) return false;
        if (line == "G4 P.0") { seconds = 0; return true; }
        char *rest = nullptr;
        const auto ms = std::strtoull(line.c_str() + 4, &rest, 10);
        if (!rest || *rest || rest == line.c_str() + 4) return false;
        seconds = double(ms) / 1000.;
        return true;
    }

    // Reads the script into rows. Throws when it is not the exporter's shape.
    Parsed parse_script() const
    {
        using F = sla::LayerParameter;
        const auto bytes = m_zip.extract(m_script, script_limit);
        const std::string text(bytes.begin(), bytes.end());
        Parsed parsed;
        for (size_t at = 0;;) {
            const auto end = text.find('\n', at);
            if (end == std::string::npos) { parsed.lines.push_back(text.substr(at)); break; }
            parsed.lines.push_back(text.substr(at, end - at));
            at = end + 1;
        }
        auto &lines = parsed.lines;
        const std::string where = "Klipper script is not in the exporter's layout: ";
        size_t at = 0;
        for (; at < lines.size() && !lines[at].empty() && lines[at][0] == ';'; ++at) {
            const auto colon = lines[at].find(':');
            if (colon != std::string::npos) parsed.comment_line.emplace(lines[at].substr(1, colon - 1), at);
        }
        for (const char *key : {"ResolutionX", "ResolutionY", "MachineX", "DisplayHeight", "LayerCount"})
            if (!parsed.comment_line.count(key)) throw RuntimeError(where + "header field " + key + " is missing");
        const double columns = comment_number(lines[parsed.comment_line["ResolutionX"]]), rows = comment_number(lines[parsed.comment_line["ResolutionY"]]);
        if (!(columns > 0) || !(rows > 0)) throw RuntimeError(where + "invalid resolution");
        parsed.pixel_x = comment_number(lines[parsed.comment_line["MachineX"]]) / columns;
        parsed.pixel_y = comment_number(lines[parsed.comment_line["DisplayHeight"]]) / rows;
        for (; at < lines.size() && !boost::algorithm::starts_with(lines[at], ";LAYER_START:"); ++at) {
            if (boost::algorithm::starts_with(lines[at], "SET_PRINT_STATS_INFO ") && parsed.stats_line == npos) parsed.stats_line = at;
            if (boost::algorithm::starts_with(lines[at], "PRINT_START ")) parsed.start_line = at;
        }
        if (parsed.stats_line == npos || parsed.start_line == npos) throw RuntimeError(where + "no PRINT_START");
        for (size_t layer = 0; layer < count; ++layer) {
            const size_t begin = at;
            auto need = [&](const char *prefix) -> const std::string & {
                if (at >= lines.size() || !boost::algorithm::starts_with(lines[at], prefix))
                    throw RuntimeError(where + "layer " + std::to_string(layer + 1) + " lacks '" + prefix + "'");
                return lines[at++];
            };
            if (at >= lines.size() || lines[at] != ";LAYER_START: " + std::to_string(layer))
                throw RuntimeError(where + "layer " + std::to_string(layer + 1) + " does not start where expected");
            ++at;
            need("SET_PRINT_STATS_INFO CURRENT_LAYER=");
            const auto &before = need("BEFORE_LAYER_CHANGE ");
            need("M1451 F");
            const size_t motion_first = at;
            LayerStat stat;
            const double z = token_number(before, "HEIGHT=");
            stat.pixels = std::strtoull(token(before, "PIXELS=").c_str(), nullptr, 10);
            stat.area = token_number(before, "AREA=");
            {
                const auto bounds = token(before, "BOUNDS=");
                if (std::sscanf(bounds.c_str(), "%lf,%lf,%lf,%lf", &stat.x, &stat.y, &stat.w, &stat.h) != 4)
                    throw RuntimeError(where + "layer " + std::to_string(layer + 1) + " has invalid BOUNDS");
            }
            double lift_z = 0, lift_speed = 0, lift2_z = 0, lift2_speed = 0, wait_lift = 0, back_z = 0, back_speed = 0, back2_speed = 0,
                   wait_before = 0, wait_after = 0;
            if (!parse_move(at < lines.size() ? lines[at] : "", lift_z, lift_speed)) throw RuntimeError(where + "layer " + std::to_string(layer + 1) + " lacks its lift move");
            ++at;
            const bool second_lift = at < lines.size() && parse_move(lines[at], lift2_z, lift2_speed);
            if (second_lift) ++at;
            if (at >= lines.size() || lines[at++] != "M400" || at >= lines.size() || !parse_wait(lines[at++], wait_lift))
                throw RuntimeError(where + "layer " + std::to_string(layer + 1) + " lacks its lift wait");
            if (!parse_move(at < lines.size() ? lines[at] : "", back_z, back_speed)) throw RuntimeError(where + "layer " + std::to_string(layer + 1) + " lacks its retract move");
            ++at;
            double final_z = 0;
            const bool second_back = at < lines.size() && parse_move(lines[at], final_z, back2_speed);
            if (second_back) ++at;
            if (at >= lines.size() || lines[at++] != "M400" || at >= lines.size() || !parse_wait(lines[at++], wait_before))
                throw RuntimeError(where + "layer " + std::to_string(layer + 1) + " lacks its pre-cure wait");
            const auto &cure = need("M1400 S");
            int pwm = 0;
            unsigned long long exposure_ms = 0;
            if (std::sscanf(cure.c_str(), "M1400 S%d P%llu", &pwm, &exposure_ms) != 2)
                throw RuntimeError(where + "layer " + std::to_string(layer + 1) + " has an invalid exposure command");
            if (at >= lines.size() || !parse_wait(lines[at++], wait_after))
                throw RuntimeError(where + "layer " + std::to_string(layer + 1) + " lacks its post-cure wait");
            const size_t motion_last = at;
            need("M1450");
            const size_t consume = at - begin;
            need("SET_PRINT_STATS_INFO CONSUME_MATERIAL=");
            need("AFTER_LAYER_CHANGE");
            if (at >= lines.size() || lines[at++] != ";LAYER_END") throw RuntimeError(where + "layer " + std::to_string(layer + 1) + " lacks ;LAYER_END");
            native_parameters::Row row;
            auto rd = [](double value) { return native_parameters::round_decimals(value, 1e6); };
            row[F::PositionZ] = rd(z);
            row[F::LiftDistance] = rd(lift_z - z);
            row[F::LiftSpeed] = rd(lift_speed);
            row[F::LiftDistance2] = second_lift ? rd(lift2_z - lift_z) : 0.;
            row[F::LiftSpeed2] = second_lift ? rd(lift2_speed) : 0.;
            row[F::WaitAfterLift] = wait_lift;
            row[F::RetractDistance2] = rd(back_z - z);
            row[F::RetractSpeed] = rd(back_speed);
            row[F::RetractSpeed2] = second_back ? rd(back2_speed) : 0.;
            row[F::WaitBefore] = wait_before;
            row[F::Exposure] = double(exposure_ms) / 1000.;
            row[F::WaitAfter] = wait_after;
            row[F::LightPWM] = pwm;
            for (const auto &[field, value] : row)
                if (!std::isfinite(value) || value < 0)
                    throw RuntimeError(where + "layer " + std::to_string(layer + 1) + " has a negative or invalid value");
            // The exporter's own rendering of this row must reproduce the original motion lines.
            const auto expected = motion_lines(row);
            if (expected.size() != motion_last - motion_first || !std::equal(expected.begin(), expected.end(), lines.begin() + motion_first))
                throw RuntimeError(where + "layer " + std::to_string(layer + 1) + " motion differs from the exporter's");
            parsed.blocks.push_back({begin, at});
            parsed.motion_begin.push_back(motion_first - begin);
            parsed.motion_end.push_back(motion_last - begin);
            parsed.consume_offset.push_back(consume);
            parsed.rows.push_back(std::move(row));
            parsed.stats.push_back(stat);
        }
        return parsed;
    }

    static constexpr uint64_t script_limit = 64ULL * 1024 * 1024;
    static constexpr uint64_t image_limit = 256ULL * 1024 * 1024;
    const MemoryZip &m_zip;
    std::vector<std::string> m_names;
    unsigned m_color_type = 0;

    static PngHeaderInfo read_ihdr(const std::vector<uint8_t> &png, size_t layer)
    {
        const std::string where = "Klipper ZIP layer " + std::to_string(layer);
        const auto ihdr = read_png_header(png, where);
        // The writer emits 8-bit gray (mono) or 8-bit RGB (rgb.zip); anything
        // else would not survive an edit unchanged.
        if (ihdr.bit_depth != 8 || (ihdr.color_type != 0 && ihdr.color_type != 2))
            throw RuntimeError(where + " must be an 8-bit grayscale or RGB PNG");
        return ihdr;
    }

    struct Header { size_t columns = 0, rows = 0, layers = 0; };
    // Reads only what identifies the raster; per-layer motion and statistics
    // stay in the script untouched.
    static Header scan_script(const std::vector<uint8_t> &bytes)
    {
        Header header;
        size_t print_start_layers = 0, stats_layers = 0;
        std::map<std::string, std::string> comments;
        const std::string text(bytes.begin(), bytes.end());
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            const auto line = boost::algorithm::trim_copy(text.substr(at, end - at));
            at = end + 1;
            if (line.empty()) continue;
            if (line[0] == ';') {
                const auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                comments.emplace(boost::algorithm::trim_copy(line.substr(1, colon - 1)), boost::algorithm::trim_copy(line.substr(colon + 1)));
            } else if (boost::algorithm::istarts_with(line, "MSLA_DISPLAY_VALIDATE ")) {
                const auto at_resolution = line.find("RESOLUTION=");
                if (at_resolution != std::string::npos) {
                    char *rest = nullptr;
                    const auto x = std::strtoull(line.c_str() + at_resolution + 11, &rest, 10);
                    if (rest && *rest == ',') {
                        header.columns = x;
                        header.rows = std::strtoull(rest + 1, nullptr, 10);
                    }
                }
            } else if (boost::algorithm::istarts_with(line, "PRINT_START ")) {
                print_start_layers = keyed_number(line, "LAYERS=");
            } else if (boost::algorithm::istarts_with(line, "SET_PRINT_STATS_INFO ") && !stats_layers) {
                stats_layers = keyed_number(line, "TOTAL_LAYER=");
            }
        }
        // ";Key:value" header comments win over commands, as in UVtools.
        auto integral = [&](const char *key, size_t &target) {
            const auto found = comments.find(key);
            if (found == comments.end()) return;
            char *rest = nullptr;
            const double value = std::strtod(found->second.c_str(), &rest);
            if (rest == found->second.c_str() || *rest || !std::isfinite(value) || value < 0 || value != std::floor(value) || value > 1e9)
                throw RuntimeError("Klipper ZIP header field " + std::string(key) + " is not a whole number");
            target = size_t(value);
        };
        integral("ResolutionX", header.columns);
        integral("ResolutionY", header.rows);
        integral("LayerCount", header.layers);
        if (!header.layers) header.layers = print_start_layers ? print_start_layers : stats_layers;
        return header;
    }
    static size_t keyed_number(const std::string &line, const char *key)
    {
        const auto at = line.find(key);
        return at == std::string::npos ? 0 : size_t(std::strtoull(line.c_str() + at + std::strlen(key), nullptr, 10));
    }
};

} // namespace Slic3r

#endif
