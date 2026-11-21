# Automation without the GUI: layer recipes

A **layer recipe** is a JSON file that chains the native layer operations (the ports of the UVtools
operations), issue detection, suggestions, exports and a save/convert, and runs them from the command line:

```sh
sla-slicer --sla-run-recipe recipe.json [--input file] [--output file] [--dry-run] [--report report.json] [--sla-accept-adjustments]
```

Source: `src/libslic3r/SLA/LayerRecipe.{hpp,cpp}` (format, parser, runner), `src/CLI/ProcessActions.cpp`
(`process_sla_run_recipe`), tests `tests/sla_print/sla_layer_recipe_tests.cpp` (`[sla_layer_recipe]`, part of
the `sla_layer_document` ctest entry).

## What this is, and what it is not

Recipes replace UVtools' **C# scripting** with a declarative, native mechanism. **Arbitrary C# scripts (or any
other code) are not supported and cannot be loaded.** A recipe is data: a fixed catalogue of operations with
validated parameters. It cannot read other files than the ones it names, run programs or loop; anything that
needs logic belongs in the shell that calls `sla-slicer`. The advantage is safety (a recipe can be checked
completely before it touches a file, `--dry-run`) and that no .NET runtime is involved.

## File format (version 1)

```json
{
  "version": 1,
  "description": "optional free text",
  "steps": [
    { "op": "open", "path": "part.ctb" },
    { "op": "flip", "direction": "horizontal", "first_layer": 0, "last_layer": 99 },
    { "op": "threshold", "threshold": 127 },
    { "op": "edit_parameters", "values": { "exposure_s": 2.5 }, "bottom": { "exposure_s": 25 } },
    { "op": "detect_issues", "output": "issues.json" },
    { "op": "export_images", "folder": "preview", "filename_prefix": "layer" },
    { "op": "convert", "format": "goo", "path": "part.goo", "printer_config": "printer.ini" }
  ]
}
```

* Top level keys: `version` (required, must be 1), `description`, `steps` (non-empty list). Unknown keys are errors.
* Every step is an object with `op` (required) and an optional `name` (a label shown in reports); all other
  keys are parameters of the operation. **Unknown or misspelt parameters are errors** (the message lists the
  valid ones), as are wrong types and out-of-range values.
* Parameter names are the UVtools property names in `snake_case`. Layer numbers are **0-based and inclusive**;
  an omitted `last_layer` means "to the last layer". `roi` is `{"x":..,"y":..,"width":..,"height":..}` in pixels.
* Relative paths in a recipe are relative to the recipe file's folder. `--input` and `--output` are relative to the
  current folder.
* `open` may only be the first step; without it the recipe needs `--input` (generic recipes). A step list is
  executed strictly in order.

### Operations

