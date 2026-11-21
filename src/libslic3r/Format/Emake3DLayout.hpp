// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader-side layout of the Emake3D Galaxy QDT text format. Port of the UVtools
// QDTFile.cs decode path; the writer is Emake3DArchive.cpp.
//
//   JieHe,<layer um>,<resolution y>,<resolution x>,2,019,0,FA
//   per layer:  <layer number from 1>
//               preview lines  (a 10 percent scan of the layer, ignored)
//               FB
//               laser lines    "x,y,flag" (the layer, see below)
//               FC
//   FD
//   <layer count>
//
// Laser lines are vertical white segments in column order. A segment is
// written as one or two "laser off" moves (flag 0: either a relative Y step
// inside the same column, or two absolute points when the column changes), then
// "x,length,1" for the lit stroke. Odd layer indices (0-based) are mirrored in
// X as x' = width - x + 1.
//
// The layer is binary, so the editor snaps painted gray to 0 or 255. An edited
// layer is re-vectorised with the exporter's own span encoder (the same
// bytes the exporter would write for that mask); unchanged layers keep their
// original text.
//
// Print parameters: masks-only by design. QDTFile.cs: the file stores no print parameters at all (only the layer height and the laser
// lines), so there is nothing to edit per layer or globally.
#ifndef SLIC3R_EMAKE3D_LAYOUT_HPP
#define SLIC3R_EMAKE3D_LAYOUT_HPP

#include <algorithm>
#include <array>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "BinaryLayout.hpp"
#include "Emake3DArchive.hpp"

namespace Slic3r {

class Emake3DLayout {
public:
    static bool is_format(const std::string &format) { return format == "qdt"; }
    static bool recognizes(const uint8_t *first8) { return std::memcmp(first8, "JieHe,", 6) == 0; }
    static uint8_t quantize(uint8_t value) { return value >= 128 ? 255 : 0; }

    std::string format = "qdt";
    size_t columns = 0, rows = 0, count = 0;

    explicit Emake3DLayout(const sla::NativeBytes &bytes, const std::function<void()> &poll = {}) : m_bytes(bytes)
    {
        const binary_layout::Window scan(bytes); // the file is walked line by line
        const size_t size = bytes.size();
        size_t at = 0;
        const auto header = next_line(scan, at, size);
        const auto fields = split(header.text);
        if (fields.size() < 8 || fields[0] != "JieHe" || fields[7] != "FA") throw RuntimeError("QDT file has an invalid header line");
        rows = number(fields[2], "resolution y");
        columns = number(fields[3], "resolution x");
        if (columns > 65535 || rows > 65535) throw RuntimeError("QDT resolution exceeds 16 bits");
        binary_layout::check_raster(columns, rows, "QDT");
        if (columns < 6 || rows < 6) throw RuntimeError("QDT resolution is too small for its 10 percent preview");

        enum class Expect { Layer, Fb, Fc } expect = Expect::Layer;
        size_t layer_start = at;
        bool finished = false;
        while (at < size) {
            if (poll && m_layers.size() % 256 == 0) poll();
            const size_t line_start = at;
            const auto line = next_line(scan, at, size);
            const auto trimmed = trim(line.text);
            if (trimmed.empty()) continue;
            const auto parts = split(trimmed);
            if (parts.size() == 1) {
                const auto &word = parts[0];
                if (word == "FB") {
                    if (expect != Expect::Fb) throw RuntimeError("QDT file has an unexpected FB marker");
                    expect = Expect::Fc;
                } else if (word == "FC") {
                    if (expect != Expect::Fc) throw RuntimeError("QDT file has an unexpected FC marker");
                    m_layers.emplace_back(layer_start, at);
                    expect = Expect::Layer;
                } else if (word == "FD") {
                    if (expect != Expect::Layer) throw RuntimeError("QDT file ends in the middle of a layer");
                    m_tail = line_start;
                    const auto declared = at < size ? next_line(scan, at, size) : Line{};
                    count = number(trim(declared.text), "layer count");
                    finished = true;
                    break;
                } else {
                    if (expect != Expect::Layer) throw RuntimeError("QDT file has a layer number inside a layer");
                    if (number(word, "layer number") != m_layers.size() + 1)
                        throw RuntimeError("QDT layer " + std::to_string(m_layers.size() + 1) + " is missing or out of order");
                    layer_start = line_start;
                    expect = Expect::Fb;
                }
            } else if (parts.size() != 3 || expect == Expect::Layer) {
                throw RuntimeError("QDT file has an unexpected line: " + trimmed.substr(0, 40));
            }
        }
        if (!finished) throw RuntimeError("QDT file is truncated: the FD marker and layer count are missing");
        if (!count || count > binary_layout::max_layers) throw RuntimeError("QDT layer count is zero or exceeds the editor limit");
        if (count != m_layers.size())
            throw RuntimeError("QDT file declares " + std::to_string(count) + " layers but contains " + std::to_string(m_layers.size()));
    }

