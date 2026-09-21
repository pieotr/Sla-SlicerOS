#ifndef SLIC3R_SLA_LAYER_DOCUMENT_HPP
#define SLIC3R_SLA_LAYER_DOCUMENT_HPP
#include <string>
#include <vector>
#include "libslic3r/PNGReadWrite.hpp"

namespace Slic3r::sla {
// Raster-space editing: origin at the PNG top-left. No second printer transform.
// Original archive entries (including unknown metadata) are preserved byte-for-byte.
class LayerDocument {
    struct Entry { std::string name; std::vector<uint8_t> bytes; };
    struct Edit { size_t entry; std::vector<uint8_t> before, after; uint64_t before_revision, after_revision; };
    std::vector<Entry> m_entries;
    std::vector<size_t> m_layers;
    std::vector<Edit> m_undo, m_redo;
    uint64_t m_revision = 0, m_saved_revision = 0, m_next_revision = 0;
    void replace(size_t layer, const png::ImageGreyscale &image);
public:
    static LayerDocument load_sl1(const std::string &path);
    size_t layer_count() const { return m_layers.size(); }
    png::ImageGreyscale image(size_t layer) const;
    // A circular stamp in native pixels; value 0 erases, 255 fully exposes.
    void paint(size_t layer, int x, int y, unsigned radius, uint8_t value);
    void threshold(size_t layer, uint8_t cutoff);
    bool undo();
    bool redo();
    bool dirty() const { return m_revision != m_saved_revision; }
    bool can_undo() const { return !m_undo.empty(); }
    bool can_redo() const { return !m_redo.empty(); }
    // Writes a temporary sibling first. Failure preserves the previous output.
    void save_sl1(const std::string &path);
};
}
#endif
