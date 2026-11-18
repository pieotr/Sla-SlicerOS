#include "CalibrationRaster.hpp"
#include "ExposureFinderTable.hpp"
#include "AGGRaster.hpp"
#include "PrinterRaster.hpp"
#include "GeneratedLayerOwnership.hpp"
#include "ProjectedPNGMaskEncoder.hpp"
#include "HersheyFontData.hpp"
#include "LayerMemoryBudget.hpp"
#include "libslic3r/Exception.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <opencv2/imgproc.hpp>
#include <sstream>
namespace Slic3r::sla {
void CalibrationRecipe::validate() const {
  const auto &p = parameters;
  if (int(p.pattern) < 0 || int(p.pattern) > 3 || int(precision) < 0 || int(precision) > 18)
    throw RuntimeError("Invalid calibration recipe pattern or palette");
  if ((p.pattern == CalibrationPattern::ExposureFinder) != bool(exposure_finder))
    throw RuntimeError("ExposureFinder recipe requires matching typed table provenance");
  if (exposure_finder) {
    if (p.lift_taper_step || p.lift_taper_min != 10)
      throw RuntimeError("ExposureFinder recipe cannot contain lift taper settings");
    exposure_finder->validate();
  }
  for (double value : {p.size_x, p.size_y, p.height, p.margin})
    if (!std::isfinite(value) || value < 0 || value > 100000.)
      throw RuntimeError("Invalid calibration recipe dimension");
  if (p.size_x == 0 || p.size_y == 0 || p.height == 0)
    throw RuntimeError("Calibration recipe dimensions must be positive");
  if (p.lift_taper_step > 99 || p.lift_taper_min < 1 || p.lift_taper_min > 99 ||
      (p.pattern != CalibrationPattern::LiftArea && p.lift_taper_step != 0) ||
      (p.lift_taper_step != 0 && 100 - p.lift_taper_step < p.lift_taper_min))
    throw RuntimeError("Invalid lift calibration size range");
  if (p.pattern != CalibrationPattern::GrayscalePatches) {
    if (!requested.empty() || !effective.empty())
      throw RuntimeError("Non-grayscale calibration recipe contains dose levels");
    return;
  }
  if (p.grayscale_steps < 2 || p.grayscale_steps > 256 ||
      p.grayscale_first >= p.grayscale_last || requested.size() != p.grayscale_steps ||
      effective.size() != requested.size())
    throw RuntimeError("Invalid calibration recipe grayscale range");
  std::array<bool, 256> seen{};
  for (unsigned i = 0; i < p.grayscale_steps; ++i) {
    const auto value = uint8_t(std::lround(p.grayscale_first +
        double(p.grayscale_last - p.grayscale_first) * i / (p.grayscale_steps - 1)));
    if (requested[i] != value || effective[i] != project_mask_value(value, precision) || seen[effective[i]])
      throw RuntimeError("Invalid calibration recipe requested/effective levels");
    seen[effective[i]] = true;
  }
}
std::shared_ptr<const CalibrationRecipe> CalibrationRecipe::clone() const {
  auto copy = std::make_shared<CalibrationRecipe>(*this);
  if (exposure_finder)
    copy->exposure_finder = std::make_shared<const ExposureFinderRecipe>(*exposure_finder);
  return copy;
}
namespace {
class ColoredRaster final : public RasterGrayscaleAA {
public:
  ColoredRaster(Resolution resolution, PixelDim pitch, const Trafo &transform)
      : RasterGrayscaleAA(resolution, pitch, transform, agg::gamma_none()) {}
  void color(uint8_t value) {
    m_renderer.color(_RasterGrayscaleAA::TColor{value});
  }
};
ExPolygon rectangle(double x, double y, double width, double height) {
  return ExPolygon{
      Polygon{Points{Point::new_scale(x, y), Point::new_scale(x + width, y),
                     Point::new_scale(x + width, y + height),
                     Point::new_scale(x, y + height)}}};
}
size_t calibration_layer_count(const DynamicPrintConfig &config, double height) {
  const double layer_height = config.opt_float("layer_height");
  if (!std::isfinite(layer_height) || layer_height <= 0. || !std::isfinite(height) || height <= 0.)
    throw RuntimeError("Invalid calibration layer height or test height");
  const double count = std::round(height / layer_height);
  if (!std::isfinite(count) || count < 1. || count > 100000. ||
      std::abs(count * layer_height - height) > 1e-6)
    throw RuntimeError("Calibration height must be an exact multiple of layer height (maximum 100000 layers)");
  return size_t(count);
}
} // namespace
CalibrationLiftPlan calibration_lift_plan(
    const DynamicPrintConfig &configuration, const CalibrationParameters &parameters) {
  CalibrationRecipe recipe;
  recipe.parameters = parameters;
  recipe.validate();
  if (parameters.pattern != CalibrationPattern::LiftArea)
    throw RuntimeError("Lift size planning requires the Lift area pattern");
  DynamicPrintConfig config;
  config.apply(SLAFullPrintConfig{}); config.apply(configuration);
  const size_t count = calibration_layer_count(config, parameters.height);
  CalibrationLiftPlan plan;
  if (parameters.lift_taper_step) {
    // Signed subtraction avoids unsigned wraparound at the last factor.
    for (int factor = 100 - int(parameters.lift_taper_step);
         factor >= int(parameters.lift_taper_min); factor -= int(parameters.lift_taper_step))
      plan.factors.push_back(unsigned(factor));
    const int bottom = config.opt_int("sla_bottom_layers");
    if (bottom < 0 || count <= plan.factors.size() || count - plan.factors.size() < size_t(bottom))
      throw RuntimeError("Increase test height: decreasing sizes must follow all bottom layers and at least one full-size layer");
  }
  plan.base_layers = count - plan.factors.size();
  return plan;
}
CalibrationGrayscalePlan calibration_grayscale_plan(
    const DynamicPrintConfig &configuration, const CalibrationParameters &parameters) {
  if (parameters.grayscale_steps < 2 || parameters.grayscale_steps > 256 ||
      parameters.grayscale_first >= parameters.grayscale_last)
    throw RuntimeError("Grayscale calibration requires 2–256 ordered values");
  DynamicPrintConfig config;
  config.apply(SLAFullPrintConfig{}); config.apply(configuration);
  CalibrationGrayscalePlan plan;
  const auto format = config.opt_string("sla_archive_format");
  if(const auto precision=independent_generated_mask_precision(format,config.opt_string("sla_anycubic_version")))
    plan.precision=*precision;
  else if (generated_layer_ownership(format, FreshLayerProducer::Calibration,
                                config.opt_string("sla_anycubic_version")))
    plan.precision = generated_mask_precision(format, config.opt_int("sla_cbddlp_aa"),
                                             config.opt_string("sla_anycubic_version"));
  std::array<bool, 256> seen{};
  for (unsigned i = 0; i < parameters.grayscale_steps; ++i) {
    const auto requested = uint8_t(std::lround(parameters.grayscale_first +
        double(parameters.grayscale_last - parameters.grayscale_first) * i /
            (parameters.grayscale_steps - 1)));
    const auto effective = project_mask_value(requested, plan.precision);
    plan.requested.push_back(requested); plan.effective.push_back(effective);
    plan.changed_levels += requested != effective;
    if (!seen[effective]) { seen[effective] = true; ++plan.distinct_levels; }
  }
  return plan;
}
std::unique_ptr<SLAPrint>
create_calibration(const DynamicPrintConfig &configuration,
                   const CalibrationParameters &parameters) {
  if (parameters.pattern == CalibrationPattern::ExposureFinder)
    throw RuntimeError("ExposureFinder requires its dedicated generator; the basic pattern generator cannot create it");
  DynamicPrintConfig config;
  config.apply(SLAFullPrintConfig{});
  config.apply(configuration);
  if (config.option("printer_technology")->getInt() != ptSLA)
    throw RuntimeError("Calibration requires an SLA printer");
  CalibrationLiftPlan lift;
  if (parameters.pattern == CalibrationPattern::LiftArea)
    lift = calibration_lift_plan(config, parameters);
  else if (parameters.lift_taper_step)
    throw RuntimeError("Decreasing sizes require the Lift area pattern");
  CalibrationGrayscalePlan grayscale;
  if (parameters.pattern == CalibrationPattern::GrayscalePatches) {
    grayscale = calibration_grayscale_plan(config, parameters);
    if (grayscale.has_duplicates())
      throw RuntimeError("Grayscale calibration levels repeat in the generated palette; reduce steps or widen the grayscale range");
  }
  SLAPrinterConfig printer;
  printer.apply(config, true);
  // Validate resolution/memory limits with the same contract as mesh/PCB masks.
  auto reference = create_printer_raster(printer);
  const auto transform = reference->trafo();
  reference.reset();
  for (double value : {parameters.size_x, parameters.size_y, parameters.height,
                       parameters.margin})
    if (!std::isfinite(value) || value < 0. || value > 100000.)
      throw RuntimeError("Invalid calibration dimension");
  const double width = printer.display_width.value,
               height = printer.display_height.value;
  const double sx = parameters.size_x, sy = parameters.size_y,
               margin = parameters.margin;
  if (sx <= 0. || sy <= 0. || parameters.height <= 0. ||
      parameters.height > printer.max_print_height.value)
    throw RuntimeError(
        "Calibration sizes and height must be positive and fit the printer");
  const double layer_height = config.opt_float("layer_height");
  const size_t count = calibration_layer_count(config, parameters.height);
  Resolution resolution{size_t(printer.display_pixels_x.value),
                        size_t(printer.display_pixels_y.value)};
  const PixelDim pitch{width / resolution.width_px,
                       height / resolution.height_px};
  if (printer.display_orientation.value == sladoPortrait)
    std::swap(resolution.width_px, resolution.height_px);
  ColoredRaster raster{resolution, pitch, transform};
  unsigned cols = 1, rows = 1;
  if (parameters.pattern == CalibrationPattern::XYZBlocks) {
    cols = rows = 3;
  } else if (parameters.pattern == CalibrationPattern::GrayscalePatches) {
    cols = unsigned(std::ceil(std::sqrt(double(parameters.grayscale_steps))));
    rows = (parameters.grayscale_steps + cols - 1) / cols;
  }
  if (cols * sx + (cols + 1) * margin > width ||
      rows * sy + (rows + 1) * margin > height)
    throw RuntimeError("Calibration patterns do not fit the selected LCD. "
                       "Reduce their size or margin.");
  if (parameters.pattern == CalibrationPattern::XYZBlocks) {
    for (unsigned row = 0; row < 3; ++row)
      for (unsigned col = 0; col < 3; ++col) {
        const double x = col == 0   ? margin
                         : col == 1 ? (width - sx) * .5
                                    : width - margin - sx;
        const double y = row == 0   ? margin
                         : row == 1 ? (height - sy) * .5
                                    : height - margin - sy;
        raster.draw(rectangle(x, y, sx, sy));
      }
  } else {
    const double origin_x = (width - cols * sx - (cols - 1) * margin) * .5;
    const double origin_y = (height - rows * sy - (rows - 1) * margin) * .5;
    const unsigned patterns =
        parameters.pattern == CalibrationPattern::GrayscalePatches
            ? parameters.grayscale_steps
            : 1;
    for (unsigned i = 0; i < patterns; ++i) {
      if (patterns > 1)
        raster.color(grayscale.effective[i]);
      raster.draw(rectangle(origin_x + (i % cols) * (sx + margin),
                            origin_y + (i / cols) * (sy + margin), sx, sy));
    }
  }
  // Keep the canonical source PNG (including AA edges) equal to its native
  // PW0 decode. Palette support does not manufacture an owned parameter writer.
  const auto independent_precision=independent_generated_mask_precision(
      config.opt_string("sla_archive_format"),config.opt_string("sla_anycubic_version"));
  const RasterEncoder encoder=independent_precision
      ? RasterEncoder(ProjectedPNGMaskEncoder{*independent_precision,{}})
      : RasterEncoder(PNGRasterEncoder{});
  const auto encoded = raster.encode(encoder);
  if (encoded.size() == 0 ||
      uint64_t(encoded.size()) * count > layer_editor_byte_budget())
    throw RuntimeError("Calibration exceeds the " + layer_mib(layer_editor_byte_budget()) + " encoded-mask budget");
  std::vector<EncodedRaster> masks(lift.factors.empty() ? count : lift.base_layers, encoded);
  uint64_t mask_bytes = uint64_t(encoded.size()) * masks.size();
  for (unsigned factor : lift.factors) {
    ColoredRaster reduced{resolution, pitch, transform};
    const double reduced_x = sx * factor / 100., reduced_y = sy * factor / 100.;
    reduced.draw(rectangle((width - reduced_x) * .5, (height - reduced_y) * .5, reduced_x, reduced_y));
    auto mask = reduced.encode(encoder);
    mask_bytes += mask.size();
    if (mask.size() == 0 || mask_bytes > layer_editor_byte_budget())
      throw RuntimeError("Calibration exceeds the " + layer_mib(layer_editor_byte_budget()) + " encoded-mask budget");
    masks.push_back(std::move(mask));
  }
  std::vector<double> positions;
  positions.reserve(count);
  for (size_t i = 0; i < count; ++i)
    positions.push_back((i + 1) * layer_height);
  config.set("gamma_correction", 1.);
  config.set("initial_layer_height", layer_height);
  config.set("sla_transition_layers", 0);
  auto job = SLAPrint::create_raster_job(
      config, independent_precision ? LayerDocument::from_precision_rasters(std::move(masks),*independent_precision) :
          LayerDocument::from_rasters(std::move(masks)), positions,
      generated_layer_ownership(config.opt_string("sla_archive_format"),
                                FreshLayerProducer::Calibration,
                                config.option("sla_anycubic_version")?config.opt_string("sla_anycubic_version"):"auto"));
  CalibrationRecipe recipe;
  recipe.parameters = parameters;
  if (const auto &fresh = job->layer_document()->fresh_job()) recipe.precision = fresh->mask_precision;
  else if(independent_precision)recipe.precision=*independent_precision;
  recipe.requested = grayscale.requested; recipe.effective = grayscale.effective;
  job->set_calibration_recipe(std::make_shared<const CalibrationRecipe>(std::move(recipe)));
  return job;
}

// ===========================================================================
// UVtools-derived pixel-domain calibrations (elephant foot, blooming, stress
// tower). All three draw into 8-bit OpenCV images in oriented raster pixels.
// ===========================================================================
namespace {
constexpr size_t max_calibration_layers = 100000;

struct PixelSetup {
  DynamicPrintConfig config;
  int width = 0, height = 0;  // oriented raster size, px
  double pitch_w_mm = 0, pitch_h_mm = 0;  // mm per oriented raster px
  double layer_height = 0;                // mm
  double max_height = 0;                  // mm
  bool mirror_x = false, mirror_y = false;
  std::optional<LayerMaskPrecision> independent;
  RasterEncoder encoder;
  double pixels_per_mm() const { return std::max(1. / pitch_w_mm, 1. / pitch_h_mm); }
};

PixelSetup make_pixel_setup(const DynamicPrintConfig &configuration, const CalibrationCheck &check) {
  PixelSetup setup;
  setup.config.apply(SLAFullPrintConfig{});
  setup.config.apply(configuration);
  if (setup.config.option("printer_technology")->getInt() != ptSLA)
    throw RuntimeError("Calibration requires an SLA printer");
  SLAPrinterConfig printer;
  printer.apply(setup.config, true);
  // Same resolution/memory contract as mesh and PCB masks.
  create_printer_raster(printer).reset();
  const double dw = printer.display_width.value, dh = printer.display_height.value;
  const int px = int(printer.display_pixels_x.value), py = int(printer.display_pixels_y.value);
  const bool portrait = printer.display_orientation.value == sladoPortrait;
  setup.width = portrait ? py : px;
  setup.height = portrait ? px : py;
  setup.pitch_w_mm = (portrait ? dh : dw) / setup.width;
  setup.pitch_h_mm = (portrait ? dw : dh) / setup.height;
  setup.layer_height = setup.config.opt_float("layer_height");
  if (!std::isfinite(setup.layer_height) || setup.layer_height <= 0.)
    throw RuntimeError("Invalid calibration layer height");
  setup.max_height = printer.max_print_height.value;
  setup.mirror_x = printer.display_mirror_x.value;
  setup.mirror_y = printer.display_mirror_y.value;
  const auto format = setup.config.opt_string("sla_archive_format");
  const auto version = setup.config.opt_string("sla_anycubic_version");
  setup.independent = independent_generated_mask_precision(format, version);
  setup.encoder = setup.independent ? RasterEncoder(ProjectedPNGMaskEncoder{*setup.independent, check})
                                    : RasterEncoder(PNGRasterEncoder{});
  return setup;
}

// Layers for a height in mm; the epsilon absorbs 3/0.05 style float error,
// matching UVtools' decimal truncation.
unsigned layers_for(double mm, double layer_height) {
  if (!std::isfinite(mm) || mm < 0.) throw RuntimeError("Invalid calibration height");
  const double count = std::floor(mm / layer_height + 1e-6);
  if (count > double(max_calibration_layers)) throw RuntimeError("Calibration exceeds 100000 layers");
  return unsigned(count);
}

// Compact C# "decimal" style number: 2 decimals at most, no trailing zeros.
std::string format_number(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.2f", value);
  std::string text = buffer;
  while (text.back() == '0') text.pop_back();
  if (text.back() == '.') text.pop_back();
  return text;
}

// --- Hershey Duplex text (OpenCV putText metrics) -------------------------
// ExposureFinderPattern keeps its own copy in an anonymous namespace; only the
// Duplex face is needed here.
const int *duplex_font() { return hershey::fonts[2]; }
const char *duplex_glyph(char character) {
  unsigned c = static_cast<unsigned char>(character);
  if (c < 32 || c > 126) c = '?';
  return hershey::glyphs[duplex_font()[c - 32 + 1]];
}
int text_line_height(double scale, int thickness) {
  const int font = duplex_font()[0];
  return int(std::lrint((((font >> 4) & 15) + (font & 15)) * scale + (thickness + 1) / 2));
}
void put_text_line(cv::Mat &image, const std::string &text, cv::Point origin, double scale, int color,
                   int thickness, int line_type) {
  constexpr int64_t unit = int64_t(1) << 16;  // OpenCV 16.16 fixed point
  const int64_t factor = std::lrint(scale * unit);
  int64_t x = int64_t(origin.x) * unit, y = int64_t(origin.y) * unit - int64_t(duplex_font()[0] & 15) * factor;
  for (char character : text) {
    const char *p = duplex_glyph(character);
    x -= int64_t(p[0] - 'R') * factor;
    const int64_t advance = int64_t(p[1] - 'R') * factor;
    p += 2;
    std::vector<cv::Point> stroke;
    const auto flush = [&] {
      if (stroke.size() > 1)
        cv::polylines(image, std::vector<std::vector<cv::Point>>{stroke}, false, cv::Scalar(color), thickness,
                      line_type, 16);
      stroke.clear();
    };
    while (*p) {
      if (*p == ' ') { flush(); ++p; continue; }
      const int64_t px = x + int64_t(p[0] - 'R') * factor, py = y + int64_t(p[1] - 'R') * factor;
      if (px < INT32_MIN || px > INT32_MAX || py < INT32_MIN || py > INT32_MAX)
        throw RuntimeError("Calibration text exceeds fixed-point coordinate limits");
      stroke.emplace_back(int(px), int(py));
      p += 2;
    }
    flush();
    x += advance;
  }
}
// Multi-line text with UVtools' PutTextExtended line spacing.
void put_text(cv::Mat &image, const std::string &text, cv::Point origin, double scale, int color,
              int thickness, bool antialiasing, int line_gap_offset = 0) {
  thickness = std::max(1, thickness);
  const int line_type = antialiasing ? cv::LINE_AA : cv::LINE_8;
  const int height = text_line_height(scale, thickness);
  const int step = height + height / 3 + line_gap_offset;
  std::istringstream stream(text);
  std::string line;
  for (int i = 0; std::getline(stream, line, '\n'); ++i)
    if (!line.empty()) put_text_line(image, line, {origin.x, origin.y + i * step}, scale, color, thickness, line_type);
}

// --- shared job assembly ---------------------------------------------------
cv::Mat blank(const PixelSetup &setup) { return cv::Mat::zeros(setup.height, setup.width, CV_8UC1); }

// The printer profile's mirroring is applied to the output raster, like the
// AGG pipeline does for model slices (UVtools' "mirror output" default).
void apply_mirror(const PixelSetup &setup, cv::Mat &image) {
  if (setup.mirror_x) cv::flip(image, image, 1);
  if (setup.mirror_y) cv::flip(image, image, 0);
}

EncodedRaster encode_mask(const PixelSetup &setup, const cv::Mat &image) {
  CV_Assert(image.isContinuous() && image.type() == CV_8UC1);
  return setup.encoder(image.data, size_t(image.cols), size_t(image.rows), 1);
}

struct MaskBudget {
  uint64_t bytes = 0;
  void add(const EncodedRaster &mask, size_t repeat = 1) {
    if (mask.size() == 0) throw RuntimeError("Calibration produced an empty mask");
    bytes += uint64_t(mask.size()) * repeat;
    if (bytes > layer_editor_byte_budget()) throw RuntimeError("Calibration exceeds the " + layer_mib(layer_editor_byte_budget()) + " encoded-mask budget");
  }
};

std::unique_ptr<SLAPrint> finish_job(PixelSetup &setup, std::vector<EncodedRaster> masks,
                                     const std::vector<double> &positions, ExposurePositionPolicy policy,
                                     unsigned bottom_layers) {
  auto &config = setup.config;
  config.set("gamma_correction", 1.);
  config.set("initial_layer_height", setup.layer_height);
  config.set("sla_transition_layers", 0);
  config.set("sla_bottom_layers", int(bottom_layers));
  const std::string format = config.opt_string("sla_archive_format");
  const std::string version = config.option("sla_anycubic_version") ? config.opt_string("sla_anycubic_version") : "auto";
  return SLAPrint::create_raster_job(
      config,
      setup.independent ? LayerDocument::from_precision_rasters(std::move(masks), *setup.independent)
                        : LayerDocument::from_rasters(std::move(masks)),
      positions, generated_layer_ownership(format, FreshLayerProducer::Calibration, version), policy);
}

std::vector<double> increasing_positions(size_t count, double layer_height) {
  std::vector<double> positions;
  positions.reserve(count);
  for (size_t i = 0; i < count; ++i) positions.push_back(double(i + 1) * layer_height);
  return positions;
}

// Moves the image content by (dx, dy) px; what leaves the screen is dropped.
cv::Mat translated(const cv::Mat &image, int dx, int dy) {
  cv::Mat result = cv::Mat::zeros(image.size(), image.type());
  const cv::Rect source = cv::Rect(0, 0, image.cols, image.rows) & cv::Rect(-dx, -dy, image.cols, image.rows);
  if (source.area() > 0) image(source).copyTo(result(source + cv::Point(dx, dy)));
  return result;
}

// Bounding box of all non-zero pixels (empty rectangle for a blank image).
cv::Rect non_zero_bounds(const cv::Mat &image) {
  cv::Mat columns, rows;
  cv::reduce(image, columns, 0, cv::REDUCE_MAX);  // 1 x width
  cv::reduce(image, rows, 1, cv::REDUCE_MAX);     // height x 1
  const auto span = [](const cv::Mat &profile, int length, int &first, int &last) {
    first = length; last = -1;
    for (int i = 0; i < length; ++i)
      if (profile.data[i]) { first = std::min(first, i); last = i; }
  };
  int x0, x1, y0, y1;
  span(columns, image.cols, x0, x1);
  span(rows, image.rows, y0, y1);
  return x1 < 0 || y1 < 0 ? cv::Rect() : cv::Rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

// Normalised ellipse axes that keep a circle round on non-square pixels
// (UVtools PixelsToNormalizedPitch).
cv::Size normalized_pitch(const PixelSetup &setup, int size) {
  const double sw = setup.pitch_h_mm / setup.pitch_w_mm, sh = setup.pitch_w_mm / setup.pitch_h_mm;
  if (sw == sh) return {size, size};
  return sw > sh ? cv::Size(int(size * sw), size) : cv::Size(size, int(size * sh));
}
void draw_disc(cv::Mat &image, cv::Point center, cv::Size radius, bool antialiasing) {
  const int line_type = antialiasing ? cv::LINE_AA : cv::LINE_8;
  if (radius.width == radius.height) cv::circle(image, center, radius.width, cv::Scalar(255), cv::FILLED, line_type);
  else cv::ellipse(image, center, radius, 0, 0, 360, cv::Scalar(255), cv::FILLED, line_type);
}
void erase_disc(cv::Mat &image, cv::Point center, cv::Size radius, bool antialiasing) {
  const int line_type = antialiasing ? cv::LINE_AA : cv::LINE_8;
  if (radius.width == radius.height) cv::circle(image, center, radius.width, cv::Scalar(0), cv::FILLED, line_type);
  else cv::ellipse(image, center, radius, 0, 0, 360, cv::Scalar(0), cv::FILLED, line_type);
}

// --- elephant foot ---------------------------------------------------------
constexpr int elephant_start_px = 2;  // UVtools' startX/startY border

void validate_elephant(const ElephantFootParameters &p) {
  if (!std::isfinite(p.part_scale) || p.part_scale < .1 || p.part_scale > 50.)
    throw RuntimeError("Part scale must be between 0.1 and 50");
  if (p.margin_px > 10000) throw RuntimeError("Part margin is too large");
  if (!std::isfinite(p.text_height_mm) || p.text_height_mm < 0. || p.text_height_mm > 1000.)
    throw RuntimeError("Invalid text height");
  if (p.erode) {
    if (p.erode_step < 1 || p.erode_start > 255 || p.erode_end > 255)
      throw RuntimeError("Erode iterations must be 0-255 with a step of at least 1");
    if (p.erode_start > p.erode_end)
      throw RuntimeError("Erode start iterations can't be higher than end iterations.");
  }
  if (p.dimming) {
    if (p.dimming_step < 1 || p.dimming_start > 255 || p.dimming_end > 255 || p.dimming_wall_px > 255)
      throw RuntimeError("Wall dimming brightness must be 0-255 with a step of at least 1");
    if (p.dimming_start > p.dimming_end)
      throw RuntimeError("Wall dimming start brightness can't be higher than end brightness.");
  }
}

// The test part: a stepped "tooth" outline with a hole and an elliptic foot,
// drawn at 250 x 250 px for scale 1 (UVtools geometry).
cv::Mat elephant_shape(const PixelSetup &setup, double scale, bool antialiasing) {
  const int line_type = antialiasing ? cv::LINE_AA : cv::LINE_8;
  const int length = int(250 * scale), triangle = int(50 * scale);
  int x = elephant_start_px, y = elephant_start_px, max_x = x, max_y = y;
  std::vector<cv::Point> points{{x, y}};
  const auto add = [&] {
    max_x = std::max(max_x, x);
    max_y = std::max(max_y, y);
    points.emplace_back(x, y);
  };
  x += length; add();
  x -= triangle; y += triangle; add();
  y += triangle; add();
  x += triangle; y += triangle; add();
  x -= triangle; y += triangle; add();
  x += triangle; y += triangle; add();
  x -= triangle; y += triangle; add();
  x += triangle; add();
  y += triangle; add();
  x -= triangle; y += triangle; add();
  x = elephant_start_px; add();
  const int ellipse_height = int(50 * scale);
  max_y += ellipse_height;
  const cv::Size size(max_x + elephant_start_px, max_y + elephant_start_px);
  if (size.width <= 0 || size.height <= 0 || size.width > setup.width || size.height > setup.height)
    throw RuntimeError("The elephant foot part does not fit the screen; reduce the part scale");
  cv::Mat shape = cv::Mat::zeros(size, CV_8UC1);
  cv::fillPoly(shape, std::vector<std::vector<cv::Point>>{points}, cv::Scalar(255), line_type);
  erase_disc(shape, {0, 0}, normalized_pitch(setup, length / 4), antialiasing);
  cv::ellipse(shape, {max_x / 2, max_y - ellipse_height}, {max_x / 3, ellipse_height}, 0, 0, 360, cv::Scalar(255),
              cv::FILLED, line_type);
  erase_disc(shape, {length / 2, int(max_y - 100 * scale)}, normalized_pitch(setup, length / 5), antialiasing);
  return shape;
}

ElephantFootPlan plan_elephant(const PixelSetup &setup, const ElephantFootParameters &requested) {
  validate_elephant(requested);
  ElephantFootParameters p = requested;
  p.part_scale = std::round(p.part_scale * 100.) / 100.;
  ElephantFootPlan plan;
  plan.bottom_layers = p.bottom_layers ? p.bottom_layers : layers_for(1., setup.layer_height);
  plan.normal_layers = p.normal_layers ? p.normal_layers : layers_for(3.5, setup.layer_height);
  plan.text_layers = p.extrude_text ? layers_for(p.text_height_mm, setup.layer_height) : 0;
  plan.total_layers = plan.bottom_layers + plan.normal_layers + plan.text_layers;
  if (plan.bottom_layers < 1 || plan.normal_layers < 1) throw RuntimeError("Bottom and normal layers must be at least 1");
  if (plan.total_layers > max_calibration_layers) throw RuntimeError("Calibration exceeds 100000 layers");
  if (plan.total_layers * setup.layer_height > setup.max_height + 1e-9)
    throw RuntimeError("The calibration is taller than the printer");
  const int sizing = int(250 * p.part_scale);
  if (sizing < 8) throw RuntimeError("Part scale is too small");
  // Only the shape size is needed for layout; build it once on a scratch grid.
  const cv::Mat shape = elephant_shape(setup, p.part_scale, p.antialiasing);
  plan.shape_width_px = shape.cols;
  plan.shape_height_px = shape.rows;
  const int w = shape.cols, h = shape.rows, margin = int(p.margin_px);
  plan.requested_objects = (p.output_original ? 1 : 0) + (p.erode ? (p.erode_end - p.erode_start) / p.erode_step + 1 : 0) +
                           (p.dimming ? (p.dimming_end - p.dimming_start) / p.dimming_step + 1 : 0);
  if (plan.requested_objects == 0) throw RuntimeError("No objects to output, please adjust the settings.");
  if (w + elephant_start_px >= setup.width || h >= setup.height)
    throw RuntimeError("The elephant foot part does not fit the screen; reduce the part scale");
  int cx = 0, cy = 0;
  unsigned count = 0;
  if (p.output_original) {
    plan.cells.push_back({ElephantFootCell::Kind::Original, 0, 0, 0, ++count});
  } else
    cx -= w + margin;
  bool full = false;
  // Erode and dimming rows share the flow; they differ only in the x restart.
  const auto place = [&](ElephantFootCell::Kind kind, unsigned value, int restart_x) {
    cx += w + margin;
    if (cx + w >= setup.width) { cx = restart_x; cy += h + margin; }
    if (cy + h >= setup.height) { full = true; return; }
    plan.cells.push_back({kind, cx, cy, value, ++count});
  };
  if (p.erode)
    for (unsigned it = p.erode_start; it <= p.erode_end && !full; it += p.erode_step)
      place(ElephantFootCell::Kind::Erode, it, elephant_start_px);
  if (p.dimming && !full)
    for (unsigned b = p.dimming_start; b <= p.dimming_end && !full; b += p.dimming_step)
      place(ElephantFootCell::Kind::Dimming, b, 0);
  return plan;
}
} // namespace

ElephantFootPlan elephant_foot_plan(const DynamicPrintConfig &configuration, const ElephantFootParameters &parameters) {
  return plan_elephant(make_pixel_setup(configuration, {}), parameters);
}

std::unique_ptr<SLAPrint> create_elephant_foot_calibration(
    const DynamicPrintConfig &configuration, const ElephantFootParameters &requested, const CalibrationCheck &check) {
  auto setup = make_pixel_setup(configuration, check);
  const auto plan = plan_elephant(setup, requested);
  const double scale = std::round(requested.part_scale * 100.) / 100.;
  const bool aa = requested.antialiasing;
  const cv::Mat shape = elephant_shape(setup, scale, aa);
  const cv::Rect shape_rect(0, 0, shape.cols, shape.rows);
  // 0 = bottom layers (modified parts), 1 = normal layers (original parts,
  // engraved labels), 2 = raised label layers.
  cv::Mat layers[3] = {blank(setup), blank(setup), blank(setup)};
  const double bottom_exposure = setup.config.opt_float("initial_exposure_time"),
               normal_exposure = setup.config.opt_float("exposure_time");
  const int microns = int(setup.layer_height * 1000.);
  const int font_margin = int(42 * scale), font_x = int(30 * scale), font_y = int(250 * scale) / 4 + font_margin;
  const double font_scale = 1.3 * scale;
  const int font_thickness = int(3 * scale);
  const int color = requested.extrude_text ? 255 : 0;
  const auto label = [&](cv::Mat &mat, unsigned number, const std::vector<std::string> &extra) {
    put_text(mat, std::to_string(number), {int(100 * scale), int(55 * scale)}, 1.5 * scale, color, int(4 * scale), aa);
    put_text(mat, "SLA EF", {font_x, font_y}, .8 * scale, color, int(2 * scale), aa);
    put_text(mat, std::to_string(microns) + "um", {font_x, font_y + font_margin}, font_scale, color, font_thickness, aa);
    put_text(mat, format_number(bottom_exposure) + "|" + format_number(normal_exposure) + "s",
             {font_x, font_y + font_margin * 2}, font_scale, color, font_thickness, aa);
    for (size_t i = 0; i < extra.size(); ++i)
      put_text(mat, extra[i], {font_x, font_y + font_margin * int(i + 3)}, font_scale, color, font_thickness, aa);
  };
  const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, {3, 3});
  for (const auto &cell : plan.cells) {
    if (check) check();
    const cv::Rect rect = shape_rect + cv::Point(cell.x_px, cell.y_px);
    cv::Mat bottom = layers[0](rect), normal = layers[1](rect), text_target = requested.extrude_text ? layers[2](rect) : normal;
    shape.copyTo(normal);
    std::vector<std::string> extra;
    if (cell.kind == ElephantFootCell::Kind::Original) {
      shape.copyTo(bottom);
      extra = {"ORI"};
    } else if (cell.kind == ElephantFootCell::Kind::Erode) {
      cv::Mat eroded;
      cv::erode(shape, eroded, kernel, cv::Point(-1, -1), int(cell.value), cv::BORDER_REFLECT_101);
      eroded.copyTo(bottom);
      extra = {"E: " + std::to_string(cell.value) + "i"};
    } else {
      // Wall dimming: the outer wall keeps only `brightness`, the core stays full.
      cv::Mat eroded, wall;
      cv::erode(shape, eroded, kernel, cv::Point(-1, -1), int(requested.dimming_wall_px), cv::BORDER_REFLECT_101);
      cv::subtract(shape, cv::Scalar(255 - int(cell.value)), wall);
      cv::add(eroded, wall, bottom);
      extra = {"W: " + std::to_string(requested.dimming_wall_px), "B: " + std::to_string(cell.value)};
    }
    label(text_target, cell.number, extra);
  }
  for (auto &mat : layers) apply_mirror(setup, mat);
  // UVtools centers the bottom-layer footprint, moving all three images alike.
  const cv::Rect bounds = non_zero_bounds(layers[0]);
  if (bounds.area() > 0) {
    const int dx = (setup.width - bounds.width) / 2 - bounds.x, dy = (setup.height - bounds.height) / 2 - bounds.y;
    for (auto &mat : layers) mat = translated(mat, dx, dy);
  }
  EncodedRaster encoded[3];
  MaskBudget budget;
  for (int i = 0; i < 3; ++i) {
    if (i == 2 && !requested.extrude_text) break;
    if (check) check();
    encoded[i] = encode_mask(setup, layers[i]);
  }
  std::vector<EncodedRaster> masks;
  masks.reserve(plan.total_layers);
  for (unsigned i = 0; i < plan.total_layers; ++i) {
    const auto &mask = i < plan.bottom_layers ? encoded[0] : i < plan.bottom_layers + plan.normal_layers ? encoded[1] : encoded[2];
    budget.add(mask);
    masks.push_back(mask);
  }
  const size_t layer_count = masks.size();
  return finish_job(setup, std::move(masks), increasing_positions(layer_count, setup.layer_height),
                    ExposurePositionPolicy::IncreasingLayers, plan.bottom_layers);
}

namespace {
// --- blooming effect -------------------------------------------------------
// Waits are kept in integer hundredths of a second like UVtools' 2-decimal decimals.
long to_centiseconds(double seconds) { return std::lround(seconds * 100.); }

void validate_blooming(const PixelSetup &setup, const BloomingParameters &p) {
  if (!std::isfinite(p.wait_start_s) || p.wait_start_s < 0. || p.wait_start_s > 600.)
    throw RuntimeError("Wait time before cure must be between 0 and 600 s");
  if (!std::isfinite(p.wait_increment_s) || p.wait_increment_s < .05 || p.wait_increment_s > 600.)
    throw RuntimeError("Wait time increment must be at least 0.05 s");
  if (p.object_count < 1 || p.object_count > 255) throw RuntimeError("Object count must be 1-255");
  if (p.object_diameter_px < 20) throw RuntimeError("Object diameter must be at least 20 px");
  if (!std::isfinite(p.object_height_mm) || p.object_height_mm < 0.) throw RuntimeError("Invalid object height");
  if (p.bottom_layers < 1) throw RuntimeError("Bottom layers must be at least 1");
  const long w = setup.width, h = setup.height;
  if (w - long(p.left_right_margin_px) * 2 <= 0) throw RuntimeError("The left/right margin is too big, it overlaps the screen resolution.");
  if (h - long(p.top_bottom_margin_px) * 2 <= 0) throw RuntimeError("The top/bottom margin is too big, it overlaps the screen resolution.");
  if (long(p.left_right_margin_px) + p.object_diameter_px > w - long(p.left_right_margin_px) ||
      long(p.top_bottom_margin_px) + p.object_diameter_px > h - long(p.top_bottom_margin_px))
    throw RuntimeError("The margin or object diameter is too big, it overlaps the screen resolution.");
}

BloomingPlan plan_blooming(const PixelSetup &setup, const BloomingParameters &p) {
  validate_blooming(setup, p);
  BloomingPlan plan;
  const long start = to_centiseconds(p.wait_start_s), step = to_centiseconds(p.wait_increment_s);
  const long last = start + step * long(p.object_count);
  const int max_x = setup.width - int(p.left_right_margin_px), max_y = setup.height - int(p.top_bottom_margin_px);
  int x = int(p.left_right_margin_px), y = int(p.top_bottom_margin_px);
  for (long wait = start; wait <= last; wait += step) {
    if (x + int(p.object_diameter_px) > max_x) {
      x = int(p.left_right_margin_px);
      y += int(p.object_diameter_px + p.object_margin_px);
      if (y + int(p.object_diameter_px) > max_y) break;
    }
    plan.objects.push_back({x, y, double(wait) / 100.});
    x += int(p.object_diameter_px + p.object_margin_px);
  }
  plan.requested_objects = size_t(p.object_count) + 1;
  plan.object_layers = size_t(std::ceil(p.object_height_mm / setup.layer_height - 1e-6)) + 1;  // + the first layer
  plan.text_layers = size_t(std::ceil(1. / setup.layer_height - 1e-6));  // 1 mm of raised text
  plan.total_layers = plan.objects.size() * plan.object_layers + plan.text_layers;
  if (plan.total_layers > max_calibration_layers) throw RuntimeError("Calibration exceeds 100000 layers");
  if ((plan.object_layers + plan.text_layers) * setup.layer_height > setup.max_height + 1e-9)
    throw RuntimeError("The calibration is taller than the printer");
  return plan;
}

std::string blooming_label(const PixelSetup &setup, double wait_s) {
  return "E: " + format_number(setup.config.opt_float("initial_exposure_time")) + "s/" +
         format_number(setup.config.opt_float("exposure_time")) + "s\nW: " + format_number(wait_s) + "s";
}
} // namespace

BloomingPlan blooming_plan(const DynamicPrintConfig &configuration, const BloomingParameters &parameters) {
  return plan_blooming(make_pixel_setup(configuration, {}), parameters);
}

std::unique_ptr<SLAPrint> create_blooming_calibration(
    const DynamicPrintConfig &configuration, const BloomingParameters &p, const CalibrationCheck &check) {
  auto setup = make_pixel_setup(configuration, check);
  const auto plan = plan_blooming(setup, p);
  const std::string format = setup.config.opt_string("sla_archive_format");
  if (format != "uvj" && format != "cxdlpv4")
    throw RuntimeError("Blooming calibration needs a format with repeated-height exposures (UVJ or CXDLPv4)");
  const int diameter = int(p.object_diameter_px);
  // One mask per square: each is cured with its own wait time before cure.
  std::vector<EncodedRaster> object_masks;
  MaskBudget budget;
  for (const auto &object : plan.objects) {
    if (check) check();
    cv::Mat mat = blank(setup);
    // Emgu draws Rectangle(x, y, w, h) up to the inclusive corner (x + w, y + h).
    cv::rectangle(mat, {object.x_px, object.y_px}, {object.x_px + diameter, object.y_px + diameter}, cv::Scalar(255),
                  cv::FILLED);
    apply_mirror(setup, mat);
    object_masks.push_back(encode_mask(setup, mat));
  }
  cv::Mat text = blank(setup);
  for (const auto &object : plan.objects)
    put_text(text, blooming_label(setup, object.wait_s), {object.x_px + 20, object.y_px + diameter / 2}, 2., 255, 2,
             false, 10);
  apply_mirror(setup, text);
  const auto text_mask = encode_mask(setup, text);

  std::vector<EncodedRaster> masks;
  std::vector<double> positions;
  std::vector<double> waits;  // per mask; text uses the normal default (NaN)
  masks.reserve(plan.total_layers);
  for (size_t layer = 0; layer < plan.object_layers; ++layer)
    for (size_t i = 0; i < object_masks.size(); ++i) {
      budget.add(object_masks[i]);
      masks.push_back(object_masks[i]);
      positions.push_back(double(layer + 1) * setup.layer_height);
      waits.push_back(plan.objects[i].wait_s);
    }
  for (size_t i = 0; i < plan.text_layers; ++i) {
    budget.add(text_mask);
    masks.push_back(text_mask);
    positions.push_back(double(plan.object_layers + i + 1) * setup.layer_height);
    waits.push_back(std::nan(""));
  }
  const double bottom_limit = double(p.bottom_layers) * setup.layer_height + 1e-9;
  auto job = finish_job(setup, std::move(masks), positions, ExposurePositionPolicy::RepeatedExposureHeights, p.bottom_layers);

  // Fresh rows pick bottom/normal values per record; here bottom is a physical
  // height, so rebuild each row from the right defaults and add the wait time.
  auto document = *job->layer_document();
  const auto &fresh = document.fresh_job();
  if (!fresh) throw RuntimeError("Blooming calibration requires an owned layer job");
  using F = LayerParameter;
  const auto capabilities = document.parameter_capabilities();
  const auto editable = [&](F field) {
    return std::any_of(capabilities.begin(), capabilities.end(), [&](const auto &c) { return c.field == field && c.editable; });
  };
  const F wait_field = editable(F::WaitBefore) ? F::WaitBefore : F::LightOff;
  if (!editable(wait_field)) throw RuntimeError("The selected format has no per-layer wait time before cure");
  std::vector<LayerParameters> table;
  table.reserve(document.layer_count());
  for (size_t i = 0; i < document.layer_count(); ++i) {
    if (check) check();
    auto row = document.layer_parameters(i);
    const auto &defaults = positions[i] <= bottom_limit ? fresh->globals.bottom : fresh->globals.normal;
    for (const auto &[field, value] : defaults)
      if (field != F::PositionZ && editable(field)) row[field] = value;
    if (!std::isnan(waits[i])) row[wait_field] = waits[i];
    table.push_back(std::move(row));
  }
  document.replace_layer_parameter_table(table);
  job->replace_layer_document(document);
  return job;
}

namespace {
// --- stress tower ----------------------------------------------------------
cv::Point rotate_about(cv::Point point, double degrees, cv::Point pivot) {
  if (std::fmod(degrees, 360.) == 0.) return point;
  const double angle = degrees * 3.14159265358979323846 / 180., c = std::cos(angle), s = std::sin(angle);
  const int dx = point.x - pivot.x, dy = point.y - pivot.y;
  return {int(std::lround(c * dx - s * dy + pivot.x)), int(std::lround(s * dx + c * dy + pivot.y))};
}

StressTowerPlan plan_stress_tower(const PixelSetup &setup, const StressTowerParameters &p) {
  for (double value : {p.base_diameter_mm, p.base_height_mm, p.body_height_mm, p.ceil_height_mm, p.spiral_diameter_mm,
                       p.spiral_angle_step_deg})
    if (!std::isfinite(value) || value < 0. || value > 100000.) throw RuntimeError("Invalid stress tower dimension");
  if (p.base_diameter_mm <= 0.) throw RuntimeError("Base diameter must be positive");
  if (p.spirals < 1 || p.spirals > 255) throw RuntimeError("Spirals must be 1-255");
  if (p.chamfer_layers > 255) throw RuntimeError("Chamfer layers must be 0-255");
  StressTowerPlan plan;
  const double ppmm = setup.pixels_per_mm();
  plan.base_radius_px = int(unsigned(p.base_diameter_mm * ppmm) / 2);
  plan.spiral_radius_px = int(unsigned(p.spiral_diameter_mm * ppmm) / 2);
  plan.base_layers = layers_for(p.base_height_mm, setup.layer_height);
  plan.body_layers = layers_for(p.body_height_mm, setup.layer_height);
  plan.ceil_layers = layers_for(p.ceil_height_mm, setup.layer_height);
  plan.total_layers = layers_for(p.base_height_mm + p.body_height_mm + p.ceil_height_mm, setup.layer_height);
  if (plan.total_layers < 1) throw RuntimeError("The stress tower has no layers");
  if (double(plan.total_layers) * setup.layer_height > setup.max_height + 1e-9)
    throw RuntimeError("The stress tower is taller than the printer");
  if (plan.base_radius_px < 1 || 2 * plan.base_radius_px > std::min(setup.width, setup.height))
    throw RuntimeError("The base diameter must fit the screen");
  return plan;
}
} // namespace

StressTowerPlan stress_tower_plan(const DynamicPrintConfig &configuration, const StressTowerParameters &parameters) {
  return plan_stress_tower(make_pixel_setup(configuration, {}), parameters);
}

std::unique_ptr<SLAPrint> create_stress_tower_calibration(
    const DynamicPrintConfig &configuration, const StressTowerParameters &p, const CalibrationCheck &check) {
  auto setup = make_pixel_setup(configuration, check);
  const auto plan = plan_stress_tower(setup, p);
  const int bottom_layers = setup.config.opt_int("sla_bottom_layers");
  const cv::Point center(setup.width / 2, setup.height / 2);
  const double offset_angle = 360. / double(p.spirals);
  const unsigned base_plus_body = plan.base_layers + plan.body_layers;
  std::vector<EncodedRaster> masks;
  masks.reserve(plan.total_layers);
  MaskBudget budget;
  // Layers that cannot differ (chamfer finished, ceiling) reuse their mask;
  // body layers always differ because the spirals rotate.
  EncodedRaster cached;
  long cached_key = -1;
  for (unsigned layer = 0; layer < plan.total_layers; ++layer) {
    if (check) check();
    const bool in_body = layer >= plan.base_layers && layer < base_plus_body;
    const long key = layer < plan.base_layers ? long(std::max(0, int(p.chamfer_layers) - int(layer))) : -1000000;
    if (in_body || key != cached_key) {
      cv::Mat mat = blank(setup);
      if (layer < plan.base_layers) {
        // The chamfer shrinks the first layers so the bottom does not flare.
        const int radius = plan.base_radius_px - int(key);
        if (radius > 0) draw_disc(mat, center, {radius, radius}, p.antialiasing);
      } else if (in_body) {
        const double angle = std::fmod(double(layer) * p.spiral_angle_step_deg, 360.);
        const cv::Size spiral_disc(plan.spiral_radius_px, plan.spiral_radius_px);
        for (unsigned spiral = 0; spiral < p.spirals; ++spiral) {
          double spiral_angle = std::fmod(offset_angle * spiral + angle, 360.);
          if (p.spiral_direction == StressTowerSpiral::Alternate && spiral % 2 == 0) spiral_angle = -spiral_angle;
          const cv::Point location(center.x - plan.base_radius_px + plan.spiral_radius_px, center.y);
          draw_disc(mat, rotate_about(location, spiral_angle, center), spiral_disc, p.antialiasing);
          if (p.spiral_direction == StressTowerSpiral::Both)
            draw_disc(mat, rotate_about(location, -spiral_angle, center), spiral_disc, p.antialiasing);
        }
      } else
        draw_disc(mat, center, {plan.base_radius_px, plan.base_radius_px}, p.antialiasing);
      apply_mirror(setup, mat);
      cached = encode_mask(setup, mat);
      cached_key = in_body ? -1 : key;
    }
    budget.add(cached);
    masks.push_back(cached);
  }
  const size_t layer_count = masks.size();
  return finish_job(setup, std::move(masks), increasing_positions(layer_count, setup.layer_height),
                    ExposurePositionPolicy::IncreasingLayers, unsigned(std::max(0, bottom_layers)));
}


// ===========================================================================
// XYZ accuracy, tolerance, lift height and the external test list.
// ===========================================================================
namespace {
// Whole pixels for a length, truncated like UVtools ((uint)(mm * ppmm)); the
// epsilon absorbs binary representation error (15.0 mm * 10 px/mm = 149.99999).
int pixels_for(double mm, double pitch_mm) {
  if (!std::isfinite(mm) || mm < 0.) throw RuntimeError("Invalid calibration size");
  const double pixels = std::floor(mm / pitch_mm + 1e-6);
  if (pixels > 32768.) throw RuntimeError("Calibration size exceeds the raster");
  return int(pixels);
}
unsigned bottom_layers_or_preset(const PixelSetup &setup, unsigned requested) {
  return requested ? requested : unsigned(std::max(0, setup.config.opt_int("sla_bottom_layers")));
}
void require_fits_height(const PixelSetup &setup, unsigned layers) {
  if (layers < 1) throw RuntimeError("The calibration needs at least one layer");
  if (layers > max_calibration_layers) throw RuntimeError("Calibration exceeds 100000 layers");
  if (double(layers) * setup.layer_height > setup.max_height + 1e-9)
    throw RuntimeError("The calibration is taller than the printer");
}

// --- XYZ accuracy -----------------------------------------------------------
struct XYZBlock { cv::Rect rect; std::string name; };
struct XYZLayout { XYZAccuracyPlan plan; std::vector<XYZBlock> blocks; };

XYZLayout layout_xyz(const PixelSetup &setup, const XYZAccuracyParameters &p) {
  for (double value : {p.x_mm, p.y_mm, p.z_mm, p.wall_thickness_mm, p.drain_hole_mm})
    if (!std::isfinite(value) || value < 0. || value > 100000.) throw RuntimeError("Invalid XYZ accuracy size");
  XYZLayout layout;
  auto &plan = layout.plan;
  plan.block_x_px = pixels_for(p.x_mm, setup.pitch_w_mm);
  plan.block_y_px = pixels_for(p.y_mm, setup.pitch_h_mm);
  if (plan.block_x_px < 1 || plan.block_y_px < 1) throw RuntimeError("XYZ block is smaller than one pixel");
  plan.total_layers = layers_for(p.z_mm, setup.layer_height);
  require_fits_height(setup, plan.total_layers);
  plan.bottom_layers = bottom_layers_or_preset(setup, p.bottom_layers);
  plan.real_x_mm = plan.block_x_px * setup.pitch_w_mm;
  plan.real_y_mm = plan.block_y_px * setup.pitch_h_mm;
  plan.real_z_mm = plan.total_layers * setup.layer_height;
  if (p.hollow && p.wall_thickness_mm > 0.) {
    plan.wall_x_px = pixels_for(p.wall_thickness_mm, setup.pitch_w_mm);
    plan.wall_y_px = pixels_for(p.wall_thickness_mm, setup.pitch_h_mm);
    if (plan.wall_x_px * 2 >= plan.block_x_px || plan.wall_y_px * 2 >= plan.block_y_px)
      throw RuntimeError("The wall thickness leaves no cavity; reduce it or enlarge the block");
  }
  if ((p.hollow || p.center_hole_relief) && p.drain_hole_mm > 0. && plan.total_layers > plan.bottom_layers)
    plan.vent_layers = std::min(plan.total_layers - plan.bottom_layers,
                                unsigned(std::floor(p.drain_hole_mm / setup.layer_height + 1e-6)) + 1);
  const int W = setup.width, H = setup.height;
  const int lr = int(p.left_right_margin_px), tb = int(p.top_bottom_margin_px);
  const int xs[3] = {lr, W / 2 - plan.block_x_px / 2, W - plan.block_x_px - lr};
  const int ys[3] = {tb, H / 2 - plan.block_y_px / 2, H - plan.block_y_px - tb};
  static const char *rows = "TMB", *columns = "LCR";
  for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 3; ++column) {
      if (!p.objects[size_t(row * 3 + column)]) continue;
      layout.blocks.push_back({cv::Rect(xs[column], ys[row], plan.block_x_px, plan.block_y_px),
                               std::string{rows[row], columns[column]}});
    }
  plan.object_count = unsigned(layout.blocks.size());
  if (layout.blocks.empty()) throw RuntimeError("No objects to output, select at least one position");
  const cv::Rect screen(0, 0, W, H);
  for (size_t i = 0; i < layout.blocks.size(); ++i) {
    if ((layout.blocks[i].rect & screen) != layout.blocks[i].rect)
      throw RuntimeError("Block " + layout.blocks[i].name + " does not fit the screen; reduce the size or margins");
    for (size_t j = i + 1; j < layout.blocks.size(); ++j)
      if ((layout.blocks[i].rect & layout.blocks[j].rect).area() > 0)
        throw RuntimeError("Blocks " + layout.blocks[i].name + " and " + layout.blocks[j].name +
                           " overlap; reduce the size or select fewer positions");
  }
  return layout;
}
} // namespace

