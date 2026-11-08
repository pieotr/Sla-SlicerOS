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
#include <optional>

namespace Slic3r { class SLAPrint; }

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
        std::vector<uint8_t> bytes;
        // Immutable while bytes are unchanged; deliberately absent from
        // project serialization. Every history replacement invalidates it.
        std::optional<LayerMaskStatistics> statistics;
    };
    struct Change {
        enum class Target { Entry, FreshRow, FreshOrigin };
        size_t entry; // Entry index or owned row index, according to target.
        std::vector<uint8_t> before, after;
        Target target = Target::Entry;
    };
    struct Replacement {
        std::vector<uint8_t> bytes;
        LayerParameters values;
        FreshLayerParameterOrigin origin = FreshLayerParameterOrigin::Explicit;
    };
    struct Edit { std::vector<Change> changes; uint64_t before_revision, after_revision; };
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
    LayerParameterCapabilities parameter_capabilities() const;
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
    png::ReadBuf encoded_layer(size_t layer, LayerMaskPlane plane = LayerMaskPlane::Main) const;
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
    bool paint_stroke(size_t first, size_t last, const std::vector<BrushPoint> &points,
                      unsigned radius, uint8_t value, LayerMaskPlane plane = LayerMaskPlane::Main,
                      const LayerProgress &progress = {});
    void paint(size_t layer, int x, int y, unsigned radius, uint8_t value, LayerMaskPlane plane = LayerMaskPlane::Main);
    static void draw_line(png::ImageGreyscale &image, int x0, int y0, int x1, int y1,
                          unsigned radius, uint8_t value);
    void replace_image(size_t layer, const png::ImageGreyscale &image, LayerMaskPlane plane = LayerMaskPlane::Main);
    void threshold(size_t layer, uint8_t cutoff, LayerMaskPlane plane = LayerMaskPlane::Main);
    // Reverse mask polarity without discarding antialiased grayscale values.
    void invert(size_t layer, LayerMaskPlane plane = LayerMaskPlane::Main);
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
