#include "PCBExposure.hpp"
#include "PrinterRaster.hpp"
#include "GeneratedLayerOwnership.hpp"
#include "ProjectedPNGMaskEncoder.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <locale>
#include <map>
#include <regex>
#include <sstream>

namespace Slic3r::sla {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr size_t input_limit = 64 * 1024 * 1024;
constexpr size_t operation_limit = 1000000;
[[noreturn]] void invalid(const std::string &message) {
  throw RuntimeError("PCB: " + message);
}
void bounded(double value) {
  if (!std::isfinite(value) || std::abs(value) > 100000.)
    invalid("invalid coordinate or dimension");
}
double number(const std::string &text) {
  std::istringstream input(text);
  input.imbue(std::locale::classic());
  double value = 0.;
  input >> value;
  if (!input || input.peek() != std::char_traits<char>::eof())
    invalid("invalid number: " + text);
  if (!std::isfinite(value) || std::abs(value) > 1e15)
    invalid("invalid numeric value");
  return value;
}
void validate_input(const std::string &text, double tolerance) {
  if (text.empty() || text.size() > input_limit)
    invalid("empty or oversized input (maximum 64 MiB)");
  if (!std::isfinite(tolerance) || tolerance <= 0. || tolerance > 1.)
    invalid("invalid curve tolerance");
  if (text.find('\0') != std::string::npos)
    invalid("binary input is not a Gerber/drill file");
}
Point point(const Vec2d &p) {
  bounded(p.x());
  bounded(p.y());
  return Point::new_scale(p.x(), p.y());
}
Polygon ellipse(const Vec2d &center, double width, double height,
                double tolerance) {
  if (width <= 0. || height <= 0.)
    invalid("aperture size must be positive");
  bounded(width);
  bounded(height);
  const double radius = std::max(width, height) * .5;
  const double step =
      2. * std::acos(std::clamp(1. - tolerance / radius, -1., 1.));
  const size_t count = size_t(
      std::clamp(std::ceil(2. * pi / std::max(step, .00001)), 16., 65536.));
  Points points;
  points.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const double angle = 2. * pi * i / count;
    points.push_back(point(center + Vec2d{std::cos(angle) * width * .5,
                                          std::sin(angle) * height * .5}));
  }
  return Polygon{std::move(points)};
}
unsigned integer(const std::string &text) {
  if (text.empty() || text.size() > 6 ||
      !std::all_of(text.begin(), text.end(),
                   [](unsigned char c) { return std::isdigit(c); }))
    invalid("invalid integer command field: " + text);
  return unsigned(std::stoul(text));
}
struct Aperture {
  char shape = 'C';
  double width = 0., height = 0., hole = 0., rotation = 0.;
  unsigned vertices = 0;
  ExPolygons macro_shape;
};
Polygon outline(const Aperture &a, const Vec2d &p, double tolerance) {
  if (a.shape == 'C')
    return ellipse(p, a.width, a.width, tolerance);
  if (a.shape == 'R')
    return Polygon{Points{point(p + Vec2d{-a.width * .5, -a.height * .5}),
                          point(p + Vec2d{a.width * .5, -a.height * .5}),
                          point(p + Vec2d{a.width * .5, a.height * .5}),
                          point(p + Vec2d{-a.width * .5, a.height * .5})}};
  if (a.shape == 'O') {
    const bool horizontal = a.width >= a.height;
    const double diameter = std::min(a.width, a.height),
                 shift = (std::max(a.width, a.height) - diameter) * .5;
    const Vec2d delta = horizontal ? Vec2d{shift, 0.} : Vec2d{0., shift};
    Points points = ellipse(p - delta, diameter, diameter, tolerance).points;
    auto second = ellipse(p + delta, diameter, diameter, tolerance).points;
    points.insert(points.end(), second.begin(), second.end());
    return Geometry::convex_hull(points);
  }
  Points points;
  for (unsigned i = 0; i < a.vertices; ++i) {
    const double angle = (a.rotation / 180. + 2. * i / a.vertices) * pi;
    points.push_back(point(p + Vec2d{std::cos(angle) * a.width * .5,
                                     std::sin(angle) * a.width * .5}));
  }
  return Polygon{std::move(points)};
}
// LM/LR/LS graphics state. Applied about the aperture origin in the order
// mirror -> rotate -> scale (RS-274X 4.9), then the shape is moved to the
// flash/draw point. Persistent until changed by the next LM/LR/LS command.
struct ApertureTransform {
  bool mirror_x = false, mirror_y = false;
  double rotation_deg = 0., scale = 1.;
  bool identity() const {
    return !mirror_x && !mirror_y && rotation_deg == 0. && scale == 1.;
  }
};
ExPolygons transform_shapes(ExPolygons shapes, const ApertureTransform &t) {
  if (t.identity())
    return shapes;
  for (auto &shape : shapes) {
    if (t.mirror_x || t.mirror_y)
      shape.scale(t.mirror_x ? -1. : 1., t.mirror_y ? -1. : 1.);
    if (t.rotation_deg != 0.)
      shape.rotate(t.rotation_deg * pi / 180.);
    if (t.scale != 1.)
      shape.scale(t.scale, t.scale);
  }
  // Mirroring flips winding; a union restores canonical contour/hole winding.
  return union_ex(shapes);
}
ExPolygons flash(const Aperture &a, const Vec2d &p, double tolerance) {
  if (a.shape == 'M') {
    auto geometry = a.macro_shape;
    for (auto &shape : geometry)
      shape.translate(point(p));
    return geometry;
  }
  ExPolygon shape{outline(a, p, tolerance)};
  if (a.hole > 0.) {
    auto hole = ellipse(p, a.hole, a.hole, tolerance);
    hole.reverse();
    shape.holes.push_back(std::move(hole));
  }
  return {std::move(shape)};
}
ExPolygons flash(const Aperture &a, const Vec2d &p, double tolerance,
                 const ApertureTransform &transform) {
  if (transform.identity())
    return flash(a, p, tolerance);
  auto shapes = transform_shapes(flash(a, Vec2d{0., 0.}, tolerance), transform);
  for (auto &shape : shapes)
    shape.translate(point(p));
  return shapes;
}
ExPolygons stroke(const Aperture &a, const Vec2d &from, const Vec2d &to,
                  double tolerance, const ApertureTransform &transform = {}) {
  if (a.shape == 'M')
    invalid("drawing with macro apertures is not supported; use flashes or "
            "regions");
  if (a.hole > 0.)
    invalid("drawing with holed apertures is not supported");
  if (!transform.identity()) {
    // Hull of the transformed aperture placed at both end points.
    const auto base = transform_shapes(flash(a, Vec2d{0., 0.}, tolerance), transform);
    Points points;
    for (const auto &shape : base)
      for (const Point &vertex : shape.contour.points) {
        points.push_back(vertex + point(from));
        points.push_back(vertex + point(to));
      }
    if (points.size() < 3)
      invalid("transformed aperture has no area");
    return {ExPolygon{Geometry::convex_hull(points)}};
  }
  auto points = outline(a, from, tolerance).points;
  auto end = outline(a, to, tolerance).points;
  points.insert(points.end(), end.begin(), end.end());
  return {ExPolygon{Geometry::convex_hull(points)}};
}
// Flush at polarity changes: dark tracks are batched, clear operations remain
// ordered. This avoids a polygon union for every segment on a large PCB.
class Painter {
  ExPolygons m_result, m_pending;
  bool m_dark = true;
  size_t m_vertices = 0;

public:
  void flush() {
    if (m_pending.empty())
      return;
    const auto merged = union_ex(m_pending);
    m_result = m_dark ? union_ex(m_result, merged) : diff_ex(m_result, merged);
    m_pending.clear();
  }
  void polarity(bool dark) {
    if (dark != m_dark) {
      flush();
      m_dark = dark;
    }
  }
  void draw(ExPolygons shapes) {
    for (const auto &shape : shapes) {
      m_vertices += shape.contour.points.size();
      for (const auto &hole : shape.holes)
        m_vertices += hole.points.size();
    }
    if (m_vertices > 12000000)
      invalid("geometry exceeds the 12 million vertex budget");
    m_pending.insert(m_pending.end(), std::make_move_iterator(shapes.begin()),
                     std::make_move_iterator(shapes.end()));
    if (m_pending.size() >= 4096)
      flush();
  }
  ExPolygons finish() {
    flush();
    return std::move(m_result);
  }
};