XYZAccuracyPlan xyz_accuracy_plan(const DynamicPrintConfig &configuration, const XYZAccuracyParameters &parameters) {
  return layout_xyz(make_pixel_setup(configuration, {}), parameters).plan;
}

std::unique_ptr<SLAPrint> create_xyz_accuracy_calibration(
    const DynamicPrintConfig &configuration, const XYZAccuracyParameters &p, const CalibrationCheck &check) {
  auto setup = make_pixel_setup(configuration, check);
  const auto layout = layout_xyz(setup, p);
  const auto &plan = layout.plan;
  const bool aa = p.antialiasing;
  // 0 = bottom (solid), 1 = normal (hollow), 2 = vent (hollow + drain slot).
  cv::Mat images[3] = {blank(setup), blank(setup), blank(setup)};
  const std::string size_label = format_number(p.x_mm) + "," + format_number(p.y_mm) + "," + format_number(p.z_mm);
  for (int i = 0; i < 3; ++i)
    for (const auto &block : layout.blocks) {
      const cv::Rect &r = block.rect;
      cv::rectangle(images[i], r, cv::Scalar(255), cv::FILLED);
      put_text(images[i], block.name, {r.x + 30, r.y + 50}, 1.3, 0, 3, aa);
      put_text(images[i], size_label, {r.x + 30, r.y + r.height - 50 + 25}, 1.3, 0, 3, aa);
      if (p.center_hole_relief)
        erase_disc(images[i], {r.x + r.width / 2, r.y + r.height / 2},
                   normalized_pitch(setup, std::min(r.width, r.height) / 4), aa);
      if (p.hollow && i > 0 && plan.wall_x_px > 0)
        cv::rectangle(images[i], cv::Rect(r.x + plan.wall_x_px, r.y + plan.wall_y_px, r.width - plan.wall_x_px * 2,
                                          r.height - plan.wall_y_px * 2), cv::Scalar(0), cv::FILLED);
      // UVtools centres the slot with the X size (a bug for non-square blocks); Y is the slot's own axis.
      if (i == 2 && p.drain_hole_mm > 0.) {
        const int slot = std::max(1, int(p.drain_hole_mm / setup.pitch_h_mm));
        cv::rectangle(images[i], cv::Rect(r.x, r.y + r.height / 2 - slot / 2, r.width, slot), cv::Scalar(0), cv::FILLED);
      }
    }
  EncodedRaster encoded[3];
  for (int i = 0; i < 3; ++i) {
    if (check) check();
    apply_mirror(setup, images[i]);
    encoded[i] = encode_mask(setup, images[i]);
  }
  std::vector<EncodedRaster> masks;
  masks.reserve(plan.total_layers);
  MaskBudget budget;
  for (unsigned layer = 0; layer < plan.total_layers; ++layer) {
    const bool vent = layer >= plan.bottom_layers && layer < plan.bottom_layers + plan.vent_layers;
    const auto &mask = layer < plan.bottom_layers ? encoded[0] : vent ? encoded[2] : encoded[1];
    budget.add(mask);
    masks.push_back(mask);
  }
  const size_t layer_count = masks.size();  // read before the vector is moved
  return finish_job(setup, std::move(masks), increasing_positions(layer_count, setup.layer_height),
                    ExposurePositionPolicy::IncreasingLayers, plan.bottom_layers);
}

