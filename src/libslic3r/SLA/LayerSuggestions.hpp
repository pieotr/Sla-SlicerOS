// SPDX-License-Identifier: AGPL-3.0-or-later
// UVtools "Suggestions" (print parameter advice) as a pure value-type backend.
//
// Port of UVtools.Core/Suggestions: BottomLayerCount, LayerHeight,
// TransitionLayerCount, WaitTimeBeforeCure, WaitTimeAfterCure, ModelPosition.
// Every suggestion only EVALUATES a job description and REPORTS whether it is
// already applied plus the change it proposes (with an explanation). Nothing
// is ever applied here: the caller (CLI, GUI) turns a proposal into edits via
// the document's set_layer_parameters / Undo-Redo machinery.
//
// The input is plain data (no LayerDocument, no file format, no GUI) so it is
// unit-testable with synthetic jobs. Units: mm, seconds, pixels.
#ifndef slic3r_SLA_LayerSuggestions_hpp_
#define slic3r_SLA_LayerSuggestions_hpp_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/SLA/LayerIssues.hpp"      // LayerMask, LayerMaskProvider
#include "libslic3r/SLA/LayerParameters.hpp"  // LayerParameter

namespace Slic3r { namespace sla {

// ------------------------------------------------------------------ input

// What a suggestion needs to know about one layer mask.
struct SuggestionLayerStats
{
    size_t      nonzero_pixels = 0; // pixels with a non-zero value (UVtools NonZeroPixelCount)
    BoundingBox bbox;               // px, inclusive; !defined for an empty layer
};

// Must be thread-safe: layers are requested from TBB workers, each at most once.
using SuggestionLayerStatsProvider = std::function<SuggestionLayerStats(size_t layer)>;

// Computes nonzero-pixel count and bbox of the masks handed out by `masks`.
SuggestionLayerStatsProvider layer_stats_from_masks(LayerMaskProvider masks);

// Description of the job. Per-layer arrays hold one entry per layer (layer
// count = z_mm.size()). An EMPTY wait/exposure array means the target format
// cannot store that value per layer; the matching suggestion is then
// reported as Unavailable instead of inventing data.
struct SuggestionJob
{
    std::vector<double> z_mm;            // top Z of every layer, ascending
    std::vector<double> exposure_s;      // per layer
    std::vector<double> wait_before_s;   // per layer (wait before cure / light-off delay)
    std::vector<double> wait_after_s;    // per layer (wait after cure)

    double layer_height_mm = 0;          // nominal layer height of the file
    size_t bottom_layer_count = 0;
    double bottom_exposure_s = 0;        // global bottom exposure
    double normal_exposure_s = 0;        // global normal exposure
    size_t transition_layer_count = 0;   // the file's own value
    bool   transition_in_firmware = false; // true: count is a printer setting, layers are not rewritten

    // Format capabilities. A suggestion needing one that is false is Unavailable.
    bool can_set_bottom_layer_count = true;
    bool can_set_exposure          = true;
    bool can_prepend_blank_layer   = false; // WaitTimeBeforeCure "empty first layer" trick (Chitubox/CTB/GOO only)

    // Display / raster geometry. pixel size 0 = unknown (area based rules unavailable).
    size_t resolution_x = 0, resolution_y = 0;
    double pixel_width_mm = 0, pixel_height_mm = 0;

    // Mask statistics; required by WaitTimeBeforeCure (all layers) and ModelPosition.
    SuggestionLayerStatsProvider stats;

