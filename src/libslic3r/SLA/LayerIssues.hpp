// SPDX-License-Identifier: AGPL-3.0-or-later
// Native layer issue detection (islands, overhangs, resin traps, suction cups,
// touching bounds, print height, empty layers). Behaviour is a port of the
// UVtools IssueManager.DetectIssues pipeline, without OpenCV and without any
// dependency on file formats or the GUI.
#ifndef slic3r_SLA_LayerIssues_hpp_
#define slic3r_SLA_LayerIssues_hpp_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_set>
#include <vector>

#include "libslic3r/BoundingBox.hpp"

namespace Slic3r { namespace sla {

// One 8-bit grayscale layer, row-major, width * height bytes. An empty `data`
// vector is a blank (all black) layer of any size.
struct LayerMask
{
    size_t               width  = 0;
    size_t               height = 0;
    std::vector<uint8_t> data;
};

// Returns layer `index`. Must be thread-safe: it is called concurrently from
// TBB workers (each layer is requested a small, bounded number of times).
using LayerMaskProvider = std::function<LayerMask(size_t index)>;

struct IslandDetectionConfig
{
    bool enabled = true;
    // Layers to check; nullopt checks every layer (UVtools WhiteListLayers).
    std::optional<std::unordered_set<size_t>> white_list_layers;
    // Discard islands that do not also show an overhang (UVtools EnhancedDetection).
    bool    enhanced_detection = true;
    bool    allow_diagonal_bonds = false;       // 8-connectivity when true
    uint8_t binary_threshold = 1;               // pixels > value are white; 0 = any non-zero
    uint16_t required_area_to_process_check_px = 1;
    uint8_t required_pixel_brightness_to_process_check = 10;
    uint8_t required_pixels_to_support = 10;    // kept for parity; UVtools does not use it
    double  required_pixels_to_support_multiplier = 0.25;
    uint8_t required_pixel_brightness_to_support = 150;
};

struct OverhangDetectionConfig
{
    bool enabled = true;
    std::optional<std::unordered_set<size_t>> white_list_layers;
    bool    independent_from_islands = true;    // kept for parity; UVtools does not use it
    uint8_t required_pixels_to_consider = 1;
    // (current - previous) is eroded with a cross of radius `erode_iterations` px.
    uint8_t erode_iterations = 40;
};

struct ResinTrapDetectionConfig
{
    bool     enabled = true;
    size_t   start_layer_index = 0;             // first layer of the model (raft bypass)
    uint8_t  binary_threshold = 100;            // contour extraction; 0 = any non-zero
    uint8_t  required_area_to_process_check_px = 4;
    uint8_t  required_black_pixels_to_drain = 10;
    uint8_t  maximum_pixel_brightness_to_drain = 30; // 0 = any non-zero is solid
    bool     detect_suction_cups = true;
    uint32_t required_area_to_consider_suction_cup_px = 100;
    double   required_height_to_consider_suction_cup_mm = 0.5;
};

struct TouchingBoundDetectionConfig
{
    bool    enabled = true;
    uint8_t minimum_pixel_brightness = 127;
    uint8_t margin_left_px = 5, margin_top_px = 5, margin_right_px = 5, margin_bottom_px = 5;
};

struct PrintHeightDetectionConfig
{
    bool   enabled = true;
    double machine_height_mm = 0.;              // 0 disables the check (unknown printer)
    double offset_mm = 0.;
};

struct EmptyLayerDetectionConfig
{
    bool enabled = true;
    bool ignore_starting = false, ignore_loose = false, ignore_ending = false;
};

struct IssueDetectionConfig
{
    IslandDetectionConfig        island;
    OverhangDetectionConfig      overhang;
    ResinTrapDetectionConfig     resin_trap;
    TouchingBoundDetectionConfig touching_bound;
    PrintHeightDetectionConfig   print_height;
    EmptyLayerDetectionConfig    empty_layer;
};

// Horizontal run of pixels [x0, x1) on row y.
struct PixelRun
{
    int32_t y, x0, x1;
};

// The part of an issue on a single layer.
struct LayerIssuePart
{
    size_t              layer = 0;
    BoundingBox         bbox;                   // px, inclusive; undefined for print-height / empty layer
    size_t              pixel_count = 0;
    std::vector<PixelRun> runs;                 // raster-ordered (y, x0)
};

struct LayerIssue
{
    enum class Type { Island, Overhang, ResinTrap, SuctionCup, TouchingBound, PrintHeight, EmptyLayer };

    Type        type = Type::Island;
    size_t      first_layer = 0, last_layer = 0;
    BoundingBox bbox;                           // union of the parts (px)
    size_t      pixel_count = 0;                // sum of the parts
    std::vector<LayerIssuePart> parts;          // ascending layer
};

// Runs the enabled detections. `layer_z_mm` holds the Z of every layer (its
// size is the layer count). Throws Slic3r::CanceledException when `cancel`
// returns true (checked between layers and passes; may be called from worker
// threads so it must be thread-safe). `progress(done,total)` is called from
// the calling thread only. Result order is deterministic: type, first layer,
// pixel_count descending, bbox y, bbox x. Hollow areas use pixel counts where
// UVtools uses contour polygon areas.
std::vector<LayerIssue> detect_layer_issues(const LayerMaskProvider&                   layers,
                                            const std::vector<double>&                 layer_z_mm,
                                            const IssueDetectionConfig&                config,
                                            const std::function<void(size_t, size_t)>& progress = {},
                                            const std::function<bool()>&               cancel   = {});

}} // namespace Slic3r::sla

#endif