// --- Tolerance --------------------------------------------------------------
namespace {
struct ToleranceLayout {
  TolerancePlan plan;
  std::vector<std::pair<cv::Point, std::string>> labels;
};

// Walks the UVtools layout; draws into canvas when given, so plan and image
// can never disagree. UVtools' axis quirks are kept where they only shift the
// layout (male parts start at the ring's vertical centre) and fixed where they
// would distort the part on non-square pixels (X size used for Y).
ToleranceLayout layout_tolerance(const PixelSetup &setup, const ToleranceParameters &p, cv::Mat *canvas) {
  for (double value : {p.z_mm, p.female_diameter_mm, p.female_hole_diameter_mm, p.thinner_offset_mm, p.thinner_step_mm,
                       p.thicker_offset_mm, p.thicker_step_mm})
    if (!std::isfinite(value) || std::abs(value) > 100000.) throw RuntimeError("Invalid tolerance size");
  if (p.female_hole_diameter_mm >= p.female_diameter_mm)
    throw RuntimeError("Hole diameter must be smaller than female diameter.");
  if (p.thinner_models > 1000 || p.thicker_models > 1000) throw RuntimeError("Too many tolerance models");
  ToleranceLayout layout;
  auto &plan = layout.plan;
  plan.total_layers = layers_for(p.z_mm, setup.layer_height);
  require_fits_height(setup, plan.total_layers);
  plan.bottom_layers = bottom_layers_or_preset(setup, p.bottom_layers);
  const int fx = pixels_for(p.female_diameter_mm, setup.pitch_w_mm), fy = pixels_for(p.female_diameter_mm, setup.pitch_h_mm);
  const int hx = pixels_for(p.female_hole_diameter_mm, setup.pitch_w_mm), hy = pixels_for(p.female_hole_diameter_mm, setup.pitch_h_mm);
  if (hx < 1 || hy < 1 || hx >= fx || hy >= fy) throw RuntimeError("Female ring or hole is too small for the pixel size");
  plan.female_x_px = fx; plan.female_y_px = fy; plan.hole_x_px = hx; plan.hole_y_px = hy;
  const bool same_part = p.output_same_diameter_part && !p.fuse_parts;
  plan.requested_objects = (same_part ? 1 : 0) + p.thinner_models + p.thicker_models + (p.fuse_parts ? 0 : 1);
  if (plan.requested_objects == 0) throw RuntimeError("No objects to output.");
  const bool circle = p.shape == ToleranceShape::Circle, aa = p.antialiasing;
  const int line_type = aa ? cv::LINE_AA : cv::LINE_8;
  const auto fill_rect = [&](cv::Rect r, int color) {
    if (canvas) cv::rectangle(*canvas, r, cv::Scalar(color), cv::FILLED, line_type);
  };
  const auto fill_disc = [&](cv::Point c, cv::Size radius, int color) {
    if (!canvas) return;
    if (color) draw_disc(*canvas, c, radius, aa); else erase_disc(*canvas, c, radius, aa);
  };
  // Female ring with its hole whose top-left corner is (x, y).
  const auto ring = [&](int x, int y) {
    if (circle) {
      const cv::Point c(x + fx / 2, y + fy / 2);
      fill_disc(c, {fx / 2, fy / 2}, 255);
      fill_disc(c, {hx / 2, hy / 2}, 0);
    } else {
      fill_rect({x, y, fx, fy}, 255);
      fill_rect({x + (fx - hx) / 2, y + (fy - hy) / 2, hx, hy}, 0);
    }
  };
  const int start_x = std::max(2, int(p.left_right_margin_px)), start_y = std::max(2, int(p.top_bottom_margin_px));
  const int margin = int(p.part_margin_px);
  int x = start_x, y = start_y;
  if (!p.fuse_parts) {
    if (start_x + fx >= setup.width || start_y + fy >= setup.height)
      throw RuntimeError("The female ring does not fit the screen; reduce the diameter or margins");
    ring(x, y);
    x += fx + margin;
    y = start_y + fy / 2;  // UVtools: male parts start at the ring's vertical centre
  }
  const auto add_part = [&](double step) {
    const double size_mm = std::round((p.female_hole_diameter_mm + step) * 100.) / 100.;
    if (size_mm <= 0.) return false;
    const int w = pixels_for(size_mm, setup.pitch_w_mm), h = pixels_for(size_mm, setup.pitch_h_mm);
    if (w < 1 || h < 1) return false;
    TolerancePart part;
    part.step_mm = step; part.size_mm = size_mm; part.width_px = w; part.height_px = h;
    cv::Point text_point;
    if (p.fuse_parts) {
      if (w >= hx || h >= hy) return false;  // must fit inside the hole
      if (x + fx + int(p.left_right_margin_px) >= setup.width) { x = start_x; y += fy + margin; }
      if (y + fy + int(p.top_bottom_margin_px) >= setup.height) return false;
      ring(x, y);
      if (circle) fill_disc({x + fx / 2, y + fy / 2}, {w / 2, h / 2}, 255);
      else fill_rect({x + (fx - w) / 2, y + (fy - h) / 2, w, h}, 255);
      part.x_px = x + (fx - w) / 2; part.y_px = y + (fy - h) / 2;
      text_point = {x + fx / 2 - 60, y + fy / 2 + 10};
      x += fx + margin;
    } else {
      if (x + w + int(p.left_right_margin_px) >= setup.width) { x = start_x; y += h + margin; }
      if (y + h + int(p.top_bottom_margin_px) >= setup.height) return false;
      if (circle) fill_disc({x + w / 2, y + h / 2}, {w / 2, h / 2}, 255);
      else fill_rect({x, y, w, h}, 255);
      part.x_px = x; part.y_px = y;
      text_point = {x + w / 2 - 60, y + h / 2 + 10};
      x += w + margin;
    }
    char text[32];
    std::snprintf(text, sizeof(text), step > 0 ? "+%.2f" : "%.2f", step);
    layout.labels.emplace_back(text_point, text);
    plan.parts.push_back(part);
    return true;
  };
  if (same_part) add_part(0.);
  for (unsigned i = 1; i <= p.thinner_models; ++i)
    if (!add_part(p.thinner_offset_mm + p.thinner_step_mm * i)) break;
  for (unsigned i = 1; i <= p.thicker_models; ++i)
    if (!add_part(p.thicker_offset_mm + p.thicker_step_mm * i)) break;
  if (plan.parts.empty() && p.fuse_parts) throw RuntimeError("No fused part fits the screen or the hole");
  return layout;
}
} // namespace

