// SPDX-License-Identifier: AGPL-3.0-or-later
// Read-only view of a ZIP archive held in memory, shared by the layer-editor
// readers of ZIP based printer formats (Anycubic ZIP, Klipper, NanoDLP, ...).
//
// WHY a separate class: those readers keep the complete original file in the
// document (that is what makes an unchanged save byte-identical) and decode one
// layer at a time from it. Extracting every entry up front, as the generic ZIP
// importer does, would hold the compressed masks twice.
#ifndef SLIC3R_MEMORY_ZIP_HPP
#define SLIC3R_MEMORY_ZIP_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/Exception.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/SLA/NativeBytes.hpp"

namespace Slic3r {

// Names of the non-directory entries, read from the central directory only.
// Returns false (instead of throwing) when the file is not a readable ZIP, so a
// caller can fall through to the next format probe and let it report the error.
inline bool zip_entry_names(const std::string &path, std::set<std::string> &names)
{
    MZ_Archive archive;
    if (!open_zip_reader(&archive.arch, path))
        return false;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive.arch); ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive.arch, i, &stat)) {
            close_zip_reader(&archive.arch);
            return false;
        }
        if (!mz_zip_reader_is_file_a_directory(&archive.arch, i))
            names.insert(stat.m_filename);
    }
    close_zip_reader(&archive.arch);
    return true;
}

class MemoryZip {
public:
    // `bytes` must outlive this object: miniz reads straight from the buffer.
    MemoryZip(const std::vector<uint8_t> &bytes, const char *kind) : m_kind(kind)
    {
        std::memset(&m_zip, 0, sizeof(m_zip));
        if (!mz_zip_reader_init_mem(&m_zip, bytes.data(), bytes.size(), 0))
            throw RuntimeError(m_kind + " is not a readable ZIP archive: " + MZ_Archive::get_errorstr(m_zip.m_last_error));
        m_open = true;
        build_index();
    }
    // An archive that may sit in a ByteStore (the source file, or a file-backed output): miniz pulls
    // the central directory and each entry through a read callback, so the archive is never resident.
    MemoryZip(const sla::NativeBytes &image, const char *kind) : m_kind(kind), m_image(image)
    {
        std::memset(&m_zip, 0, sizeof(m_zip));
        if (image.vector()) {
            if (!mz_zip_reader_init_mem(&m_zip, image.vector()->data(), image.vector()->size(), 0))
                throw RuntimeError(m_kind + " is not a readable ZIP archive: " + MZ_Archive::get_errorstr(m_zip.m_last_error));
        } else {
            m_zip.m_pRead = [](void *opaque, mz_uint64 offset, void *buffer, size_t count) -> size_t {
                const auto &source = *static_cast<const sla::NativeBytes *>(opaque);
                if (offset >= source.size()) return 0;
                count = size_t(std::min<mz_uint64>(count, source.size() - offset));
                try { const auto got = source.read(offset, count); if (count) std::memcpy(buffer, got.data(), count); }
                catch (...) { return 0; }
                return count;
            };
            m_zip.m_pIO_opaque = &m_image;
            if (!mz_zip_reader_init(&m_zip, mz_uint64(image.size()), 0))
                throw RuntimeError(m_kind + " is not a readable ZIP archive: " + MZ_Archive::get_errorstr(m_zip.m_last_error));
        }
        m_open = true;
        build_index();
    }
    ~MemoryZip() { if (m_open) mz_zip_reader_end(&m_zip); }
    MemoryZip(const MemoryZip &) = delete;
    MemoryZip &operator=(const MemoryZip &) = delete;

    bool has(const std::string &name) const { return m_index.count(name) != 0; }
    uint64_t size_of(const std::string &name) const
    {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&m_zip, m_index.at(name), &stat))
            throw RuntimeError(m_kind + " has a damaged ZIP directory");
        return stat.m_uncomp_size;
    }
    // Extracts one entry. `limit` bounds the allocation, so a forged
    // uncompressed size in the central directory cannot exhaust memory.
    std::vector<uint8_t> extract(const std::string &name, uint64_t limit) const
    {
        const auto found = m_index.find(name);
        if (found == m_index.end())
            throw RuntimeError(m_kind + " is missing the entry '" + name + "'");
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&m_zip, found->second, &stat))
            throw RuntimeError(m_kind + " has a damaged ZIP directory");
        if (stat.m_uncomp_size > limit)
            throw RuntimeError(m_kind + " entry '" + name + "' is larger than its format allows");
        std::vector<uint8_t> data(size_t(stat.m_uncomp_size));
        // miniz verifies the CRC of the extracted data, so corruption surfaces here.
        if (!data.empty() && !mz_zip_reader_extract_to_mem(&m_zip, found->second, data.data(), data.size(), 0))
            throw RuntimeError(m_kind + " entry '" + name + "' cannot be extracted: " + MZ_Archive::get_errorstr(m_zip.m_last_error));
        return data;
    }
    // Writes every entry in its original order. Entries named in `replacements`
    // get the new bytes; all others are copied unchanged, so unknown entries
    // (previews, vendor metadata) survive an edit.
    void rewrite(Zipper &out, const std::map<std::string, std::vector<uint8_t>> &replacements,
                 const std::function<void()> &poll) const
    {
        for (const auto &name : ordered_names()) {
            poll();
            const auto replaced = replacements.find(name);
            if (replaced != replacements.end()) {
                out.add_entry(name, replaced->second.empty() ? static_cast<const void *>("") : static_cast<const void *>(replaced->second.data()),
                              replaced->second.size());
                continue;
            }
            const auto data = extract(name, uint64_t(1) << 30);
            // The binary overload is the one that writes zero-byte marker
            // entries (e.g. ".klipper"); never hand it a null pointer.
            out.add_entry(name, data.empty() ? static_cast<const void *>("") : static_cast<const void *>(data.data()), data.size());
        }
    }
    // Entry names in central-directory order.
    std::vector<std::string> ordered_names() const
    {
        std::vector<std::pair<mz_uint, std::string>> sorted;
        for (const auto &[name, index] : m_index)
            sorted.emplace_back(index, name);
        std::sort(sorted.begin(), sorted.end());
        std::vector<std::string> names;
        names.reserve(sorted.size());
        for (auto &entry : sorted)
            names.push_back(std::move(entry.second));
        return names;
    }
    const std::map<std::string, mz_uint> &index() const { return m_index; }

private:
    void build_index()
    {
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&m_zip); ++i) {
            mz_zip_archive_file_stat stat;
            if (!mz_zip_reader_file_stat(&m_zip, i, &stat))
                throw RuntimeError(m_kind + " has a damaged ZIP directory: " + MZ_Archive::get_errorstr(m_zip.m_last_error));
            if (mz_zip_reader_is_file_a_directory(&m_zip, i))
                continue;
            // An ambiguous name would make "the" layer image undefined and a
            // rewrite would silently drop or duplicate one of them.
            if (!m_index.emplace(stat.m_filename, i).second)
                throw RuntimeError(m_kind + " contains the duplicate entry '" + stat.m_filename + "'");
        }
    }
    std::string m_kind;
    sla::NativeBytes m_image;
    mutable mz_zip_archive m_zip;
    bool m_open = false;
    std::map<std::string, mz_uint> m_index;
};

} // namespace Slic3r

#endif
