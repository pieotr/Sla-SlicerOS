// SPDX-License-Identifier: AGPL-3.0-or-later
// Hershey vector text (the classical OpenCV putText faces) shared by the calibration generators and the pixel
// editor's Text tool. OpenCV 5 maps the Hershey API to other fonts, so the glyph data (HersheyFontData.hpp) and the
// 16.16 fixed-point stroke walk are ours; only the stroke rasteriser (polylines) is OpenCV's, kept in HersheyText.cpp.
// The multi-line / alignment / rotation rules follow UVtools EmguExtensions.PutTextExtended / PutTextRotated.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Slic3r::sla::hershey_text {

enum class LineType { FourConnected = 4, EightConnected = 8, AntiAlias = 16 };
// UVtools PutTextLineAlignment: None = left, trailing blanks trimmed only; Left / Center / Right trim both ends.
enum class Alignment { None, Left, Center, Right };

struct Extent { int width = 0, height = 0, baseline = 0; };

// Faces: 0 Simplex, 1 Plain, 2 Duplex, 3 Complex, 4 Triplex, 5 Complex small, 6 Script simplex, 7 Script complex; +16 = italic.
bool valid_face(int face);
// Size of ONE line (cv::getTextSize semantics with the classical metrics). Throws std::invalid_argument for a bad face.
Extent text_extent(const std::string &line, int face, double scale, int thickness);

// ONE line, origin = baseline-left corner (OpenCV convention). `flip_vertical` = OpenCV's bottomLeftOrigin.
// `legacy_thin_line`: strokes of thickness 1 / LINE_8 use the classical fixed-point DDA (calibration footprints);
// false rasterises everything with cv::polylines. `row_stride` = bytes between rows (0 = cols; a sub-matrix view has more). Pixels are overwritten with `value` (AA strokes blend at the edge).
void draw_line(uint8_t *data, int rows, int cols, const std::string &line, int x, int y, int face, double scale, int value,
               int thickness, LineType line_type, bool flip_vertical = false, bool legacy_thin_line = true, size_t row_stride = 0);

struct TextSpec {
    std::string text;
    int         face = 0;
    double      scale = 1;
    int         thickness = 1;
    bool        mirror = false;        // UVtools passes it as bottomLeftOrigin: the text is flipped vertically
    Alignment   alignment = Alignment::Left;
    double      angle = 0;             // degrees, positive = clockwise, around the origin
    LineType    line_type = LineType::AntiAlias;
    int         line_gap_offset = 0;
};
// "" = valid. UVtools UI limits: non-empty text, scale >= 0.2, thickness >= 1, a known face.
std::string validate_text(const TextSpec &);

// UVtools GetTextSizeExtended: size of the whole (multi-line) block.
Extent block_extent(const TextSpec &);
// UVtools PutTextRotated over an 8-bit image: draws `spec.text` with its origin at (x, y). Returns the number of
// pixels whose value changed. The image is not resized; everything is clipped. Rotation by a multiple of 90 degrees is
// exact; other angles resample bilinearly (+-1 level / sub-pixel differences against OpenCV's rotate).
size_t put_text(uint8_t *data, int rows, int cols, int x, int y, const TextSpec &spec, uint8_t value);

} // namespace Slic3r::sla::hershey_text