TolerancePlan tolerance_plan(const DynamicPrintConfig &configuration, const ToleranceParameters &parameters) {
  return layout_tolerance(make_pixel_setup(configuration, {}), parameters, nullptr).plan;
}

std::unique_ptr<SLAPrint> create_tolerance_calibration(
    const DynamicPrintConfig &configuration, const ToleranceParameters &p, const CalibrationCheck &check) {
  auto setup = make_pixel_setup(configuration, check);
  cv::Mat base = blank(setup);
  const auto layout = layout_tolerance(setup, p, &base);
  const auto &plan = layout.plan;
  const unsigned count = plan.total_layers;
  // Erosion per layer: softer bottom against elephant foot, then a chamfer at
  // both ends. Erode(a) then erode(b) equals erode(a + b), so layers sharing
  // the iteration total (and the label flag) share one mask.
  std::vector<unsigned> iterations(count, 0);
  for (unsigned layer = 0; layer < std::min(plan.bottom_layers, count); ++layer)
    iterations[layer] += p.erode_bottom_iterations;
  for (unsigned offset = 0; offset < std::min(p.chamfer_layers, count); ++offset) {
    iterations[offset] += p.chamfer_layers - offset;
    iterations[count - 1 - offset] += p.chamfer_layers - offset;
  }
  const unsigned label_layers = std::min(15u, count);  // labels are engraved in the last layers only
  cv::Mat mirrored_base = base.clone();
  apply_mirror(setup, mirrored_base);
  // UVtools finishes with a Move that centres the footprint of the whole test.
  const cv::Rect bounds = non_zero_bounds(mirrored_base);
  const int dx = bounds.area() > 0 ? (setup.width - bounds.width) / 2 - bounds.x : 0;
  const int dy = bounds.area() > 0 ? (setup.height - bounds.height) / 2 - bounds.y : 0;
  const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, {3, 3});
  std::map<unsigned long, EncodedRaster> encoded;
  std::vector<EncodedRaster> masks;
  masks.reserve(count);
  MaskBudget budget;
  for (unsigned layer = 0; layer < count; ++layer) {
    if (check) check();
    const bool labelled = layer + label_layers >= count;
    const unsigned long key = (unsigned long)iterations[layer] * 2 + (labelled ? 1 : 0);
    auto found = encoded.find(key);
    if (found == encoded.end()) {
      cv::Mat image = base.clone();
      if (iterations[layer])
        cv::erode(base, image, kernel, cv::Point(-1, -1), int(iterations[layer]), cv::BORDER_REFLECT_101);
      if (labelled)
        for (const auto &label : layout.labels) put_text(image, label.second, label.first, 1., 0, 2, p.antialiasing);
      apply_mirror(setup, image);
      found = encoded.emplace(key, encode_mask(setup, translated(image, dx, dy))).first;
    }
    budget.add(found->second);
    masks.push_back(found->second);
  }
  return finish_job(setup, std::move(masks), increasing_positions(count, setup.layer_height),
                    ExposurePositionPolicy::IncreasingLayers, plan.bottom_layers);
}

