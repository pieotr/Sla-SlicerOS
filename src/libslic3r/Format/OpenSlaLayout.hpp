// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader-side layout of the Open SLA (OSLA v0) universal binary format. Port of
// the UVtools OSLAFile.cs decode path; the writer is OpenStandardsArchive.cpp.
// All integers and floats are little endian.
//
//   0     "OSLATiCo", version (u16), created/modified stamps and tool names
//   150   header, "table size" bytes long (199): resolution x/y (u32), machine
//         Z, display width/height (f32), mirror, preview and layer data type
//         names, preview count, layer height, bottom layers (u16), layer
//         count, layer record size (73), layer table address, G-code address,
//         print time, material volume and cost, material and machine names
//   then  custom table (u32 size + bytes), previews, the layer table, the layer
//         images (u32 size + PNG; identical layers share one image), the G-code
//         (u32 size + text) and the closing ";OSLATiCo"
//
// Layer record: image address (u32), 12 floats (Z, lift, speeds, waits,
// exposure), PWM (u8), lit pixel count, bounding box x, y, width, height (u32).
//
// An edited layer gets a new image and refreshed pixel count and bounding box
// (the fields the exporter derives from the mask), and the stored material
// volume and cost are scaled by the thickness-weighted change in lit pixels.
// Everything else (previews, unknown bytes) is carried over.
//
// Per-layer parameters are the 12 floats and the PWM byte of the record. When
// they change, the print time, the volume-based statistics and (if it is the
// program our exporter writes) the G-code motion program follow them.
#ifndef SLIC3R_OPEN_SLA_LAYOUT_HPP
#define SLIC3R_OPEN_SLA_LAYOUT_HPP

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "BinaryLayout.hpp"
#include "NativeParameterLayout.hpp"
#include "PngHeaderProbe.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/SLA/RasterBase.hpp"

namespace Slic3r {

class OpenSlaLayout {
public:
    static constexpr size_t file_def_size = 150, record_prefix = 73;
    static bool is_format(const std::string &format) { return format == "osla"; }
    static bool recognizes(const uint8_t *first8) { return std::memcmp(first8, "OSLATiCo", 8) == 0; }

    std::string format = "osla";
    size_t columns = 0, rows = 0, count = 0;

    explicit OpenSlaLayout(const sla::NativeBytes &bytes, const std::function<void()> &poll = {}) : m_bytes(bytes), m_in(bytes, "OSLA")
    {
        constexpr size_t header_min = 199;
        if (bytes.size() < file_def_size + header_min + 4 + 9) throw RuntimeError("OSLA file is truncated: the header needs " + std::to_string(file_def_size + header_min + 13) + " bytes");
        if (!bytes.equals(0, "OSLATiCo", 8)) throw RuntimeError("OSLA file has the wrong magic number");
        if (!bytes.equals(bytes.size() - 9, ";OSLATiCo", 9))
            throw RuntimeError("OSLA file is truncated: the closing marker is missing");
        const uint32_t table_size = m_in.u32(h(0));
        if (table_size < header_min || table_size > 65536) throw RuntimeError("OSLA header table size is invalid");
        columns = m_in.u32(h(4));
        rows = m_in.u32(h(8));
        binary_layout::check_raster(columns, rows, "OSLA");
        if (type_name(h(41)) != "PNG") throw RuntimeError("OSLA layer data type '" + type_name(h(41)) + "' is not supported, only PNG");
        count = m_in.u32(h(71));
        if (!count || count > binary_layout::max_layers) throw RuntimeError("OSLA layer count is zero or exceeds the editor limit");
        m_record_size = m_in.u32(h(75));
        if (m_record_size < record_prefix || m_record_size > 4096) throw RuntimeError("OSLA layer record size is invalid");
        m_table = m_in.u32(h(79));
        m_gcode = m_in.u32(h(83));
        m_in.need(m_table, uint64_t(count) * m_record_size, "the layer table");
        const uint64_t custom = h(0) + uint64_t(table_size);
        if (m_table < custom + 4 || m_gcode < m_table + uint64_t(count) * m_record_size || m_gcode > bytes.size() - 9)
            throw RuntimeError("OSLA layer table or G-code address is outside the file");
        m_images.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (poll && i % 1024 == 0) poll();
            const std::string where = "OSLA layer " + std::to_string(i + 1);
            const size_t address = m_in.u32(m_table + i * m_record_size);
            if (address < m_table + uint64_t(count) * m_record_size || address + 4 > m_gcode)
                throw RuntimeError(where + " image address is outside the layer data");
            const uint64_t size = m_in.u32(address);
            if (size > m_gcode - address - 4) throw RuntimeError(where + " image data is truncated");
            m_images.push_back({address, size_t(size)});
        }
    }

