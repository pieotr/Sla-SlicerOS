// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Native port of the UVtools "OperationLayerExport*" family (goal G6, batch 2). Everything is plain C++:
// the GIF89a encoder (LZW) and the binary STL writer are written here, PNG goes through the project's
// libpng encoder (encode_png_raster). Nothing here changes the layers; results are returned as bytes /
// masks (pure functions) and the export_* helpers write files.
//
// Operations matrix (UVtools op -> our function -> notes / deviations)
// ---------------------------------------------------------------------------------------------
// OperationLayerExportImage    -> render_layer_images / export_layer_images
//                                 PNG and PGM (8-bit binary) only; JPG/TIF/BMP/SVG are not ported. Optional ROI crop,
//                                 flip then rotate (physical orientation), padded file index like UVtools
//                                 ("<prefix><index padded to the digit count of the layer count>.png").
// OperationLayerExportGif      -> encode_gif / render_layer_gif / export_layer_gif
//                                 Native GIF89a: 256 grey global palette, LZW, per-frame GCE (delay in 1/100 s,
//                                 disposal "restore to background"), NETSCAPE2.0 loop count. Frame picking, scale
//                                 (bilinear), clip to model bounds, skip, fps like UVtools. The layer-number text
//                                 overlay (render_layer_count) is NOT ported.
// OperationLayerExportHeatMap  -> render_heat_map / export_heat_map
//                                 sum of the layers divided by the layer count, 8-bit, same rounding as OpenCV.
//                                 "merge same positioned layers" is supported (needs the Z list). The ROI is cropped
//                                 BEFORE rotate / flip (UVtools applies the ROI to the rotated image, a bug).
// OperationLayerExportSkeleton -> render_skeleton / export_skeleton
//                                 morphological skeleton (3x3 rectangle) of every layer, summed with saturation.
// OperationLayerExportHtml     -> render_html / export_html
//                                 One self-contained file: inline CSS + JS, PNG previews as data: URIs. UVtools links
//                                 Bootstrap from a CDN; this does not use any external resource.
// OperationLayerExportMesh     -> build_voxel_mesh / encode_binary_stl / export_mesh
//                                 Voxel surface (exposed faces, greedy rectangle merge per plane) as binary STL, not
//                                 UVtools' KD-tree collapse (different triangulation, same surface). Quality 1/2/3/6/8
//                                 = box downsample. Watertight (every edge shared by exactly two triangles) for
//                                 shapes without T-junctions, always for a box; set merge_faces=false for a strictly
//                                 manifold surface of any shape. STL only (no OBJ/PLY/3MF).
//
// File writing is staged: every file is first written to "<name>.part" and renamed only when the whole export
// finished, so a cancel (progress callback returns false -> LayerOperationCancelled) leaves the folder untouched.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "LayerOperations.hpp"

