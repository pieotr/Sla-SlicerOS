// SPDX-License-Identifier: AGPL-3.0-or-later
// Native port of UVtools OperationRedrawModel (U/Operations/OperationRedrawModel.cs): redraws the supports (or the model)
// of the document with a set brightness, using a SECOND file of the same object sliced without supports and raft.
// Our LayerDocument has no model / support layer pair, so the second file is a document the caller provides.
// Differences from UVtools: the empty-layer test uses the layer that is really processed (UVtools indexes the full file with
// the body layer number, a bug when the file has a raft); the ROI is validated instead of silently clipped.
// Quirks of UVtools that are kept: outside the support / model mask the operators Multiply, Divide and Minimum use the
// pattern value 0 (they clear those pixels) and Set copies the pattern where the layer is lit (clears the rest).
// OpenCV (findContours / drawContours) is private to LayerRedrawModel.cpp.
#pragma once

#include "LayerOperations.hpp"

namespace Slic3r::sla::layerops {

enum class RedrawType { Supports, Model };
enum class RedrawOperator { Set, Add, Subtract, Multiply, Divide, Minimum, Maximum, AbsDiff };
struct RedrawModelParams {
    Rect           roi;                       // empty = whole layer
    RedrawType     type = RedrawType::Supports;
    RedrawOperator op = RedrawOperator::Minimum;
    uint8_t        brightness = 220;
    bool           contact_points_only = true;      // Supports only
    bool           ignore_contactless_pixels = true;
};
std::string validate_redraw_model(const RedrawModelParams &);
// One layer: `full` = layer of the document, `body` = the same layer of the model-only file, `body_next` = the next layer of the
// model-only file (nullptr for the last one). Returns true when the layer was modified. Sizes must match.
bool redraw_model_layer(Mask &full, const Mask &body, const Mask *body_next, const RedrawModelParams &);
// Body layer j aligns with full layer (full_count - body_count + j). `body_at(j)` returns body layer j (thread safe).
using RedrawBodyFetch = std::function<std::shared_ptr<const Mask>(size_t body_layer)>;
std::string  validate_redraw_model_files(const RedrawModelParams &, size_t full_count, size_t width, size_t height, size_t body_count, size_t body_width, size_t body_height);
StreamKernel redraw_model_kernel(const RedrawModelParams &, const StreamLayout &, size_t body_count, const RedrawBodyFetch &body_at);
void         redraw_model_layers(std::vector<Mask> &full, const std::vector<Mask> &body, const RedrawModelParams &, const LayerProgress &progress = {});

} // namespace Slic3r::sla::layerops