    std::vector<uint8_t> decode(size_t layer) const
    {
        const std::string where = "OSLA layer " + std::to_string(layer + 1);
        const auto &image = m_images.at(layer);
        const std::vector<uint8_t> png = m_bytes.read(image.address + 4, image.size);
        const auto header = read_png_header(png, where);
        if (header.bit_depth != 8 || header.color_type != 0) throw RuntimeError(where + " must be an 8-bit grayscale PNG");
        if (header.width != columns || header.height != rows) throw RuntimeError(where + " differs in size from the printer resolution");
        png::ImageGreyscale decoded;
        if (!png::decode_color_png({png.data(), png.size()}, decoded) || decoded.cols != columns || decoded.rows != rows)
            throw RuntimeError(where + " is not a valid PNG");
        return std::move(decoded.buf);
    }

    std::vector<uint8_t> encode(const std::vector<uint8_t> &pixels) const
    {
        const auto encoded = sla::PNGRasterEncoder{}(pixels.data(), columns, rows, 1);
        if (!encoded.size()) throw RuntimeError("Cannot encode OSLA layer image");
        const auto *data = static_cast<const uint8_t *>(encoded.data());
        return {data, data + encoded.size()};
    }

    // ---- Per-layer parameters -------------------------------------------------------------
    // Record offsets after the image address, in file order.
    static constexpr std::pair<sla::LayerParameter, size_t> float_fields[] = {
        {sla::LayerParameter::PositionZ, 4}, {sla::LayerParameter::LiftDistance, 8}, {sla::LayerParameter::LiftSpeed, 12},
        {sla::LayerParameter::LiftDistance2, 16}, {sla::LayerParameter::LiftSpeed2, 20}, {sla::LayerParameter::WaitAfterLift, 24},
        {sla::LayerParameter::RetractSpeed, 28}, {sla::LayerParameter::RetractDistance2, 32}, {sla::LayerParameter::RetractSpeed2, 36},
        {sla::LayerParameter::WaitBefore, 40}, {sla::LayerParameter::Exposure, 44}, {sla::LayerParameter::WaitAfter, 48}};
    static constexpr size_t pwm_offset = 52;

    static sla::LayerParameterCapabilities parameter_capabilities()
    {
        using F = sla::LayerParameter;
        return native_parameters::capabilities({F::PositionZ, F::Exposure, F::LiftDistance, F::LiftSpeed, F::LiftDistance2,
            F::LiftSpeed2, F::RetractSpeed, F::RetractDistance2, F::RetractSpeed2, F::WaitBefore, F::WaitAfter, F::WaitAfterLift,
            F::LightPWM});
    }
    native_parameters::Row parameter_row(size_t layer) const
    {
        const size_t record = m_table + layer * m_record_size;
        native_parameters::Row row;
        for (const auto &[field, offset] : float_fields) row[field] = m_in.f32(record + offset);
        row[sla::LayerParameter::LightPWM] = m_in.u8(record + pwm_offset);
        for (const auto &[field, value] : row)
            if (!std::isfinite(value) || value < 0)
                throw RuntimeError("OSLA layer " + std::to_string(layer + 1) + " has an invalid parameter");
        return row;
    }
    native_parameters::Table parameter_rows() const
    {
        native_parameters::Table rows;
        rows.reserve(count);
        for (size_t i = 0; i < count; ++i) rows.push_back(parameter_row(i));
        return rows;
    }
    double height_limit() const { return m_in.f32(h(12)); }
    size_t bottom_layers() const { return m_in.u16(h(69)); }