// --- Lift height ------------------------------------------------------------
namespace {
LiftHeightPlan plan_lift_height(const PixelSetup &setup, const LiftHeightParameters &p) {
  LiftHeightPlan plan;
  plan.block_width_px = setup.width - int(p.left_right_margin_px) * 2;
  plan.block_height_px = setup.height - int(p.top_bottom_margin_px) * 2;
  if (plan.block_width_px <= 0 || plan.block_height_px <= 0)
    throw RuntimeError("The margins overlap the screen resolution.");
  plan.block_x_px = (setup.width - plan.block_width_px) / 2;
  plan.block_y_px = (setup.height - plan.block_height_px) / 2;
  if (p.decrease_image) {
    if (p.decrease_factor_pct < 1 || p.decrease_factor_pct > 99)
      throw RuntimeError("The image decrease factor must be between 1 and 99%");
    if (p.minimum_factor_pct < 1 || p.minimum_factor_pct > 99)
      throw RuntimeError("The minimum image decrease factor must be between 1 and 99%");
    for (int factor = 100 - int(p.decrease_factor_pct); factor >= int(p.minimum_factor_pct);
         factor -= int(p.decrease_factor_pct))
      plan.factors_pct.push_back(unsigned(factor));
  }
  plan.bottom_layers = p.bottom_layers;
  plan.normal_layers = p.normal_layers;
  plan.decrease_layers = unsigned(plan.factors_pct.size());
  plan.total_layers = plan.bottom_layers + plan.normal_layers + plan.decrease_layers;
  require_fits_height(setup, plan.total_layers);
  plan.total_height_mm = plan.total_layers * setup.layer_height;
  return plan;
}
void require_motion_value(const std::optional<double> &value, const char *name, bool allow_zero) {
  if (value && (!std::isfinite(*value) || *value < 0. || *value > 100000. || (!allow_zero && *value == 0.)))
    throw RuntimeError(std::string("Invalid ") + name);
}
} // namespace