    std::vector<uint8_t> decode(size_t layer) const
    {
        const std::string where = "QDT layer " + std::to_string(layer + 1);
        const auto [begin, end] = m_layers.at(layer);
        const auto text = m_bytes.read(begin, end - begin); // the layer's own lines, read in one piece
        std::vector<std::array<size_t, 3>> coords;
        size_t at = 0;
        const size_t length = text.size();
        next_line(text, at, length); // layer number
        bool laser = false;
        while (at < length) {
            const auto line = trim(next_line(text, at, length).text);
            if (line == "FB") { laser = true; continue; }
            if (line == "FC" || line.empty() || !laser) continue; // the preview scan is not the mask
            const auto parts = split(line);
            const size_t x = number(parts.at(0), "x"), y = number(parts.at(1), "y"), flag = number(parts.at(2), "flag");
            if (x > 65535 || y > 65535 || flag > 1) throw RuntimeError(where + " has a coordinate out of range");
            coords.push_back({x, y, flag});
        }
        std::vector<uint8_t> pixels(columns * rows, 0);
        if (coords.empty()) return pixels;
        if (coords[0][2] != 0) throw RuntimeError(where + " must start with a laser-off move");
        const bool mirror = layer % 2 != 0;
        size_t zeros = 1;
        size_t start_y = coords[0][1], end_y = start_y;
        for (size_t i = 1; i < coords.size(); ++i) {
            const auto &c = coords[i];
            if (c[2] == 0) { ++zeros; continue; }
            if (zeros < 1 || zeros > 2) throw RuntimeError(where + " has a stroke with an invalid number of positioning moves");
            const auto &last = coords[i - 1];
            if (zeros == 1) { if (i > 1) start_y = end_y + last[1]; }
            else start_y = last[1];
            end_y = start_y + c[1];
            zeros = 0;
            // Mirrored layers store x' = width - x + 1.
            const int64_t x = mirror ? int64_t(columns) - int64_t(c[0]) + 1 : int64_t(c[0]);
            if (x < 0 || x >= int64_t(columns) || end_y >= rows) throw RuntimeError(where + " has a line outside the raster");
            for (size_t y = start_y; y <= end_y; ++y) pixels[y * columns + size_t(x)] = 255;
        }
        return pixels;
    }

    // Byte range [begin, end) of a layer's text (number line to FC's newline).
    std::pair<size_t, size_t> record_range(size_t layer) const { return m_layers.at(layer); }

    // Text of one layer exactly as the exporter writes it.
    std::string encode_text(size_t layer, const std::vector<uint8_t> &pixels) const
    {
        const auto packet = Emake3DGalaxyArchive::encode_mask(pixels.data(), columns, rows, 1);
        const auto *p = static_cast<const uint8_t *>(packet.data());
        const size_t main_size = (size_t(p[0]) << 24) | (size_t(p[1]) << 16) | (size_t(p[2]) << 8) | p[3];
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << layer + 1 << '\n';
        // The 10 percent preview scan comes first, then FB, then the layer itself.
        emit(out, spans(p + 4 + main_size, packet.size() - 4 - main_size), false);
        out << "FB\n";
        emit(out, spans(p + 4, main_size), layer % 2 != 0, columns);
        out << "FC\n";
        return out.str();
    }

