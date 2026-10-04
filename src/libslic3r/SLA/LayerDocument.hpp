#ifndef SLIC3R_SLA_LAYER_DOCUMENT_HPP
#define SLIC3R_SLA_LAYER_DOCUMENT_HPP
#include <string>
#include <vector>
#include <functional>
#include "libslic3r/PNGReadWrite.hpp"
#include "RasterBase.hpp"

namespace Slic3r::sla {
// Raster-space editing: origin at the PNG top-left. No second printer transform.
// Unknown archive entries are preserved. Edited SL1 volume metadata is updated
// when its profile supplies enough physical geometry to calculate it.
class LayerDocument {
    struct Entry { std::string name; std::vector<uint8_t> bytes; };
    struct Change { size_t entry; std::vector<uint8_t> before, after; };
    struct Edit { std::vector<Change> changes; uint64_t before_revision, after_revision; };
    std::vector<Entry> m_entries;
    std::vector<size_t> m_layers;
    std::vector<Edit> m_undo, m_redo;
    std::string m_archive_format; // Empty for new rasters; otherwise original container or standalone PNG.
    uint64_t m_revision = 0, m_saved_revision = 0, m_next_revision = 0;
    void replace(size_t layer, const png::ImageGreyscale &image);
    void commit(std::vector<Change> changes);
public:
    enum class MaskOperation { Threshold, Invert, MirrorHorizontal, MirrorVertical, Rotate180, Fill,
                               Erode, Dilate, Open, Close, Gradient, WhiteTopHat, BlackTopHat, BoxBlur, MedianBlur };
    // Inclusive zero-based range. Cancel/error leaves masks and history intact;
    // a completed range is one undo step. Progress runs between whole layers.
    bool transform_layers(size_t first, size_t last, MaskOperation operation, uint8_t value = 255,
                          const std::function<bool(size_t, size_t)> &progress = {}, unsigned radius = 1);
    // Canonical raster-space masks, independent of the printer's file container.
    static LayerDocument from_rasters(std::vector<EncodedRaster> layers);
    static LayerDocument load_sl1(const std::string &path);
    // Probe contents: PNG, SL1, UVJ, CWS, Generic ZIP, VDA, Chitubox ZIP,
    // and binary CXDLP v1/v2/v3/v4. Binary editing preserves metadata and previews.
    // ZIP containers preserve unknown entries and their original mask order.
    static LayerDocument load_archive(const std::string &path);
    const std::string &archive_format() const { return m_archive_format; }
    size_t layer_count() const { return m_layers.size(); }
    png::ImageGreyscale image(size_t layer) const;
    // Borrowed PNG bytes, valid until the document is edited or destroyed.
    // Useful for deduplicating repeated calibration masks without decoding.
    png::ReadBuf encoded_layer(size_t layer) const;
    // A circular stamp in native pixels; value 0 erases, 255 fully exposes.
    void paint(size_t layer, int x, int y, unsigned radius, uint8_t value);
    static void draw_line(png::ImageGreyscale &image, int x0, int y0, int x1, int y1,
                          unsigned radius, uint8_t value);
    void replace_image(size_t layer, const png::ImageGreyscale &image);
    void threshold(size_t layer, uint8_t cutoff);
    // Reverse mask polarity without discarding antialiased grayscale values.
    void invert(size_t layer);
    bool undo();
    bool redo();
    bool dirty() const { return m_revision != m_saved_revision; }
    uint64_t revision() const { return m_revision; }
    void mark_saved() { m_saved_revision = m_revision; }
    // A persisted edited result is a new saved baseline; undo history is
    // intentionally local to an editing session rather than serialized.
    void mark_restored_edit() { m_revision = ++m_next_revision; m_saved_revision = m_revision; }
    bool can_undo() const { return !m_undo.empty(); }
    bool can_redo() const { return !m_redo.empty(); }
    // Writes a temporary sibling first. Failure preserves the previous output.
    void save_sl1(const std::string &path);
    void save_archive(const std::string &path);
};
}
#endif