| op | parameters (defaults) |
|---|---|
| `open` | `path` (may be empty when `--input` is given). Opens any readable archive or `.sla-project` |
| `flip` | `direction` horizontal\|vertical\|both (horizontal), `make_copy` (false) |
| `rotate` | `angle_degrees` (90, -360..360) |
| `resize` | `x_percent`, `y_percent` (100), `constrain_xy`, `is_fade` |
| `move` | `anchor` top_left..bottom_right\|none (middle_center), `margin_left/top/right/bottom`, `is_cut_move` (true), `roi` = source |
| `threshold` | `threshold` (127), `maximum` (255), `type` binary\|binary_inv\|trunc\|to_zero\|to_zero_inv\|otsu\|triangle |
| `blur` | `algorithm` stack_blur\|box_blur\|pyramid\|median_blur\|gaussian_blur\|filter_2d (stack_blur), `size` (1; odd for stack / gaussian / median), `kernel` (filter_2d: raw 8-bit values, rows separated by `\n`, default `"1 1 1\n1 1 1\n1 1 1"`; **not normalised**, the default saturates to 255 like UVtools), `anchor_x`, `anchor_y` (-1 = centre), range, `roi` (the whole layer is filtered, only the ROI is replaced, like OpenCV on a sub-matrix) |
| `mask_image` | `image` (required; **PNG**, path relative to the recipe), `invert`, `resize_to_layer` (false: bilinear fit of the picture to the layer / ROI), range, `roi`. Output = `round(layer * mask / 255)`. A mask of another size than the layer (or ROI) is an **error** (UVtools skips it silently) |
| `layer_import` | `files` (required; PNG pictures become ONE source in the listed order, other paths are opened as printer files; BMP / JPEG / GIF are refused), `import_type` insert\|replace\|stack\|merge_sum\|merge_max\|subtract\|abs_diff\|bitwise_and\|bitwise_or\|bitwise_xor (stack), `start_layer` (0), `extend_beyond_layer_count` (true; replace / stack / merge types), `discard_unmodified_layers` (false), `stack_margin` (50). Sources that do not fit are skipped and reported; nothing imported = error. `insert`, extending and discarding change the layer list (sliced / generated documents); everything else only the pixels (every document) |
| `stir_resin` | `method` stir_only\|stir_and_print (stir_only), `stirs` (10), `output_dummy_pixel` (true), `exposure_s` (0 = 0.01), `lift_height_mm` (45), `lift_speed` (0 = machine maximum), `retract_speed` (0 = lift speed), `wait_time_before_stir_s`, `wait_time_after_lift_s` (0). Stir layers are blank (or one dim pixel) at Z 0.001 mm. `stir_only` replaces the print, `stir_and_print` puts the stirs in front and removes a previous stir block. Sliced / generated documents only |
| `redraw_model` | `file` (required: the same object sliced WITHOUT supports and raft, any printer file), `redraw_type` supports\|model (supports), `operator` set\|add\|subtract\|multiply\|divide\|minimum\|maximum\|abs_diff (minimum), `brightness` (220), `contact_points_only` (true), `ignore_contact_less_pixels` (true), `roi`. The model-only file may have fewer layers (the raft): it is aligned at the END. Quirks of UVtools are kept (outside the support mask Multiply / Divide / Minimum clear the pixels, Set clears lit pixels outside it) |
| `draw_text` | `text` (required), `x`, `y` (required, baseline-left origin), `font` simplex\|plain\|duplex\|complex\|triplex\|complex_small\|script_simplex\|script_complex, `italic`, `font_scale` (1, >= 0.2), `thickness` (1), `brightness` (255; 0 erases), `mirror` (vertical flip), `alignment` none\|left\|center\|right, `angle` (0, clockwise), `line_type` four_connected\|eight_connected\|anti_alias, `line_gap_offset`, `first_layer`, `last_layer` |
| `draw_support`, `drill_drain_hole` | `layer` (the clicked layer, required: the column is dug BELOW it, the layer itself is not changed), `x`, `y`; support: `tip_diameter` (19), `pillar_diameter` (32), `base_diameter` (60), `brightness` (255); drain: `diameter` (50); `anti_alias` (true) |
| `solidify` | `minimum_area` (1), `area_check` more\|less |
| `pattern` | `anchor` (none), `cols`, `rows`, `col_spacing`, `row_spacing`, `roi` |
| `pixel_dimming` | `lightening_pixels`, `wall_thickness_start/end` (10), `walls_only`, `chamfer`, `pattern`, `alternate_pattern`, `alternate_pattern_per_layers` |
| `pixel_arithmetic` | `operator` (set, add, subtract, ..., keep_region, discard_region), `apply` (model, all, model_surface, ...), `value`, `value_step`, `wall_thickness_start/end`, `wall_chamfer`, `threshold_type`, `threshold_max` |
| `light_bleed` | `subject`, `lookup`, `dim_by` ("25,15,10,5") |
| `clone` | `first_layer`, `last_layer`, `clones` (1), `keep_same_z` |
| `remove` | `first_layer`, `last_layer`, `use_threshold`, `pixel_threshold` |
| `edit_parameters` | `first_layer`, `last_layer`, `per_layer_override` (false), `set_layers`, `skip_layers`, `values`, `bottom` (objects with `exposure_s`, `wait_before_cure_s`, `wait_after_cure_s`, `wait_after_lift_s`, `lift_height_mm`, `lift_speed`, `lift_height2_mm`, `lift_speed2`, `retract_speed`, `retract_height2_mm`, `retract_speed2`, `light_pwm`). Global mode: `values` go to normal layers, `bottom` to bottom layers; per-layer mode: `values` go to the selected layers |
| `repair_layers` | `first_layer`, `last_layer`, `repair_islands` (true), `repair_resin_traps` (true), `repair_suction_cups` (false), `remove_empty_layers` (**false**, UVtools: true), `remove_islands_below_equal_pixels` (5), `remove_islands_recursive_iterations` (4; 0 = until done, 1 = single pass), `attach_islands_below_layers` (2), `resin_traps_overlap_by` (5), `suction_cups_vent_hole` (16), `gap_closing_iterations` (1), `noise_removal_iterations` (0). At least one repair must be on. Without `remove_empty_layers` it is a pixel operation that works on every document; with it the layer list changes |
| `reheight` | `method` reheight\|offset_position_z (reheight), `layer_height_mm` (the new height; required for `reheight`, must be an exact split/merge of the current layer count and height, otherwise the error lists the possible heights), `anti_aliasing` none\|difference\|average, `position_z_offset_mm` (0.01, for `offset_position_z`), `first_layer`, `last_layer` (range of `offset_position_z`) |
| `double_exposure` | `first_layer`, `last_layer`, `bottom_layer_count`, `first_bottom_exposure_s`, `first_normal_exposure_s`, `second_bottom_exposure_s`, `second_normal_exposure_s` (omitted: bottom count and exposures of the document; the second pass defaults to the first), `first_bottom_erode` (4), `second_bottom_erode` (0), `first_normal_erode` (1), `second_normal_erode` (0), `second_layer_difference` (true), `second_layer_difference_overlap_erode` (10), `different_settings_for_second_layer` (true), `second_layer_lift_enabled` (true), `second_layer_lift_mm` (0.1), `second_layer_wait_enabled` (true), `second_layer_wait_s` (0) |
| `phased_exposure` | `first_layer`, `last_layer`, `phases` (2 or 3 objects `{bottom_exposure_s, bottom_iterations (0), exposure_s, iterations (0)}`; both exposures required), `difference_only` (true), `difference_overlap_iterations` (10), `different_settings_for_sequential_layers` (true), `sequential_lift_enabled` (true), `sequential_lift_mm`, `sequential_wait_before_cure_enabled` (true), `sequential_wait_before_cure_s`. Omitted values take the layer editor defaults: `{first bottom exposure, 10 px, first normal exposure, 4 px}` and `{normal, 0, normal, 0}`, lift 0.1 mm, wait 0 s |
| `dynamic_lifts` | `first_layer`, `last_layer`, `method` traditional\|full_range, `smallest_bottom_lift_height`, `largest_bottom_lift_height`, `smallest_lift_height`, `largest_lift_height`, `slowest_bottom_lift_speed`, `fastest_bottom_lift_speed`, `slowest_lift_speed`, `fastest_lift_speed` (0 / omitted = value of the first bottom / normal layer) |
| `fade_exposure` | `first_layer` (0), `layer_count` (10), `from_exposure_s`, `to_exposure_s` (omitted: first bottom / first normal exposure), `disable_firmware_transition_layers` (true; only affects a value the document does not take, see the limits) |
| `timelapse` | `first_layer`, `last_layer`, `mode` lift_height\|virtual_layer, `raise_position_z_mm` (0 = machine Z, which a document does not know: required in practice), `output_dummy_pixel` (true), `raise_each_nth_height_mm` (1), `wait_after_lift_s` (1), `exposure_s` (0 = 0.05), `ensure_last_layer` (true), `use_custom_lift` (false), `slow_lift_height_mm` (3), `lift_speed`, `lift_speed2`, `slow_retract_height_mm` (3), `retract_speed`, `retract_speed2` (speeds: first normal layer) |
| `dynamic_layer_height` | `first_layer`, `last_layer`, `minimum_layer_height_mm`, `maximum_layer_height_mm` (defaults of the editor form: at least the file layer height, 0.1 mm), `strip_anti_aliasing`, `reconstruct_anti_aliasing`, `maximum_erodes` (10), `exposure_type` linear\|multiplier\|manual (linear), `iterate_bottom_exposure` (false), `bottom_exposure_s`, `exposure_s` (omitted: first bottom / normal layer), `bottom_exposure_step_s` (0.5), `exposure_step_s` (0.2), `manual_table` (list of `{layer_height_mm, bottom_exposure_s, exposure_s}`, required for `manual`) |
| `raise_on_print_finish` | `position_z_mm` (0 = machine Z, unknown to a document: give it), `wait_time_s` (1200), `output_dummy_pixel` (true) |
| `change_resolution` | `new_width`, `new_height` (both required), `fix_ratio` (false), `ratio_x`, `ratio_y` (1). Not for sliced / generated documents |
| `infill` | `first_layer`, `last_layer`, `roi`, `type` pillars\|concentric\|waves\|cubic_cross\|cubic_cross_alternating\|cubic_star\|honeycomb\|gyroid (cubic_cross_alternating), `floor_ceil_thickness_mm` (3), `wall_thickness` (64 px), `infill_thickness` (45 px), `infill_spacing` (300 px), `infill_brightness` (255), `reinforce_infill` (false) |
| `raft_relief` | `roi`, `type` relief\|dimming\|decimate\|tabs\|linked_lines (relief), `mask_layer_index` (0 = detect the first layer with supports), `ignore_first_layers`, `low_brightness` (0), `dilate_iterations` (15), `wall_margin` (40), `hole_diameter` (80), `hole_spacing` (40), `high_brightness` (255), `tab_triangle_base` (200), `tab_triangle_height` (250), `linked_line_thickness` (26; below 4 is raised to 4 like UVtools), `linked_minimum_links` (4), `linked_external_supports` (true); the link colour of `linked_lines` is `high_brightness`. No raft found = error, nothing changes |
| `layer_arithmetic` | `sentence` (required, e.g. `"10,11 = 11+12-10*5; 3:7 = 3\|4"`, operators `+ - * / & \| ^ $`, evaluated left to right, 0-based layers), `roi` |
| `lithophane` | `image` (required; **PNG**, 8-bit grey or true colour, path relative to the recipe file), `layer_height_mm`, `bottom_layer_count`, `bottom_exposure_s`, `normal_exposure_s` (omitted: those of the document), `rotate` none\|rotate_90_clockwise\|rotate_180\|rotate_90_counter_clockwise, `mirror`, `invert_color`, `resize_factor_percent` (100), `enhance_contrast`, `brightness_gain` (-127..127), `gap_closing_iterations`, `remove_noise_iterations`, `gaussian_blur`, `separate_grayscale_pixels` (true), `start_threshold` (1), `end_threshold` (255), `base_thickness_mm` (2), `base_type` none\|square\|model (square), `base_margin_px` (80), `lithophane_height_mm` (3), `one_layer_per_threshold`, `anti_aliasing` (true). **Replaces all layers**; lifts / speeds / waits stay those of the document's first bottom / normal layer |
| `detect_issues` | `output` (JSON report), `fail_on_issues`, `disable` (list of island, overhang, resin_trap, suction_cup, touching_bound, print_height, empty_layer), `island_required_area`, `island_support_percent`, `overhang_erode`, `resin_trap_required_area`, `suction_cup_height_mm`, `touching_margin`, `machine_height_mm` |
| `suggestions` | `only` / `disable` (ids: bottom_layer_count, layer_height, transition_layer_count, wait_time_before_cure, wait_time_after_cure, model_position), `apply` (false; also sets the bottom layer count where the document allows it, see the limits), `output` |
| `save` | `path`: the document's own container format |
| `convert` | `format` (registered id, e.g. `ctb`, `goo`), `path` (extension added when missing), `printer_config` (printer .ini; without it the built-in defaults are used and the geometry check of the destination decides), `accept_adjustments` (boolean, default `false`: see below) |
| `export_images` | `folder` (must not exist or be empty), `filename_prefix`, `type` png\|pgm, `pad_layer_index`, `crop_by_roi`, `orientation`, selection |
| `export_gif` | `file`, `fps`, `repeats`, `skip`, `scale_percent`, `clip_by_volume_bounds`, `orientation`, selection |
| `export_heat_map` | `file`, `merge_same_positioned_layers`, `crop_by_roi`, `orientation`, selection |
| `export_skeleton` | `file`, `crop_by_roi`, selection |
| `export_mesh` | `file` (STL), `pixel_width_mm`, `pixel_height_mm`, `quality` (1,2,3,6,8), `merge_faces`, `orientation` |
| `export_html` | `file`, `title`, `export_layer_settings`, `export_layer_preview`, `preview_first/last/step`, `preview_scale_percent`, `orientation` |

