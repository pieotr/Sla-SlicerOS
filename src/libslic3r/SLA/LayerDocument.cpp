#include "LayerDocument.hpp"
#include "RasterBase.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/Exception.hpp"
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <set>
#include <limits>

namespace Slic3r::sla {
LayerDocument LayerDocument::load_sl1(const std::string &path)
{
    struct Reader : MZ_Archive {
        explicit Reader(const std::string &p) {
            if (!open_zip_reader(&arch, p)) throw FileIOError(get_errorstr());
        }
        ~Reader() { close_zip_reader(&arch); }
    } zip(path);
    LayerDocument result;
    std::set<std::string> names;
    bool config = false, profile = false;
    // Explicit memory budget until a disk-backed archive store is available.
    constexpr uint64_t limit = uint64_t(1) << 30;
    uint64_t total = 0;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip.arch); ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip.arch, i, &stat)) throw FileIOError(zip.get_errorstr());
        if (mz_zip_reader_is_file_a_directory(&zip.arch, i)) continue;
        std::string name = stat.m_filename, lower = boost::algorithm::to_lower_copy(name);
        if (!names.insert(lower).second) throw RuntimeError("Duplicate SL1 archive entry");
        if (stat.m_uncomp_size > limit - total) throw RuntimeError("Layer editor archive exceeds the 1 GiB memory budget");
        total += stat.m_uncomp_size;
        Entry entry{name, std::vector<uint8_t>(size_t(stat.m_uncomp_size))};
        if (!mz_zip_reader_extract_to_mem(&zip.arch, i, entry.bytes.data(), entry.bytes.size(), 0))
            throw FileIOError(zip.get_errorstr());
        config |= lower == "config.ini";
        profile |= lower == "prusaslicer.ini";
        // SL1 layers use a top-level prefix followed by a zero-padded index.
        // Keep other PNG assets intact rather than exposing them as print layers.
        bool is_layer = lower.find('/') == std::string::npos && lower.find('\\') == std::string::npos &&
                        boost::algorithm::ends_with(lower, ".png");
        size_t digits = 0;
        if (is_layer)
            for (size_t p = lower.size()-4; p > 0 && lower[p-1] >= '0' && lower[p-1] <= '9'; --p) ++digits;
        if (is_layer && digits >= 5)
            result.m_layers.push_back(result.m_entries.size());
        result.m_entries.push_back(std::move(entry));
    }
    if (!config || !profile || result.m_layers.empty())
        throw RuntimeError("The layer editor requires an SL1 archive with a profile and PNG layers");
    std::sort(result.m_layers.begin(), result.m_layers.end(), [&](size_t a, size_t b) {
        return boost::algorithm::to_lower_copy(result.m_entries[a].name) <
               boost::algorithm::to_lower_copy(result.m_entries[b].name);
    });
    // Decode only on demand, rather than allocating every full-resolution mask.
    result.image(0);
    return result;
}

png::ImageGreyscale LayerDocument::image(size_t layer) const
{
    const auto &bytes = m_entries.at(m_layers.at(layer)).bytes;
    // Bound decoded allocation before invoking libpng (PNG IHDR width/height).
    if (bytes.size() < 24 || !png::is_png({bytes.data(), bytes.size()}))
        throw RuntimeError("Invalid layer PNG");
    auto u32 = [&](size_t p) { return (uint32_t(bytes[p]) << 24) | (uint32_t(bytes[p+1]) << 16) |
                                    (uint32_t(bytes[p+2]) << 8) | uint32_t(bytes[p+3]); };
    const uint64_t cols = u32(16), rows = u32(20);
    if (!cols || !rows || cols > 32768 || rows > 32768 || cols * rows > 128 * 1024 * 1024)
        throw RuntimeError("Layer dimensions exceed the editor memory budget");
    png::ImageGreyscale result;
    if (!png::decode_png(png::ReadBuf{bytes.data(), bytes.size()}, result))
        throw RuntimeError("Cannot decode grayscale layer PNG");
    return result;
}

void LayerDocument::replace(size_t layer, const png::ImageGreyscale &image)
{
    const size_t index = m_layers.at(layer);
    const auto encoded = PNGRasterEncoder{}(image.buf.data(), image.cols, image.rows, 1);
    const auto *data = static_cast<const uint8_t *>(encoded.data());
    std::vector<uint8_t> after(data, data + encoded.size());
    if (after == m_entries[index].bytes) return;
    const uint64_t next_revision = m_next_revision + 1;
    m_undo.push_back({index, m_entries[index].bytes, after, m_revision, next_revision});
    // Bound history by both count and compressed size.
    size_t bytes = 0;
    for (const auto &edit : m_undo) bytes += edit.before.size() + edit.after.size();
    while (m_undo.size() > 1 && (m_undo.size() > 32 || bytes > 128 * 1024 * 1024)) {
        bytes -= m_undo.front().before.size() + m_undo.front().after.size();
        m_undo.erase(m_undo.begin());
    }
    m_entries[index].bytes = std::move(after);
    m_redo.clear();
    m_revision = m_next_revision = next_revision;
}

void LayerDocument::paint(size_t layer, int x, int y, unsigned radius, uint8_t value)
{
    auto img = image(layer);
    if (x < 0 || y < 0 || size_t(x) >= img.cols || size_t(y) >= img.rows || radius > 4096)
        throw RuntimeError("Brush position or radius is outside the supported range");
    const int r = int(radius);
    bool changed = false;
    for (int py = std::max(0, y-r); py <= std::min(int(img.rows)-1, y+r); ++py)
        for (int px = std::max(0, x-r); px <= std::min(int(img.cols)-1, x+r); ++px)
            if (int64_t(px-x)*(px-x) + int64_t(py-y)*(py-y) <= int64_t(r)*r) {
                auto &pixel = img.buf[size_t(py)*img.cols + size_t(px)];
                changed |= pixel != value;
                pixel = value;
            }
    if (changed) replace(layer, img);
}
void LayerDocument::threshold(size_t layer, uint8_t cutoff)
{
    auto img = image(layer);
    bool changed = false;
    for (auto &p : img.buf) {
        const uint8_t value = p >= cutoff ? 255 : 0;
        changed |= value != p;
        p = value;
    }
    if (changed) replace(layer, img);
}
bool LayerDocument::undo()
{
    if (m_undo.empty()) return false;
    auto edit = std::move(m_undo.back()); m_undo.pop_back();
    m_entries[edit.entry].bytes = edit.before;
    m_revision = edit.before_revision;
    m_redo.push_back(std::move(edit)); return true;
}
bool LayerDocument::redo()
{
    if (m_redo.empty()) return false;
    auto edit = std::move(m_redo.back()); m_redo.pop_back();
    m_entries[edit.entry].bytes = edit.after;
    m_revision = edit.after_revision;
    m_undo.push_back(std::move(edit)); return true;
}
void LayerDocument::save_sl1(const std::string &path)
{
    if (m_layers.empty()) throw RuntimeError("Cannot save an empty layer document");
    const boost::filesystem::path output(path);
    const auto temporary = output.parent_path() / boost::filesystem::unique_path(".sla-edit-%%%%-%%%%.tmp");
    try {
        {
            Zipper zip(temporary.string());
            for (const auto &entry : m_entries)
                zip.add_entry(entry.name, entry.bytes.data(), entry.bytes.size());
            zip.finalize();
        }
        boost::filesystem::rename(temporary, output);
    } catch (...) {
        boost::system::error_code ignored;
        boost::filesystem::remove(temporary, ignored);
        throw;
    }
    m_saved_revision = m_revision;
}
}