// Small bounded arithmetic parser for aperture macro expressions. No scripting
// engine, filesystem access or external runtime is involved.
class MacroExpression {
  const std::string &m_text;
  const std::map<unsigned, double> &m_variables;
  size_t m_position = 0;
  unsigned m_depth = 0;
  double primary() {
    if (++m_depth > 64 || m_position >= m_text.size())
      invalid("invalid macro expression");
    double value = 0.;
    const char current = m_text[m_position];
    if (current == '+' || current == '-') {
      ++m_position;
      value = primary();
      if (current == '-')
        value = -value;
    } else if (current == '(') {
      ++m_position;
      value = sum();
      if (m_position >= m_text.size() || m_text[m_position++] != ')')
        invalid("unclosed macro expression");
    } else if (current == '$') {
      const size_t start = ++m_position;
      while (m_position < m_text.size() &&
             std::isdigit(static_cast<unsigned char>(m_text[m_position])))
        ++m_position;
      const auto index = integer(m_text.substr(start, m_position - start));
      if (!index)
        invalid("macro variables start at $1");
      const auto found = m_variables.find(index);
      value = found == m_variables.end() ? 0. : found->second;
    } else {
      const size_t start = m_position;
      while (m_position < m_text.size() &&
             (std::isdigit(static_cast<unsigned char>(m_text[m_position])) ||
              m_text[m_position] == '.'))
        ++m_position;
      if (start == m_position)
        invalid("unsupported macro expression token");
      value = number(m_text.substr(start, m_position - start));
    }
    --m_depth;
    if (!std::isfinite(value) || std::abs(value) > 1e15)
      invalid("macro expression overflow");
    return value;
  }
  double product() {
    double value = primary();
    while (m_position < m_text.size() &&
           (m_text[m_position] == 'x' || m_text[m_position] == 'X' ||
            m_text[m_position] == '/')) {
      const char op = m_text[m_position++];
      const double right = primary();
      if (op == '/') {
        if (right == 0.)
          invalid("division by zero in macro");
        value /= right;
      } else
        value *= right;
      if (!std::isfinite(value) || std::abs(value) > 1e15)
        invalid("macro expression overflow");
    }
    return value;
  }
  double sum() {
    double value = product();
    while (m_position < m_text.size() &&
           (m_text[m_position] == '+' || m_text[m_position] == '-')) {
      const char op = m_text[m_position++];
      const double right = product();
      value += op == '+' ? right : -right;
      if (!std::isfinite(value) || std::abs(value) > 1e15)
        invalid("macro expression overflow");
    }
    return value;
  }

public:
  MacroExpression(const std::string &text,
                  const std::map<unsigned, double> &variables)
      : m_text(text), m_variables(variables) {}
  double evaluate() {
    const double value = sum();
    if (m_position != m_text.size())
      invalid("invalid macro expression suffix");
    return value;
  }
};
ExPolygons build_macro(const std::vector<std::string> &statements,
                       const std::vector<double> &arguments, double unit,
                       double tolerance) {
  std::map<unsigned, double> variables;
  for (size_t i = 0; i < arguments.size(); ++i)
    variables.emplace(unsigned(i + 1), arguments[i]);
  ExPolygons result;
  size_t vertices_used = 0;
  for (const auto &statement : statements) {
    if (statement.empty() || statement[0] == '0')
      continue;
    if (statement[0] == '$') {
      const auto equal = statement.find('=');
      if (equal == std::string::npos)
        invalid("invalid macro assignment");
      const unsigned index = integer(statement.substr(1, equal - 1));
      if (!index || variables.size() > 4096)
        invalid("macro variable budget exceeded");
      const auto expression = statement.substr(equal + 1);
      variables[index] = MacroExpression(expression, variables).evaluate();
      continue;
    }
    std::vector<std::string> tokens;
    size_t start = 0;
    while (start < statement.size()) {
      const auto end = statement.find(',', start);
      tokens.push_back(statement.substr(start, end - start));
      if (end == std::string::npos)
        break;
      start = end + 1;
    }
    if (tokens.size() < 2)
      invalid("invalid macro primitive");
    const unsigned code = integer(tokens[0]);
    std::vector<double> values;
    for (size_t i = 1; i < tokens.size(); ++i)
      values.push_back(MacroExpression(tokens[i], variables).evaluate());
    if (code != 6 && code != 7 && values[0] != 0. && values[0] != 1.)
      invalid("macro exposure must be 0 or 1");
    Polygon shape;
    double rotation = 0.;
    auto mm = [&](size_t index) {
      const double value = values.at(index) * unit;
      bounded(value);
      return value;
    };
    if (code == 6 || code == 7) {
      // Moire and thermal primitives are always dark. Their first fields
      // are centre coordinates, rather than an exposure flag. Build holes
      // locally so they cannot erase copper from earlier macro primitives.
      if (values.size() != (code == 6 ? 9 : 6))
        invalid("invalid moire or thermal macro primitive");
      const Vec2d center{mm(0), mm(1)};
      const double diameter = mm(2);
      if (diameter <= 0.) invalid("invalid moire or thermal outer diameter");
      ExPolygons geometry;
      auto disk = [&](double size) {
        auto polygon = ellipse(center, size, size, tolerance);
        vertices_used += polygon.points.size();
        if (vertices_used > 1000000) invalid("macro geometry budget exceeded");
        return ExPolygons{ExPolygon{std::move(polygon)}};
      };
      auto rectangle = [&](double width, double height) {
        Aperture rect; rect.shape = 'R'; rect.width = width; rect.height = height;
        return ExPolygons{ExPolygon{outline(rect, center, tolerance)}};
      };
      if (code == 6) {
        const double thickness = mm(3), gap = mm(4), cross_width = mm(6), cross_length = mm(7);
        if (thickness <= 0. || gap < 0. || cross_width < 0. || cross_length < 0. ||
            values[5] < 0. || values[5] > 1024. || std::floor(values[5]) != values[5])
          invalid("invalid moire ring or crosshair dimensions");
        for (unsigned ring = 0; ring < unsigned(values[5]); ++ring) {
          const double outer = diameter - 2. * ring * (thickness + gap);
          if (outer <= 0.) break;
          const double inner = outer - 2. * thickness;
          auto annulus = disk(outer);
          if (inner > 0.) annulus = diff_ex(annulus, disk(inner));
          // Batch concentric rings; repeatedly unioning an ever-growing
          // target would make large alignment marks unnecessarily costly.
          for (auto &polygon : annulus) geometry.push_back(std::move(polygon));
        }
        if (cross_width > 0. && cross_length > 0.) {
          geometry = union_ex(geometry, rectangle(cross_length, cross_width));
          geometry = union_ex(geometry, rectangle(cross_width, cross_length));
        }
      } else {
        const double inner = mm(3), gap = mm(4);
        if (inner < 0. || inner >= diameter || gap < 0. || gap >= diameter)
          invalid("invalid thermal inner diameter or gap");
        geometry = disk(diameter);
        if (inner > 0.) geometry = diff_ex(geometry, disk(inner));
        if (gap > 0.) {
          geometry = diff_ex(geometry, rectangle(diameter, gap));
          geometry = diff_ex(geometry, rectangle(gap, diameter));
        }
      }
      const double rotation = values.back();
      bounded(rotation);
      for (auto &polygon : geometry) {
        polygon.rotate(rotation * pi / 180.);
        vertices_used += polygon.contour.points.size();
        for (const auto &hole : polygon.holes) vertices_used += hole.points.size();
      }
      if (vertices_used > 1000000) invalid("macro geometry budget exceeded");
      result = union_ex(result, geometry);
      continue;
    }
    if (code == 1) {
      if (values.size() != 4 && values.size() != 5)
        invalid("invalid circle macro primitive");
      if (mm(1) < 0.)
        invalid("negative macro circle diameter");
      if (mm(1) == 0.)
        continue;
      shape = ellipse(Vec2d{mm(2), mm(3)}, mm(1), mm(1), tolerance);
      if (values.size() == 5)
        rotation = values[4];
    } else if (code == 21 || code == 22) {
      if (values.size() != 6)
        invalid("invalid center-line macro primitive");
      Aperture rect;
      rect.shape = 'R';
      rect.width = mm(1);
      rect.height = mm(2);
      if (rect.width < 0. || rect.height < 0.)
        invalid("invalid macro rectangle dimensions");
      if (rect.width == 0. || rect.height == 0.)
        continue;
      Vec2d center{mm(3), mm(4)};
      if (code == 22)
        center += Vec2d{rect.width * .5, rect.height * .5};
      shape = outline(rect, center, tolerance);
      rotation = values[5];
    } else if (code == 20 || code == 2) {
      if (values.size() != 7 || mm(1) < 0.)
        invalid("invalid vector-line macro primitive");
      if (mm(1) == 0.)
        continue;
      const Vec2d from{mm(2), mm(3)}, to{mm(4), mm(5)};
      const auto delta = to - from;
      if (delta.norm() == 0.)
        invalid("zero-length macro vector line");
      const Vec2d normal =
          Vec2d{-delta.y(), delta.x()}.normalized() * mm(1) * .5;
      shape = Polygon{Points{point(from - normal), point(to - normal),
                             point(to + normal), point(from + normal)}};
      rotation = values[6];
    } else if (code == 5) {
      if (values.size() != 6 || values[1] < 3 || values[1] > 64 ||
          std::floor(values[1]) != values[1])
        invalid("invalid polygon macro primitive");
      Aperture polygon;
      polygon.shape = 'P';
      polygon.vertices = unsigned(values[1]);
      polygon.width = mm(4);
      if (polygon.width < 0.)
        invalid("invalid macro polygon diameter");
      if (polygon.width == 0.)
        continue;
      shape = outline(polygon, Vec2d{mm(2), mm(3)}, tolerance);
      rotation = values[5];
    } else if (code == 4) {
      if (values.size() < 9 || values[1] < 3 || values[1] > 5000 ||
          std::floor(values[1]) != values[1])
        invalid("invalid outline macro primitive");
      const auto count = size_t(values[1]);
      if (values.size() != 2 * count + 5)
        invalid("macro outline vertex count mismatch");
      Points points;
      for (size_t i = 0; i <= count; ++i)
        points.push_back(point(Vec2d{mm(2 + 2 * i), mm(3 + 2 * i)}));
      if (points.front() != points.back())
        invalid("macro outline must be closed");
      points.pop_back();
      shape = Polygon{std::move(points)};
      rotation = values.back();
    } else
      invalid("unsupported aperture macro primitive: " + std::to_string(code));
    bounded(rotation);
    shape.rotate(rotation * pi / 180.);
    if (shape.area() < 0.)
      shape.reverse();
    vertices_used += shape.points.size();
    if (vertices_used > 1000000)
      invalid("macro geometry budget exceeded");
    const ExPolygons geometry{ExPolygon{std::move(shape)}};
    result = values[0] == 1. ? union_ex(result, geometry)
                             : diff_ex(result, geometry);
  }
  if (result.empty())
    invalid("macro aperture contains no geometry");
  return result;
}
struct Coordinates {
  unsigned xi = 3, xf = 6, yi = 3, yf = 6;
  bool trailing = false;
  double unit = 1.;
  double read(const std::string &token, bool x) const {
    if (token.find('.') != std::string::npos) {
      const double value = number(token) * unit;
      bounded(value);
      return value;
    }
    std::string digits = token;
    const bool negative = !digits.empty() && digits[0] == '-';
    if (!digits.empty() && (digits[0] == '+' || digits[0] == '-'))
      digits.erase(0, 1);
    const unsigned integer = x ? xi : yi, fraction = x ? xf : yf;
    if (digits.empty() || digits.size() > integer + fraction ||
        !std::all_of(digits.begin(), digits.end(),
                     [](unsigned char c) { return std::isdigit(c); }))
      invalid("invalid fixed coordinate");
    if (trailing)
      digits.append(integer + fraction - digits.size(), '0');
    const double value =
        number(digits) / std::pow(10., fraction) * unit * (negative ? -1. : 1.);
    bounded(value);
    return value;
  }
};
std::map<char, std::string> fields(const std::string &command) {
  std::map<char, std::string> result;
  for (size_t i = 0; i < command.size();) {
    const char key = command[i++];
    if (!std::isupper(static_cast<unsigned char>(key)))
      invalid("invalid command: " + command);
    const size_t start = i;
    while (i < command.size() &&
           !std::isupper(static_cast<unsigned char>(command[i])))
      ++i;
    if (start == i || result.count(key))
      invalid("repeated or empty command field: " + command);
    result.emplace(key, command.substr(start, i - start));
  }
  return result;
}
std::vector<Vec2d> arc_points(Vec2d from, Vec2d to, Vec2d center,
                              bool clockwise, double tolerance) {
  const double radius = (from - center).norm(),
               end_radius = (to - center).norm();
  if (radius <= 0. ||
      std::abs(radius - end_radius) > std::max(tolerance * 2., radius * .00001))
    invalid("invalid circular interpolation radius");
  const double start = std::atan2(from.y() - center.y(), from.x() - center.x());
  double sweep = std::atan2(to.y() - center.y(), to.x() - center.x()) - start;
  if (clockwise) {
    if (sweep >= 0.)
      sweep -= 2. * pi;
  } else if (sweep <= 0.)
    sweep += 2. * pi;
  const double step =
      2. * std::acos(std::clamp(1. - tolerance / radius, -1., 1.));
  const size_t count =
      size_t(std::ceil(std::abs(sweep) / std::max(step, .00001)));
  if (!count || count > 65536)
    invalid("arc exceeds geometry budget");
  std::vector<Vec2d> result;
  result.reserve(count);
  for (size_t i = 1; i < count; ++i) {
    const double angle = start + sweep * i / count;
    result.push_back(center +
                     Vec2d{radius * std::cos(angle), radius * std::sin(angle)});
  }
  result.push_back(to);
  return result;
}
// Angle (rad, in (0, 2pi]) travelled from `from` to `to` around `center`.
double arc_sweep(const Vec2d &from, const Vec2d &to, const Vec2d &center,
                 bool clockwise) {
  const double a0 = std::atan2(from.y() - center.y(), from.x() - center.x()),
               a1 = std::atan2(to.y() - center.y(), to.x() - center.x());
  double sweep = clockwise ? a0 - a1 : a1 - a0;
  while (sweep <= 0.)
    sweep += 2. * pi;
  while (sweep > 2. * pi)
    sweep -= 2. * pi;
  return sweep;
}
// G74 single-quadrant mode: I/J are unsigned, so try the four sign
// combinations and keep the centre that is equidistant from both end points
// and yields an arc of at most 90 degrees in the requested direction.
Vec2d single_quadrant_center(const Vec2d &from, const Vec2d &to, double i,
                             double j, bool clockwise, double tolerance) {
  bool found = false;
  Vec2d best{0., 0.};
  double best_error = 0.;
  for (double sx : {-1., 1.})
    for (double sy : {-1., 1.}) {
      const Vec2d center = from + Vec2d{sx * i, sy * j};
      const double radius = (from - center).norm(),
                   error = std::abs(radius - (to - center).norm());
      if (radius <= 0. || error > std::max(tolerance * 2., radius * .00001))
        continue;
      if (arc_sweep(from, to, center, clockwise) > pi * .5 + 1e-6)
        continue;
      if (!found || error < best_error) {
        found = true;
        best = center;
        best_error = error;
      }
    }
  if (!found)
    invalid("G74 arc has no centre giving an arc of at most 90 degrees");
  return best;
}
// Operations recorded between %SR..%/%SR*% or %ABDnn*%/%AB*%. Each keeps its
// polarity so a block replays exactly like the original command sequence.
struct BlockOp {
  bool dark;
  ExPolygons geometry;
};
struct BlockFrame {
  bool aperture_block = false; // false: step-and-repeat
  int aperture_code = 0;
  unsigned repeat_x = 1, repeat_y = 1;
  Vec2d step{0., 0.};
  std::vector<BlockOp> ops;
  // Graphics state restored when an aperture block closes.
  bool saved_dark = true;
  ApertureTransform saved_transform;
  int saved_selected = 0;
};
constexpr size_t block_depth_limit = 16, repeat_limit = 100000;
} // namespace
PCBArtwork parse_gerber(const std::string &text, double tolerance,
                        const std::function<void()> &check_cancel) {
  validate_input(text, tolerance);
  Coordinates coordinates;
  std::map<int, Aperture> apertures;
  std::map<std::string, std::vector<std::string>> macros;
  std::string macro_name;
  bool extended = false;
  Painter painter;
  Vec2d position{0., 0.};
  int selected = 0, modal = 2, interpolation = 1;
  bool region = false, ended = false, units = false, format = false,
       dark = true;
  enum class Quadrant { unset, single, multi } quadrant = Quadrant::unset;
  ApertureTransform transform;
  std::vector<BlockFrame> frames;
  size_t recorded_vertices = 0;
  // Route finished geometry to the innermost open SR/AB block, or the board.
  auto emit_as = [&](bool polarity, ExPolygons geometry) {
    if (frames.empty()) {
      painter.polarity(polarity);
      painter.draw(std::move(geometry));
      return;
    }
    for (const auto &shape : geometry) {
      recorded_vertices += shape.contour.points.size();
      for (const auto &hole : shape.holes)
        recorded_vertices += hole.points.size();
    }
    if (recorded_vertices > 12000000)
      invalid("block geometry exceeds the 12 million vertex budget");
    frames.back().ops.push_back({polarity, std::move(geometry)});
  };
  auto emit = [&](ExPolygons geometry) { emit_as(dark, std::move(geometry)); };
  Polygons contours;
  Points contour;
  size_t operations = 0;
  auto finish_contour = [&] {
    if (!contour.empty()) {
      if (contour.size() < 3)
        invalid("region contour has fewer than three vertices");
      contours.emplace_back(std::move(contour));
      contour = {};
    }
  };
  std::string command;
  auto process = [&](std::string cmd) {
    if (cmd.empty())
      return;
    if (check_cancel)
      check_cancel();
    if (++operations > operation_limit)
      invalid("command budget exceeded");
    if (ended)
      invalid("commands after end-of-file");
    if (!macro_name.empty()) {
      if (macros.at(macro_name).size() >= 4096)
        invalid("macro primitive budget exceeded");
      macros.at(macro_name).push_back(cmd);
      return;
    }
    if (cmd.rfind("AM", 0) == 0) {
      const auto name = cmd.substr(2);
      if (!extended ||
          !std::regex_match(name, std::regex("[A-Za-z_][A-Za-z0-9_.$]*")) ||
          macros.count(name) || macros.size() >= 4096)
        invalid("invalid or duplicate aperture macro name");
      macros.emplace(name, std::vector<std::string>{});
      macro_name = name;
      return;
    }

    if (cmd.rfind("AB", 0) == 0) {
      if (!extended || region || !units)
        invalid("aperture block outside units declaration or inside a region");
      if (cmd == "AB") {
        // Close: render the block to its own image (own polarity) and store
        // it as a flashable aperture. Outer graphics state is restored.
        if (frames.empty() || !frames.back().aperture_block)
          invalid("%AB*% without an open aperture block");
        BlockFrame frame = std::move(frames.back());
        frames.pop_back();
        Painter block_painter;
        for (auto &op : frame.ops) {
          block_painter.polarity(op.dark);
          block_painter.draw(std::move(op.geometry));
        }
        Aperture block;
        block.shape = 'M';
        block.macro_shape = block_painter.finish();
        apertures.emplace(frame.aperture_code, std::move(block));
        dark = frame.saved_dark;
        transform = frame.saved_transform;
        selected = frame.saved_selected;
        return;
      }
      std::smatch match;
      if (!std::regex_match(cmd, match, std::regex("ABD([0-9]+)")))
        invalid("invalid aperture block command: " + cmd);
      const int code = int(integer(match[1]));
      if (code < 10 || apertures.count(code) || frames.size() >= block_depth_limit)
        invalid("invalid or duplicate aperture block number, or nesting too deep");
      for (const auto &open : frames)
        if (open.aperture_block && open.aperture_code == code)
          invalid("aperture block references itself");
      BlockFrame frame;
      frame.aperture_block = true;
      frame.aperture_code = code;
      frame.saved_dark = dark;
      frame.saved_transform = transform;
      frame.saved_selected = selected;
      frames.push_back(std::move(frame));
      // A block starts from the default graphics state.
      dark = true;
      transform = {};
      return;
    }
    if (cmd.rfind("SR", 0) == 0) {
      if (!extended || region || !units)
        invalid("step-and-repeat outside units declaration or inside a region");
      if (cmd == "SR") {
        if (frames.empty() || frames.back().aperture_block)
          invalid("%SR*% without an open step-and-repeat block");
        BlockFrame frame = std::move(frames.back());
        frames.pop_back();
        for (unsigned row = 0; row < frame.repeat_y; ++row)
          for (unsigned column = 0; column < frame.repeat_x; ++column) {
            const Point shift =
                point(Vec2d{column * frame.step.x(), row * frame.step.y()});
            for (const auto &op : frame.ops) {
              if (check_cancel)
                check_cancel();
              auto copy = op.geometry;
              for (auto &shape : copy)
                shape.translate(shift);
              emit_as(op.dark, std::move(copy));
            }
          }
        return;
      }
      for (const auto &open : frames)
        if (!open.aperture_block)
          invalid("nested step-and-repeat blocks are not allowed");
      const auto sr = fields(cmd.substr(2));
      if (sr.size() != 4 || !sr.count('X') || !sr.count('Y') || !sr.count('I') ||
          !sr.count('J'))
        invalid("step-and-repeat needs X, Y, I and J: " + cmd);
      BlockFrame frame;
      frame.repeat_x = integer(sr.at('X'));
      frame.repeat_y = integer(sr.at('Y'));
      if (frame.repeat_x < 1 || frame.repeat_y < 1 ||
          size_t(frame.repeat_x) * frame.repeat_y > repeat_limit ||
          frames.size() >= block_depth_limit)
        invalid("step-and-repeat counts out of range");
      frame.step = Vec2d{number(sr.at('I')) * coordinates.unit,
                         number(sr.at('J')) * coordinates.unit};
      bounded(frame.step.x());
      bounded(frame.step.y());
      frames.push_back(std::move(frame));
      return;
    }
    if (cmd.rfind("LM", 0) == 0 || cmd.rfind("LR", 0) == 0 ||
        cmd.rfind("LS", 0) == 0) {
      if (region)
        invalid("aperture transformation inside a region");
      const std::string value = cmd.substr(2);
      if (cmd[1] == 'M') {
        if (value != "N" && value != "X" && value != "Y" && value != "XY")
          invalid("invalid mirroring command: " + cmd);
        transform.mirror_x = value.find('X') != std::string::npos;
        transform.mirror_y = value.find('Y') != std::string::npos;
      } else if (cmd[1] == 'R') {
        transform.rotation_deg = number(value);
        bounded(transform.rotation_deg);
      } else {
        transform.scale = number(value);
        if (transform.scale <= 0. || transform.scale > 1000.)
          invalid("aperture scale must be positive");
      }
      return;
    }
    if (cmd.rfind("G04", 0) == 0 || cmd.rfind("G4", 0) == 0 ||
        cmd.rfind("TF", 0) == 0 || cmd.rfind("TA", 0) == 0 ||
        cmd.rfind("TO", 0) == 0 || cmd.rfind("TD", 0) == 0)
      return;
    if (cmd.rfind("FS", 0) == 0) {
      std::smatch match;
      if (!std::regex_match(
              cmd, match,
              std::regex("FS([LT])AX([1-6])([1-7])Y([1-6])([1-7])"))) {
        if (cmd.size() > 4 && cmd[3] == 'I')
          invalid("incremental (relative) coordinates are not supported; "
                  "export the Gerber with absolute coordinates (FSxA)");
        invalid("unsupported FS coordinate format: " + cmd);
      }
      coordinates.trailing = match[1] == "T";
      coordinates.xi = std::stoul(match[2]);
      coordinates.xf = std::stoul(match[3]);
      coordinates.yi = std::stoul(match[4]);
      coordinates.yf = std::stoul(match[5]);
      format = true;
      return;
    }
    if (cmd == "MOMM" || cmd == "MOIN") {
      coordinates.unit = cmd == "MOMM" ? 1. : 25.4;
      units = true;
      return;
    }
    if (cmd == "LPD" || cmd == "LPC") {
      if (region)
        invalid("polarity change inside a region");
      dark = cmd == "LPD";
      painter.polarity(dark);
      return;
    }
    if (cmd.rfind("ADD", 0) == 0) {
      if (!units)
        invalid("declare units before apertures");
      std::smatch match;
      if (!std::regex_match(
              cmd, match,
              std::regex("ADD([0-9]+)([A-Za-z_][A-Za-z0-9_.$]*)(?:,(.*))?")))
        invalid("unsupported aperture or aperture macro: " + cmd);
      const int code = int(integer(match[1]));
      if (code < 10 || code > 999999 || apertures.count(code))
        invalid("invalid or duplicate aperture number");
      std::vector<double> values;
      std::string params = match[3];
      size_t start = 0;
      while (start < params.size()) {
        auto end = params.find('X', start);
        values.push_back(number(params.substr(start, end - start)));
        if (end == std::string::npos)
          break;
        start = end + 1;
      }
      Aperture a;
      const std::string name = match[2];
      if (name.size() != 1 ||
          std::string("CROP").find(name[0]) == std::string::npos) {
        if (!macros.count(name))
          invalid("unknown macro aperture: " + name);
        a.shape = 'M';
        a.macro_shape =
            build_macro(macros.at(name), values, coordinates.unit, tolerance);
        apertures.emplace(code, std::move(a));
        return;
      }
      a.shape = name[0];
      if (values.empty())
        invalid("missing aperture parameters");
      a.width = values[0] * coordinates.unit;
      if (a.shape == 'C') {
        if (values.size() > 2)
          invalid("rectangular aperture holes are unsupported");
        a.height = a.width;
        if (values.size() > 1)
          a.hole = values[1] * coordinates.unit;
      } else if (a.shape == 'P') {
        if (values.size() < 2 || values.size() > 4 || values[1] < 3 ||
            values[1] > 64 || std::floor(values[1]) != values[1])
          invalid("invalid polygon aperture");
        a.vertices = unsigned(values[1]);
        a.height = a.width;
        if (values.size() > 2)
          a.rotation = values[2];
        if (values.size() > 3)
          a.hole = values[3] * coordinates.unit;
      } else {
        if (values.size() < 2 || values.size() > 3)
          invalid("invalid rectangular/obround aperture");
        a.height = values[1] * coordinates.unit;
        if (values.size() > 2)
          a.hole = values[2] * coordinates.unit;
      }
      if (a.width <= 0. || a.height <= 0. || a.hole < 0. ||
          a.hole >= std::min(a.width, a.height))
        invalid("invalid aperture dimensions");
      if (a.shape == 'P' && a.hole >= a.width * std::cos(pi / a.vertices))
        invalid("polygon aperture hole exceeds its inscribed circle");
      bounded(a.width);
      bounded(a.height);
      apertures.emplace(code, a);
      return;
    }
    // Parse leading G codes separately: common files combine G01 with XYD.
    while (!cmd.empty() && cmd[0] == 'G') {
      size_t i = 1;
      while (i < cmd.size() && std::isdigit(static_cast<unsigned char>(cmd[i])))
        ++i;
      const int code = int(integer(cmd.substr(1, i - 1)));
      cmd.erase(0, i);
      if (code == 1 || code == 2 || code == 3)
        interpolation = code;
      else if (code == 74)
        quadrant = Quadrant::single;
      else if (code == 75)
        quadrant = Quadrant::multi;
      else if (code == 91)
        invalid("relative coordinates (G91) are not supported; export the "
                "Gerber with absolute coordinates (G90)");
      else if (code == 90 || code == 54) {
      } else if (code == 70 || code == 71) {
        coordinates.unit = code == 70 ? 25.4 : 1.;
        units = true;
      } else if (code == 36) {
        if (region)
          invalid("nested regions");
        region = true;
        contours.clear();
        contour.clear();
      } else if (code == 37) {
        if (!region)
          invalid("region end without start");
        finish_contour();
        emit(union_ex(contours, ClipperLib::pftEvenOdd));
        region = false;
      } else
        invalid("unsupported Gerber G command: " + std::to_string(code));
    }
    if (cmd.empty())
      return;
    if (cmd == "M02" || cmd == "M2") {
      if (region)
        invalid("unterminated region");
      if (!frames.empty())
        invalid("unterminated step-and-repeat or aperture block");
      ended = true;
      return;
    }
    const auto tokens = fields(cmd);
    for (const auto &field : tokens)
      if (std::string("XYIJD").find(field.first) == std::string::npos)
        invalid("unsupported command: " + cmd);
    if (tokens.count('D')) {
      const int operation = int(integer(tokens.at('D')));
      if (operation >= 10) {
        if (tokens.size() != 1 || !apertures.count(operation) || region)
          invalid("invalid aperture selection");
        selected = operation;
        return;
      }
      if (operation < 1 || operation > 3)
        invalid("invalid draw operation");
      modal = operation;
    }
    if (!format || !units)
      invalid("declare FS coordinates and MO units before drawing");
    Vec2d target = position;
    if (tokens.count('X'))
      target.x() = coordinates.read(tokens.at('X'), true);
    if (tokens.count('Y'))
      target.y() = coordinates.read(tokens.at('Y'), false);
    if (modal == 2) {
      if (region) {
        finish_contour();
        contour.push_back(point(target));
      }
      position = target;
      return;
    }
    if (region && modal == 3)
      invalid("flash inside region");
    if (!region && !apertures.count(selected))
      invalid("draw without selected aperture");
    if (modal == 3)
      emit(flash(apertures.at(selected), target, tolerance, transform));
    else {
      std::vector<Vec2d> path;
      if (interpolation == 1) {
        if (tokens.count('I') || tokens.count('J'))
          invalid("arc offsets with linear interpolation");
        path.push_back(target);
      } else {
        if (quadrant == Quadrant::unset)
          invalid("declare G74 or G75 before circular interpolation");
        Vec2d offset{0., 0.};
        if (tokens.count('I'))
          offset.x() = coordinates.read(tokens.at('I'), true);
        if (tokens.count('J'))
          offset.y() = coordinates.read(tokens.at('J'), false);
        if (quadrant == Quadrant::multi)
          path = arc_points(position, target, position + offset,
                            interpolation == 2, tolerance);
        else if (position == target)
          path = {target}; // G74 has no full circles: a null arc.
        else
          path = arc_points(
              position, target,
              single_quadrant_center(position, target, std::abs(offset.x()),
                                     std::abs(offset.y()), interpolation == 2,
                                     tolerance),
              interpolation == 2, tolerance);
      }
      if (region && contour.empty())
        contour.push_back(point(position));
      Vec2d from = position;
      for (const auto &to : path) {
        if (region)
          contour.push_back(point(to));
        else
          emit(stroke(apertures.at(selected), from, to, tolerance, transform));
        from = to;
      }
    }
    position = target;
  };

  // Extended macro blocks preserve their full sequence until the closing %.
  for (unsigned char c : text) {
    if (c == '*') {
      process(command);
      command.clear();
    } else if (c == '%') {
      if (!command.empty())
        invalid("unterminated extended statement");
      if (extended) {
        macro_name.clear();
        extended = false;
      } else
        extended = true;
    } else if (std::isspace(c))
      continue;
    else
      command.push_back(char(c));
  }
  if (extended || !macro_name.empty())
    invalid("unterminated extended block");
  if (!command.empty())
    invalid("unterminated statement");
  if (!ended || region)
    invalid("missing Gerber end-of-file");
  PCBArtwork result{painter.finish()};
  if (result.copper.empty())
    invalid("board contains no exposed geometry");
  return result;
}
void subtract_excellon(PCBArtwork &board, const std::string &text,
                       double tolerance,
                       const std::function<void()> &check_cancel) {
  validate_input(text, tolerance);
  Coordinates coordinates;
  coordinates.trailing =
      true; // Excellon LZ includes leading zeros (trailing suppression).
  bool units = false, header = false, ended = false, explicit_format = false;
  std::map<int, double> tools;
  int selected = 0;
  Vec2d position{0., 0.};
  // Excellon routing state: G00 enters rout mode (until G05), M15/M16 lower or
  // raise the tool, and G01/G02/G03 choose the interpolation while it is down.
  bool routing = false, tool_down = false;
  int rout_code = 0;
  ExPolygons holes;
  size_t hole_vertices = 0;
  auto add_holes = [&](ExPolygons geometry) {
    for (auto &polygon : geometry) {
      hole_vertices += polygon.contour.points.size();
      if (hole_vertices > 1000000) invalid("drill geometry budget exceeded");
      holes.push_back(std::move(polygon));
    }
  };
  std::istringstream input(text);
  std::string line;
  size_t operations = 0;
  while (std::getline(input, line)) {
    if (check_cancel)
      check_cancel();
    if (++operations > operation_limit)
      invalid("drill command budget exceeded");
    line.erase(std::remove_if(line.begin(), line.end(),
                              [](unsigned char c) { return std::isspace(c); }),
               line.end());
    if (line.empty())
      continue;
    if (line[0] == ';') {
      // KiCad declares a precise fixed-coordinate format in this comment.
      std::smatch m;
      if (std::regex_match(line, m,
                           std::regex(";FILE_FORMAT=([1-6]):([1-7])"))) {
        coordinates.xi = coordinates.yi = std::stoul(m[1]);
        coordinates.xf = coordinates.yf = std::stoul(m[2]);
        explicit_format = true;
      }
      continue;
    }
    if (ended)
      invalid("drill commands after end-of-file");
    if (line == "M48") {
      header = true;
      continue;
    }
    if (line == "%" || line == "M95") {
      header = false;
      continue;
    }
    if (line == "M30") {
      ended = true;
      continue;
    }
    if (line == "G05" || line == "G5") {
      routing = tool_down = false; // back to drill mode
      continue;
    }
    if (line == "G91" || line.rfind("ICI,ON", 0) == 0)
      invalid("incremental drill coordinates are not supported; export "
              "absolute coordinates");
    if (line == "G90" || line == "G40" || line.rfind("FMAT,", 0) == 0 ||
        line.rfind("VER,", 0) == 0 || line.rfind("DETECT,", 0) == 0 ||
        line.rfind("ATC,", 0) == 0 || line.rfind("ICI,OFF", 0) == 0)
      continue;
    if (line == "M71" || line == "M72") {
      // Unit switch used instead of the METRIC/INCH header line by some CAMs.
      const bool inches = line == "M72";
      coordinates.unit = inches ? 25.4 : 1.;
      units = true;
      if (!explicit_format) {
        coordinates.xi = coordinates.yi = inches ? 2 : 3;
        coordinates.xf = coordinates.yf = inches ? 4 : 3;
      }
      continue;
    }
    if (line == "M15" || line == "M16" || line == "M17") {
      if (!routing)
        invalid("tool up/down command outside routing mode (missing G00)");
      tool_down = line == "M15";
      continue;
    }
    if (line.rfind("METRIC", 0) == 0 || line.rfind("INCH", 0) == 0) {
      const bool inches = line.rfind("INCH", 0) == 0;
      coordinates.unit = inches ? 25.4 : 1.;
      units = true;
      if (!explicit_format) {
        coordinates.xi = coordinates.yi = inches ? 2 : 3;
        coordinates.xf = coordinates.yf = inches ? 4 : 3;
      }
      if (line.find(",TZ") != std::string::npos)
        coordinates.trailing = false;
      else if (line.find(",LZ") != std::string::npos)
        coordinates.trailing = true;
      const size_t comma = line.rfind(',');
      if (comma != std::string::npos && comma + 1 < line.size() &&
          line[comma + 1] == '0') {
        const auto spec = line.substr(comma + 1);
        const auto dot = spec.find('.');
        if (dot == std::string::npos || dot < 1 || dot > 6 ||
            spec.size() - dot - 1 < 1 || spec.size() - dot - 1 > 7 ||
            spec.find_first_not_of("0.") != std::string::npos)
          invalid("invalid drill coordinate format");
        coordinates.xi = coordinates.yi = unsigned(dot);
        coordinates.xf = coordinates.yf = unsigned(spec.size() - dot - 1);
        explicit_format = true;
      }
      continue;
    }
    if (line[0] == 'T') {
      const auto tokens = fields(line);
      // Besides C (diameter) a declaration may carry feed/speed/retract/hit
      // count/depth parameters, which do not affect the exposed geometry.
      for (const auto &field : tokens)
        if (std::string("TCFSBHZ").find(field.first) == std::string::npos)
          invalid("unsupported drill tool command: " + line);
      if (!tokens.count('T') || (tokens.size() > 1 && !tokens.count('C')))
        invalid("unsupported drill tool command: " + line);
      const int tool = int(integer(tokens.at('T')));
      if (tool < 0 || tool > 999999)
        invalid("invalid drill tool");
      if (tokens.count('C')) {
        if (!units || !header || tools.count(tool))
          invalid("invalid drill tool declaration");
        const double diameter = number(tokens.at('C')) * coordinates.unit;
        if (diameter <= 0.)
          invalid("drill diameter must be positive");
        tools[tool] = diameter;
      } else {
        if (tool == 0) {
          selected = 0;
          continue;
        }
        if (!tools.count(tool))
          invalid("unknown drill tool");
        selected = tool;
      }
      continue;
    }
    if (!units || header)
      invalid("drill hit without units or tool");
    // Leading G codes select drill or routing modes; any M15/M16 suffix is
    // applied after the move on the same line.
    while (line.size() > 1 && line[0] == 'G' && line.find("G85") != 0) {
      size_t digits = 1;
      while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits])))
        ++digits;
      const int code = int(integer(line.substr(1, digits - 1)));
      line.erase(0, digits);
      if (code == 0 || code == 1 || code == 2 || code == 3) {
        if (code != 0 && !routing)
          invalid("G01/G02/G03 routing requires a preceding G00 rout move");
        routing = true;
        rout_code = code;
        if (code == 0)
          tool_down = false;
      } else if (code == 5) {
        routing = tool_down = false;
      } else if (code != 90)
        invalid("unsupported drill G command: " + std::to_string(code));
    }
    int after_move = 0; // 15 lowers, 16 raises the tool after the move
    if (line.size() >= 3 && (line.compare(line.size() - 3, 3, "M15") == 0 ||
                             line.compare(line.size() - 3, 3, "M16") == 0)) {
      after_move = line[line.size() - 1] == '5' ? 15 : 16;
      line.erase(line.size() - 3);
      if (!routing)
        invalid("tool up/down command outside routing mode (missing G00)");
    }
    if (line.empty()) {
      if (after_move)
        tool_down = after_move == 15;
      continue;
    }
    if (routing && line.find("G85") == std::string::npos) {
      const auto values = fields(line);
      for (const auto &field : values)
        if (std::string("XYIJA").find(field.first) == std::string::npos ||
            (rout_code < 2 && std::string("IJA").find(field.first) != std::string::npos))
          invalid("unsupported drill routing command: " + line);
      Vec2d target = position;
      if (values.count('X'))
        target.x() = coordinates.read(values.at('X'), true);
      if (values.count('Y'))
        target.y() = coordinates.read(values.at('Y'), false);
      if (tool_down && rout_code != 0) {
        if (!tools.count(selected))
          invalid("routing without a selected tool");
        Aperture cutter;
        cutter.width = tools.at(selected);
        std::vector<Vec2d> path;
        if (rout_code == 1) {
          path.push_back(target);
        } else {
          const bool clockwise = rout_code == 2;
          Vec2d center;
          if (values.count('A')) {
            // Radius form: the centre giving the smaller (<= 180 degree) arc.
            const double radius = std::abs(coordinates.read(values.at('A'), true));
            const Vec2d chord = target - position;
            const double half = chord.norm() * .5;
            if (half <= 0. || radius < half * (1. - 1e-6))
              invalid("invalid routed arc radius");
            const Vec2d middle = (position + target) * .5,
                        normal = Vec2d{-chord.y(), chord.x()}.normalized();
            const double offset = std::sqrt(std::max(0., radius * radius - half * half));
            center = middle + normal * offset;
            if (arc_sweep(position, target, center, clockwise) > pi + 1e-6)
              center = middle - normal * offset;
          } else {
            Vec2d offset{0., 0.};
            if (values.count('I'))
              offset.x() = coordinates.read(values.at('I'), true);
            if (values.count('J'))
              offset.y() = coordinates.read(values.at('J'), false);
            center = position + offset;
          }
          path = arc_points(position, target, center, clockwise, tolerance);
        }
        Vec2d from = position;
        for (const auto &to : path) {
          add_holes(stroke(cutter, from, to, tolerance));
          from = to;
        }
      }
      position = target;
      if (after_move)
        tool_down = after_move == 15;
      continue;
    }
    if (!tools.count(selected))
      invalid("drill hit without units or tool");
    const auto slot = line.find("G85");
    if (slot != std::string::npos) {
      if (line.find("G85", slot+3) != std::string::npos)
        invalid("multiple slot commands on one drill line");
      auto endpoint = [&](const std::string &text, Vec2d previous, bool required) {
        if (text.empty()) {
          if (required) invalid("slot endpoint is missing");
          return previous;
        }
        const auto values = fields(text);
        if (values.empty()) invalid("slot endpoint is missing");
        for (const auto &field : values) {
          if (field.first != 'X' && field.first != 'Y') invalid("unsupported slot coordinate command");
          previous[field.first == 'X' ? 0 : 1] = coordinates.read(field.second, field.first == 'X');
        }
        return previous;
      };
      const auto from = endpoint(line.substr(0, slot), position, false);
      const auto to = endpoint(line.substr(slot+3), from, true);
      Aperture cutter; cutter.width = tools.at(selected);
      add_holes(stroke(cutter, from, to, tolerance));
      position = to;
      continue;
    }
    const auto tokens = fields(line);
    for (const auto &field : tokens)
      if (field.first != 'X' && field.first != 'Y')
        invalid("unsupported drill routing command: " + line);
    if (tokens.count('X'))
      position.x() = coordinates.read(tokens.at('X'), true);
    if (tokens.count('Y'))
      position.y() = coordinates.read(tokens.at('Y'), false);
    add_holes({ExPolygon{ellipse(position, tools.at(selected), tools.at(selected), tolerance)}});
  }
  if (!ended)
    invalid("missing drill end-of-file");
  // Preserve the artwork on parse failure; apply the subtraction only now.
  auto result = diff_ex(board.copper, union_ex(holes));
  board.copper = std::move(result);
}
namespace {
// Mirrors, centers and offsets the artwork in physical LCD millimetres. Shared by
// the final rasterizer and the preview so both show exactly the same placement.
ExPolygons place_pcb_geometry(ExPolygons geometry, const BoundingBox &reference_bounds,
                              const SLAPrinterConfig &printer, const PCBPlacement &placement) {
  for (double value :
       {placement.offset_x, placement.offset_y, placement.margin})
    bounded(value);
  if (placement.margin < 0.)
    invalid("margin cannot be negative");
  if (!std::isfinite(placement.scale) || placement.scale < .1 || placement.scale > 10.)
    invalid("size scale must be between 0.1 and 10");
  auto bounds = reference_bounds;
  const double width = printer.display_width.value,
               height = printer.display_height.value;
  if (!std::isfinite(width) || !std::isfinite(height) || width <= 0. ||
      height <= 0.)
    invalid("invalid printer display dimensions");
  const Vec2d size = unscaled(bounds.size());
  if (size.x() * placement.scale + placement.margin * 2. > width ||
      size.y() * placement.scale + placement.margin * 2. > height)
    invalid("board does not fit the selected printer display");
  const Vec2d center = unscaled(bounds.min) + (size * .5);
  for (auto &polygon : geometry) {
    polygon.translate(point(-center));
    polygon.scale((placement.mirror_x ? -1. : 1.) * placement.scale,
                  (placement.mirror_y ? -1. : 1.) * placement.scale);
    polygon.translate(point(center));
  }
  const Vec2d offset =
      Vec2d{placement.offset_x, placement.offset_y} +
      (placement.center ? Vec2d{width * .5, height * .5} - center
                        : Vec2d{0., 0.});
  for (auto &polygon : geometry)
    polygon.translate(point(offset));
  bounds = get_extents(geometry);
  const auto low = unscaled(bounds.min), high = unscaled(bounds.max);
  if (low.x() < placement.margin - 1e-6 || low.y() < placement.margin - 1e-6 ||
      high.x() > width - placement.margin + 1e-6 ||
      high.y() > height - placement.margin + 1e-6)
    invalid("placement would clip the board at the LCD edge");
  return geometry;
}
EncodedRaster rasterize_pcb_geometry(ExPolygons geometry,
                                    const BoundingBox &reference_bounds,
                                    const SLAPrinterConfig &requested_printer,
                                    const PCBPlacement &placement,
                                    const std::function<void()> &check_cancel, bool invert) {
  geometry = place_pcb_geometry(std::move(geometry), reference_bounds, requested_printer, placement);
  // Gamma 0 makes the AGG rasterizer threshold, i.e. disables anti-aliasing.
  SLAPrinterConfig printer = requested_printer;
  if (!placement.antialiasing)
    printer.gamma_correction.value = 0.;
  auto raster = create_printer_raster(printer);
  for (const auto &polygon : geometry) {
    if (check_cancel)
      check_cancel();
    raster->draw(polygon);
  }
  // Preserve the actual native alphabet before PNG becomes source/editor
  // data. Pixel support does not enable an owned executable job for a codec
  // (notably v1 cannot encode the stationary motion intent below).
  const auto precision=independent_generated_mask_precision(
      printer.sla_archive_format.value,printer.sla_anycubic_version.value);
  const RasterEncoder encoder = precision || invert
      ? RasterEncoder(ProjectedPNGMaskEncoder{
          precision.value_or(LayerMaskPrecision::Gray8), check_cancel, invert})
      : RasterEncoder(PNGRasterEncoder{});
  return raster->encode(encoder);
}
// Validates the inputs and concatenates all copper; every file keeps the same
// physical coordinates, so the combined bounds define the shared registration.
ExPolygons combine_boards(const std::vector<PCBArtwork> &boards,
                          const std::function<void()> &check_cancel) {
  if (boards.empty() || boards.size() > 1000)
    invalid("choose 1–1000 copper files");
  ExPolygons combined;
  size_t vertices = 0;
  for (const auto &board : boards) {
    if (check_cancel) check_cancel();
    if (board.copper.empty()) invalid("a copper file contains no exposed geometry");
    for (const auto &polygon : board.copper) {
      vertices += polygon.contour.points.size();
      for (const auto &hole : polygon.holes) vertices += hole.points.size();
    }
    if (vertices > 12000000) invalid("combined copper exceeds the geometry budget");
    combined.insert(combined.end(),board.copper.begin(),board.copper.end());
  }
  return combined;
}
LayerDocument rasterize_pcb_layers_impl(const std::vector<PCBArtwork> &boards,
                                   const SLAPrinterConfig &printer, bool merge,
                                   const PCBPlacement &placement,
                                   const std::function<void()> &check_cancel, bool invert) {
  const ExPolygons combined = combine_boards(boards, check_cancel);
  const auto bounds = get_extents(combined);
  std::vector<EncodedRaster> masks;
  if (merge) {
    if (check_cancel) check_cancel();
    masks.push_back(rasterize_pcb_geometry(union_ex(combined),bounds,printer,placement,check_cancel,invert));
  } else {
    size_t bytes = 0;
    for (const auto &board : boards) {
      if (check_cancel) check_cancel();
      auto encoded = rasterize_pcb_geometry(board.copper,bounds,printer,placement,check_cancel,invert);
      bytes += encoded.size();
      if (bytes > 1024ULL * 1024 * 1024) invalid("PCB masks exceed the 1 GiB budget");
      masks.push_back(std::move(encoded));
    }
  }
  const auto precision=independent_generated_mask_precision(
      printer.sla_archive_format.value,printer.sla_anycubic_version.value);
  return precision ? LayerDocument::from_precision_rasters(std::move(masks),*precision,
      [&](size_t,size_t){if(check_cancel)check_cancel();return true;}) :
      LayerDocument::from_rasters(std::move(masks));
}

} // namespace