LiftHeightPlan lift_height_plan(const DynamicPrintConfig &configuration, const LiftHeightParameters &parameters) {
  return plan_lift_height(make_pixel_setup(configuration, {}), parameters);
}

std::unique_ptr<SLAPrint> create_lift_height_calibration(
    const DynamicPrintConfig &configuration, const LiftHeightParameters &p, const CalibrationCheck &check) {
  auto setup = make_pixel_setup(configuration, check);
  const auto plan = plan_lift_height(setup, p);
  require_motion_value(p.bottom_exposure_s, "bottom exposure", false);
  require_motion_value(p.normal_exposure_s, "normal exposure", false);
  require_motion_value(p.bottom_lift_height_mm, "bottom lift height", true);
  require_motion_value(p.lift_height_mm, "lift height", true);
  require_motion_value(p.bottom_lift_speed_mm_min, "bottom lift speed", false);
  require_motion_value(p.lift_speed_mm_min, "lift speed", false);
  require_motion_value(p.retract_speed_mm_min, "retract speed", false);
  auto &config = setup.config;
  // These overrides live only in this generated job; resin/printer presets stay untouched.
  const auto set_if = [&](const char *key, const std::optional<double> &value) {
    if (value) config.set(key, *value);
  };
  set_if("initial_exposure_time", p.bottom_exposure_s);
  set_if("exposure_time", p.normal_exposure_s);
  config.set("max_exposure_time", std::max(config.opt_float("max_exposure_time"), config.opt_float("exposure_time")));
  config.set("max_initial_exposure_time",
             std::max(config.opt_float("max_initial_exposure_time"), config.opt_float("initial_exposure_time")));
  set_if("sla_bottom_lift_height", p.bottom_lift_height_mm);
  set_if("sla_lift_height", p.lift_height_mm);
  set_if("sla_bottom_lift_speed", p.bottom_lift_speed_mm_min);
  set_if("sla_lift_speed", p.lift_speed_mm_min);
  set_if("sla_retract_speed", p.retract_speed_mm_min);
  set_if("sla_bottom_retract_speed", p.retract_speed_mm_min);

  const cv::Rect block(plan.block_x_px, plan.block_y_px, plan.block_width_px, plan.block_height_px);
  cv::Mat full = blank(setup);
  cv::rectangle(full, block, cv::Scalar(255), cv::FILLED);
  const EncodedRaster full_mask = encode_mask(setup, full);
  std::vector<EncodedRaster> masks;
  masks.reserve(plan.total_layers);
  MaskBudget budget;
  for (unsigned i = 0; i < plan.bottom_layers + plan.normal_layers; ++i) {
    budget.add(full_mask);
    masks.push_back(full_mask);
  }
  for (unsigned factor : plan.factors_pct) {
    if (check) check();
    // Integer percentages of the block, centred on the screen (UVtools).
    const int width = block.width * int(factor) / 100, height = block.height * int(factor) / 100;
    cv::Mat reduced = blank(setup);
    cv::rectangle(reduced, cv::Rect((setup.width - width) / 2, (setup.height - height) / 2, width, height),
                  cv::Scalar(255), cv::FILLED);
    masks.push_back(encode_mask(setup, reduced));
    budget.add(masks.back());
  }
  const size_t layer_count = masks.size();  // read before the vector is moved
  return finish_job(setup, std::move(masks), increasing_positions(layer_count, setup.layer_height),
                    ExposurePositionPolicy::IncreasingLayers, plan.bottom_layers);
}