Selection = `first_layer`, `last_layer`, `roi`. `orientation` = `{"flip": none|horizontal|vertical|both, "rotate":
none|rotate_90_clockwise|rotate_180|rotate_90_counter_clockwise}`. `sla-slicer --sla-run-recipe` without a
file prints the operation list.

Limits that come from the document, not from the recipe. The recipe asks the document first
(`layer_list_refusal`, `parameter_operation_refusal`); the refusal text becomes the step error
(`steps[2] (reheight): This document was opened from a 'SL1' file ...`) and a dry run reports it as well.

| kind | operations | works on | refused for |
|---|---|---|---|
| pixel operations | `flip` ... `light_bleed`, `infill`, `raft_relief`, `layer_arithmetic`, `repair_layers` (without `remove_empty_layers`), `blur`, `mask_image`, `layer_import` (replace / stack / merge types without extending), `redraw_model`, `draw_text`, `draw_support`, `drill_drain_hole` | every document | - |
| layer-list operations | `clone`, `remove`, `reheight`, `double_exposure`, `repair_layers` with `remove_empty_layers`, `layer_import` (insert / extend / discard), `stir_resin` | sliced and generated documents, plain raster documents | documents opened from a file (layer count and heights live in a native header) |
| resolution | `change_resolution` | plain raster documents | sliced / generated documents (pixel size is tied to the display) and opened files |
| parameter operations without a layer-list change | `edit_parameters`, `dynamic_lifts`, `fade_exposure`, `timelapse` (lift_height) | documents with an editable per-layer table (sliced, generated, and the opened formats whose table is editable) | documents without a table; a value the format cannot store is refused with the field name |
| parameter operations that add / remove layers | `phased_exposure`, `timelapse` (virtual_layer), `dynamic_layer_height`, `raise_on_print_finish`, `lithophane` | sliced and generated documents | opened files, plain rasters |