LayerDocument rasterize_pcb(const PCBArtwork &board,
                            const SLAPrinterConfig &printer,
                            const PCBPlacement &placement,
                            const std::function<void()> &check_cancel) {
  return rasterize_pcb_layers_impl({board}, printer, true, placement, check_cancel, false);
}

LayerDocument rasterize_pcb_layers(const std::vector<PCBArtwork> &boards,
                                   const SLAPrinterConfig &printer, bool merge,
                                   const PCBPlacement &placement,
                                   const std::function<void()> &check_cancel) {
  return rasterize_pcb_layers_impl(boards, printer, merge, placement, check_cancel, false);
}

std::unique_ptr<SLAPrint> create_pcb_exposure_job(
    const DynamicPrintConfig &configuration, const std::vector<PCBArtwork> &boards,
    bool merge_files, const PCBPlacement &placement, double exposure_seconds,
    bool invert_mask, const std::function<void()> &check_cancel) {
  if (!std::isfinite(exposure_seconds) || exposure_seconds < .001 || exposure_seconds > 86400.)
    invalid("exposure must be between 0.001 and 86400 seconds");
  if (check_cancel) check_cancel();
  DynamicPrintConfig config; config.apply(SLAFullPrintConfig{}); config.apply(configuration);
  if (config.option("printer_technology")->getInt() != ptSLA)
    invalid("exposure requires an SLA printer");
  SLAPrinterConfig printer; printer.apply(config, true);
  printer.gamma_correction.value = 1.;
  // Generator polarity is part of the first canonical source PNG, not an
  // editor transaction or a second decode/encode pass.
  auto document = rasterize_pcb_layers_impl(
      boards, printer, merge_files, placement, check_cancel, invert_mask);
  config.set("gamma_correction", 1.);
  config.set("sla_bottom_layers", 0);
  config.set("sla_transition_layers", 0);
  config.set("exposure_time", exposure_seconds);
  config.set("initial_exposure_time", exposure_seconds);
  // PCB exposure is a separate, stationary job. These overrides do
  // not change any resin/process/printer preset used by the scene.
  config.set("max_exposure_time",
             std::max(config.opt_float("max_exposure_time"),
                      exposure_seconds));
  config.set("max_initial_exposure_time",
             std::max(config.opt_float("max_initial_exposure_time"),
                      exposure_seconds));
  for (const char *key :
       {"sla_lift_height", "sla_lift_height2", "sla_retract_height2",
        "sla_bottom_lift_height", "sla_bottom_lift_height2",
        "sla_bottom_retract_height2", "sla_lift_speed", "sla_lift_speed2",
        "sla_retract_speed", "sla_retract_speed2", "sla_bottom_lift_speed",
        "sla_bottom_lift_speed2", "sla_bottom_retract_speed",
        "sla_bottom_retract_speed2", "sla_wait_before_cure",
        "sla_wait_after_cure", "sla_wait_after_lift",
        "sla_bottom_wait_before_cure", "sla_bottom_wait_after_cure",
        "sla_bottom_wait_after_lift"})
    config.set(key, 0.);
  config.set("sla_light_pwm", 255);
  config.set("sla_bottom_light_pwm", 255);
  if (config.opt_string("sla_archive_format") == "SL1SVG")
    config.set("sla_archive_format", std::string("uvj"));
  if (check_cancel) check_cancel();
  std::vector<double> positions;
  for (size_t i = 0; i < document.layer_count(); ++i)
    positions.push_back((i+1) * config.opt_float("layer_height"));
  return SLAPrint::create_raster_job(config, document, positions,
      generated_layer_ownership(config.opt_string("sla_archive_format"),
                                FreshLayerProducer::PCB,
                                config.option("sla_anycubic_version")?config.opt_string("sla_anycubic_version"):"auto"));
}

