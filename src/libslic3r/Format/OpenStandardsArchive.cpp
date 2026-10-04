///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "OpenStandardsArchive.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Zipper.hpp"
#include <boost/locale/encoding_utf.hpp>
#include <boost/nowide/fstream.hpp>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace Slic3r {

namespace {
// OSLA v0 is little endian, with fixed-width tables. Keep serialization
// independent of host alignment and native struct sizes.
class OSLAOutput {
  boost::nowide::ofstream stream;

public:
  explicit OSLAOutput(const std::string &path)
      : stream(path, std::ios::binary | std::ios::trunc) {
    if (!stream)
      throw FileIOError("Cannot write OSLA archive");
    stream.exceptions(std::ios::badbit | std::ios::failbit);
  }
  uint32_t position() {
    const auto offset = stream.tellp();
    if (offset < 0 || uint64_t(offset) > UINT32_MAX)
      throw ExportError("OSLA archive exceeds 32-bit addresses");
    return uint32_t(offset);
  }
  void seek(uint32_t position) { stream.seekp(position); }
  void bytes(const void *data, size_t size) {
    stream.write(static_cast<const char *>(data), std::streamsize(size));
  }
  void u8(uint8_t value) { stream.put(char(value)); }
  void u16(uint16_t value) {
    u8(uint8_t(value));
    u8(uint8_t(value >> 8));
  }
  void u32(uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
      u8(uint8_t(value >> (8 * i)));
  }
  void number(double value) {
    const float encoded = float(value);
    if (!std::isfinite(encoded) || encoded < 0)
      throw ExportError("Invalid OSLA numeric parameter");
    uint32_t bits;
    std::memcpy(&bits, &encoded, sizeof(bits));
    u32(bits);
  }
  void text(const std::string &value, size_t length, bool terminated = true) {
    // UVtools BinarySerialization uses UTF-8. FieldLength is a byte limit;
    // never truncate a code point or let metadata shift following fields.
    if (value.size() > length - size_t(terminated) ||
        std::any_of(value.begin(), value.end(),
                    [](unsigned char c) { return c == 0; }))
      throw ExportError("OSLA metadata must fit its fixed UTF-8 field");
    boost::locale::conv::utf_to_utf<char32_t>(value, boost::locale::conv::stop);
    bytes(value.data(), value.size());
    for (size_t i = value.size(); i < length; ++i)
      u8(0);
  }
  void close() { stream.close(); }
};
struct OSLAMaskInfo {
  uint32_t count = 0, x = 0, y = 0, width = 0, height = 0, address = 0;
};
} // namespace

// OpenSLAArchive
std::unique_ptr<sla::RasterBase> OpenSLAArchive::create_raster() const {
  return sla::create_printer_raster(m_cfg);
}

sla::RasterEncoder OpenSLAArchive::get_encoder() const {
  return sla::PNGRasterEncoder{};
}