Other consequences worth knowing:

* The result of an operation must be storable: e.g. `phased_exposure` with a `sequential_lift_mm` smaller than the
  second retract height, or `timelapse` in virtual_layer mode with photo layers in the middle of the print (Z would go
  down again), are refused by the document ("The result cannot be stored in this document: ...") and change nothing.
* `fade_exposure` writes the per-layer exposures only. UVtools may also update the global transition layer count; the
  document offers no way to write it, so that part is skipped.
* `lithophane` reads **PNG** pictures only (8-bit grey, or colour converted to grey); other image formats are not
  supported (no image codec beyond PNG in the engine). It replaces all layers of the open document on the
  document's own canvas, so it needs a sliced or generated document; the GUI alternative (save the result as a new
  layer project) has no recipe equivalent, use `save` after the step. `repair_layers`: `first_layer` / `last_layer`
  limit only the per-layer stage (gap closing, noise, resin trap fill); islands are detected on the whole document
  (UVtools behaviour). The thumbnail text of the lithophane is not ported (a layer document has no thumbnail slot).
* `blur`: StackBlur / Box / Median / Gaussian / Filter2D call the same OpenCV functions as UVtools (byte identical in the parity runs; StackBlur sizes 5 to 9 differ because UVtools' OpenCV 4.12 has a bug there, see `tests/oracle/layer_ops_vs_uvtools.py`). **Pyramid** is a no-op in UVtools 7.0.0 (its result is never written back); the native one runs the described down + up pyramid.
* `layer_import`, `redraw_model`: the second file is read with the normal archive reader. PNG pictures of different sizes are refused (UVtools packs them with the largest size). `stir_resin`: no thumbnail is generated, and `stir_and_print` keeps the bottom layer count of the document (UVtools behaviour).
* `timelapse` and `raise_on_print_finish` cannot know the machine height of the printer, so give
  `raise_position_z_mm` / `position_z_mm` explicitly (0 means "machine Z" and is refused with a message).
* `suggestions` with `apply` writes the per-layer values (exposure, wait before / after cure) **and, on sliced and
  generated documents, the bottom layer count**, together as ONE undo step. The transition layer count, a blank first
  layer and model moves are reported but never applied (the document has no way to write them); the bottom layer count of
  a file opened from a container is reported as not applicable. `detect_issues` and `export_*` take Z from the layers
  (fallback: constant layer pitch).
* Layer ranges of `reheight` (`offset_position_z`), `double_exposure`, `phased_exposure`, `timelapse`, `dynamic_*`,
  `repair_layers` and `infill` are 0-based and inclusive, checked against the document before anything runs.

## Safety and atomicity

* **Validation first.** `parse_recipe` checks the whole file before anything runs. Errors carry a location:
  `line 3, column 5` for JSON syntax, `steps[2].angle_degrees` for a value, `steps[1].op` for an unknown operation.
* **Each step is atomic.** Mask, layer-list and parameter steps use the document edit APIs: a step is applied
  completely, as one undo step, or not at all (error, cancel).
* **Outputs are staged.** Every file or folder a step writes (`save`, `convert`, exports, reports) is first written
  to a hidden name next to its target (`.recipe-<n>-<name>`) and renamed into place only after the **last** step
  succeeded. A failed or cancelled run (including Ctrl+C) leaves all outputs and the input file untouched, and the
  staging files are removed. Existing files are replaced by the rename; an existing non-empty output folder, an
  output that is a folder, or two steps writing the same path are refused before writing. If the final renames
  themselves fail (disk full), outputs already renamed stay and the error says which one failed.
* **`--dry-run`** opens the input read only, checks every step against the document (ranges, layer-list
  support, destination format via the conversion plan) and checks that outputs can be written. It writes nothing,
  not even staging files. After a layer-list step (`clone`, `remove`) later checks are reported as "not checked"
  because the new layer list is not simulated.
* `--output FILE` replaces the path of the **last** `save`/`convert` step; a recipe without such a step gets an
  implicit native `save` to that file. `--input FILE` replaces the `open` path.

## Approving adjusted values (`accept_adjustments`, `--sla-accept-adjustments`)

A destination that cannot store a setting exactly (whole seconds, whole mm/s, a fixed lift, a Creality model name, ...)
is **refused by default**, with one message listing every problem (never one value at a time). The GUI offers the
nearest storable values in the dialog "Adjust values for export"; the same plan is applied here when the `convert` step
has `"accept_adjustments": true` or the command line has `--sla-accept-adjustments` (which covers every convert step).
The adjusted values are written to the file only (printer config and presets are never modified) and listed in the
step message (`adjusted: <field> <old> -> <new> (reason)`). Values no adjustment can fix (second stage motion,
transition layers, geometry) stay refused. Today an imported document converts only to destinations with a
parameter-preserving conversion (the CTB v4/v5, CXDLP v4, UVJ family), none of which has adjustable values, so the
field matters for destinations that gain such a conversion; the same flags on the main CLI export
(`sla-slicer --slice ... --sla-accept-adjustments --sla-accept-conversion`, see the manual) work for every format.

## Output and exit codes

A JSON report (`ok`, `cancelled`, `dry_run`, `error`, `steps[]` with status done|planned|failed|cancelled|not_run
and message, `outputs[]`) goes to stdout or to `--report FILE`; a readable summary goes to stderr.
Exit code 0 = success, 1 = recipe or step error (nothing written), 130 = cancelled.

## Status of verification

Syntax-checked against the project build flags; the unit tests in `sla_layer_recipe_tests.cpp` assert exact
pixels and parameters on synthetic documents (8x4 and 16x16 generated documents, a plain raster document and an
SL1 file opened from disk for the refusals), one test case per operation (parse errors, exact outcome, refusals, undo
as one step, dry run). The CLI action and the real-archive `open`, `save` and
`convert` steps were not run against the UVtools oracle yet (see the open items in the report that accompanied this
change).
