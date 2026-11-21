// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader-side layout of the ZIP based ZCode (UnizMaker IBEE) and ZCodex
// (Zortrax Z-Suite) formats. Port of the UVtools ZCodeFile.cs / ZCodexFile.cs
// decode paths; the writers live in ZCodeArchive.cpp.
//
//   ZCode   task.xml (<Print><Job><layers>N</layers>), lcd.gcode (RSA-encrypted
//           command lines), preview.png and "<n>.png" layers numbered from one
//   ZCodex  ResinMetadata (JSON "TotalLayersCount"), UserSettingsData,
//           ZCodeMetadata, ResinGCodeData, Preview.png and
//           "ResinSlicesData/Slice<nnnnn>.png" layers numbered from zero
//
// The masks are always editable. The per-layer print parameters are stored in
// the G-code program (UVtools ZCodeFile.cs / ZCodexFile.cs
// PrintParameterPerLayerModifiers) and are editable when the program has the
// exact shape our exporter writes (every layer block is re-rendered and
// compared): ZCode keeps the whole per-layer program (Z, exposure, lifts,
// retracts, waits, PWM), ZCodex only Z, lift, lift/retract speed and PWM (its
// exposure and rest are global fields of ResinMetadata). Anything else (the
// previews, the manifests' other fields, untouched layers) is carried over
// byte for byte. Files of other writers (UVtools) are edited the same way: the zero-wait spelling
// ("G4 P0;" vs "G4 P.0;") and the metadata JSON layout (key order, indent) are read from the file
// and copied by every rewritten block / document, so nothing else changes.
#ifndef SLIC3R_ZCODE_LAYOUT_HPP
#define SLIC3R_ZCODE_LAYOUT_HPP

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <nlohmann/json.hpp>

#include "MemoryZip.hpp"
#include "NativeParameterLayout.hpp"
#include "PngHeaderProbe.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "ZCodeRSA.hpp"

#include <cstdio>
#include <cstring>
#include <iomanip>
#include <locale>
#include <optional>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

namespace Slic3r {

class ZCodeLayout {
public:
    enum class Kind { ZCode, ZCodex };

    static bool recognizes(const std::set<std::string> &names, Kind kind)
    {
        if (kind == Kind::ZCode)
            return names.count("task.xml") && names.count("lcd.gcode");
        return names.count("ResinMetadata") && names.count("ZCodeMetadata") && names.count("ResinGCodeData");
    }
    static bool is_format(const std::string &format) { return format == "zcode" || format == "zcodex"; }

    std::string format;
    size_t columns = 0, rows = 0, count = 0;