    size_t layer_count() const { return z_mm.size(); }
};

// ----------------------------------------------------------------- configs

// UVtools SuggestionApplyWhen: "OutsideLimits" tolerates any value inside the
// configured range, "Different" demands exactly the recommended value.
enum class SuggestionApplyWhen { OutsideLimits, Different };

struct BottomLayerCountSuggestionConfig
{
    bool enabled = true;
    SuggestionApplyWhen apply_when = SuggestionApplyWhen::OutsideLimits;
    double  target_bottom_height_mm = 0.25;
    double  minimum_bottom_height_mm = 0.07;
    double  maximum_bottom_height_mm = 0.4;
    uint8_t minimum_bottom_layer_count = 3;
    uint8_t maximum_bottom_layer_count = 7;
};

struct LayerHeightSuggestionConfig
{
    bool enabled = true;
    double  minimum_layer_height_mm = 0.03;
    double  maximum_layer_height_mm = 0.10;
    uint8_t maximum_decimal_digits = 2;
};

struct TransitionLayerCountSuggestionConfig
{
    bool enabled = true;
    SuggestionApplyWhen apply_when = SuggestionApplyWhen::Different;
    double   transition_step_time_s = 2;
    uint16_t minimum_transition_layer_count = 3;
    uint16_t maximum_transition_layer_count = 10;
};

struct WaitTimeBeforeCureSuggestionConfig
{
    enum class SetType { Fixed, ProportionalLayerPixels, ProportionalLayerArea };
    enum class MassFrom { Previous, Average, Maximum };

    bool enabled = true;
    SuggestionApplyWhen apply_when = SuggestionApplyWhen::Different;
    SetType set_type = SetType::ProportionalLayerArea;

    double  bottom_height_mm = 1;                   // layers up to this Z count as bottom (besides the bottom layer count)
    double  fixed_bottom_s = 15, fixed_normal_s = 2;
    uint8_t transition_layer_count = 8;             // Fixed mode: linear ramp from bottom to normal wait
    double  proportional_bottom_s = 30, proportional_normal_s = 10;     // wait for the reference mass below
    uint32_t proportional_bottom_layer_pixels = 5000000, proportional_layer_pixels = 5000000;
    uint32_t proportional_bottom_layer_area_mm2 = 10000, proportional_layer_area_mm2 = 10000;
    double  proportional_bottom_max_difference_s = 1, proportional_normal_max_difference_s = 1; // vs previous layer
    MassFrom mass_from = MassFrom::Previous;
    double  mass_relative_height_mm = 0.2;         // look-back window of Average / Maximum
    double  minimum_bottom_s = 10, minimum_normal_s = 1;
    double  maximum_bottom_s = 30, maximum_normal_s = 4;
    bool    create_empty_first_layer = true;        // only proposed when the job allows it
};

struct WaitTimeAfterCureSuggestionConfig
{
    enum class SetType { Fixed, ProportionalExposure };

    bool enabled = true;
    SuggestionApplyWhen apply_when = SuggestionApplyWhen::Different;
    SetType set_type = SetType::Fixed;
    double fixed_bottom_s = 7, fixed_normal_s = 1;
    double proportional_wait_s = 1, proportional_exposure_s = 3; // wait per reference exposure
    double minimum_bottom_s = 3, minimum_normal_s = 1;
    double maximum_bottom_s = 20, maximum_normal_s = 12;
};

struct ModelPositionSuggestionConfig
{
    // Random (UVtools picks a corner at random) is made deterministic here: the corner nearest to the model.
    enum class Anchor { Random, TopLeft, TopRight, BottomRight, BottomLeft };