    // The motion program of our exporter (OpenStandardsArchive.cpp), from rows.
    static std::string motion_program(const native_parameters::Table &table)
    {
        using F = sla::LayerParameter;
        std::ostringstream motion;
        motion.imbue(std::locale::classic());
        motion << std::fixed << std::setprecision(6);
        motion << "G21;\nG90;\nM17;\nM106 S0;\nG28 Z0;\n";
        for (size_t i = 0; i < table.size(); ++i) {
            const auto &row = table[i];
            auto param = [&](F field) { return native_parameters::value_of(row, field); };
            const double z = param(F::PositionZ), lift = param(F::LiftDistance), lift2 = param(F::LiftDistance2),
                         retract2 = param(F::RetractDistance2);
            auto move = [&](double target, double speed) {
                if (speed <= 0) throw RuntimeError("OSLA movement requires a positive speed");
                motion << "G0 Z" << target << " F" << speed << ";\n";
            };
            auto wait = [&](double time) {
                // Microsecond rounding first, as in the exporter, so float noise does not leak into the program.
                if (time > 0) motion << "G4 P" << std::round(time * 1e6) / 1000. << ";\n";
            };
            motion << ";LAYER_START:" << i << "\nM6054 " << i << ";\n";
            if (lift > 0) move(z + lift, param(F::LiftSpeed));
            if (lift2 > 0) move(z + lift + lift2, param(F::LiftSpeed2));
            wait(param(F::WaitAfterLift));
            if (lift + lift2 - retract2 > 0) move(z + retract2, param(F::RetractSpeed));
            if (retract2 > 0) move(z, param(F::RetractSpeed2));
            if (lift + lift2 == 0) move(z, param(F::LiftSpeed));
            wait(param(F::WaitBefore));
            motion << "M106 S" << param(F::LightPWM) << ";\n";
            wait(param(F::Exposure));
            motion << "M106 S0;\n";
            wait(param(F::WaitAfter));
        }
        motion << "M106 S0;\nM18;\n";
        return motion.str();
    }
    // The stored program, when it is exactly what motion_program(rows) writes.
    bool program_is_generated(const native_parameters::Table &rows) const
    {
        const uint64_t size = m_in.u32(m_gcode);
        m_in.need(m_gcode + 4, size, "the G-code");
        try {
            const auto text = motion_program(rows);
            return text.size() == size && m_bytes.equals(m_gcode + 4, text.data(), text.size());
        } catch (const RuntimeError &) {
            return false;
        }
    }