    // Writes the file with the given layers re-vectorised into `out`, an image built from an empty base
    // (sla::PatchedImage(sla::NativeBytes())): header, unedited layers and the FD tail are copied range by range.
    void rebuild_to(sla::PatchedImage &out, const sla::NativeEdits &edits, const std::function<void()> &poll = {}) const
    {
        out.append_from(m_bytes, 0, m_layers.front().first);
        for (size_t i = 0; i < count; ++i) {
            if (poll && i % 64 == 0) poll();
            if (!edits.is_edited(i)) {
                out.append_from(m_bytes, m_layers[i].first, m_layers[i].second - m_layers[i].first);
                continue;
            }
            const std::string text = encode_text(i, edits.pixels(i));
            out.append(reinterpret_cast<const uint8_t *>(text.data()), text.size());
        }
        out.append_from(m_bytes, m_tail, m_bytes.size() - m_tail);
    }

private:
    struct Line { std::string text; };
    struct Span { size_t start, end, x; };
    sla::NativeBytes m_bytes;
    std::vector<std::pair<size_t, size_t>> m_layers; // first byte of the number line, byte after FC's newline
    size_t m_tail = 0;                               // offset of the FD line

    // One line of `source` (anything indexable by byte: a Window over the file, a vector holding one layer).
    template<class Source> static Line next_line(const Source &source, size_t &at, size_t size)
    {
        Line line;
        while (at < size && source[at] != '\n') line.text.push_back(char(source[at++]));
        if (at < size) ++at;
        return line;
    }
    static std::string trim(const std::string &text)
    {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }
    static std::vector<std::string> split(const std::string &text)
    {
        std::vector<std::string> parts;
        std::stringstream stream(text);
        std::string part;
        while (std::getline(stream, part, ',')) {
            part = trim(part);
            if (!part.empty()) parts.push_back(part);
        }
        return parts;
    }
    static size_t number(const std::string &text, const char *what)
    {
        if (text.empty() || text.size() > 9 || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
            throw RuntimeError(std::string("QDT ") + what + " is not a whole number");
        return size_t(std::stoul(text));
    }

    // Spans of an exporter span packet: u32 count, count * (start y, end y, x), CR LF.
    static std::vector<Span> spans(const uint8_t *p, size_t length)
    {
        auto be16 = [](const uint8_t *q) { return size_t(q[0] << 8 | q[1]); };
        const size_t lines = (size_t(p[0]) << 24) | (size_t(p[1]) << 16) | (size_t(p[2]) << 8) | p[3];
        if (lines * 6 + 6 != length) throw RuntimeError("Internal QDT span packet is inconsistent");
        std::vector<Span> result;
        result.reserve(lines);
        for (size_t i = 0; i < lines; ++i) result.push_back({be16(p + 4 + i * 6), be16(p + 6 + i * 6), be16(p + 8 + i * 6)});
        return result;
    }

    // Same laser-off bookkeeping as the exporter: relative Y inside a column,
    // two absolute moves between columns, and a closing off point. Mirrored
    // layers store x' = width + 1 - x with the column groups in reverse order
    // (ascending Y inside each group).
    static void emit(std::ostream &out, const std::vector<Span> &table, bool mirror, size_t cols = 0)
    {
        bool first = true;
        size_t previous_x = 0, previous_start = 0, previous_end = 0;
        auto line = [&](const Span &span) {
            const size_t x = mirror ? cols + 1 - span.x : span.x;
            if (first) out << x << ',' << span.start << ",0\n";
            else if (x == previous_x) out << x << ',' << span.start - previous_end << ",0\n";
            else out << previous_x << ',' << previous_start << ",0\n" << x << ',' << span.start << ",0\n";
            out << x << ',' << span.end - span.start << ",1\n";
            previous_x = x; previous_start = span.start; previous_end = span.end;
            first = false;
        };
        if (!mirror)
            for (const auto &span : table) line(span);
        else
            for (size_t end = table.size(); end;) {
                size_t begin = end - 1;
                while (begin && table[begin - 1].x == table[end - 1].x) --begin;
                for (size_t i = begin; i < end; ++i) line(table[i]);
                end = begin;
            }
        if (!first) out << previous_x << ',' << previous_end << ",0\n";
    }
};

} // namespace Slic3r

#endif
