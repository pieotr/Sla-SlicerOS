/// Native port of local UVtools QDTFile.cs (AGPLv3).
#include "Emake3DArchive.hpp"
#include "MakerbaseArchive.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
#include "libslic3r/SLAPrint.hpp"
#include <fstream>
#include <locale>

namespace Slic3r {
namespace {
uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t *p) { return uint16_t(uint16_t(p[0]) << 8 | p[1]); }
size_t preview_dimension(size_t value) { return size_t(std::nearbyint(double(value) * .1)); }
struct SpanTable {
    const uint8_t *data;
    uint32_t count;
    SpanTable(const uint8_t *p, size_t length, size_t cols, size_t rows) : data(p), count(0) {
        if (length < 6)
            throw ExportError("Invalid QDT span packet");
        count = be32(p);
        if (uint64_t(count) * 6 + 6 != length || p[length - 2] != 13 || p[length - 1] != 10)
            throw ExportError("Invalid QDT span extent");
        for (size_t i = 0; i < count; ++i) {
            const auto *line = p + 4 + i * 6;
            if (be16(line) > be16(line + 2) || be16(line + 2) >= rows || be16(line + 4) >= cols)
                throw ExportError("QDT span exceeds screen geometry");
        }
    }
    uint16_t x(size_t i) const { return be16(data + 4 + i * 6 + 4); }
    uint16_t start(size_t i) const { return be16(data + 4 + i * 6); }
    uint16_t end(size_t i) const { return be16(data + 4 + i * 6 + 2); }
};
// Laser-off coordinates alternate relative Y increments within a column and
// two absolute moves when changing columns. The final off point is required.
void emit(std::ostream &out, const SpanTable &table, size_t cols, bool mirror) {
    bool first = true;
    size_t previous_x = 0, previous_start = 0, previous_end = 0;
    auto line = [&](size_t index) {
        const size_t x = mirror ? cols + 1 - table.x(index) : table.x(index);
        const size_t start = table.start(index), end = table.end(index);
        if (first)
            out << x << ',' << start << ",0\n";
        else if (x == previous_x)
            out << x << ',' << start - previous_end << ",0\n";
        else
            out << previous_x << ',' << previous_start << ",0\n" << x << ',' << start << ",0\n";
        out << x << ',' << end - start << ",1\n";
        previous_x = x;
        previous_start = start;
        previous_end = end;
        first = false;
    };
    if (!mirror)
        for (size_t i = 0; i < table.count; ++i)
            line(i);
    else {
        // Reverse column groups, retaining ascending Y within each group.
        for (size_t end = table.count; end;) {
            size_t begin = end - 1;
            while (begin && table.x(begin - 1) == table.x(end - 1))
                --begin;
            for (size_t i = begin; i < end; ++i)
                line(i);
            end = begin;
        }
    }
    if (!first)
        out << previous_x << ',' << previous_end << ",0\n";
}
} // namespace
sla::EncodedRaster Emake3DGalaxyArchive::encode_mask(const void *ptr, size_t width, size_t height, size_t components) {
    const size_t pw = preview_dimension(width), ph = preview_dimension(height);
    if (!ptr || !pw || !ph || components != 1 || width > 32768 || height > 32768 ||
        uint64_t(width) * height > (uint64_t(128) << 20))
        throw ExportError("QDT requires a grayscale mask with a nonempty 10 percent preview within raster limits");
    const auto *pixels = static_cast<const uint8_t *>(ptr);
    const auto main = MakerbaseArchive::encode_mask(ptr, width, height, components);
    std::vector<uint8_t> small(pw * ph);
    // OpenCV INTER_LINEAR at fx=fy=0.1 samples source centers 4.5,14.5,...
    // before thresholding. Replicate its four-pixel average and edge clamp.
    for (size_t y = 0; y < ph; ++y)
        for (size_t x = 0; x < pw; ++x) {
            const size_t x0 = std::min(width - 1, x * 10 + 4), x1 = std::min(width - 1, x * 10 + 5);
            const size_t y0 = std::min(height - 1, y * 10 + 4), y1 = std::min(height - 1, y * 10 + 5);
            const unsigned sum =
                pixels[y0 * width + x0] + pixels[y0 * width + x1] + pixels[y1 * width + x0] + pixels[y1 * width + x1];
            small[y * pw + x] = uint8_t((sum + 2) / 4);
        }
    const auto preview = MakerbaseArchive::encode_mask(small.data(), pw, ph, 1);
    if (main.size() > UINT32_MAX)
        throw ExportError("QDT internal span packet exceeds uint32");
    std::vector<uint8_t> packet;
    packet.reserve(4 + main.size() + preview.size());
    for (int shift = 24; shift >= 0; shift -= 8)
        packet.push_back(uint8_t(uint32_t(main.size()) >> shift));
    const auto *m = static_cast<const uint8_t *>(main.data()), *p = static_cast<const uint8_t *>(preview.data());
    packet.insert(packet.end(), m, m + main.size());
    packet.insert(packet.end(), p, p + preview.size());
    return sla::EncodedRaster(std::move(packet), "qdt");
}
std::unique_ptr<sla::RasterBase> Emake3DGalaxyArchive::create_raster() const {
    return sla::create_printer_raster(m_cfg);
}
sla::RasterEncoder Emake3DGalaxyArchive::get_encoder() const { return encode_mask; }
void Emake3DGalaxyArchive::validate_configuration(const SLAPrint &print) {
    const auto &cfg = print.printer_config();
    const bool portrait = cfg.display_orientation.value == sladoPortrait;
    const double width = portrait ? cfg.display_height.value : cfg.display_width.value;
    const double height = portrait ? cfg.display_width.value : cfg.display_height.value;
    if (width != 400 || height != 200 || cfg.max_print_height.value != 400)
        throw ExportError("QDT has no physical geometry fields: Emake3D Galaxy 1 requires 400x200 mm and Z=400 mm");
    if (cfg.display_pixels_x.value < 6 || cfg.display_pixels_y.value < 6 || cfg.display_pixels_x.value > 32768 ||
        cfg.display_pixels_y.value > 32768 ||
        uint64_t(cfg.display_pixels_x.value) * cfg.display_pixels_y.value > (uint64_t(128) << 20))
        throw ExportError("Invalid QDT resolution or 10 percent preview dimensions");
    if (!cfg.display_mirror_x.value || cfg.display_mirror_y.value)
        throw ExportError("QDT requires horizontal mirroring and no vertical mirroring");
    const double lh = print.default_object_config().layer_height.value, um = lh * 1000;
    if (!std::isfinite(um) || um < 1 || um > UINT16_MAX || std::abs(um - std::round(um)) > 1e-5 ||
        float(lh) != float(print.material_config().initial_layer_height.value))
        throw ExportError("QDT requires a shared initial/normal height in whole micrometers, 1..65535");
    const auto &material = print.material_config();
    if (material.exposure_time.value != 3 || material.initial_exposure_time.value != 30 ||
        material.sla_bottom_layers.value != 4 || material.sla_transition_layers.value != 0)
        throw ExportError("QDT cannot store exposure/bottom settings; UVtools interprets 3/30 seconds, 4 bottom layers "
                          "and no transitions");
    sla::require_single_stage_motion(print, "QDT", false);
    for (const bool bottom : {false, true}) {
        if (sla::motion_parameter(print, "lift_height", bottom) != 6 ||
            sla::motion_parameter(print, "lift_speed", bottom) != 100 ||
            sla::motion_parameter(print, "retract_speed", bottom) != 100 ||
            sla::motion_parameter(print, "wait_before_cure", bottom) != 0)
            throw ExportError(
                "QDT has no motion/rest fields; UVtools interprets 6 mm, 100 mm/min and zero explicit rest");
    }
}
void Emake3DGalaxyArchive::export_print(const std::string fname, const SLAPrint &print, const ThumbnailsList &,
                                        const std::string &) {
    validate_configuration(print);
    if (m_layers.empty() || m_layers.size() != print.print_layers().size() || m_layers.size() > UINT32_MAX)
        throw ExportError("QDT requires a complete nonempty layer stack");
    const bool portrait = m_cfg.display_orientation.value == sladoPortrait;
    const size_t cols = portrait ? m_cfg.display_pixels_y.value : m_cfg.display_pixels_x.value;
    const size_t rows = portrait ? m_cfg.display_pixels_x.value : m_cfg.display_pixels_y.value;
    const double lh = print.default_object_config().layer_height.value;
    // Validate all packets before opening an existing destination.
    for (size_t i = 0; i < m_layers.size(); ++i) {
        if (std::abs(double(sla::layer_z(print, i)) - lh * (i + 1)) > 1e-5)
            throw ExportError("QDT cannot store variable layer Z");
        const auto &mask = m_layers[i];
        const auto *p = static_cast<const uint8_t *>(mask.data());
        if (mask.size() < 16)
            throw ExportError("Invalid QDT internal packet");
        const size_t length = be32(p);
        if (length > mask.size() - 4)
            throw ExportError("Invalid QDT internal extent");
        SpanTable(p + 4, length, cols, rows);
        SpanTable(p + 4 + length, mask.size() - 4 - length, preview_dimension(cols), preview_dimension(rows));
    }
    std::ofstream out(fname, std::ios::binary | std::ios::trunc);
    out.imbue(std::locale::classic());
    if (!out)
        throw ExportError("Cannot open QDT output");
    out << "JieHe," << uint16_t(std::round(lh * 1000)) << ',' << rows << ',' << cols << ",2,019,0,FA\n";
    for (size_t i = 0; i < m_layers.size(); ++i) {
        const auto &mask = m_layers[i];
        const auto *p = static_cast<const uint8_t *>(mask.data());
        const size_t length = be32(p);
        const SpanTable main(p + 4, length, cols, rows),
            preview(p + 4 + length, mask.size() - 4 - length, preview_dimension(cols), preview_dimension(rows));
        out << i + 1 << '\n';
        emit(out, preview, preview_dimension(cols), false);
        out << "FB\n";
        emit(out, main, cols, i % 2 != 0);
        out << "FC\n";
    }
    out << "FD\n" << m_layers.size() << '\n';
    out.flush();
    if (!out)
        throw ExportError("Cannot finish QDT output");
}
} // namespace Slic3r