namespace Slic3r::sla::layerexport {

using layerops::FlipDirection;
using layerops::LayerParams;
using layerops::LayerSelection;
using layerops::Mask;
using layerops::StreamLayout;
using layerops::Rect;
using layerops::RotateDirection;

// Physical orientation of an exported picture: UVtools flips first and rotates afterwards. Use the printer's
// display mirror for `flip` to get what the LCD shows.
struct Orientation {
    std::optional<FlipDirection> flip;
    RotateDirection              rotate = RotateDirection::None;
};

// Every renderer/exporter below has a second form taking a layerops::StreamLayout: its `fetch` decodes one layer on
// demand (called concurrently), its `scan` visits all layers, so a document is exported layer by layer and never
// decoded as a whole (peak memory = a few layers + the result). The vector forms run the same code over a batch layout.
// ---- PNG helpers ------------------------------------------------------------------------------------
// 8-bit grey PNG; empty vector on failure. `cancel` may throw (e.g. LayerOperationCancelled).
std::vector<uint8_t> encode_png(const Mask &, const std::function<void()> &cancel = {});

// ---- Layer images -----------------------------------------------------------------------------------
enum class ImageType { Png, Pgm };
struct ExportImageParams {
    LayerSelection selection;           // first/last layer + optional ROI
    std::string    filename_prefix = "layer";
    ImageType      type = ImageType::Png;
    Orientation    orientation;
    bool           pad_layer_index = true;
    bool           crop_by_roi = true;  // false: the whole layer is exported even when a ROI is set
};
struct ExportedFile { std::string name; std::vector<uint8_t> data; };
// "layer0007.png" style name. `layer_count` decides the padding width (digits of the count).
std::string layer_image_filename(const ExportImageParams &, size_t layer_index, size_t layer_count);
std::vector<ExportedFile> render_layer_images(const std::vector<Mask> &layers, const ExportImageParams &, const LayerProgress &progress = {});
std::vector<ExportedFile> render_layer_images(const StreamLayout &layers, const ExportImageParams &, const LayerProgress &progress = {});
// Writes into `folder` (created when missing). Returns the number of files.
size_t export_layer_images(const std::vector<Mask> &layers, const ExportImageParams &, const std::string &folder, const LayerProgress &progress = {});
size_t export_layer_images(const StreamLayout &layers, const ExportImageParams &, const std::string &folder, const LayerProgress &progress = {});

// ---- GIF --------------------------------------------------------------------------------------------
// All frames must have the same size. delay_cs = frame delay in 1/100 s, repeats = loop count (0 = forever).
std::vector<uint8_t> encode_gif(const std::vector<Mask> &frames, uint16_t delay_cs, uint16_t repeats);
struct ExportGifParams {
    LayerSelection selection;
    bool        clip_by_volume_bounds = false; // crop to the model bounds instead of the ROI
    unsigned    fps = 30;                      // 1..100 (delay is (1000 / fps) / 10 centiseconds, as UVtools)
    unsigned    repeats = 0;
    unsigned    skip = 0;                      // layers skipped between frames
    double      scale_percent = 50;
    Orientation orientation;
};
// Number of frames: floor(range / (skip + 1)).
size_t      gif_frame_count(const ExportGifParams &, size_t layer_count);
std::string validate_export_gif(const ExportGifParams &, size_t layer_count);
std::vector<uint8_t> render_layer_gif(const std::vector<Mask> &layers, const ExportGifParams &, const LayerProgress &progress = {});
std::vector<uint8_t> render_layer_gif(const StreamLayout &layers, const ExportGifParams &, const LayerProgress &progress = {});
void export_layer_gif(const std::vector<Mask> &layers, const ExportGifParams &, const std::string &file, const LayerProgress &progress = {});
void export_layer_gif(const StreamLayout &layers, const ExportGifParams &, const std::string &file, const LayerProgress &progress = {});

// ---- Heat map ---------------------------------------------------------------------------------------
struct ExportHeatMapParams {
    LayerSelection selection;
    Orientation    orientation;
    bool           merge_same_positioned_layers = true; // layers with equal Z count once (max of them)
    bool           crop_by_roi = true;
};
std::string validate_export_heat_map(const ExportHeatMapParams &, size_t layer_count);
// `z_mm` may be empty when merge_same_positioned_layers is false.
Mask render_heat_map(const std::vector<Mask> &layers, const std::vector<double> &z_mm, const ExportHeatMapParams &, const LayerProgress &progress = {});
Mask render_heat_map(const StreamLayout &layers, const std::vector<double> &z_mm, const ExportHeatMapParams &, const LayerProgress &progress = {});
void export_heat_map(const std::vector<Mask> &layers, const std::vector<double> &z_mm, const ExportHeatMapParams &, const std::string &file,
                     const LayerProgress &progress = {});
void export_heat_map(const StreamLayout &layers, const std::vector<double> &z_mm, const ExportHeatMapParams &, const std::string &file,
                     const LayerProgress &progress = {});

// ---- Skeleton ---------------------------------------------------------------------------------------
struct ExportSkeletonParams {
    LayerSelection selection;
    bool           crop_by_roi = true;
};
// Lantuejoul skeleton of one mask with a 3x3 rectangle element (UVtools EmguExtensions.Skeletonize).
Mask skeletonize(const Mask &, const std::function<void()> &cancel = {});
Mask render_skeleton(const std::vector<Mask> &layers, const ExportSkeletonParams &, const LayerProgress &progress = {});
Mask render_skeleton(const StreamLayout &layers, const ExportSkeletonParams &, const LayerProgress &progress = {});
void export_skeleton(const std::vector<Mask> &layers, const ExportSkeletonParams &, const std::string &file, const LayerProgress &progress = {});
void export_skeleton(const StreamLayout &layers, const ExportSkeletonParams &, const std::string &file, const LayerProgress &progress = {});

// ---- HTML -------------------------------------------------------------------------------------------
struct ExportHtmlParams {
    std::string title = "Layer export";
    std::vector<std::pair<std::string, std::string>> properties;       // file information table
    std::vector<std::pair<std::string, std::vector<uint8_t>>> thumbnails_png; // name + already encoded PNG
    bool export_properties = true;
    bool export_layer_settings = true;     // per layer table (needs `params` and `z_mm`)
    std::string gcode;                     // exported when non empty and export_gcode
    bool export_gcode = true;
    bool export_layer_preview = true;      // PNG data URIs + slider
    size_t preview_first = 0, preview_last = layerops::kLastLayer, preview_step = 1;
    double preview_scale_percent = 25;
    Orientation orientation;
};
std::string render_html(const std::vector<Mask> &layers, const std::vector<double> &z_mm, const std::vector<LayerParams> &params,
                        const ExportHtmlParams &, const LayerProgress &progress = {});
std::string render_html(const StreamLayout &layers, const std::vector<double> &z_mm, const std::vector<LayerParams> &params,
                        const ExportHtmlParams &, const LayerProgress &progress = {});
void export_html(const std::vector<Mask> &layers, const std::vector<double> &z_mm, const std::vector<LayerParams> &params,
                 const ExportHtmlParams &, const std::string &file, const LayerProgress &progress = {});
void export_html(const StreamLayout &layers, const std::vector<double> &z_mm, const std::vector<LayerParams> &params,
                 const ExportHtmlParams &, const std::string &file, const LayerProgress &progress = {});
std::string html_escape(const std::string &);
std::string base64_encode(const std::vector<uint8_t> &);

// ---- Mesh -------------------------------------------------------------------------------------------
struct ExportMeshParams {
    LayerSelection selection;              // layer range; the ROI is ignored (the model bounds are used)
    double      pixel_width_mm = 0.035, pixel_height_mm = 0.035; // 0 -> 0.035 like UVtools
    unsigned    quality = 1;               // 1 accurate, 2 average, 3 quick, 6 dirty, 8 minecraft (box downsample factor)
    Orientation orientation;               // applied to every layer before voxelisation
    bool        merge_faces = true;        // greedy rectangle merge of coplanar faces
};
struct Triangle { float v[3][3]; float n[3]; };
struct VoxelMesh {
    std::vector<Triangle> triangles;
    size_t voxels = 0;                     // solid voxels
};
std::string validate_export_mesh(const ExportMeshParams &, size_t layer_count);
// x right, y up (image y is flipped), z = layer Z in mm; origin at the model bounds corner / the Z of the layer below the range.
VoxelMesh build_voxel_mesh(const std::vector<Mask> &layers, const std::vector<double> &z_mm, const ExportMeshParams &, const LayerProgress &progress = {});
// Streaming form: layers are decoded one slab at a time (twice); what stays in memory is the occupancy grid of the model
// bounds (bytes = voxels, i.e. about the size of the cropped model), which is the size of the mesh's input.
VoxelMesh build_voxel_mesh(const StreamLayout &layers, const std::vector<double> &z_mm, const ExportMeshParams &, const LayerProgress &progress = {});
std::vector<uint8_t> encode_binary_stl(const VoxelMesh &, const std::string &header = "SLA-Slicer layer mesh");
void export_mesh(const std::vector<Mask> &layers, const std::vector<double> &z_mm, const ExportMeshParams &, const std::string &file,
                 const LayerProgress &progress = {});
void export_mesh(const StreamLayout &layers, const std::vector<double> &z_mm, const ExportMeshParams &, const std::string &file,
                 const LayerProgress &progress = {});

} // namespace Slic3r::sla::layerexport
