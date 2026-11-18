#ifndef SLIC3R_SLA_LAYER_DOCUMENT_HPP
#define SLIC3R_SLA_LAYER_DOCUMENT_HPP
#include <string>
#include <array>
#include <vector>
#include <functional>
#include "libslic3r/PNGReadWrite.hpp"
#include "RasterBase.hpp"
#include "LayerProgress.hpp"
#include "ExportGeneration.hpp"
#include "LayerParameters.hpp"
#include "LayerJobParameters.hpp"
#include "CrealityCXDLPv4JobParameters.hpp"
#include "LayerRasterGeometry.hpp"
#include "FreshLayerJob.hpp"
#include "LayerOperations.hpp"
#include <optional>
#include <memory>
#include <set>
#include "LayerMemoryBudget.hpp"
#include "NativeBytes.hpp"

namespace Slic3r { class SLAPrint; }
namespace Slic3r::sla { class HistorySpill; }

namespace Slic3r::sla {
struct LayerExportProfile;
struct FreshLayerMaterialPricing;
// Main is the default for slicing/export. Secondary is an explicit editor choice.
enum class LayerMaskPlane { Main, Secondary };
// Derived solely from a successful full PNG/palette validation. Physical
// area, volume and timing must always use the current job's geometry/rows.
struct LayerMaskStatistics {
    size_t columns = 0, rows = 0, nonzero_pixels = 0;
};
// Raster-space editing: origin at the PNG top-left. No second printer transform.
// Unknown archive entries are preserved. Edited SL1 volume metadata is updated
// when its profile supplies enough physical geometry to calculate it.
class LayerDocument {
    friend class NativeArchiveExportRequest;
    // Declared first so assignment retires an old source before replacing data.
    ExportGenerationOwner m_export_source;
    // Project serialization has narrow access to the complete current state,
    // including native source bytes; editing history remains session-local.
    friend struct ProjectDocumentAccess;
    // Only a new export container may adopt a complete converted table. This
    // does not relax editor or persisted-project readonly-field validation.
    friend class Slic3r::SLAPrint;
    void save_converted_ctb_encrypted(const std::string &path,
        const std::vector<LayerParameters> &table, const LayerJobParameters *globals,
        const FreshLayerMaterialPricing *pricing = nullptr, const LayerProgress &progress = {});
    // Short-lived proof from the conversion's mandatory pixel comparison.
    // It cannot escape into public editing, history or project persistence.
    struct ChituboxConversionMasks {
        const LayerDocument *owner;
        uint64_t revision;
        ExportGenerationGuard generation;
        std::vector<LayerMaskStatistics> layers;
        explicit ChituboxConversionMasks(const LayerDocument &document)
            : owner(&document), revision(document.m_revision),
              generation(document.export_generation_guard()) {}
    };
    // Conversion verifies decoded native pixels during import. Public imports
    // still materialize and validate the same canonical editable document.
    struct ChituboxConversionReadback;
    static LayerDocument load_archive_impl(const std::string &path,
        const LayerProgress &progress, ChituboxConversionReadback *readback);
    static LayerDocument load_converted_chitubox(const std::string &path,
        const std::string &format, const LayerDocument &source,
        std::vector<LayerMaskStatistics> &statistics, const LayerProgress &progress);
    void require_converted_chitubox_masks(const ChituboxConversionMasks &masks) const;
    void save_converted_chitubox(const std::string &path,
        const std::vector<LayerParameters> &table, const LayerJobParameters *globals,
        const FreshLayerMaterialPricing *pricing, ChituboxConversionMasks masks,
        const LayerProgress &progress = {});
    // Conversion owns this bootstrap document and has already compared every
    // decoded mask with the source. Parameter transfer retains its original
    // container; final native readback must prove those payloads unchanged.
    void verify_converted_chitubox(const std::string &path,
        const std::vector<LayerParameters> &table, const LayerJobParameters *globals,
        const LayerProgress &progress) const;
    void save_archive_impl(const std::string &path, const LayerProgress &progress,
        const LayerExportProfile *profile, const LayerJobParameters *globals,
        const std::vector<LayerParameters> *converted_table,
        const FreshLayerMaterialPricing *pricing = nullptr,
        const ExportGenerationGuard *generation = nullptr,
        const ChituboxConversionMasks *converted_masks = nullptr) const;
    void validate_persisted_state(const LayerProgress &progress = {},
        std::vector<std::optional<LayerMaskStatistics>> *derived = nullptr) const;
    void validate_and_capture_mask_statistics(const LayerProgress &progress);
    bool mark_saved_if_current(const ExportGenerationGuard &generation);
    struct Entry {
        std::string name;
        std::vector<uint8_t> bytes; // empty while `stored` is set (layer images only are ever stored)
        // Immutable while bytes are unchanged; deliberately absent from
        // project serialization. Every history replacement invalidates it.
        std::optional<LayerMaskStatistics> statistics;
        // Set when the payload lives in the document's spill file or in its source archive.
        StoredBytes stored;
        uint64_t size() const { return stored ? stored.len : bytes.size(); }
        // Content; for resident bytes a non-owning alias, valid until the entry changes.
        std::shared_ptr<const std::vector<uint8_t>> load() const
        { return stored ? layer_payload_cached_load(stored) : std::shared_ptr<const std::vector<uint8_t>>(std::shared_ptr<void>(), &bytes); }
        std::vector<uint8_t> copy() const { return *load(); }
        void set(std::vector<uint8_t> value) { stored = {}; bytes = std::move(value); }
        bool same_as(const std::vector<uint8_t> &other) const
        { if (!derived() && size() != other.size()) return false; const auto mine = load(); return *mine == other; }
        // A lazily produced payload (native container layer): its length is only known once produced.
        bool derived() const { return stored && stored.len == 0; }
    };
    struct Change {
        enum class Target { Entry, FreshRow, FreshOrigin };
        size_t entry; // Entry index or owned row index, according to target.
        std::vector<uint8_t> before, after;
        Target target = Target::Entry;
        // Set when an old edit moved its Entry payloads to a temporary file (one per edit, deleted
        // with the last owner). before/after are then empty; the offsets/lengths address the file.
        std::shared_ptr<HistorySpill> spill;
        uint64_t before_at = 0, before_len = 0, after_at = 0, after_len = 0;
    };
    struct Replacement {
        std::vector<uint8_t> bytes;
        StoredBytes stored; // set instead of bytes when the payload already sits in the history spill
        LayerParameters values;
        FreshLayerParameterOrigin origin = FreshLayerParameterOrigin::Explicit;
    };
    // Complete layer layout, swapped as a whole by layer-list operations (Clone, Remove,
    // ReHeight, ...) because entry indices change when the layer count changes.
    struct Structure {
        std::vector<Entry> entries;
        std::vector<size_t> layers, secondary_layers, layer_parameters;
        std::optional<FreshLayerJob> fresh_job;
    };
    // `structure` is set only for a layer-list edit: it holds the state the NEXT undo/redo
    // switches to (undo and redo exchange it with the live layout; `changes` stays empty).
    struct Edit { std::vector<Change> changes; uint64_t before_revision, after_revision;
                  std::shared_ptr<Structure> structure; };
    std::vector<Entry> m_entries;
    std::vector<size_t> m_layers;
    // Shared secondary masks use the same entry: edits and Undo remain linked.
    std::vector<size_t> m_secondary_layers;
    // Compact parameter entries participate in the same transactional history.
    std::vector<size_t> m_layer_parameters;
    std::optional<FreshLayerJob> m_fresh_job;
    // Explicit generated-source palette, independent of parameter ownership.
    // Absent on historical PNG documents; never inferred from export settings.
    std::optional<LayerMaskPrecision> m_mask_precision;
    // Per-layer rows of the PHZ/FDG/OSLA/VDT/Anycubic ZIP/Klipper containers (see load_archive).
    void attach_native_parameters();
    size_t layer_entry(size_t layer, LayerMaskPlane plane) const;
    std::vector<Edit> m_undo, m_redo;
    std::string m_archive_format; // Empty for new rasters; otherwise original container or standalone image.
    uint64_t m_revision = 0, m_saved_revision = 0, m_next_revision = 0;
    void quantize_mask(std::vector<uint8_t> &pixels) const;
    void replace(size_t layer, const png::ImageGreyscale &image, LayerMaskPlane plane);
    void commit(std::vector<Change> changes);
    std::vector<Replacement> prepare_replacements(const std::vector<Change> &changes, bool after) const;
    void apply_metadata(const std::vector<Change> &changes, const std::vector<Replacement> &replacements);
    void validate_metadata_edit(const std::vector<Change> &changes, const std::vector<Replacement> &replacements);
    void apply_replacements(const std::vector<Change> &changes, std::vector<Replacement> &replacements);
    void trim_undo_history();
    // Moves cold history payloads to disk until the resident part fits its budget.
    void spill_cold_history();
    // Layer payloads: keep the resident part within layer_payload_resident_budget() by moving the
    // rest into the document's spill file (entries listed in `keep` stay in RAM).
    void spill_payloads(const std::set<size_t> *keep = nullptr);
    // Store one freshly produced layer payload, spilling it at once when over the resident budget.
    void store_payload(Entry &entry, std::vector<uint8_t> bytes, uint64_t &resident);
    std::shared_ptr<HistorySpill> payload_spill();
    // Before the source file is replaced: copy every payload that still points into it elsewhere.
    void detach_source(const std::string &about_to_replace) const { if (m_source_store && m_source_store->same_file(about_to_replace)) m_source_store->detach(); }
    std::shared_ptr<HistorySpill> m_payload_spill;
    std::shared_ptr<SourceFileStore> m_source_store;
    // Binary containers (CTB, GOO, Anycubic, ...) opened without holding the file: entry 0 is a reference to
    // the whole source file (m_source_store) and every layer image is a lazily produced PNG
    // (stored.store == m_native_layers, stored.at == layer number) decoded from the layer's own RLE.
    std::shared_ptr<ByteStore> m_native_layers;
    // The container image the native writers read: entry 0 (resident or referenced).
    NativeBytes native_original() const;
    // True while a layer image still is the untouched decode of the source RLE (edit, undo and redo
    // restore the same reference), so a native save may copy the source RLE byte for byte.
    bool native_layer_pristine(size_t entry_index, uint64_t native_number) const
    { const auto &e = m_entries.at(entry_index); return m_native_layers && e.stored.store == m_native_layers && e.stored.at == native_number; }
    void exchange_structure(std::shared_ptr<Structure> &stored);
    // Append one changed layer to a whole-document edit; once the pending payloads exceed the
    // resident history budget everything moves into one spill file (`file`) and later changes go
    // straight there, so the edit never holds the document in RAM twice.
    void add_streamed_change(std::vector<Change> &changes, std::shared_ptr<HistorySpill> &file, uint64_t &resident, Change change) const;
public:
    LayerDocument() = default;
    LayerDocument(const LayerDocument &) = default;
    LayerDocument(LayerDocument &&) noexcept = default;
    LayerDocument &operator=(const LayerDocument &) = default;
    LayerDocument &operator=(LayerDocument &&) noexcept = default;
    // Retire the source before buffers/history start to be destroyed.
    ~LayerDocument() { m_export_source.invalidate(); }
    ExportGenerationGuard export_generation_guard() const { return m_export_source.guard(); }
    ExportInputGuard export_input_guard() const { return m_export_source.input_guard(); }
    enum class MaskOperation { Threshold, Invert, MirrorHorizontal, MirrorVertical, Rotate180, Fill,
                               Erode, Dilate, Open, Close, Gradient, WhiteTopHat, BlackTopHat, BoxBlur, MedianBlur };
    // Inclusive zero-based range. Cancel/error leaves masks and history intact;
    // a completed range is one undo step. Progress runs between whole layers.
    bool transform_layers(size_t first, size_t last, MaskOperation operation, uint8_t value = 255,
                          const std::function<bool(size_t, size_t)> &progress = {}, unsigned radius = 1, LayerMaskPlane plane = LayerMaskPlane::Main);
    // Canonical raster-space masks, independent of the printer's file container.
    // Progress reports each local phase; false throws LayerOperationCancelled.
    // Construction stays local until all copy/projection/validation succeeds.
    static LayerDocument from_rasters(std::vector<EncodedRaster> layers, const LayerProgress &progress = {});
    // Already canonical generated masks. Reject inconsistent source bytes;
    // loading/creation never silently repairs claimed precision provenance.
    static LayerDocument from_precision_rasters(std::vector<EncodedRaster> layers,
        LayerMaskPrecision precision, const LayerProgress &progress = {});
    const std::optional<LayerMaskPrecision> &mask_contract_precision() const { return m_mask_precision; }
    // Scalar editor hints/strokes use the exact same source projection as
    // vector edits. Build all256 entries with one native-layout parse;
    // neither the current printer nor the selected export target is consulted.
    std::array<uint8_t,256> edit_value_projection() const;
    // Complete owned intentions; native/editor capabilities remain separate.
    static LayerDocument from_fresh_rasters(std::vector<EncodedRaster> layers, FreshLayerJob job, const LayerProgress &progress = {});
    // Explicit production projection before masks become an owned document.
    // Loading/restoring a document uses from_fresh_rasters and never projects.
    static LayerDocument from_generated_rasters(std::vector<EncodedRaster> layers, FreshLayerJob job, const LayerProgress &progress = {});
    const std::optional<FreshLayerJob> &fresh_job() const { return m_fresh_job; }
    // Metadata-only model refresh preserves PNG entries and mask Undo/Redo.
    // Explicit/imported intentions cannot be replaced through this API.
    void refresh_automatic_model_parameters(FreshLayerJob job);
    static LayerDocument load_sl1(const std::string &path, const LayerProgress &progress = {});
    // Probe contents: standalone layer images, SL1, UVJ, CWS, Generic ZIP, VDA, Chitubox ZIP,
    // and binary Anycubic 1/515/516/517/518, CTB v1–v5/GKtwo, CBDDLP/Photon v1/v2,
    // CTBEncrypted v4/v5 and CXDLP v1/v2/v3/v4.
    // Binary editing preserves metadata/previews. Anycubic masks retain the original AA levels;
    // v518 shared subimages follow main edits; independent subimages are validated and preserved.
    // Secondary selection edits that image; an originally shared mask stays linked to main.
    // ZIP containers preserve unknown entries and their original mask order.
    // False progress throws LayerOperationCancelled. No partial document is published.
    static LayerDocument load_archive(const std::string &path, const LayerProgress &progress = {});
    // Detached current data for export. Session history is not copied; native
    // entries, plane links, parameters, ownership and revision markers survive.
    LayerDocument snapshot(const LayerProgress &progress = {}) const;
    const std::string &archive_format() const { return m_archive_format; }
    size_t layer_count() const { return m_layers.size(); }
    // Number of independently selectable image planes in this document.
    size_t mask_plane_count() const { return m_secondary_layers.empty() ? 1 : 2; }
    bool mask_is_shared(size_t layer) const;
    // Fields the open document stores per layer (empty: none, everything read-only). Besides CTB,
    // CTB AES and Anycubic binary, these containers expose rows: PHZ/FDG (Z, exposure, light-off),
    // OSLA and VDT (Z, exposure, lifts, retracts, waits, PWM), Anycubic ZIP (Z, exposure, lift
    // distance and speed) and Klipper ZIP (Z, exposure, lifts, retracts, waits, PWM). Edits are
    // written back on save together with the statistics that depend on them.
    LayerParameterCapabilities parameter_capabilities() const;
    // True when the parameter table can be carried into another container by the conversion
    // planner (CTB, CTB AES, Anycubic). The tables of the other families are edited in place
    // and saved natively; raster conversion of such a document drops them like any image-only export.
    bool has_convertible_parameter_table() const;
    bool has_parameter_edits() const;
    LayerParameters layer_parameters(size_t layer) const;
    // Global defaults are read from the source container, independently of
    // layer edits. Supported native tables: Anycubic 516/517 and CTB/AES v4/v5.
    std::optional<LayerJobParameters> job_parameters() const;
    // Native metadata with absent bottom fields, separate from complete
    // conversion/editing defaults. Does not grant parameter capabilities.
    std::optional<CXDLPv4NativeJobSnapshot> cxdlpv4_native_job() const;
    // Derived bottom fields use current masks. No editable/export capabilities
    // are implied by the reference view; cancellation leaves history intact.
    std::optional<CXDLPv4ReferenceBottomView> cxdlpv4_reference_bottom(const LayerProgress &progress = {}) const;
    // Native output-oriented axes. Unknown physical dimensions stay absent;
    // image-only rasters do not acquire guessed dimensions or printer identity.
    std::optional<LayerRasterGeometry> raster_geometry() const;
    // Verify the destination's global native float representation before
    // publication. Writer rounding must not silently change source defaults.
    void require_job_parameters(const LayerJobParameters &expected) const;
    // A patch contains only requested fields; unsupported values must fail
    // before changing entries, revision or Undo history.
    void set_layer_parameters(size_t layer, const LayerParameters &patch);
    // Replace complete canonical rows as one edit. Neighbour Z is validated
    // against the final table, not against partially updated intermediate rows.
    // Failure leaves entries, revision and Undo/Redo unchanged.
    void replace_layer_parameter_table(const std::vector<LayerParameters> &table);
    png::ImageGreyscale image(size_t layer, LayerMaskPlane plane = LayerMaskPlane::Main) const;
    // Borrowed PNG bytes, valid until the document is edited or destroyed.
    // Useful for deduplicating repeated calibration masks without decoding.
    // Keep the returned view alive while the bytes are used: stored payloads are loaded on demand.
    struct EncodedView : png::ReadBuf {
        std::shared_ptr<const std::vector<uint8_t>> owner;
        EncodedView(std::shared_ptr<const std::vector<uint8_t>> data)
            : png::ReadBuf{data->data(), data->size()}, owner(std::move(data)) {}
    };
    EncodedView encoded_layer(size_t layer, LayerMaskPlane plane = LayerMaskPlane::Main) const;
    // Streaming whole-document operation: every layer is decoded, passed to `op` and, when it
    // changed, re-encoded; the result is one undo step and the old/new PNGs go straight to the
    // history spill. `op(mask, layer)` must not change the mask size and is called concurrently for
    // different layers (it must not share mutable state). Cancelling leaves the document unchanged.
    using MaskStreamOperation = std::function<void(layerops::Mask &, size_t layer)>;
    bool apply_mask_stream(LayerMaskPlane plane, const MaskStreamOperation &op, const LayerProgress &progress = {});
    // Same engine with the layer index, the layer count and a sliding window of ORIGINAL neighbour layers:
    // `context.neighbour(j)` is a cached decode for layer - before <= j <= layer + after and a one-off decode
    // for any other layer. Only layers of [first, last] for which `applies` (when set) is true are visited.
    struct MaskStreamContext {
        size_t layer_count = 0;
        const layerops::LayerFetch *fetch = nullptr;
        std::shared_ptr<const layerops::Mask> neighbour(size_t layer) const { return (*fetch)(layer); }
    };
    struct MaskWindowSpec {
        size_t first = 0, last = layerops::kLastLayer; // inclusive, last is clamped
        size_t before = 0, after = 0;                  // neighbour layers kept decoded on each side
        // Not empty: neighbour(j) returns layer j cropped to this rectangle and only the crop is kept in the window.
        layerops::Rect neighbour_crop;
        std::function<bool(size_t)> applies;
    };
    using MaskWindowOperation = std::function<void(layerops::Mask &, size_t layer, const MaskStreamContext &)>;
    bool apply_mask_window(LayerMaskPlane plane, const MaskWindowSpec &spec, const MaskWindowOperation &op, const LayerProgress &progress = {});
    // Visits every layer once (concurrently from several threads, in no particular order); false = cancelled.
    bool scan_masks(LayerMaskPlane plane, const std::function<void(size_t, const layerops::Mask &)> &visit,
                    const LayerProgress &progress = {}) const;
    // Compressed payload bytes currently held in RAM / stored in a temporary file or the source archive.
    uint64_t resident_payload_bytes() const;
    // Size of the container image of a native document and how much of it is resident (0 when it stays in the source file).
    uint64_t native_image_bytes() const { return m_entries.empty() ? 0 : m_entries[0].size(); }
    bool native_image_resident() const { return !m_entries.empty() && !m_entries[0].stored && !m_entries[0].bytes.empty(); }
    uint64_t stored_payload_bytes() const;
    // Document's payload store that points into its source archive (nullptr when none).
    bool references_source_file() const { return bool(m_source_store); }
    // No decode, allocation or lazy mutation: safe for parallel const readers.
    // Missing data requires the existing full decode/count path (e.g. Gray8,
    // imported/project masks or any mask changed by editing/Undo/Redo).
    std::optional<LayerMaskStatistics> validated_mask_statistics(size_t layer,
        LayerMaskPlane plane = LayerMaskPlane::Main) const
    { return m_entries.at(layer_entry(layer, plane)).statistics; }
    // A circular stamp in native pixels; value 0 erases, 255 fully exposes.
    struct BrushPoint { int x, y; };
    // Maximum-intensity overlay is a preview only; painting preserves each
    // original mask outside the stroke and commits the range as one edit.
    png::ImageGreyscale overlay(size_t first, size_t last, LayerMaskPlane plane = LayerMaskPlane::Main,
                               const LayerProgress &progress = {}) const;
    // Stamp geometry. radius r gives a (2r+1)-pixel disk diameter or square side.
    enum class BrushShape { Round, Square };
    // Shape tools of the pixel editor. Line is swept by the brush stamp; the
    // others ignore the brush size. Coordinates are inclusive native pixels.
    enum class ShapeTool { Line, Rectangle, Ellipse, Fill };
    struct ShapeOp {
        ShapeTool tool = ShapeTool::Line;
        BrushShape brush = BrushShape::Round;
        unsigned radius = 0;
        uint8_t value = 255;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // Fill uses (x0,y0) as seed only
    };
    bool paint_stroke(size_t first, size_t last, const std::vector<BrushPoint> &points,
                      unsigned radius, uint8_t value, LayerMaskPlane plane = LayerMaskPlane::Main,
                      const LayerProgress &progress = {}, BrushShape shape = BrushShape::Round);
    // One undo step for the whole range, like paint_stroke. Fill is evaluated on
    // each layer's own mask (not on the max-intensity preview overlay).
    bool paint_shape(size_t first, size_t last, const ShapeOp &op,
                     LayerMaskPlane plane = LayerMaskPlane::Main, const LayerProgress &progress = {});
    // Pure rasterisers. All clip to the image; return the number of pixels
    // written (pixels whose grey value was set, even if it was already equal).
    static size_t draw_stroke(png::ImageGreyscale &image, int x0, int y0, int x1, int y1,
                              unsigned radius, uint8_t value, BrushShape shape);
    static size_t fill_rectangle(png::ImageGreyscale &image, int x0, int y0, int x1, int y1, uint8_t value);
    // Ellipse inscribed in the inclusive box; a pixel belongs if its centre is inside.
    static size_t fill_ellipse(png::ImageGreyscale &image, int x0, int y0, int x1, int y1, uint8_t value);
    // 4-connected fill of the region sharing the seed's exact grey value.
    static size_t flood_fill(png::ImageGreyscale &image, int seed_x, int seed_y, uint8_t value);
    static size_t apply_shape(png::ImageGreyscale &image, const ShapeOp &op);
    // Shared by every range edit: decodes each layer (in parallel batches), runs
    // `edit` on it, and commits all changed masks as ONE undo step.
    bool edit_mask_range(size_t first, size_t last, LayerMaskPlane plane, const LayerProgress &progress,
                         const std::function<void(png::ImageGreyscale &)> &edit);
    void paint(size_t layer, int x, int y, unsigned radius, uint8_t value, LayerMaskPlane plane = LayerMaskPlane::Main);
    static void draw_line(png::ImageGreyscale &image, int x0, int y0, int x1, int y1,
                          unsigned radius, uint8_t value);
    void replace_image(size_t layer, const png::ImageGreyscale &image, LayerMaskPlane plane = LayerMaskPlane::Main);
    void threshold(size_t layer, uint8_t cutoff, LayerMaskPlane plane = LayerMaskPlane::Main);
    // Reverse mask polarity without discarding antialiased grayscale values.
    void invert(size_t layer, LayerMaskPlane plane = LayerMaskPlane::Main);
    // ---- UVtools-style layer operations bridge (see SLA/LayerOperations.hpp) ----------------------
    // Whole-document mask operation. Decodes every layer of `plane` into plain masks, runs `op`
    // (which edits them in place; it receives a progress callback it must honour), and commits ONLY
    // the masks that changed as ONE undo step. Whole-document (not per-range) because many
    // operations need layers outside their selection (model bounds, layer arithmetic, raft detection).
    // Returns false when cancelled (document untouched). Throws RuntimeError when the working set
    // exceeds 1 GiB of decoded pixels or the operation changes a mask size.
    using MaskBatchOperation = std::function<void(std::vector<layerops::Mask> &, const LayerProgress &)>;
    bool apply_mask_batch(LayerMaskPlane plane, const MaskBatchOperation &op, const LayerProgress &progress = {});
    // Runs a layerops kernel (see LayerOperations.hpp) through the streaming engine: the factory is given the layout
    // of the document (size, `z_mm`, random access and a scan) and returns the per-layer kernel; one undo step.
    // When the factory throws layerops::NotStreamable the `fallback` batch operation (when given) is applied instead.
    using MaskKernelFactory = std::function<layerops::StreamKernel(const layerops::StreamLayout &)>;
    bool apply_mask_kernel(LayerMaskPlane plane, const std::vector<double> &z_mm, const MaskKernelFactory &factory,
                           const LayerProgress &progress = {}, const MaskBatchOperation &fallback = {});
    // Result of a layer-list operation: which input layer produced each output layer plus optional
    // per-OUTPUT-layer parameter overrides (DoubleExposure). A negative value keeps the source value.
    struct LayerListChange {
        layerops::LayerListResult list;
        std::vector<double> exposure_s, lift_mm, wait_before_cure_s; // empty vector = no override
        std::optional<size_t> bottom_layer_count;                     // new nominal bottom layer count
        // Optional per-OUTPUT-layer patch of additional fields (lift speeds, waits, PWM, ...) applied on top of
        // the source layer's row. Only for documents that own parameter rows (sliced / generated).
        std::vector<LayerParameters> row_patch;
    };
    // `z_mm` is the top Z of each layer in mm (input and, after the call, output).
    using LayerListOperation = std::function<LayerListChange(std::vector<layerops::Mask> &, std::vector<double> &z_mm,
                                                              const LayerProgress &)>;
    // Empty string = this document can represent a changed layer list (and, if requested, resolution).
    // Otherwise a user-facing reason. Only generated documents (sliced / calibration / raster) can: files
    // opened from a container keep layer count, heights and resolution in native headers that the
    // writers copy verbatim, so changing them would corrupt the file.
    std::string layer_list_refusal(bool changes_resolution = false) const;
    // Applies `op` as ONE undoable step; per-layer parameters follow `source_index`. Throws
    // RuntimeError (document untouched) when refused or when the result is invalid (e.g. repeated Z
    // in a document that requires increasing Z). `fallback_layer_height_mm` synthesises Z for
    // documents without parameter rows. Returns false when cancelled.
    bool apply_layer_list_operation(const LayerListOperation &op, bool changes_resolution,
                                    double fallback_layer_height_mm, const LayerProgress &progress = {});
    // ---- Streamed layer-list operations -----------------------------------------------------------------
    // Layer-list operations that never hold the document decoded: the operation decodes the layers it needs
    // one (or a few) at a time through `load`, stores every NEW layer at once through `emit` (encoded, spilled
    // to disk past the resident budget) and returns, per output layer, a reference: the payload of an existing
    // layer (Clone, Remove, ReHeight division: nothing is decoded) or an emitted one. A layer that comes out
    // pixel-identical to its source keeps the source's exact PNG (same rule as apply_layer_list_operation).
    struct LayerRef { size_t index = 0; bool emitted = false; }; // existing layer `index`, or emitted blob `index`
    struct ListStreamContext {
        size_t layer_count = 0, width = 0, height = 0;
        std::vector<double> z_mm;                       // top Z of every input layer (mm)
        std::vector<layerops::LayerParams> table;       // input parameter table (parameter variant only)
        std::function<layerops::Mask(size_t)> load;     // decodes one input layer; thread safe
        // Stores a computed layer. `source` = the input layer whose payload is reused when the pixels are
        // identical (returns a reference to it), otherwise an emitted blob. Thread safe.
        std::function<LayerRef(layerops::Mask &&, std::optional<size_t> source)> emit;
        // Like emit but for an intermediate result that the operation reads back with `load_ref` and stores again:
        // it is not quantized to the document's mask precision.
        std::function<LayerRef(layerops::Mask &&, std::optional<size_t> source)> emit_scratch;
        // Decodes an input layer (emitted == false) or a layer stored with emit / emit_scratch; thread safe.
        std::function<layerops::Mask(LayerRef)> load_ref;
        // Runs fn(i) for i in [first,last] in parallel chunks, checking cancellation between chunks;
        // `buffers` = decoded layer-sized buffers one task holds at once (bounds the chunk by the budget).
        std::function<void(size_t first, size_t last, size_t buffers, const std::function<void(size_t)> &fn)> parallel;
        LayerProgress progress;
    };
    struct ListStreamResult {
        LayerListChange change;                         // source_index (+ overrides) as for apply_layer_list_operation
        std::vector<double> z_mm;                       // output Z
        std::vector<layerops::LayerParams> table;       // output parameter table (parameter variant only)
        std::vector<LayerRef> refs;                     // output layer pixels
    };
    using ListStreamOperation = std::function<ListStreamResult(ListStreamContext &)>;
    // One undo step; cancellation (false) and errors leave the document untouched. Same refusal rules as
    // apply_layer_list_operation.
    bool apply_layer_list_stream(const ListStreamOperation &op, bool changes_resolution,
                                 double fallback_layer_height_mm, const LayerProgress &progress = {});
    // Parameter variant (phased exposure, timelapse, raise on finish, lithophane): ctx.table is the input table, the
    // result's table the output one; fields that differ from the source layer's row become the row patch.
    bool apply_parameter_list_stream(const ListStreamOperation &op, double fallback_layer_height_mm,
                                     const LayerProgress &progress = {});
    // Pixel-only variant: `op` returns one reference per layer (result.refs.size() == layer_count, the layer list and the
    // parameters stay as they are); layers whose reference is an emitted blob get that blob as their new mask. One undo
    // step, works on every document (also files opened from a container), cancellation / errors leave it untouched.
    // ctx.z_mm holds the Z of every layer (the stored one, else fallback_layer_height_mm * (index + 1)).
    bool apply_pixel_list_stream(const ListStreamOperation &op, double fallback_layer_height_mm, const LayerProgress &progress = {});
private:
    // Shared tail of the layer-list operations: validates `change`/`z_mm`, builds the new layout from
    // `make_entry(i)` (output i), derives the fresh-job rows and commits ONE undo step.
    void commit_layer_list(const LayerListChange &change, const std::vector<double> &z_mm, size_t count, size_t out,
                           const std::function<Entry(size_t)> &make_entry, size_t old_bottom, size_t old_transition,
                           const ExportGenerationGuard &generation, const LayerProgress &progress);
    bool run_list_stream(const ListStreamOperation &op, bool changes_resolution, bool with_table, double fallback_layer_height_mm,
                         const LayerProgress &progress, bool pixels_only = false);
public:
    // ---- Per-layer parameter operations (UVtools batch 2: lifts, fades, phases, timelapse, ...) ----------
    // The operation edits a canonical table (one layerops::LayerParams per layer, plus top Z). Empty string
    // = the document stores per-layer print parameters; otherwise a user-facing reason.
    std::string parameter_operation_refusal() const;
    // Current table in operation units (is_bottom from the global bottom layer count). Empty when refused.
    std::vector<layerops::LayerParams> parameter_table() const;
    // Printer/format facts the operations need (which optional fields the format can store, layer height).
    layerops::PrinterContext parameter_printer_context(double fallback_layer_height_mm) const;
    // The operation may edit `table` / `z_mm` (and read masks when `needs_masks`); the layer list and the
    // pixels must stay as they are. Changed fields are written through the per-layer parameter capabilities
    // (replace_layer_parameter_table) as ONE undo step. Throws RuntimeError (document untouched) naming the
    // field when the format cannot store a changed value. `masks` is empty when !needs_masks.
    using ParameterOperation = std::function<LayerListChange(std::vector<layerops::Mask> &, std::vector<double> &z_mm,
                                                              std::vector<layerops::LayerParams> &table, const LayerProgress &)>;
    bool apply_parameter_operation(const ParameterOperation &op, bool needs_masks, double fallback_layer_height_mm,
                                   const LayerProgress &progress = {});
    // Same operation signature, but the result may add / remove layers (phased exposure, timelapse virtual
    // layers, dynamic layer height, raise on finish). Generated documents only; goes through
    // apply_layer_list_operation, so it is one undo step as well. Fields that differ from the source layer's
    // row are carried through LayerListChange::row_patch.
    bool apply_parameter_list_operation(const ParameterOperation &op, double fallback_layer_height_mm,
                                        const LayerProgress &progress = {});
    // New generated document from complete masks and a parameter table (Lithophane when the open document
    // cannot take a different layer list). `tmpl` supplies printer geometry and global values; every row is
    // the template's first row patched with the table. Masks must match the template's resolution.
    static LayerDocument from_parameter_masks(const FreshLayerJob &tmpl, const std::vector<layerops::Mask> &masks,
                                              const std::vector<double> &z_mm,
                                              const std::vector<layerops::LayerParams> &table,
                                              const LayerProgress &progress = {});
    bool undo();
    bool redo();
    bool dirty() const { return m_revision != m_saved_revision; }
    uint64_t revision() const { return m_revision; }
    void mark_saved() { m_saved_revision = m_revision; }
    // A persisted edited result is a new saved baseline; undo history is
    // intentionally local to an editing session rather than serialized.
    void mark_restored_edit() { m_export_source.invalidate(); m_revision = ++m_next_revision; m_saved_revision = m_revision; }
    bool can_undo() const { return !m_undo.empty(); }
    bool can_redo() const { return !m_redo.empty(); }
    // Writes a temporary sibling first. Failure preserves the previous output.
    void save_sl1(const std::string &path, const LayerProgress &progress = {});
    // Cancellation before atomic publication preserves output, dirty state and history.
    // An optional typed global overlay is supported for CTB/AES v4/v5 only;
    // output enables the per-layer table while preserving distinct global defaults.
    void save_archive(const std::string &path, const LayerProgress &progress = {},
                      const LayerExportProfile *profile = nullptr,
                      const LayerJobParameters *globals = nullptr);
    // A single selected layer writes a file. A longer inclusive range writes
    // numbered masks into a new directory, published only after every codec
    // succeeds. The directory must not exist; cancellation leaves no outputs.
    // Exporting masks does not mark a printer document as saved or applied.
    bool export_images(const std::string &path, const std::string &format, size_t first, size_t last,
                       const std::function<bool(size_t, size_t)> &progress = {},
                       LayerMaskPlane plane = LayerMaskPlane::Main) const;
};
}
#endif