    ZCodeLayout(const MemoryZip &zip, Kind kind) : m_zip(zip)
    {
        m_kind = kind;
        format = kind == Kind::ZCode ? "zcode" : "zcodex";
        m_label = kind == Kind::ZCode ? "ZCode" : "ZCodex";
        const size_t declared = kind == Kind::ZCode ? declared_zcode_layers(zip) : declared_zcodex_layers(zip);

        // ZCode numbers layers from one, ZCodex from zero (UVtools IndexStartNumber).
        const size_t first_index = kind == Kind::ZCode ? 1 : 0;
        std::map<size_t, std::string> images;
        for (const auto &name : zip.ordered_names()) {
            std::string stem;
            if (kind == Kind::ZCode) {
                if (name.find('/') != std::string::npos || !boost::algorithm::iends_with(name, ".png")) continue;
                stem = name.substr(0, name.size() - 4);
            } else {
                static const std::string prefix = "ResinSlicesData/Slice";
                if (!boost::algorithm::starts_with(name, prefix) || !boost::algorithm::iends_with(name, ".png")) continue;
                stem = name.substr(prefix.size(), name.size() - prefix.size() - 4);
            }
            if (stem.empty() || stem.size() > 7 || !std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; }))
                continue; // preview.png and other assets
            if (!images.emplace(size_t(std::stoul(stem)), name).second)
                throw RuntimeError(m_label + " has two layer images numbered " + std::to_string(std::stoul(stem)));
        }
        if (images.empty())
            throw RuntimeError(m_label + " contains no layer images");
        count = declared ? declared : images.rbegin()->first + 1 - first_index;
        if (!count || count > 100000)
            throw RuntimeError(m_label + " layer count is zero or exceeds the editor limit");
        if (images.rbegin()->first >= first_index + count)
            throw RuntimeError(m_label + " layer image " + std::to_string(images.rbegin()->first) + " is beyond its declared " +
                               std::to_string(count) + " layers");
        m_names.reserve(count);
        for (size_t layer = 0; layer < count; ++layer) {
            const auto found = images.find(first_index + layer);
            if (found == images.end())
                throw RuntimeError(m_label + " layer image " + std::to_string(first_index + layer) + " is missing");
            m_names.push_back(found->second);
        }
        const auto first = header_of(zip.extract(m_names.front(), image_limit), 0);
        columns = first.width; rows = first.height;
    }

    const std::string &layer_entry(size_t layer) const { return m_names.at(layer); }

    std::vector<uint8_t> decode(size_t layer) const
    {
        const auto bytes = m_zip.extract(layer_entry(layer), image_limit);
        const auto header = header_of(bytes, layer);
        if (header.width != columns || header.height != rows)
            throw RuntimeError(m_label + " layer " + std::to_string(layer + 1) + " differs in size from layer 1");
        png::ImageGreyscale image;
        if (!png::decode_color_png({bytes.data(), bytes.size()}, image) || image.cols != columns || image.rows != rows)
            throw RuntimeError(m_label + " layer " + std::to_string(layer + 1) + " is not a valid PNG");
        return std::move(image.buf);
    }

    // The exporter's own encoding: plain 8-bit gray PNG.
    std::vector<uint8_t> encode(const std::vector<uint8_t> &pixels) const
    {
        const auto encoded = sla::PNGRasterEncoder{}(pixels.data(), columns, rows, 1);
        if (!encoded.size()) throw RuntimeError("Cannot encode " + m_label + " layer image");
        const auto *data = static_cast<const uint8_t *>(encoded.data());
        return {data, data + encoded.size()};
    }


    // ---- Per-layer parameters -------------------------------------------------------------
    static sla::LayerParameterCapabilities parameter_capabilities(Kind kind)
    {
        using F = sla::LayerParameter;
        if (kind == Kind::ZCodex) return native_parameters::capabilities({F::PositionZ, F::LiftDistance, F::LiftSpeed, F::RetractSpeed, F::LightPWM});
        return native_parameters::capabilities({F::PositionZ, F::Exposure, F::LiftDistance, F::LiftSpeed, F::LiftDistance2, F::LiftSpeed2,
            F::WaitAfterLift, F::RetractSpeed, F::RetractDistance2, F::RetractSpeed2, F::WaitBefore, F::WaitAfter, F::LightPWM});
    }
    // What the program reads back after `value` is written: distances and times at
    // 0.001 (the G-code prints three decimals; times are whole milliseconds), ZCode speeds
    // through the centimetre-per-minute unit, ZCodex speeds as the whole numbers its decoder reads.
    static double canonical(Kind kind, sla::LayerParameter field, double value)
    {
        using F = sla::LayerParameter;
        if (field == F::LightPWM) return value; // a fractional PWM is refused by the whole-number rule, not rounded
        const bool speed = field == F::LiftSpeed || field == F::LiftSpeed2 || field == F::RetractSpeed || field == F::RetractSpeed2;
        if (speed) return kind == Kind::ZCode ? native_parameters::round_decimals(native_parameters::round_decimals(value / 10., 1000.) * 10., 100.)
                                              : native_parameters::round_decimals(value, 1.);
        return native_parameters::round_decimals(value, 1000.);
    }
    // Rules the program's own grammar imposes, so that a row never reads back differently.
    static void check_storable(Kind kind, const native_parameters::Row &row)
    {
        using F = sla::LayerParameter;
        auto v = [&](F field) { return native_parameters::value_of(row, field); };
        auto positive = [](double x) { return native_parameters::round_decimals(x, 1000.) > 0; };
        const std::string family = kind == Kind::ZCode ? "ZCode" : "ZCodex";
        const double lift = v(F::LiftDistance), lift2 = v(F::LiftDistance2), retract2 = v(F::RetractDistance2);
        const double pwm = v(F::LightPWM);
        if (pwm < 1 || pwm > 255 || pwm != std::floor(pwm))
            throw RuntimeError(family + " light PWM must be a whole number from 1 to 255");
        if (kind == Kind::ZCodex) {
            if (v(F::LiftSpeed) < 1 || (positive(lift) && v(F::RetractSpeed) < 1))
                throw RuntimeError("ZCodex needs a lift speed of at least 1 mm/min, and a retract speed with an active lift");
            if (!positive(lift) && v(F::RetractSpeed) != 0)
                throw RuntimeError("ZCodex stores the retract speed only together with a positive lift distance");
            return;
        }
        if (!positive(lift) && positive(lift2))
            throw RuntimeError("ZCode stores a second lift only together with a first lift");
        if (!positive(lift2) && v(F::LiftSpeed2) != 0)
            throw RuntimeError("ZCode stores lift speed 2 only together with a positive lift distance 2");
        if (!positive(retract2) && v(F::RetractSpeed2) != 0)
            throw RuntimeError("ZCode stores retract speed 2 only together with a positive retract distance 2");
        const double total = lift + lift2;
        if (positive(total) && !positive(total - retract2) && positive(retract2))
            throw RuntimeError("ZCode cannot tell a retract equal to the whole lift from a plain retract; keep retract distance 2 below the total lift");
        if (!positive(total - retract2) && v(F::RetractSpeed) != 0)
            throw RuntimeError("ZCode stores the retract speed only together with a positive retract of the lift");
        if (!positive(total) && v(F::LiftSpeed) <= 0)
            throw RuntimeError("ZCode stores a Z advance that needs a positive lift speed");
        for (const auto &line : zcode_block(row, 0))
            if (line.size() > 61) throw RuntimeError("A ZCode G-code line is longer than the protocol's 61 bytes");
    }

    native_parameters::Table parameter_rows() const
    {
        return m_kind == Kind::ZCode ? parse_zcode().rows : parse_zcodex().rows;
    }
    double height_limit() const
    {
        if (m_kind != Kind::ZCode) return 0.;
        const auto tree = task_tree();
        return tree.get<double>("Print.Device.<xmlattr>.z", 0.);
    }
    size_t bottom_layers() const
    {
        if (m_kind == Kind::ZCodex) return size_t(std::max<int64_t>(0, resin_json().value("BottomLayersNumber", int64_t(0))));
        const auto tree = task_tree();
        return size_t(std::max(0, tree.get<int>("Print.Profile.Slice.<xmlattr>.bottom_layers", 0)));
    }

    // The entries rewritten for `table` and the new lit areas (task.xml + lcd.gcode for ZCode;
    // the three JSON files + ResinGCodeData for ZCodex). Empty when nothing changed.
    std::map<std::string, std::vector<uint8_t>> parameter_replacements(const native_parameters::Table &table,
        const std::vector<native_parameters::MaskSummary> &old_masks,
        const std::vector<native_parameters::MaskSummary> &new_masks) const
    {
        if (table.size() != count || old_masks.size() != count || new_masks.size() != count)
            throw RuntimeError(m_label + " parameter table has the wrong layer count");
        return m_kind == Kind::ZCode ? zcode_replacements(table, old_masks, new_masks) : zcodex_replacements(table, old_masks, new_masks);
    }

    // The Z step check the ZCodex decoder needs: a lift smaller than the layer thickness would
    // write a negative retract. Other families have no such rule.
    static void check_step(Kind kind, const native_parameters::Row &row, double previous_z)
    {
        if (kind != Kind::ZCodex) return;
        using F = sla::LayerParameter;
        const double lift = native_parameters::value_of(row, F::LiftDistance);
        const double step = native_parameters::round_decimals(native_parameters::value_of(row, F::PositionZ) - previous_z, 1000.);
        if (lift > 0 && step >= lift)
            throw RuntimeError("ZCodex needs the lift distance to be larger than the layer thickness");
    }

