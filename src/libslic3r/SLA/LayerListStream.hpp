// SPDX-License-Identifier: AGPL-3.0-or-later
// Layer-list operations (Clone, Remove, ReHeight, Double / Phased exposure, Change resolution, Timelapse, Raise on
// print finish, Lithophane) as streamed operations for LayerDocument::apply_layer_list_stream /
// apply_parameter_list_stream: they reference the payloads of existing layers (nothing decoded) or decode and
// re-encode one layer (or one merge group) at a time, so no whole-document decode is needed. Each one computes the
// same result as its layerops batch function; the list/Z/parameter logic is shared with it (layerops::*_plan).
#ifndef slic3r_SLA_LayerListStream_hpp_
#define slic3r_SLA_LayerListStream_hpp_

#include "LayerDocument.hpp"
#include "LayerOperations.hpp"

namespace Slic3r::sla::liststream {

using Operation = LayerDocument::ListStreamOperation;

// Layer-list (non-parameter) operations: apply with LayerDocument::apply_layer_list_stream.
Operation clone(const layerops::CloneParams &);
Operation remove(const layerops::RemoveParams &);
Operation reheight(const layerops::ReHeightParams &);
Operation double_exposure(const layerops::DoubleExposureParams &);
// changes_resolution = true when applying.
Operation change_resolution(const layerops::ChangeResolutionParams &);

// Repair layers (islands, resin traps, suction cups, gap closing, empty layers). Without remove_empty_layers the result is
// pixels only: apply with LayerDocument::apply_pixel_list_stream (any document); with it, with apply_layer_list_stream.
Operation repair_layers(const layerops::RepairLayersParams &);

// Parameter-list operations: apply with LayerDocument::apply_parameter_list_stream.
Operation phased_exposure(const layerops::PhasedExposureParams &, const layerops::PrinterContext &);
Operation timelapse(const layerops::TimelapseParams &, const layerops::PrinterContext &);
Operation raise_on_print_finish(const layerops::RaiseOnPrintFinishParams &, const layerops::PrinterContext &);
// Stir resin: StirOnly replaces the document by the stir layers (the caller sets the global bottom layer count to 1 through
// the result's change), StirAndPrint puts them in front of the layers (a previous stir block is removed first).
// `base_row` = the first normal layer row of the document. Apply with LayerDocument::apply_parameter_list_stream.
Operation stir_resin(const layerops::StirResinParams &, const layerops::PrinterContext &, const layerops::LayerParams &base_row);
// `p` already resolved (defaults filled in); the plan reads Z and parameters from the context.
Operation dynamic_layer_height(const layerops::DynamicLayerHeightParams &, const layerops::PrinterContext &);
// `bottom_row` / `normal_row`: the parameters of the document's first bottom / normal layer (see the recipe).
Operation lithophane(const layerops::Mask &image, size_t width, size_t height, const layerops::LithophaneParams &,
                     const layerops::PrinterContext &, const layerops::LayerParams &bottom_row, const layerops::LayerParams &normal_row);

// Dynamic lifts: parameter rows only (every document that stores them). Counts the lit pixels of the layers with a
// scan and feeds the same lift logic as layerops::dynamic_lifts; false = cancelled (document untouched).
bool apply_dynamic_lifts(LayerDocument &, const layerops::DynamicLiftsParams &, double fallback_layer_height_mm,
                         const LayerProgress &progress = {});

} // namespace Slic3r::sla::liststream

#endif