void OpenSLAArchive::export_print(const std::string fname,
                                  const SLAPrint &print,
                                  const ThumbnailsList &thumbnails,
                                  const std::string &projectname) {
  // Native port of the local UVtools OSLAFile v0 schema. The indexed PNG
  // payload retains grayscale and shares identical layer image records.
  const size_t count = m_layers.size();
  if (!count || count != print.print_layers().size() || count > UINT32_MAX / 73)
    throw ExportError("OSLA layer count mismatch or table overflow");
  const bool portrait = m_cfg.display_orientation.value == sladoPortrait;
  const uint32_t columns =
      portrait ? m_cfg.display_pixels_y.value : m_cfg.display_pixels_x.value;
  const uint32_t rows =
      portrait ? m_cfg.display_pixels_x.value : m_cfg.display_pixels_y.value;
  const double width =
      portrait ? m_cfg.display_height.value : m_cfg.display_width.value;
  const double height =
      portrait ? m_cfg.display_width.value : m_cfg.display_height.value;
  if (!columns || !rows || columns > 32768 || rows > 32768 ||
      uint64_t(columns) * rows > 128 * 1024 * 1024 || width <= 0 || height <= 0)
    throw ExportError("Invalid OSLA screen dimensions");
  const auto bottom_count = sla::bottom_layers(print, count);
  if (bottom_count > UINT16_MAX)
    throw ExportError("OSLA bottom layer count exceeds 16 bits");
  // Build the authoritative per-layer parameters and an equivalent SLA
  // motion program before opening the destination. No extrusion commands.
  std::ostringstream motion;
  motion.imbue(std::locale::classic());
  motion << std::fixed << std::setprecision(6);
  motion << "G21;\nG90;\nM17;\nM106 S0;\nG28 Z0;\n";
  double seconds = 0, previous_z = 0;
  for (size_t i = 0; i < count; ++i) {
    const double z = sla::layer_z(print, i);
    const bool bottom = i < bottom_count;
    auto param = [&](const char *key) {
      return double(sla::motion_parameter(print, key, bottom));
    };
    for (const char *key :
         {"lift_height", "lift_speed", "lift_height2", "lift_speed2",
          "wait_after_lift", "retract_speed", "retract_height2",
          "retract_speed2", "wait_before_cure", "wait_after_cure", "light_pwm"})
      if (!std::isfinite(param(key)) || param(key) < 0)
        throw ExportError("Invalid OSLA layer motion");
    if (!std::isfinite(z) || z <= previous_z ||
        z > m_cfg.max_print_height.value || param("light_pwm") > 255)
      throw ExportError("Invalid OSLA layer Z or UV power");
    const double lift = param("lift_height"), lift2 = param("lift_height2"),
                 retract2 = param("retract_height2");
    if (retract2 > lift + lift2)
      throw ExportError("OSLA retract distance exceeds lift distance");
    auto move = [&](double target, double speed) {
      if (speed <= 0)
        throw ExportError("OSLA movement requires a positive speed");
      motion << "G0 Z" << target << " F" << speed << ";\n";
    };
    auto wait = [&](double time) {
      // Round the float-backed configuration to microseconds before
      // expressing milliseconds, rather than leaking binary float noise.
      if (time > 0)
        motion << "G4 P" << std::round(time * 1e6) / 1000. << ";\n";
    };
    motion << ";LAYER_START:" << i << "\nM6054 " << i << ";\n";
    if (lift > 0)
      move(z + lift, param("lift_speed"));
    if (lift2 > 0)
      move(z + lift + lift2, param("lift_speed2"));
    wait(param("wait_after_lift"));
    if (lift + lift2 - retract2 > 0)
      move(z + retract2, param("retract_speed"));
    if (retract2 > 0)
      move(z, param("retract_speed2"));
    if (lift + lift2 == 0)
      move(z, param("lift_speed"));
    wait(param("wait_before_cure"));
    motion << "M106 S" << param("light_pwm") << ";\n";
    wait(sla::layer_exposure(print, i));
    motion << "M106 S0;\n";
    wait(param("wait_after_cure"));
    previous_z = z;
    seconds += sla::lcd_layer_time(print, i);
  }
  motion << "M106 S0;\nM18;\n";
  if (!std::isfinite(seconds) || seconds < 0 || std::ceil(seconds) > UINT32_MAX)
    throw ExportError("OSLA print time exceeds its header range");
  const auto program = motion.str();
  if (program.size() > UINT32_MAX)
    throw ExportError("OSLA motion program too large");
  // Validate optional fixed-width machine metadata before opening the file.
  std::string machine = m_cfg.printer_model.value;
  if (machine.size() >= 50 ||
      std::any_of(machine.begin(), machine.end(),
                  [](unsigned char c) { return c == 0; }))
    throw ExportError("OSLA machine name must fit 49 UTF-8 bytes");
  boost::locale::conv::utf_to_utf<char32_t>(machine, boost::locale::conv::stop);
  OSLAOutput out(fname);
  out.text("OSLATiCo", 8, false);
  out.u16(0);
  std::time_t now = std::time(nullptr);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  char date[21];
  std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%SZ", &utc);
  out.text(date, 20, false);
  out.text("SLA-Slicer", 50);
  out.text(date, 20, false);
  out.text("SLA-Slicer", 50);
  const uint32_t header_address = out.position();
  for (size_t i = 0; i < 199; ++i)
    out.u8(0);
  out.u32(0); // Empty optional custom table.
  // PNG previews are legal OSLA image data types. Use a neutral fallback
  // mask when a model thumbnail is unavailable (PCB/calibration jobs).
  png::ImageGreyscale first;
  if (!png::decode_png(png::ReadBuf{m_layers[0].data(), m_layers[0].size()},
                       first))
    throw ExportError("Invalid OSLA preview mask");
  const ThumbnailData *thumbnail = nullptr;
  for (const auto &candidate : thumbnails)
    if (candidate.is_valid()) {
      thumbnail = &candidate;
      break;
    }
  for (size_t side : {size_t(400), size_t(200)}) {
    const size_t sw = thumbnail ? thumbnail->width : first.cols,
                 sh = thumbnail ? thumbnail->height : first.rows;
    const double scale = std::min(double(side) / sw, double(side) / sh);
    const size_t w = std::max(size_t(1), size_t(sw * scale)),
                 h = std::max(size_t(1), size_t(sh * scale));
    std::vector<uint8_t> rgb(side * side * 3, 0);
    for (size_t y = 0; y < h; ++y)
      for (size_t x = 0; x < w; ++x) {
        const size_t sx = std::min(sw - 1, size_t(x / scale)),
                     sy = std::min(sh - 1, size_t(y / scale));
        for (size_t c = 0; c < 3; ++c)
          rgb[((y + (side - h) / 2) * side + x + (side - w) / 2) * 3 + c] =
              thumbnail ? thumbnail->pixels[((sh - 1 - sy) * sw + sx) * 4 + c]
                        : first.buf[sy * sw + sx];
      }
    const auto image = sla::PNGRasterEncoder{}(rgb.data(), side, side, 3);
    out.u16(uint16_t(side));
    out.u16(uint16_t(side));
    out.u32(uint32_t(image.size()));
    out.bytes(image.data(), image.size());
  }
  const uint32_t table_address = out.position();
  if (uint64_t(table_address) + count * 73 > UINT32_MAX)
    throw ExportError("OSLA layer table exceeds address range");
  out.seek(uint32_t(table_address + count * 73));
  std::vector<OSLAMaskInfo> info(count);
  std::unordered_map<std::string_view, OSLAMaskInfo> cache;
  double volume = 0, previous = 0;
  for (size_t i = 0; i < count; ++i) {
    const auto &encoded = m_layers[i];
    const std::string_view key(static_cast<const char *>(encoded.data()),
                               encoded.size());
    const auto found = cache.find(key);
    if (found != cache.end())
      info[i] = found->second;
    else {
      png::ImageGreyscale mask;
      if (!png::decode_png(png::ReadBuf{encoded.data(), encoded.size()},
                           mask) ||
          mask.cols != columns || mask.rows != rows)
        throw ExportError("OSLA mask does not match screen dimensions");
      uint32_t xmin = columns, ymin = rows, xmax = 0, ymax = 0;
      for (uint32_t y = 0; y < rows; ++y)
        for (uint32_t x = 0; x < columns; ++x)
          if (mask.buf[size_t(y) * columns + x]) {
            ++info[i].count;
            xmin = std::min(xmin, x);
            ymin = std::min(ymin, y);
            xmax = std::max(xmax, x);
            ymax = std::max(ymax, y);
          }
      if (info[i].count) {
        info[i].x = xmin;
        info[i].y = ymin;
        info[i].width = xmax - xmin + 1;
        info[i].height = ymax - ymin + 1;
      }
      if (uint64_t(out.position()) + 4 + encoded.size() > UINT32_MAX)
        throw ExportError("OSLA image exceeds address range");
      info[i].address = out.position();
      out.u32(uint32_t(encoded.size()));
      out.bytes(encoded.data(), encoded.size());
      if (cache.size() < 1024)
        cache.emplace(key, info[i]);
    }
    const double z = sla::layer_z(print, i);
    volume += info[i].count * width / columns * height / rows * (z - previous);
    previous = z;
  }
  const uint32_t motion_address = out.position();
  if (uint64_t(motion_address) + 4 + program.size() + 9 > UINT32_MAX)
    throw ExportError("OSLA program exceeds address range");
  out.u32(uint32_t(program.size()));
  out.bytes(program.data(), program.size());
  out.bytes(";OSLATiCo", 9);
  out.seek(table_address);
  for (size_t i = 0; i < count; ++i) {
    auto param = [&](const char *key) {
      return sla::motion_parameter(print, key, i < bottom_count);
    };
    out.u32(info[i].address);
    out.number(sla::layer_z(print, i));
    for (const char *key :
         {"lift_height", "lift_speed", "lift_height2", "lift_speed2",
          "wait_after_lift", "retract_speed", "retract_height2",
          "retract_speed2", "wait_before_cure"})
      out.number(param(key));
    out.number(sla::layer_exposure(print, i));
    out.number(param("wait_after_cure"));
    out.u8(uint8_t(param("light_pwm")));
    out.u32(info[i].count);
    out.u32(info[i].x);
    out.u32(info[i].y);
    out.u32(info[i].width);
    out.u32(info[i].height);
  }
  out.seek(header_address);
  out.u32(199);
  out.u32(columns);
  out.u32(rows);
  out.number(m_cfg.max_print_height.value);
  out.number(width);
  out.number(height);
  out.u8(0); // The canonical masks already contain LCD mirror transforms.
  out.text("PNG", 16);
  out.text("PNG", 16);
  out.u32(8);
  out.u32(2);
  out.number(print.default_object_config().layer_height.value);
  out.u16(uint16_t(bottom_count));
  out.u32(uint32_t(count));
  out.u32(73);
  out.u32(table_address);
  out.u32(motion_address);
  out.u32(uint32_t(std::ceil(seconds)));
  out.number(volume / 1000.);
  out.number(print.material_config().bottle_volume.value > 0
                 ? volume / 1000. * print.material_config().bottle_cost.value /
                       print.material_config().bottle_volume.value
                 : 0.);
  out.text("", 50);
  out.text(machine, 50);
  if (out.position() != header_address + 199)
    throw ExportError("OSLA header table size mismatch");
  out.close();
}