private:
    static constexpr uint64_t metadata_limit = 32ULL * 1024 * 1024;
    static constexpr uint64_t image_limit = 256ULL * 1024 * 1024;
    const MemoryZip &m_zip;
    std::string m_label;
    std::vector<std::string> m_names;

    PngHeaderInfo header_of(const std::vector<uint8_t> &png, size_t layer) const
    {
        const std::string where = m_label + " layer " + std::to_string(layer + 1);
        const auto header = read_png_header(png, where);
        // The exporter writes 8-bit gray; another shape could not be written back unchanged.
        if (header.bit_depth != 8 || header.color_type != 0)
            throw RuntimeError(where + " must be an 8-bit grayscale PNG");
        return header;
    }

    // <Print><Job><layers>N</layers>; the XML is data, so no DTD or entities.
    static size_t declared_zcode_layers(const MemoryZip &zip)
    {
        const auto bytes = zip.extract("task.xml", metadata_limit);
        const std::string text(bytes.begin(), bytes.end());
        if (text.find("<!DOCTYPE") != std::string::npos || text.find("<!ENTITY") != std::string::npos)
            throw RuntimeError("ZCode task.xml must not contain a DTD or entities");
        boost::property_tree::ptree tree;
        try {
            std::istringstream stream(text);
            boost::property_tree::read_xml(stream, tree, boost::property_tree::xml_parser::trim_whitespace);
        } catch (const boost::property_tree::xml_parser_error &error) {
            throw RuntimeError(std::string("ZCode task.xml is not valid XML: ") + error.message());
        }
        const auto layers = tree.get_optional<std::string>("Print.Job.layers");
        if (!layers) return 0;
        return whole_number(*layers, "ZCode task.xml layers");
    }
    static size_t declared_zcodex_layers(const MemoryZip &zip)
    {
        nlohmann::json json;
        try {
            json = nlohmann::json::parse(zip.extract("ResinMetadata", metadata_limit));
        } catch (const nlohmann::json::exception &error) {
            throw RuntimeError(std::string("ZCodex ResinMetadata is not valid JSON: ") + error.what());
        }
        if (!json.is_object()) throw RuntimeError("ZCodex ResinMetadata is not a JSON object");
        if (!json.contains("TotalLayersCount")) return 0;
        const auto &value = json["TotalLayersCount"];
        if (!value.is_number_integer() || value.get<int64_t>() < 0 || value.get<int64_t>() > 1000000)
            throw RuntimeError("ZCodex TotalLayersCount is not a whole number within range");
        return size_t(value.get<int64_t>());
    }
    static size_t whole_number(const std::string &text, const char *what)
    {
        const auto trimmed = boost::algorithm::trim_copy(text);
        if (trimmed.empty() || trimmed.size() > 9 || !std::all_of(trimmed.begin(), trimmed.end(), [](char c) { return c >= '0' && c <= '9'; }))
            throw RuntimeError(std::string(what) + " is not a whole number");
        return size_t(std::stoul(trimmed));
    }

    // ---- Parameter parsing and rendering (private) ----------------------------------------
    Kind m_kind = Kind::ZCode;
    static constexpr uint64_t script_limit = 64ULL * 1024 * 1024;

    static std::string n3(double value) { return native_parameters::trimmed_number(value); }
    static double r3(double value) { return native_parameters::round_decimals(value, 1000.); }
    static bool pos(double value) { return r3(value) > 0; }

    // ZCode: one line of the exporter's program, in the exporter's order.
    // A zero wait is spelled "G4 P.0;" by our exporter and "G4 P0;" by UVtools (and presumably other
    // writers): the spelling is part of the file's own style, so a rewritten block copies it.
    static constexpr const char *zero_wait_exporter = "G4 P.0;", *zero_wait_uvtools = "G4 P0;";
    static std::string wait_line(double seconds, const char *zero = zero_wait_exporter)
    {
        const auto ms = std::llround(seconds * 1000);
        return ms ? "G4 P" + std::to_string(ms) + ";" : std::string(zero);
    }
    static std::string move_line(double z, double speed, const char *command = "G0")
    {
        return std::string(command) + " Z" + n3(r3(z)) + " F" + n3(native_parameters::round_decimals(speed / 10., 1000.)) + ";";
    }
    static std::vector<std::string> zcode_block(const native_parameters::Row &row, size_t layer, const char *zero = zero_wait_exporter)
    {
        using F = sla::LayerParameter;
        auto v = [&](F field) { return native_parameters::value_of(row, field); };
        const double z = v(F::PositionZ), lift = v(F::LiftDistance), lift2 = v(F::LiftDistance2), retract2 = v(F::RetractDistance2),
                     total = lift + lift2;
        std::vector<std::string> lines = {"M6054 \"" + std::to_string(layer + 1) + ".png\";"};
        if (pos(lift)) lines.push_back(move_line(z + lift, v(F::LiftSpeed)));
        if (pos(lift2)) lines.push_back(move_line(z + total, v(F::LiftSpeed2)));
        if (!pos(total)) lines.push_back(move_line(z, v(F::LiftSpeed)));
        if (pos(v(F::WaitAfterLift))) lines.push_back(wait_line(v(F::WaitAfterLift)));
        if (pos(total - retract2)) lines.push_back(move_line(z + retract2, v(F::RetractSpeed)));
        if (pos(retract2)) lines.push_back(move_line(z, v(F::RetractSpeed2)));
        lines.push_back(wait_line(v(F::WaitBefore), zero));
        lines.push_back("M106 S" + std::to_string((int(v(F::LightPWM)) * 300 + 254) / 255) + ";");
        lines.push_back(wait_line(v(F::Exposure), zero));
        lines.push_back("M106 S0;");
        if (pos(v(F::WaitAfter))) lines.push_back(wait_line(v(F::WaitAfter), zero));
        return lines;
    }
    struct Token { bool move = false; double a = 0, b = 0; }; // move: target Z and speed (mm/min); wait: seconds
    static bool parse_token(const std::string &line, Token &token)
    {
        if (line.size() < 5 || line.back() != ';') return false;
        const std::string body = line.substr(0, line.size() - 1);
        if (body == "G4 P.0") { token = {false, 0., 0.}; return true; }
        if (body.compare(0, 4, "G4 P") == 0) {
            char *rest = nullptr;
            const auto ms = std::strtoull(body.c_str() + 4, &rest, 10);
            if (rest == body.c_str() + 4 || *rest) return false;
            token = {false, double(ms) / 1000., 0.};
            return true;
        }
        if (body.compare(0, 4, "G0 Z") != 0) return false;
        char *rest = nullptr;
        const double z = std::strtod(body.c_str() + 4, &rest);
        if (rest == body.c_str() + 4 || std::strncmp(rest, " F", 2) != 0) return false;
        const double speed = std::strtod(rest + 2, &rest);
        if (!rest || *rest) return false;
        token = {true, z, native_parameters::round_decimals(speed * 10., 100.)};
        return true;
    }
    static bool pwm_line(const std::string &line, int &power)
    {
        if (line.compare(0, 6, "M106 S") != 0 || line.back() != ';') return false;
        char *rest = nullptr;
        power = int(std::strtol(line.c_str() + 6, &rest, 10));
        return rest == line.c_str() + line.size() - 1 && rest != line.c_str() + 6;
    }

    struct ZCodeProgram {
        std::vector<std::string> cipher;                 // lcd.gcode lines as stored
        std::vector<std::string> plain;                  // decrypted
        std::vector<std::pair<size_t, size_t>> blocks;   // [begin, end) of each layer
        native_parameters::Table rows;
        size_t tail_begin = 0;
        double park_target = -1;                         // first absolute Z move after the last layer
        const char *zero_wait = zero_wait_exporter;      // how this file spells a zero wait
    };

    ZCodeProgram parse_zcode() const
    {
        using F = sla::LayerParameter;
        const std::string where = "ZCode program is not in the exporter's layout: ";
        ZCodeProgram program;
        const auto bytes = m_zip.extract("lcd.gcode", script_limit);
        const std::string text(bytes.begin(), bytes.end());
        if (text.empty() || text.back() != '\n') throw RuntimeError(where + "it does not end with a line break");
        for (size_t at = 0; at < text.size();) {
            const auto end = text.find('\n', at);
            program.cipher.push_back(text.substr(at, end - at));
            at = end + 1;
        }
        program.plain.resize(program.cipher.size());
        std::atomic<bool> bad{false};
        tbb::parallel_for(tbb::blocked_range<size_t>(0, program.cipher.size(), 256), [&](const auto &range) {
            for (size_t i = range.begin(); i < range.end() && !bad; ++i) {
                program.plain[i] = sla::zcode::decrypt_line(program.cipher[i]);
                if (program.plain[i].empty()) bad = true;
            }
        });
        if (bad) throw RuntimeError(where + "a line is not a protocol line");
        const auto &plain = program.plain;
        static const char *head[] = {"G21;", "G90;", "M106 S0;", "M17;", "G28 Z0;"};
        size_t at = 0;
        for (const char *line : head)
            if (at >= plain.size() || plain[at++] != line) throw RuntimeError(where + "its header differs");
        bool zero_seen = false;
        for (size_t layer = 0; layer < count; ++layer) {
            const std::string name = "layer " + std::to_string(layer + 1) + " ";
            const size_t begin = at;
            if (at >= plain.size() || plain[at] != "M6054 \"" + std::to_string(layer + 1) + ".png\";")
                throw RuntimeError(where + name + "does not start where expected");
            ++at;
            std::vector<Token> pre;
            int power = 0;
            for (; at < plain.size() && !(pwm_line(plain[at], power) && power > 0); ++at) {
                Token token;
                if (!parse_token(plain[at], token)) throw RuntimeError(where + name + "has an unknown command");
                pre.push_back(token);
            }
            if (at >= plain.size() || pre.empty() || pre.back().move) throw RuntimeError(where + name + "lacks its pre-cure wait");
            ++at;
            Token exposure, after;
            if (at >= plain.size() || !parse_token(plain[at], exposure) || exposure.move) throw RuntimeError(where + name + "lacks its exposure wait");
            ++at;
            if (at >= plain.size() || plain[at++] != "M106 S0;") throw RuntimeError(where + name + "lacks its light-off");
            bool has_after = at < plain.size() && parse_token(plain[at], after) && !after.move;
            if (has_after) ++at;
            native_parameters::Row row;
            row[F::WaitBefore] = pre.back().a;
            pre.pop_back();
            row[F::Exposure] = exposure.a;
            row[F::WaitAfter] = has_after ? after.a : 0.;
            row[F::LightPWM] = double(power * 255 / 300);
            row[F::WaitAfterLift] = 0.;
            std::vector<Token> moves;
            std::optional<size_t> wait_at;
            for (const auto &token : pre) {
                if (token.move) { moves.push_back(token); continue; }
                if (wait_at) throw RuntimeError(where + name + "has two waits before its cure");
                wait_at = moves.size();
                row[F::WaitAfterLift] = token.a;
            }
            if (moves.empty()) throw RuntimeError(where + name + "has no lift move");
            const double z = moves.back().a;
            const auto expected = std::vector<std::string>(plain.begin() + begin, plain.begin() + at);
            bool matched = false;
            for (size_t split = 1; split <= moves.size() && !matched && split <= 2; ++split) {
                if (wait_at && *wait_at != split) continue;
                const size_t back = moves.size() - split;
                if (back > 2) continue;
                native_parameters::Row candidate = row;
                candidate[F::PositionZ] = r3(z);
                candidate[F::LiftDistance] = r3(moves[0].a - z);
                candidate[F::LiftSpeed] = moves[0].b;
                candidate[F::LiftDistance2] = split == 2 ? r3(moves[1].a - moves[0].a) : 0.;
                candidate[F::LiftSpeed2] = split == 2 ? moves[1].b : 0.;
                candidate[F::RetractDistance2] = back == 2 ? r3(moves[split].a - z) : 0.;
                candidate[F::RetractSpeed] = back >= 1 ? moves[split].b : 0.;
                candidate[F::RetractSpeed2] = back == 2 ? moves[split + 1].b : 0.;
                const bool valid = std::all_of(candidate.begin(), candidate.end(),
                    [](const auto &entry) { return std::isfinite(entry.second) && entry.second >= 0; });
                if (!valid) continue;
                // The block may spell its zero wait either way; the file's first spelling wins for rewrites.
                for (const char *zero : {zero_wait_exporter, zero_wait_uvtools})
                    if (zcode_block(candidate, layer, zero) == expected) {
                        row = candidate; matched = true;
                        const bool has_zero = std::find(expected.begin(), expected.end(), std::string(zero)) != expected.end();
                        if (has_zero && !zero_seen) { program.zero_wait = zero; zero_seen = true; }
                        break;
                    }
            }
            if (!matched) throw RuntimeError(where + name + "differs from the exporter's rendering");
            program.blocks.push_back({begin, at});
            program.rows.push_back(std::move(row));
        }
        if (at >= plain.size() || plain[at] != "M106 S0;") throw RuntimeError(where + "its tail is missing");
        program.tail_begin = at;
        for (size_t i = at; i < plain.size(); ++i) {
            Token token;
            if ((plain[i].compare(0, 4, "G0 Z") == 0 || plain[i].compare(0, 4, "G1 Z") == 0)) {
                const std::string probe = "G0" + plain[i].substr(2);
                if (parse_token(probe, token)) { program.park_target = token.a; break; }
            }
        }
        return program;
    }

    boost::property_tree::ptree task_tree() const
    {
        const auto bytes = m_zip.extract("task.xml", metadata_limit);
        const std::string text(bytes.begin(), bytes.end());
        boost::property_tree::ptree tree;
        try {
            std::istringstream stream(text);
            boost::property_tree::read_xml(stream, tree, boost::property_tree::xml_parser::trim_whitespace);
        } catch (const boost::property_tree::xml_parser_error &error) {
            throw RuntimeError(std::string("ZCode task.xml is not valid XML: ") + error.message());
        }
        return tree;
    }
    static void replace_tag(std::string &xml, const std::string &tag, const std::string &value)
    {
        const std::string open = "<" + tag + ">", close = "</" + tag + ">";
        const auto begin = xml.find(open);
        if (begin == std::string::npos) throw RuntimeError("ZCode task.xml lacks <" + tag + ">");
        const auto end = xml.find(close, begin);
        if (end == std::string::npos) throw RuntimeError("ZCode task.xml lacks </" + tag + ">");
        xml.replace(begin + open.size(), end - begin - open.size(), value);
    }
    static double tag_number(const std::string &xml, const std::string &tag)
    {
        const std::string open = "<" + tag + ">";
        const auto begin = xml.find(open);
        if (begin == std::string::npos) throw RuntimeError("ZCode task.xml lacks <" + tag + ">");
        return std::strtod(xml.c_str() + begin + open.size(), nullptr);
    }
    static std::string float_text(double value)
    {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(9) << float(value);
        return out.str();
    }

    std::map<std::string, std::vector<uint8_t>> zcode_replacements(const native_parameters::Table &table,
        const std::vector<native_parameters::MaskSummary> &old_masks, const std::vector<native_parameters::MaskSummary> &new_masks) const
    {
        using F = sla::LayerParameter;
        const auto program = parse_zcode();
        const double limit = height_limit();
        bool rows_changed = false, masks_changed = false;
        for (size_t i = 0; i < count; ++i) {
            check_storable(Kind::ZCode, table[i]);
            if (table[i] != program.rows[i]) {
                rows_changed = true;
                const double top = native_parameters::value_of(table[i], F::PositionZ) + native_parameters::value_of(table[i], F::LiftDistance) +
                                   native_parameters::value_of(table[i], F::LiftDistance2);
                if (limit > 0 && top > limit + 1e-9) throw RuntimeError("ZCode layer " + std::to_string(i + 1) + " lifts above the machine height");
            }
            if (old_masks[i] != new_masks[i]) masks_changed = true;
        }
        if (!rows_changed && !masks_changed) return {};
        std::map<std::string, std::vector<uint8_t>> result;
        if (rows_changed) {
            if (program.park_target >= 0 && native_parameters::value_of(table.back(), F::PositionZ) > program.park_target + 1e-9)
                throw RuntimeError("The last ZCode layer would end above the parking move that follows it");
            std::string out;
            auto keep = [&](size_t begin, size_t end) { for (size_t i = begin; i < end; ++i) { out += program.cipher[i]; out += '\n'; } };
            keep(0, program.blocks.front().first);
            for (size_t i = 0; i < count; ++i) {
                if (table[i] == program.rows[i]) keep(program.blocks[i].first, program.blocks[i].second);
                else out += sla::zcode::encrypt_lines(zcode_block(table[i], i, program.zero_wait));
            }
            keep(program.tail_begin, program.cipher.size());
            result["lcd.gcode"] = std::vector<uint8_t>(out.begin(), out.end());
        }
        // task.xml: print time, volume, resin weight and price follow the edit.
        const auto bytes = m_zip.extract("task.xml", metadata_limit);
        std::string xml(bytes.begin(), bytes.end());
        const std::string original_xml = xml;
        if (rows_changed) {
            const double old_total = native_parameters::total_seconds(program.rows), new_total = native_parameters::total_seconds(table);
            if (old_total != new_total) {
                const double stored = tag_number(xml, "time");
                replace_tag(xml, "time", std::to_string(native_parameters::carried_time(uint32_t(std::max(0., stored)), old_total, new_total)));
            }
        }
        const double old_weight = native_parameters::thickness_weighted_area(old_masks, native_parameters::z_column(program.rows)),
                     new_weight = native_parameters::thickness_weighted_area(new_masks, native_parameters::z_column(table));
        if (old_weight != new_weight) {
            if (old_weight <= 0) throw RuntimeError("The ZCode volume cannot be recomputed from a source without lit pixels");
            const double ratio = new_weight / old_weight;
            for (const char *tag : {"volumn", "weight", "price"})
                replace_tag(xml, tag, float_text(tag_number(xml, tag) * ratio));
        }
        if (xml != original_xml) result["task.xml"] = std::vector<uint8_t>(xml.begin(), xml.end());
        return result;
    }

    // ZCodex ---------------------------------------------------------------------------------
    nlohmann::ordered_json resin_json() const { return read_json("ResinMetadata").first; }
    // The parsed JSON and its text; the exporter's own formatting (dump(2)) is required so that
    // rewriting one value cannot reformat anything else.
    std::pair<nlohmann::ordered_json, std::string> read_json(const char *name) const
    {
        const auto bytes = m_zip.extract(name, metadata_limit);
        const std::string text(bytes.begin(), bytes.end());
        nlohmann::ordered_json json;
        try { json = nlohmann::ordered_json::parse(text); }
        catch (const nlohmann::json::exception &error) { throw RuntimeError(std::string("ZCodex ") + name + " is not valid JSON: " + error.what()); }
        if (!json.is_object()) throw RuntimeError(std::string("ZCodex ") + name + " is not a JSON object");
        return {json, text};
    }
    static std::vector<std::string> zcodex_block(const native_parameters::Row &row, size_t layer, double previous_z)
    {
        using F = sla::LayerParameter;
        auto v = [&](F field) { return native_parameters::value_of(row, field); };
        const double lift = v(F::LiftDistance), step = r3(v(F::PositionZ) - previous_z);
        std::vector<std::string> lines = {"<Slice> " + std::to_string(layer)};
        auto speed = [](double s) { return n3(s); };
        if (pos(lift)) {
            lines.push_back("G1 Z" + n3(lift) + " F" + speed(v(F::LiftSpeed)));
            lines.push_back("G1 Z-" + n3(r3(lift - step)) + " F" + speed(v(F::RetractSpeed)));
        } else
            lines.push_back("G1 Z" + n3(step) + " F" + speed(v(F::LiftSpeed)));
        lines.push_back("<Delay_blank>");
        lines.push_back("M106 S" + std::to_string(int(v(F::LightPWM))));
        lines.push_back("<Delay_model>");
        lines.push_back("M106 S0");
        return lines;
    }
    static bool parse_g1(const std::string &line, double &z, double &speed)
    {
        if (line.compare(0, 4, "G1 Z") != 0) return false;
        char *rest = nullptr;
        z = std::strtod(line.c_str() + 4, &rest);
        if (rest == line.c_str() + 4 || std::strncmp(rest, " F", 2) != 0) return false;
        speed = std::strtod(rest + 2, &rest);
        return rest && !*rest;
    }

    struct ZCodexProgram {
        std::vector<std::string> lines;
        std::vector<std::pair<size_t, size_t>> blocks;
        native_parameters::Table rows;
    };
    ZCodexProgram parse_zcodex() const
    {
        using F = sla::LayerParameter;
        const std::string where = "ZCodex program is not in the exporter's layout: ";
        ZCodexProgram program;
        const auto bytes = m_zip.extract("ResinGCodeData", script_limit);
        const std::string text(bytes.begin(), bytes.end());
        if (text.empty() || text.back() != '\n') throw RuntimeError(where + "it does not end with a line break");
        for (size_t at = 0; at < text.size();) {
            const auto end = text.find('\n', at);
            program.lines.push_back(text.substr(at, end - at));
            at = end + 1;
        }
        const auto &lines = program.lines;
        size_t at = 0;
        double z = 0;
        for (size_t layer = 0; layer < count; ++layer) {
            const std::string name = "layer " + std::to_string(layer + 1) + " ";
            const size_t begin = at;
            if (at >= lines.size() || lines[at] != "<Slice> " + std::to_string(layer)) throw RuntimeError(where + name + "does not start where expected");
            ++at;
            double v1 = 0, s1 = 0, v2 = 0, s2 = 0;
            if (at >= lines.size() || !parse_g1(lines[at++], v1, s1)) throw RuntimeError(where + name + "lacks its lift move");
            const bool two = at < lines.size() && parse_g1(lines[at], v2, s2);
            if (two) ++at;
            if (at + 4 > lines.size() || lines[at] != "<Delay_blank>" || lines[at + 2] != "<Delay_model>" || lines[at + 3] != "M106 S0")
                throw RuntimeError(where + name + "has an unexpected exposure sequence");
            int pwm = 0;
            char *rest = nullptr;
            if (lines[at + 1].compare(0, 6, "M106 S") != 0) throw RuntimeError(where + name + "lacks its M106");
            pwm = int(std::strtol(lines[at + 1].c_str() + 6, &rest, 10));
            if (!rest || *rest || pwm < 1) throw RuntimeError(where + name + "has an invalid PWM");
            at += 4;
            native_parameters::Row row;
            const double previous = z;
            double step;
            if (two) {
                if (v1 <= 0 || v2 >= 0) throw RuntimeError(where + name + "has an invalid lift or retract");
                step = r3(v1 + v2);
                row[F::LiftDistance] = r3(v1); row[F::LiftSpeed] = s1; row[F::RetractSpeed] = s2;
            } else {
                step = r3(v1);
                row[F::LiftDistance] = 0.; row[F::LiftSpeed] = s1; row[F::RetractSpeed] = 0.;
            }
            if (!(step > 0) || !std::isfinite(step)) throw RuntimeError(where + name + "has a nonpositive thickness");
            z = r3(previous + step);
            row[F::PositionZ] = z;
            row[F::LightPWM] = double(pwm);
            for (const auto &[field, value] : row)
                if (!std::isfinite(value) || value < 0) throw RuntimeError(where + name + "has a negative value");
            if (zcodex_block(row, layer, previous) != std::vector<std::string>(lines.begin() + begin, lines.begin() + at))
                throw RuntimeError(where + name + "differs from the exporter's rendering");
            program.blocks.push_back({begin, at});
            program.rows.push_back(std::move(row));
        }
        return program;
    }

    std::map<std::string, std::vector<uint8_t>> zcodex_replacements(const native_parameters::Table &table,
        const std::vector<native_parameters::MaskSummary> &old_masks, const std::vector<native_parameters::MaskSummary> &new_masks) const
    {
        using F = sla::LayerParameter;
        const auto program = parse_zcodex();
        bool rows_changed = false, masks_changed = false;
        double previous = 0;
        for (size_t i = 0; i < count; ++i) {
            check_storable(Kind::ZCodex, table[i]);
            check_step(Kind::ZCodex, table[i], previous);
            previous = native_parameters::value_of(table[i], F::PositionZ);
            if (table[i] != program.rows[i]) rows_changed = true;
            if (old_masks[i] != new_masks[i]) masks_changed = true;
        }
        if (!rows_changed && !masks_changed) return {};
        auto [resin, resin_text] = read_json("ResinMetadata");
        auto [user, user_text] = read_json("UserSettingsData");
        auto [meta, meta_text] = read_json("ZCodeMetadata");
        const auto resin_style = native_parameters::json_style(resin, resin_text), user_style = native_parameters::json_style(user, user_text),
                   meta_style = native_parameters::json_style(meta, meta_text);
        if (!resin_style || !user_style || !meta_style)
            throw RuntimeError("ZCodex metadata is laid out in a way that cannot be rewritten without reformatting it");
        std::map<std::string, std::vector<uint8_t>> result;
        auto put = [&](const char *name, const std::string &text) { result[name] = std::vector<uint8_t>(text.begin(), text.end()); };
        if (rows_changed) {
            std::string out;
            double before = 0, original_before = 0;
            for (size_t i = 0; i < count; ++i) {
                const auto &[begin, end] = program.blocks[i];
                const bool keep = table[i] == program.rows[i] && before == original_before;
                const auto lines = keep ? std::vector<std::string>(program.lines.begin() + begin, program.lines.begin() + end)
                                        : zcodex_block(table[i], i, before);
                for (const auto &line : lines) { out += line; out += '\n'; }
                before = native_parameters::value_of(table[i], F::PositionZ);
                original_before = native_parameters::value_of(program.rows[i], F::PositionZ);
            }
            for (size_t i = program.blocks.back().second; i < program.lines.size(); ++i) { out += program.lines[i]; out += '\n'; }
            put("ResinGCodeData", out);
        }
        // Statistics. The stored time is the exporter's ceil(sum) unless another writer set it.
        const double exposure = double(resin.value("LayerTime", 0)) / 1000., bottom_exposure = double(resin.value("BottomLayersTime", 0)) / 1000.,
                     blanking = double(resin.value("BlankingLayerTime", 0)) / 1000.;
        const size_t bottom = size_t(std::max<int64_t>(0, resin.value("BottomLayersNumber", int64_t(0))));
        auto model = [&](const native_parameters::Table &rows) {
            native_parameters::Table out = rows;
            for (size_t i = 0; i < out.size(); ++i) { out[i][F::Exposure] = i < bottom ? bottom_exposure : exposure; out[i][F::WaitBefore] = blanking; }
            return native_parameters::total_seconds(out);
        };
        bool resin_changed = false, user_changed = false, meta_changed = false;
        if (rows_changed && model(program.rows) != model(table)) {
            const double old_total = model(program.rows), new_total = model(table);
            const uint32_t stored = uint32_t(std::max<int64_t>(0, resin.value("PrintTime", int64_t(0))));
            const uint32_t next = native_parameters::carried_time(stored, old_total, new_total);
            resin["PrintTime"] = next; resin_changed = true;
            meta["PrintTime"] = next; meta_changed = true;
            user["PrintTime"] = std::to_string((next / 3600) % 24) + "h " + std::to_string((next / 60) % 60) + "m"; user_changed = true;
        }
        const auto old_z = native_parameters::z_column(program.rows), new_z = native_parameters::z_column(table);
        const double old_weight = native_parameters::thickness_weighted_area(old_masks, old_z),
                     new_weight = native_parameters::thickness_weighted_area(new_masks, new_z);
        if (old_weight != new_weight) {
            if (old_weight <= 0) throw RuntimeError("The ZCodex volume cannot be recomputed from a source without lit pixels");
            const double ratio = new_weight / old_weight;
            resin["TotalMaterialVolumeUsed"] = float(double(resin.value("TotalMaterialVolumeUsed", 0.)) * ratio);
            resin["TotalMaterialWeightUsed"] = float(std::round(double(resin.value("TotalMaterialWeightUsed", 0.)) * ratio * 1000) / 1000);
            user["MaterialVolume"] = float(double(user.value("MaterialVolume", 0.)) * ratio);
            resin_changed = user_changed = true;
        }
        // Per-layer used volume (the exporter's fixed Inkspire pixel area).
        if (resin.contains("Layers") && resin["Layers"].is_array() && resin["Layers"].size() == count) {
            for (size_t i = 0; i < count; ++i) {
                const double step_old = old_z[i] - (i ? old_z[i - 1] : 0.), step_new = new_z[i] - (i ? new_z[i - 1] : 0.);
                if (old_masks[i] == new_masks[i] && std::abs(step_old - step_new) < 1e-9) continue;
                resin["Layers"][i]["UsedMaterialVolume"] = float(double(new_masks[i].lit) * (74.67 / 1440) * (132.88 / 2560) * step_new / 1000.);
                resin_changed = true;
            }
        }
        if (resin_changed) put("ResinMetadata", native_parameters::json_dump(resin, *resin_style));
        if (user_changed) put("UserSettingsData", native_parameters::json_dump(user, *user_style));
        if (meta_changed) put("ZCodeMetadata", native_parameters::json_dump(meta, *meta_style));
        return result;
    }
};

} // namespace Slic3r

#endif