    bool enabled = true;
    SuggestionApplyWhen apply_when = SuggestionApplyWhen::OutsideLimits;
    Anchor   anchor = Anchor::Random;
    uint16_t target_top_bottom_margin_px = 100, target_left_right_margin_px = 100;
    uint16_t minimum_top_bottom_margin_px = 50, maximum_top_bottom_margin_px = 300;
    uint16_t minimum_left_right_margin_px = 50, maximum_left_right_margin_px = 300;
};

struct SuggestionConfig
{
    BottomLayerCountSuggestionConfig     bottom_layer_count;
    LayerHeightSuggestionConfig          layer_height;
    TransitionLayerCountSuggestionConfig transition_layer_count;
    WaitTimeBeforeCureSuggestionConfig   wait_time_before_cure;
    WaitTimeAfterCureSuggestionConfig    wait_time_after_cure;
    ModelPositionSuggestionConfig        model_position;
};

// ------------------------------------------------------------------ output

enum class SuggestionStatus
{
    Applied,      // the job already satisfies the suggestion
    NotApplied,   // a change is proposed (or, for informational ones, advised)
    Unavailable   // the job/format lacks what the suggestion needs, or the config is invalid
};

// One proposed edit of one per-layer parameter (set_layer_parameters patch).
struct SuggestedLayerChange
{
    size_t         layer = 0;  // 0-based
    LayerParameter field = LayerParameter::Exposure; // Exposure, WaitBefore or WaitAfter; seconds
    double         before = 0, after = 0;
};

// Whole-model translation in raster pixels (x right, y down).
struct SuggestedModelMove
{
    int         dx_px = 0, dy_px = 0;
    std::string anchor;       // "top_left", "top_right", "bottom_right", "bottom_left"
    BoundingBox before, after; // px, inclusive
};

struct SuggestionResult
{
    std::string id;           // UVtools class suffix, e.g. "BottomLayerCount"
    std::string title;
    std::string description;  // why the suggestion exists
    std::string message;      // "✓ Title: ..." when applied, "⚠ Title ..." otherwise
    std::string confirmation; // "Title: before » after", empty when nothing is proposed
    std::string information_url;
    SuggestionStatus status = SuggestionStatus::Unavailable;
    bool        informational_only = false; // advice only (layer height): nothing to apply
    std::string unavailable_reason;         // set when status == Unavailable

    // Proposed change; every part is optional and absent unless status == NotApplied.
    std::optional<std::pair<size_t, size_t>> bottom_layer_count;     // before, after
    std::optional<std::pair<size_t, size_t>> transition_layer_count; // before, after
    std::vector<SuggestedLayerChange>        layer_changes;
    std::optional<SuggestedModelMove>        model_move;
    bool prepend_blank_first_layer = false; // WaitTimeBeforeCure: insert a near-empty layer first (see config)
};

// Returns the first configuration error of a suggestion ("" when valid).
std::string validate_suggestion_config(const BottomLayerCountSuggestionConfig&);
std::string validate_suggestion_config(const LayerHeightSuggestionConfig&);
std::string validate_suggestion_config(const TransitionLayerCountSuggestionConfig&);
std::string validate_suggestion_config(const WaitTimeBeforeCureSuggestionConfig&);
std::string validate_suggestion_config(const WaitTimeAfterCureSuggestionConfig&);
std::string validate_suggestion_config(const ModelPositionSuggestionConfig&);

// Individual evaluators. `cancel` is polled between layer batches while
// mask statistics are gathered; when it returns true, Slic3r::CanceledException is thrown.
SuggestionResult suggest_bottom_layer_count(const SuggestionJob&, const BottomLayerCountSuggestionConfig& = {});
SuggestionResult suggest_layer_height(const SuggestionJob&, const LayerHeightSuggestionConfig& = {});
SuggestionResult suggest_transition_layer_count(const SuggestionJob&, const TransitionLayerCountSuggestionConfig& = {});
SuggestionResult suggest_wait_time_before_cure(const SuggestionJob&, const WaitTimeBeforeCureSuggestionConfig& = {},
                                               const std::function<bool()>& cancel = {});
SuggestionResult suggest_wait_time_after_cure(const SuggestionJob&, const WaitTimeAfterCureSuggestionConfig& = {});
SuggestionResult suggest_model_position(const SuggestionJob&, const ModelPositionSuggestionConfig& = {},
                                        const std::function<bool()>& cancel = {});

// Runs every enabled suggestion, in the order above. Mask statistics are
// gathered at most once even when several suggestions need them.
std::vector<SuggestionResult> evaluate_suggestions(const SuggestionJob&, const SuggestionConfig& = {},
                                                   const std::function<bool()>& cancel = {});

}} // namespace Slic3r::sla

#endif