    // Writes the container with the edited layers into `out`, an image built from an empty base
    // (sla::PatchedImage(sla::NativeBytes())). Everything up to the end of the layer table is copied and
    // patched, the unedited images are copied one by one (identical ones stay shared), each edited layer
    // gets a fresh PNG (identical edits share one) and the stats fields the exporter derives from the mask;
    // then the G-code block and the closing marker. Memory use does not grow with the file.
    void rebuild_to(sla::PatchedImage &out, const sla::NativeEdits &edits, const native_parameters::Table *table = nullptr,
                    const std::function<void()> &poll = {}) const
    {
        const size_t table_end = m_table + count * m_record_size;
        out.append_from(m_bytes, 0, table_end);
        std::map<size_t, size_t> moved;                      // original image address -> new address
        std::unordered_multimap<uint64_t, size_t> fresh;     // hash of an edited PNG -> new address (confirmed by comparing bytes)
        double old_weighted = 0, new_weighted = 0, previous_z = 0, new_previous_z = 0;
        double old_seconds = 0, new_seconds = 0;
        bool parameters_changed = false;
        for (size_t i = 0; i < count; ++i) {
            if (poll && i % 16 == 0) poll();
            const size_t record = m_table + i * m_record_size;
            const double z = m_in.f32(record + 4), thickness = z - previous_z;
            previous_z = z;
            // Edited parameters change the thickness the lit area is weighted by.
            const double new_z = table ? table->at(i).at(sla::LayerParameter::PositionZ) : z, new_thickness = new_z - new_previous_z;
            new_previous_z = new_z;
            const uint32_t old_lit = m_in.u32(record + 53);
            if (table) {
                const auto original = parameter_row(i);
                old_seconds += native_parameters::layer_seconds(original);
                new_seconds += native_parameters::layer_seconds(table->at(i));
                if (original != table->at(i)) {
                    parameters_changed = true;
                    for (const auto &[field, offset] : float_fields)
                        if (original.at(field) != table->at(i).at(field)) {
                            uint32_t bits;
                            const float number = float(table->at(i).at(field));
                            std::memcpy(&bits, &number, 4);
                            out.put32(record + offset, bits);
                        }
                    out.set(record + pwm_offset, uint8_t(table->at(i).at(sla::LayerParameter::LightPWM)));
                }
            }
            if (!edits.is_edited(i)) {
                const auto &image = m_images[i];
                const auto [slot, inserted] = moved.emplace(image.address, out.size());
                if (inserted) out.append_from(m_bytes, image.address, 4 + image.size);
                const size_t address = slot->second;
                old_weighted += double(old_lit) * thickness; new_weighted += double(old_lit) * new_thickness;
                out.put32(record, uint32_t(address)); // the table was copied above; only the address moves
                continue;
            }
            const auto pixels = edits.pixels(i);
            const auto png = encode(pixels);
            const uint64_t hash = std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char *>(png.data()), png.size()));
            size_t address = 0;
            bool shared = false;
            for (auto [it, end] = fresh.equal_range(hash); it != end && !shared; ++it)
                if (out.get32(it->second) == png.size() && out.read(it->second + 4, png.size()) == png) {
                    address = it->second;
                    shared = true;
                }
            if (!shared) {
                address = out.size();
                std::vector<uint8_t> block;
                binary_layout::append32(block, uint32_t(png.size()));
                block.insert(block.end(), png.begin(), png.end());
                out.append(block);
                fresh.emplace(hash, address);
            }
            uint32_t lit = 0, x0 = uint32_t(columns), y0 = uint32_t(rows), x1 = 0, y1 = 0;
            for (size_t y = 0; y < rows; ++y)
                for (size_t x = 0; x < columns; ++x)
                    if (pixels[y * columns + x]) {
                        ++lit; x0 = std::min<uint32_t>(x0, uint32_t(x)); y0 = std::min<uint32_t>(y0, uint32_t(y));
                        x1 = std::max<uint32_t>(x1, uint32_t(x)); y1 = std::max<uint32_t>(y1, uint32_t(y));
                    }
            old_weighted += double(old_lit) * thickness; new_weighted += double(lit) * new_thickness;
            const uint32_t stats[5] = {lit, lit ? x0 : 0, lit ? y0 : 0, lit ? x1 - x0 + 1 : 0, lit ? y1 - y0 + 1 : 0};
            out.put32(record, uint32_t(address));
            for (size_t n = 0; n < 5; ++n) out.put32(record + 53 + 4 * n, stats[n]);
        }
        if (out.size() > UINT32_MAX) throw RuntimeError("Edited OSLA file exceeds 32-bit addresses");
        // Gap-free layer data is followed by the G-code block and closing marker.
        const size_t new_gcode = out.size();
        if (parameters_changed && program_is_generated(parameter_rows())) {
            // The program is our exporter's own: regenerate it from the edited rows so it keeps matching the table.
            const auto program = motion_program(*table);
            std::vector<uint8_t> block;
            binary_layout::append32(block, uint32_t(program.size()));
            block.insert(block.end(), program.begin(), program.end());
            out.append(block);
            out.append_from(m_bytes, m_bytes.size() - 9, 9); // ";OSLATiCo"
        } else
            out.append_from(m_bytes, m_gcode, m_bytes.size() - m_gcode);
        out.put32(h(83), uint32_t(new_gcode));
        if (parameters_changed)
            out.put32(h(87), native_parameters::carried_time(m_in.u32(h(87)), old_seconds, new_seconds));
        if (out.size() > UINT32_MAX) throw RuntimeError("Edited OSLA file exceeds 32-bit addresses");
        // Stored material volume and cost follow the lit-pixel change.
        if (old_weighted > 0 && new_weighted != old_weighted)
            for (size_t offset : {h(91), h(95)}) {
                const float scaled = float(double(m_in.f32(offset)) * new_weighted / old_weighted);
                if (!std::isfinite(scaled)) throw RuntimeError("Edited OSLA material statistics overflow");
                uint32_t bits; std::memcpy(&bits, &scaled, 4);
                out.put32(offset, bits);
            }
    }

private:
    struct Image { size_t address, size; };
    sla::NativeBytes m_bytes;
    binary_layout::Reader m_in;
    size_t m_record_size = 0, m_table = 0, m_gcode = 0;
    std::vector<Image> m_images;

    static size_t h(size_t field) { return file_def_size + field; }
    std::string type_name(size_t at) const
    {
        std::string name;
        for (size_t i = 0; i < 16 && m_in.u8(at + i); ++i) name.push_back(char(m_bytes[at + i]));
        return name;
    }
};

} // namespace Slic3r

#endif