PCBPreview render_pcb_preview(const std::vector<PCBArtwork> &boards,
                              const SLAPrinterConfig &printer, bool merge,
                              const PCBPlacement &placement, bool invert,
                              size_t max_dimension_px, size_t layer_index,
                              const std::function<void()> &check_cancel) {
  if (max_dimension_px < 16 || max_dimension_px > 4096)
    invalid("preview size must be between 16 and 4096 px");
  const ExPolygons combined = combine_boards(boards, check_cancel);
  const auto bounds = get_extents(combined);
  PCBPreview preview;
  preview.layer_count = merge ? 1 : boards.size();
  preview.shown_layer = merge ? 0 : std::min(layer_index, boards.size() - 1);
  // Same LCD, fewer pixels: the pitch grows, so the physical placement is unchanged.
  SLAPrinterConfig small = printer;
  const double cols = std::max(1, printer.display_pixels_x.value),
               rows = std::max(1, printer.display_pixels_y.value);
  const double scale = std::min(1., double(max_dimension_px) / std::max(cols, rows));
  small.display_pixels_x.value = std::max(1, int(std::lround(cols * scale)));
  small.display_pixels_y.value = std::max(1, int(std::lround(rows * scale)));
  small.gamma_correction.value = 1.;
  ExPolygons shown = merge ? union_ex(combined) : boards[preview.shown_layer].copper;
  const auto placed = place_pcb_geometry(std::move(shown), bounds, small, placement);
  auto raster = create_printer_raster(small);
  for (const auto &polygon : placed) {
    if (check_cancel) check_cancel();
    raster->draw(polygon);
  }
  raster->encode([&](const void *data, size_t width, size_t height, size_t) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    preview.width_px = width;
    preview.height_px = height;
    preview.pixels.assign(bytes, bytes + width * height);
    return EncodedRaster(std::vector<uint8_t>{0}, "raw");
  });
  if (invert)
    for (auto &value : preview.pixels) value = uint8_t(255 - value);
  return preview;
}

const std::vector<PCBExposureRecipe> &pcb_exposure_presets() {
  // Starting points for 405 nm MSLA screens; real doses depend on the LED power,
  // film and resist batch, so the dialog tells the user to run a step test first.
  static const std::vector<PCBExposureRecipe> presets{
      {"Dry film resist (negative)", 45., false,
       "Photoresist film laminated on copper: light hardens the film, so copper tracks stay lit. "
       "Typical 405 nm dose 30-90 s; develop in sodium carbonate, then etch."},
      {"UV solder mask (negative)", 60., true,
       "Solder mask is cured where it must stay, i.e. everywhere except pads: the copper layer is inverted. "
       "Typical 405 nm dose 40-120 s; develop in the manufacturer's solvent, then post-cure."},
      {"Positive photoresist board", 120., true,
       "Presensitised positive board: light removes the resist, so lit areas must be the gaps between "
       "copper (inverted polarity). Typical 405 nm dose 90-240 s; develop in NaOH solution."},
  };
  return presets;
}
} // namespace Slic3r::sla
