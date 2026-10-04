///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher.
// Native serialization follows UVtools.Core/FileFormats/AnetFile.cs (AGPLv3).
#include "AnetArchive.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
#include "libslic3r/SLAPrint.hpp"
#include <algorithm>
#include <array>
#include <boost/locale/encoding_utf.hpp>
#include <boost/nowide/fstream.hpp>
#include <cmath>
#include <cstring>
#include <limits>

namespace Slic3r {
namespace {
struct AnetPrinter {
  size_t columns, rows;
  double width, height, z;
};
constexpr AnetPrinter n4{1440, 2560, 68.04, 120.96, 135.};
constexpr AnetPrinter n7{2560, 1600, 192., 120., 300.};
constexpr std::array<uint8_t, 66> bmp_header = {
    66, 77, 162, 4, 0,   0,   0, 0,   0,  0,  66, 0,  0, 0, 40, 0, 0,
    0,  4,  1,   0, 0,   140, 0, 0,   0,  1,  0,  16, 0, 3, 0,  0, 0,
    96, 4,  0,   0, 18,  11,  0, 0,   18, 11, 0,  0,  0, 0, 0,  0, 0,
    0,  0,  0,   0, 248, 0,   0, 224, 7,  0,  0,  31, 0, 0, 0};
void append_u32(std::vector<uint8_t> &bytes, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    bytes.push_back(uint8_t(value >> shift));
}
uint32_t read_u32(const uint8_t *bytes) {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
         (uint32_t(bytes[2]) << 8) | bytes[3];
}
class BitWriter {
public:
  std::vector<uint8_t> bytes;
  uint32_t position = 0;
  void write(uint32_t value, unsigned count) {
    if (count > 32 ||
        uint64_t(position) + count > std::numeric_limits<uint32_t>::max())
      throw ExportError("Anet mask exceeds bit-stream limits");
    bytes.resize((uint64_t(position) + count + 7) / 8, 0);
    for (unsigned i = 0; i < count; ++i, ++position)
      if ((value >> (count - 1 - i)) & 1)
        bytes[position / 8] |= uint8_t(1u << (position % 8));
  }
  void run(uint32_t count) {
    if (!count)
      throw ExportError("Anet RLE cannot encode an empty run");
    unsigned key = 0;
    for (uint32_t remainder = count - 1; remainder; remainder >>= 1)
      ++key;
    write(key, 5);
    write(count, key + 1);
  }
};
sla::EncodedRaster encode_mask(const void *data, size_t columns, size_t rows,
                               size_t components) {
  if (!data || components != 1 || !columns || !rows || columns > 65535 ||
      rows > 65535 || uint64_t(columns) * rows > 128ULL * 1024 * 1024)
    throw ExportError("Invalid Anet mask dimensions");
  const auto *pixels = static_cast<const uint8_t *>(data);
  const size_t count = columns * rows;
  BitWriter bits;
  bits.write(uint32_t(columns), 16);
  bits.write(uint32_t(rows), 16);
  bool previous = pixels[0] > 127;
  bits.write(previous ? 1 : 0, 1);
  uint32_t run = 0, white = 0, xmin = uint32_t(columns), ymin = uint32_t(rows),
           xmax = 0, ymax = 0;
  for (size_t i = 0; i < count; ++i) {
    const bool value = pixels[i] > 127;
    if (value) {
      ++white;
      const auto x = uint32_t(i % columns), y = uint32_t(i / columns);
      xmin = std::min(xmin, x);
      ymin = std::min(ymin, y);
      xmax = std::max(xmax, x);
      ymax = std::max(ymax, y);
    }
    if (value != previous) {
      bits.run(run);
      run = 0;
      previous = value;
    }
    ++run;
  }
  // Flush the last run separately, including a one-pixel final color change.
  bits.run(run);
  if (!white)
    xmin = ymin = xmax = ymax = 0;
  std::vector<uint8_t> encoded;
  encoded.reserve(24 + bits.bytes.size());
  for (uint32_t value : {white, xmin, ymin, xmax, ymax, bits.position})
    append_u32(encoded, value);
  encoded.insert(encoded.end(), bits.bytes.begin(), bits.bytes.end());
  return sla::EncodedRaster(std::move(encoded), "anet-mask");
}
uint32_t integer_parameter(double value, const char *name) {
  if (!std::isfinite(value) || value < 0. ||
      value > std::numeric_limits<uint32_t>::max() ||
      std::abs(value - std::round(value)) > 1e-5)
    throw ExportError(std::string("Anet requires a whole-number ") + name);
  return uint32_t(std::llround(value));
}
void write_u32(std::ostream &out, uint32_t value) {
  std::vector<uint8_t> bytes;
  append_u32(bytes, value);
  out.write(reinterpret_cast<const char *>(bytes.data()), 4);
}
void write_double(std::ostream &out, double value) {
  static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559,
                "Anet requires IEEE-754 binary64");
  uint64_t bits;
  std::memcpy(&bits, &value, 8);
  for (int shift = 56; shift >= 0; shift -= 8)
    out.put(char(uint8_t(bits >> shift)));
}
void write_string(std::ostream &out, const std::string &value) {
  const auto utf16 = boost::locale::conv::utf_to_utf<char16_t>(
      value, boost::locale::conv::stop);
  if (utf16.size() > 32768)
    throw ExportError("Anet display name exceeds 64 KiB");
  write_u32(out, uint32_t(utf16.size() * 2));
  for (char16_t code : utf16) {
    out.put(char(uint8_t(code >> 8)));
    out.put(char(uint8_t(code)));
  }
}
std::vector<uint8_t> preview(const ThumbnailsList &thumbnails) {
  std::vector<uint8_t> bytes(72866, 0);
  std::copy(bmp_header.begin(), bmp_header.end(), bytes.begin());
  const ThumbnailData *source = nullptr;
  for (const auto &candidate : thumbnails)
    if (candidate.is_valid() &&
        candidate.pixels.size() ==
            size_t(candidate.width) * candidate.height * 4) {
      source = &candidate;
      break;
    }
  if (!source)
    return bytes; // Exactly the valid blank preview emitted by UVtools without
                  // a thumbnail.
  const double scale = std::min(260. / source->width, 140. / source->height);
  const size_t width = std::max(size_t(1), size_t(source->width * scale)),
               height = std::max(size_t(1), size_t(source->height * scale));
  const size_t ox = (260 - width) / 2, oy = (140 - height) / 2;
  for (size_t y = 0; y < height; ++y)
    for (size_t x = 0; x < width; ++x) {
      const size_t sx = x * source->width / width,
                   sy = source->height - 1 - y * source->height / height;
      const auto *rgba = source->pixels.data() + 4 * (sy * source->width + sx);
      const uint16_t color =
          rgba[3] ? uint16_t((uint16_t(rgba[0] >> 3) << 11) |
                             (uint16_t(rgba[1] >> 2) << 5) | (rgba[2] >> 3))
                  : 0;
      const size_t target = 66 + 2 * ((y + oy) * 260 + x + ox);
      bytes[target] = uint8_t(color);
      bytes[target + 1] = uint8_t(color >> 8);
    }
  return bytes;
}
void export_anet(const std::string &filename, const SLAPrint &print,
                 const ThumbnailsList &thumbnails, const std::string &project,
                 const SLAPrinterConfig &config,
                 const std::vector<sla::EncodedRaster> &layers,
                 const AnetPrinter &printer) {
  if (layers.empty() || layers.size() != print.print_layers().size() ||
      layers.size() > std::numeric_limits<uint32_t>::max())
    throw ExportError("Anet requires one encoded mask per print layer");
  const bool portrait = config.display_orientation.value == sladoPortrait;
  const size_t columns = size_t(portrait ? config.display_pixels_y.value
                                         : config.display_pixels_x.value);
  const size_t rows = size_t(portrait ? config.display_pixels_x.value
                                      : config.display_pixels_y.value);
  const double width =
      portrait ? config.display_height.value : config.display_width.value;
  const double height =
      portrait ? config.display_width.value : config.display_height.value;
  // UVtools restores the machine geometry from its Anet model catalogue;
  // this container has no independent width/height/Z fields. Don't silently
  // export a different printer and report the catalogue's geometry on read.
  if (columns != printer.columns || rows != printer.rows ||
      std::abs(width - printer.width) > .001 ||
      std::abs(height - printer.height) > .001 ||
      std::abs(config.max_print_height.value - printer.z) > .001)
    throw ExportError("Anet format requires the matching N4/N7 LCD dimensions, "
                      "orientation and machine height");
  const double layer_height = print.default_object_config().layer_height.value;
  if (!std::isfinite(layer_height) || layer_height <= 0.) throw ExportError("Invalid Anet layer height");
  for (size_t i = 0; i < layers.size(); ++i) {
    if (std::abs(sla::layer_z(print, i) - (i + 1) * layer_height) >
        2 * SCALING_FACTOR)
      throw ExportError(
          "Anet cannot store a different first layer height or variable Z");
  }
  if (print.material_config().sla_transition_layers.value != 0)
    throw ExportError("Anet cannot store transition exposures");
  auto motion = [&](const char *name, bool bottom = false) {
    const auto value = sla::motion_parameter(print, name, bottom);
    if (!std::isfinite(value)) throw ExportError(std::string("Invalid Anet motion setting: ") + name);
    return value;
  };
  for (bool bottom : {false, true}) {
    for (const char *name :
         {"lift_height2", "retract_height2", "wait_before_cure",
          "wait_after_cure", "wait_after_lift"})
      if (std::abs(motion(name, bottom)) > 1e-6)
        throw ExportError(std::string("Anet cannot store ") + name);
    if (motion("light_pwm", bottom) != 255)
      throw ExportError("Anet requires full UV power (PWM 255)");
    if (std::abs(motion("lift_height", bottom) - motion("lift_height")) >
            1e-6 ||
        std::abs(motion("lift_speed", bottom) - motion("lift_speed")) > 1e-6 ||
        std::abs(motion("retract_speed", bottom) - motion("lift_speed")) > 1e-6)
      throw ExportError("Anet uses one common lift/retract speed and lift "
                        "height for all layers");
  }
  const uint32_t lift_height =
      integer_parameter(motion("lift_height"), "lift height in mm");
  const uint32_t lift_speed = integer_parameter(motion("lift_speed") / 60.,
                                                "lift/retract speed in mm/s");
  if (!lift_speed)
    throw ExportError("Anet lift/retract speed must be positive");
  const auto normal_exposure = integer_parameter(
      print.material_config().exposure_time.value, "exposure in seconds");
  const auto bottom_exposure =
      integer_parameter(print.material_config().initial_exposure_time.value,
                        "bottom exposure in seconds");
  const auto bottom_count = sla::bottom_layers(print, layers.size());
  if (bottom_count > std::numeric_limits<uint16_t>::max())
    throw ExportError("Anet bottom layer count exceeds the UVtools reader range");
  double volume = 0.;
  for (const auto &layer : layers) {
    if (layer.size() < 29)
      throw ExportError("Invalid encoded Anet layer");
    const auto *bytes = static_cast<const uint8_t *>(layer.data());
    const uint32_t bit_count = read_u32(bytes + 20);
    if (uint64_t(24) + (uint64_t(bit_count) + 7) / 8 != layer.size())
      throw ExportError("Invalid Anet bit-stream size");
    volume += read_u32(bytes) * width / columns * height / rows * layer_height;
  }
  double time = 0.;
  for (size_t i=0;i<layers.size();++i) time += sla::lcd_layer_time(print,i);
  time = std::ceil(time);
  if (!std::isfinite(time) || time < 0. ||
      time > std::numeric_limits<uint32_t>::max())
    throw ExportError("Anet print time exceeds its header range");
  auto thumbnail = preview(thumbnails);
  boost::nowide::ofstream output(filename, std::ios::binary | std::ios::trunc);
  if (!output)
    throw FileIOError("Cannot write Anet archive");
  output.exceptions(std::ios::badbit | std::ios::failbit);
  write_string(output, "3");
  write_string(output, project.empty() ? "SLA-Slicer" : project);
  write_string(output, "");
  write_double(output, width / columns);
  write_double(output, layer_height);
  write_u32(output, 0);
  write_u32(output,
            0); // Firmware-generated base masks are deliberately disabled.
  write_u32(output, normal_exposure);
  write_u32(output, bottom_exposure);
  write_u32(output, uint32_t(bottom_count));
  write_u32(output, lift_speed);
  write_u32(output, lift_height);
  write_u32(output, 260);
  write_u32(output, 140);
  write_u32(output, uint32_t(thumbnail.size()));
  output.write(reinterpret_cast<const char *>(thumbnail.data()),
               std::streamsize(thumbnail.size()));
  write_double(output, volume);
  write_u32(output, uint32_t(time));
  write_u32(output, uint32_t(layers.size()));
  for (const auto &layer : layers)
    output.write(static_cast<const char *>(layer.data()),
                 std::streamsize(layer.size()));
  write_u32(output, 0); // Supports count, matching UVtools.
  output.close();
}
} // namespace
std::unique_ptr<sla::RasterBase> AnetN4Archive::create_raster() const {
  return sla::create_printer_raster(m_cfg);
}
sla::RasterEncoder AnetN4Archive::get_encoder() const { return encode_mask; }
void AnetN4Archive::export_print(const std::string filename,
                                 const SLAPrint &print,
                                 const ThumbnailsList &thumbnails,
                                 const std::string &project) {
  export_anet(filename, print, thumbnails, project, m_cfg, m_layers, n4);
}
std::unique_ptr<sla::RasterBase> AnetN7Archive::create_raster() const {
  return sla::create_printer_raster(m_cfg);
}
sla::RasterEncoder AnetN7Archive::get_encoder() const { return encode_mask; }
void AnetN7Archive::export_print(const std::string filename,
                                 const SLAPrint &print,
                                 const ThumbnailsList &thumbnails,
                                 const std::string &project) {
  export_anet(filename, print, thumbnails, project, m_cfg, m_layers, n7);
}
} // namespace Slic3r
