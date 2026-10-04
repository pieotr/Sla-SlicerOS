// Native Vlare OSF v4, based on the OSFFile schema in UVtools (AGPLv3).
#include "UVToolsCompatArchive.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include "libslic3r/SLAPrint.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>

namespace Slic3r {
namespace {
struct OSFBytes {
  std::vector<uint8_t> bytes;
  void put(uint64_t value, unsigned width) {
    if (value >= (uint64_t(1) << (width * 8)))
      throw ExportError("OSF integer field overflow");
    for (unsigned i = width; i > 0; --i)
      bytes.push_back(uint8_t(value >> ((i - 1) * 8)));
  }
};
uint32_t scaled(double value, uint32_t scale, uint32_t maximum,
                const char *name) {
  const double encoded = std::round(value * scale);
  if (!std::isfinite(value) || value < 0 || encoded > maximum ||
      std::abs(value * scale - encoded) > 1e-3)
    throw ExportError(std::string("OSF cannot represent ") + name +
                      " at its stored precision");
  return uint32_t(encoded);
}
struct OSFRasterEncoder {
  sla::EncodedRaster operator()(const void *data, size_t width, size_t height,
                                size_t components) const {
    if (!data || components != 1 || !width || !height || width > 32768 ||
        height > 32768 || width * height > 128 * 1024 * 1024)
      throw ExportError("Invalid OSF raster");
    const auto *pixels = static_cast<const uint8_t *>(data);
    OSFBytes runs;
    uint32_t count = 0;
    for (size_t first = 0; first < width * height;) {
      const uint8_t color = pixels[first] & 0xfe;
      size_t last = first + 1;
      while (last < width * height && (pixels[last] & 0xfe) == color)
        ++last;
      const uint32_t length = uint32_t(last - first);
      ++count;
      runs.put(color | (length > 1 ? 1 : 0), 1);
      if (length > 1) {
        // UVtools locates the next layer by 0D 0A/0B. A grayscale
        // run of 10/11 pixels must not resemble that layer marker.
        const bool marker = color == 0x0c && (length == 10 || length == 11);
        if (length <= 0x7f && !marker)
          runs.put(length, 1);
        else if (length <= 0x3fff)
          runs.put(0x8000u | length, 2);
        else if (length <= 0x1fffff)
          runs.put(0xc00000u | length, 3);
        else
          runs.put(0xe0000000u | length, 4);
      }
      first = last;
    }
    OSFBytes layer;
    layer.put(0x0d0a, 2);
    layer.put(count, 4);
    layer.put(0, 2); // StartY=0; complete mask, including blank tails.
    layer.bytes.insert(layer.bytes.end(), runs.bytes.begin(), runs.bytes.end());
    return sla::EncodedRaster(std::move(layer.bytes), "osf");
  }
};
std::vector<uint8_t> preview(unsigned width, unsigned height,
                             const ThumbnailData *source,
                             const png::ImageGreyscale *mask) {
  std::vector<uint8_t> bytes(size_t(width) * height * 2, 0);
  if (!source && !mask)
    return bytes;
  const size_t source_width = source ? source->width : mask->cols;
  const size_t source_height = source ? source->height : mask->rows;
  const double scale =
      std::min(double(width) / source_width, double(height) / source_height);
  const unsigned columns =
      std::max(1u, unsigned(std::round(source_width * scale)));
  const unsigned rows =
      std::max(1u, unsigned(std::round(source_height * scale)));
  const unsigned left = (width - columns) / 2, top = (height - rows) / 2;
  for (unsigned y = 0; y < rows; ++y)
    for (unsigned x = 0; x < columns; ++x) {
      const size_t sx = size_t(x) * source_width / columns,
                   sy = size_t(y) * source_height / rows;
      const uint8_t gray = mask ? mask->buf[sy * source_width + sx] : 0;
      const uint8_t grayscale[3] = {gray, gray, gray};
      const auto *rgba =
          source ? source->pixels.data() +
                       ((source_height - 1 - sy) * source_width + sx) * 4
                 : grayscale;
      const uint16_t rgb = uint16_t(((rgba[0] & 0xf8) << 8) |
                                    ((rgba[1] & 0xfc) << 3) | (rgba[2] >> 3));
      const size_t output = (size_t(top + y) * width + left + x) * 2;
      bytes[output] = uint8_t(rgb);
      bytes[output + 1] = uint8_t(rgb >> 8); // RGB565 little-endian.
    }
  return bytes;
}
} // namespace

sla::RasterEncoder UVToolsOSFArchive::get_encoder() const {
  const bool portrait = m_cfg.display_orientation.value == sladoPortrait;
  const auto columns = size_t(portrait ? m_cfg.display_pixels_y.value
                                       : m_cfg.display_pixels_x.value);
  const auto rows = size_t(portrait ? m_cfg.display_pixels_x.value
                                    : m_cfg.display_pixels_y.value);
  return [columns, rows](const void *pixels, size_t width, size_t height,
                         size_t components) {
    if (width != columns || height != rows)
      throw ExportError("OSF masks do not match the printer resolution");
    return OSFRasterEncoder{}(pixels, width, height, components);
  };
}

void UVToolsOSFArchive::export_print(const std::string filename,
                                     const SLAPrint &print,
                                     const ThumbnailsList &thumbnails,
                                     const std::string &) {
  if (m_layers.empty() || m_layers.size() != print.print_layers().size())
    throw ExportError("OSF layer count mismatch");
  const bool portrait = m_cfg.display_orientation.value == sladoPortrait;
  const auto columns = uint32_t(portrait ? m_cfg.display_pixels_y.value
                                         : m_cfg.display_pixels_x.value);
  const auto rows = uint32_t(portrait ? m_cfg.display_pixels_x.value
                                      : m_cfg.display_pixels_y.value);
  if (!columns || !rows || columns > 32768 || rows > 32768 ||
      uint64_t(columns) * rows > 128 * 1024 * 1024)
    throw ExportError("Invalid OSF printer resolution");
  const double display_width =
      portrait ? m_cfg.display_height.value : m_cfg.display_width.value;
  const double display_height =
      portrait ? m_cfg.display_width.value : m_cfg.display_height.value;
  const uint32_t pitch =
      scaled(display_width / columns, 100000, UINT16_MAX, "pixel pitch");
  if (pitch == 0 ||
      std::abs(display_height / rows - display_width / columns) > 1e-8)
    throw ExportError("OSF requires square pixels");
  const double height = print.default_object_config().layer_height.value;
  const auto layer_height = scaled(height, 100000, 0xffffff, "layer height");
  if (layer_height == 0)
    throw ExportError("OSF layer height must be positive");
  for (size_t i = 0; i < m_layers.size(); ++i)
    if (std::abs(double(print.print_layers()[i].level()) * SCALING_FACTOR -
                 (i + 1) * height) > SCALING_FACTOR * 2)
      throw ExportError(
          "OSF requires a uniform first and subsequent layer height");
  const auto bottom = sla::bottom_layers(print, m_layers.size());
  const auto transition =
      uint32_t(print.material_config().sla_transition_layers.value);
  auto parameter = [&](const char *name, bool first) {
    return double(sla::motion_parameter(print, name, first));
  };
  OSFBytes settings;
  settings.put(columns, 2);
  settings.put(rows, 2);
  settings.put(pitch, 2);
  settings.put(0, 1); // Canonical masks already contain printer mirroring.
  settings.put(scaled(parameter("light_pwm", true), 1, 255, "bottom PWM"), 1);
  settings.put(scaled(parameter("light_pwm", false), 1, 255, "normal PWM"), 1);
  settings.put(1, 1);
  settings.put(0, 1);
  settings.put(0, 1); // Grayscale AA; distortion/delayed activation disabled.
  settings.put(m_layers.size(), 4);
  settings.put(1, 2);
  settings.put(m_layers.size() - 1, 4);
  settings.put(layer_height, 3);
  settings.put(bottom, 1);
  settings.put(scaled(print.material_config().exposure_time.value, 100,
                      0xffffff, "exposure"),
               3);
  settings.put(scaled(print.material_config().initial_exposure_time.value, 100,
                      0xffffff, "bottom exposure"),
               3);
  settings.put(0, 3);
  settings.put(0, 3); // Additional support delays.
  settings.put(transition, 1);
  settings.put(0, 1);
  const double interval =
      transition ? (print.material_config().initial_exposure_time.value -
                    print.material_config().exposure_time.value) /
                       (transition + 1)
                 : 0.;
  settings.put(scaled(interval, 100, 0xffffff, "transition exposure interval"),
               3);
  for (const char *field :
       {"wait_after_cure", "wait_after_lift", "wait_before_cure"})
    settings.put(scaled(parameter(field, false), 100, 0xffffff, field), 3);
  for (const bool retract : {false, true})
    for (const bool first : {true, false}) {
      const double lift =
          parameter("lift_height", first) + parameter("lift_height2", first);
      const double slow =
          parameter(retract ? "retract_height2" : "lift_height", first);
      if (slow > lift)
        throw ExportError("OSF slow movement exceeds total lift height");
      settings.put(scaled(slow, 1000, 0xffffff, "slow height"), 3);
      settings.put(scaled(lift, 1000, 0xffffff, "total height"), 3);
    }
  settings.put(0, 1); // UVtools' default S-shaped acceleration.
  for (const bool retract : {false, true})
    for (const bool first : {true, false}) {
      settings.put(80, 2);
      settings.put(
          scaled(parameter(retract ? "retract_speed2" : "lift_speed", first), 1,
                 UINT16_MAX, "slow speed"),
          2);
      settings.put(
          scaled(parameter(retract ? "retract_speed" : "lift_speed2", first), 1,
                 UINT16_MAX, "fast speed"),
          2);
      settings.put(5, 1);
    }
  for (int i = 0; i < 4; ++i) {
    settings.put(7, 2);
    settings.put(5, 1);
  } // Default end speeds/deceleration.
  for (const char *field :
       {"wait_after_cure", "wait_after_lift", "wait_before_cure"})
    settings.put(scaled(parameter(field, true), 100, UINT16_MAX, field), 2);
  settings.put(0, 2);
  settings.put(0, 1);
  if (settings.bytes.size() != 126)
    throw ExportError("OSF settings table size mismatch");
  const ThumbnailData *thumbnail = nullptr;
  for (const auto &candidate : thumbnails)
    if (candidate.is_valid()) {
      thumbnail = &candidate;
      break;
    }
  std::vector<std::vector<uint8_t>> previews;
  png::ImageGreyscale fallback{};
  if (!thumbnail && print.layer_document())
    fallback = print.layer_document()->image(0);
  uint32_t header_size = 7 + 12 + 126;
  for (const auto &size : std::vector<std::pair<unsigned, unsigned>>{
           {148, 80}, {300, 140}, {208, 116}, {404, 240}}) {
    previews.push_back(preview(size.first, size.second, thumbnail,
                               fallback.buf.empty() ? nullptr : &fallback));
    header_size += uint32_t(previews.back().size());
  }
  OSFBytes header;
  header.put(header_size, 4);
  header.put(4, 2);
  header.put(2, 1);
  std::ofstream output(filename, std::ios::binary | std::ios::trunc);
  if (!output)
    throw ExportError("Cannot create OSF output");
  auto write = [&](const std::vector<uint8_t> &bytes) {
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 std::streamsize(bytes.size()));
  };
  write(header.bytes);
  for (const auto &image : previews) {
    OSFBytes length;
    length.put(image.size(), 3);
    write(length.bytes);
    write(image);
  }
  write(settings.bytes);
  for (const auto &layer : m_layers)
    output.write(reinterpret_cast<const char *>(layer.data()),
                 std::streamsize(layer.size()));
  output.close();
  if (!output)
    throw ExportError("Cannot finish OSF output");
}
} // namespace Slic3r