// UVJArchive
std::unique_ptr<sla::RasterBase> UVJArchive::create_raster() const {
  return sla::create_printer_raster(m_cfg);
}

sla::RasterEncoder UVJArchive::get_encoder() const {
  return sla::PNGRasterEncoder{};
}

void UVJArchive::export_print(const std::string fname, const SLAPrint &print,
                              const ThumbnailsList &thumbnails,
                              const std::string &) {
  // Native port of UVtools.Core/FileFormats/UVJFile.cs. Its JSON schema uses
  // PascalCase and zero-based 8-digit PNG names, not the generic ZIP schema.
  using Json = nlohmann::json;
  const size_t count = m_layers.size();
  if (!count || count != print.print_layers().size())
    throw ExportError("UVJ layer count mismatch");
  const bool portrait = m_cfg.display_orientation.value == sladoPortrait;
  const int width =
      portrait ? m_cfg.display_pixels_y.value : m_cfg.display_pixels_x.value;
  const int height =
      portrait ? m_cfg.display_pixels_x.value : m_cfg.display_pixels_y.value;
  const double width_mm =
      portrait ? m_cfg.display_height.value : m_cfg.display_width.value;
  const double height_mm =
      portrait ? m_cfg.display_width.value : m_cfg.display_height.value;
  if (width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX ||
      width_mm <= 0 || height_mm <= 0)
    throw ExportError(
        "UVJ requires positive display dimensions and 16-bit resolution");
  const auto bottom_count = sla::bottom_layers(print, count);
  if (bottom_count > UINT16_MAX)
    throw ExportError("UVJ bottom layer count exceeds 16 bits");
  auto exposure = [&](bool bottom, double seconds) {
    auto param = [&](const char *key) {
      return sla::motion_parameter(print, key, bottom);
    };
    const double retract =
        param("lift_height") + param("lift_height2") - param("retract_height2");
    if (retract < 0)
      throw ExportError("UVJ retract distance exceeds total lift distance");
    return Json{{"WaitTimeBeforeCure", param("wait_before_cure")},
                {"LightOffTime", 0},
                {"LightOnTime", seconds},
                {"LightPWM", int(param("light_pwm"))},
                {"WaitTimeAfterCure", param("wait_after_cure")},
                {"LiftHeight", param("lift_height")},
                {"LiftSpeed", param("lift_speed")},
                {"LiftHeight2", param("lift_height2")},
                {"LiftSpeed2", param("lift_speed2")},
                {"WaitTimeAfterLift", param("wait_after_lift")},
                {"RetractHeight", retract},
                {"RetractSpeed", param("retract_speed")},
                {"RetractHeight2", param("retract_height2")},
                {"RetractSpeed2", param("retract_speed2")}};
  };
  auto bottom =
      exposure(true, print.material_config().initial_exposure_time.value);
  bottom["Count"] = bottom_count;
  Json layers = Json::array();
  double previous_z = 0;
  for (size_t i = 0; i < count; ++i) {
    const double z = sla::layer_z(print, i);
    if (!std::isfinite(z) || z <= previous_z ||
        z > m_cfg.max_print_height.value)
      throw ExportError(
          "UVJ requires increasing layer Z within printer height");
    layers.push_back({{"Z", z},
                      {"Exposure", exposure(i < bottom_count,
                                            sla::layer_exposure(print, i))}});
    previous_z = z;
  }
  Json settings = {
      {"Properties",
       {{"Size",
         {{"X", width},
          {"Y", height},
          {"Millimeter", {{"X", width_mm}, {"Y", height_mm}}},
          {"Layers", count},
          {"LayerHeight", print.default_object_config().layer_height.value}}},
        {"Exposure",
         exposure(false, print.material_config().exposure_time.value)},
        {"Bottom", bottom},
        {"AntiAliasLevel", 16},
        {"Vendor",
         {{"SLA-Slicer",
           {{"MachineName", m_cfg.printer_model.value},
            {"MachineZ", m_cfg.max_print_height.value},
            {"TransitionLayerCount",
             print.material_config().sla_transition_layers.value}}}}}}},
      {"Layers", layers}};
  Zipper zip(fname, Zipper::FAST_COMPRESSION);
  zip.add_entry("config.json");
  zip << settings.dump(2);
  for (size_t i = 0; i < count; ++i) {
    std::ostringstream name;
    name << "slice/" << std::setw(8) << std::setfill('0') << i << ".png";
    zip.add_entry(name.str(), m_layers[i].data(), m_layers[i].size());
  }
  // Valid preview images are required by some viewers. Use a rendered model
  // thumbnail when available; layer-only exports fall back to the first mask.
  const ThumbnailData *thumbnail = nullptr;
  for (const auto &candidate : thumbnails)
    if (candidate.is_valid()) {
      thumbnail = &candidate;
      break;
    }
  png::ImageGreyscale mask;
  if (!thumbnail &&
      !png::decode_png(
          png::ReadBuf{m_layers.front().data(), m_layers.front().size()}, mask))
    throw ExportError("UVJ cannot decode its preview source mask");
  auto preview = [&](const char *name, size_t columns, size_t rows) {
    const size_t source_width = thumbnail ? thumbnail->width : mask.cols;
    const size_t source_height = thumbnail ? thumbnail->height : mask.rows;
    const double scale =
        std::min(double(columns) / source_width, double(rows) / source_height);
    const size_t drawn_width =
        std::max(size_t(1), size_t(source_width * scale));
    const size_t drawn_height =
        std::max(size_t(1), size_t(source_height * scale));
    std::vector<uint8_t> rgb(columns * rows * 3, 0);
    for (size_t y = 0; y < drawn_height; ++y)
      for (size_t x = 0; x < drawn_width; ++x) {
        const size_t sx = std::min(source_width - 1, size_t(x / scale));
        const size_t sy = std::min(source_height - 1, size_t(y / scale));
        const size_t destination = ((y + (rows - drawn_height) / 2) * columns +
                                    x + (columns - drawn_width) / 2) *
                                   3;
        for (size_t channel = 0; channel < 3; ++channel)
          rgb[destination + channel] =
              thumbnail
                  ? thumbnail->pixels[((source_height - 1 - sy) * source_width +
                                       sx) *
                                          4 +
                                      channel]
                  : mask.buf[sy * source_width + sx];
      }
    const auto png = sla::PNGRasterEncoder{}(rgb.data(), columns, rows, 3);
    zip.add_entry(name, png.data(), png.size());
  };
  preview("preview/tiny.png", 400, 400);
  preview("preview/huge.png", 800, 480);
  zip.finalize();
}

} // namespace Slic3r