const std::vector<ExternalCalibrationTest> &external_calibration_tests() {
  // UVtools only opens these pages. They are kept as plain text because the
  // application is offline; nothing here is fetched or opened.
  static const std::vector<ExternalCalibrationTest> tests{
      {"The Complete Resin 3D Printing Settings Guide for Beginners",
       "Background on exposure, lift and layer settings that the calibrations tune.",
       "https://ameralabs.com/blog/the-complete-resin-3d-printing-settings-guide-for-beginners",
       "Use Exposure finder, Grayscale patches and Lift height from this dialog."},
      {"Photonsters Validation Matrix / Exposure finder",
       "Printed matrix of exposure times and features to pick the best exposure.",
       "https://www.thingiverse.com/thing:4707289",
       "The native Exposure finder generates the same kind of matrix without a model."},
      {"Support pull test",
       "Model that shows whether supports and tips hold at the chosen exposure and lift.",
       "https://www.thingiverse.com/thing:4308175",
       "Requires a mesh: import the downloaded model into the 3D scene. Stress tower checks motion only."},
      {"AmeraLabs town calibration part",
       "All-in-one test part for bridges, overhangs, holes, text and thin walls.",
       "https://www.thingiverse.com/thing:2810666",
       "Requires a mesh: import the downloaded model into the 3D scene."},
      {"9 settings for faster printing",
       "Article on speed-related settings; read it before changing lift and wait times.",
       "https://ameralabs.com/blog/9-settings-to-change-for-faster-resin-3d-printing",
       "Verify new lift settings with the Lift height calibration."},
  };
  return tests;
}

} // namespace Slic3r::sla
