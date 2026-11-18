#ifndef SLIC3R_SLA_NATIVE_BYTES_HPP
#define SLIC3R_SLA_NATIVE_BYTES_HPP
// Read-only random-access view of a binary container image (CTB, GOO, Anycubic, ...). The image is
// either an ordinary vector (small/restored documents, freshly built output) or a ByteStore
// (the source file the document was opened from, or a PatchedImage that overlays edits on it), so
// the format layouts can parse headers and tables without the whole file being in RAM. Store-backed
// views read through a small page cache; layer payloads are fetched with read() in one piece.
#include "LayerMemoryBudget.hpp"
#include "libslic3r/Exception.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace Slic3r::sla {

class NativeBytes {
public:
    NativeBytes() = default;
    // Non-owning: `vector` must outlive the view (same contract the layouts always had).
    NativeBytes(const std::vector<uint8_t> &vector) : m_vector(&vector), m_size(vector.size()) {}
    // Owning handle of a store; `size` is the length of the image.
    NativeBytes(std::shared_ptr<const ByteStore> store, uint64_t size)
        : m_keep(std::move(store)), m_store(m_keep.get()), m_size(size), m_cache(std::make_shared<Cache>()) {}
    // Non-owning store (stack-allocated PatchedImage).
    NativeBytes(const ByteStore &store, uint64_t size) : m_store(&store), m_size(size), m_cache(std::make_shared<Cache>()) {}

    size_t size() const { return size_t(m_size); }
    bool empty() const { return m_size == 0; }
    bool in_memory() const { return m_vector != nullptr; }
    const std::vector<uint8_t> *vector() const { return m_vector; }
    const ByteStore *store() const { return m_store; }

    uint8_t operator[](size_t at) const
    {
        if (m_vector) return (*m_vector)[at];
        if (at >= m_size) throw RuntimeError("The container is truncated");
        return page_byte(at);
    }
    uint8_t at(size_t at_) const { if (at_ >= m_size) throw RuntimeError("The container is truncated"); return (*this)[at_]; }
    // A range of the image in one piece (bounds-checked).
    std::vector<uint8_t> read(uint64_t at, uint64_t length) const
    {
        if (at > m_size || length > m_size - at) throw RuntimeError("The container section lies outside the file");
        if (m_vector) return std::vector<uint8_t>(m_vector->begin() + at, m_vector->begin() + at + length);
        return m_store->read(at, length);
    }
    bool equals(size_t at, const void *text, size_t length) const
    {
        if (at > m_size || length > m_size - at) return false;
        const auto got = read(at, length);
        return std::memcmp(got.data(), text, length) == 0;
    }
    // Contiguous pointer; only for in-memory images (callers that need bytes of a store use read()).
    const uint8_t *data() const
    {
        if (!m_vector) throw LogicError("NativeBytes::data() on a store-backed image");
        return m_vector->data();
    }
    // Entire image as one vector (small outputs, tests).
    std::vector<uint8_t> to_vector() const { return read(0, m_size); }

    class iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = uint8_t;
        using difference_type = std::ptrdiff_t;
        using pointer = const uint8_t *;
        using reference = uint8_t;
        iterator() = default;
        iterator(const NativeBytes *owner, size_t at) : m_owner(owner), m_at(at) {}
        uint8_t operator*() const { return (*m_owner)[m_at]; }
        uint8_t operator[](difference_type n) const { return (*m_owner)[m_at + n]; }
        iterator &operator++() { ++m_at; return *this; }
        iterator operator++(int) { auto c = *this; ++m_at; return c; }
        iterator &operator--() { --m_at; return *this; }
        iterator operator--(int) { auto c = *this; --m_at; return c; }
        iterator &operator+=(difference_type n) { m_at += n; return *this; }
        iterator &operator-=(difference_type n) { m_at -= n; return *this; }
        friend iterator operator+(iterator it, difference_type n) { it += n; return it; }
        friend iterator operator+(difference_type n, iterator it) { it += n; return it; }
        friend iterator operator-(iterator it, difference_type n) { it -= n; return it; }
        friend difference_type operator-(const iterator &a, const iterator &b) { return difference_type(a.m_at) - difference_type(b.m_at); }
        friend bool operator==(const iterator &a, const iterator &b) { return a.m_at == b.m_at; }
        friend bool operator!=(const iterator &a, const iterator &b) { return a.m_at != b.m_at; }
        friend bool operator<(const iterator &a, const iterator &b) { return a.m_at < b.m_at; }
        friend bool operator>(const iterator &a, const iterator &b) { return a.m_at > b.m_at; }
        friend bool operator<=(const iterator &a, const iterator &b) { return a.m_at <= b.m_at; }
        friend bool operator>=(const iterator &a, const iterator &b) { return a.m_at >= b.m_at; }
    private:
        const NativeBytes *m_owner = nullptr;
        size_t m_at = 0;
    };
    iterator begin() const { return {this, 0}; }
    iterator end() const { return {this, size_t(m_size)}; }

private:
    static constexpr size_t page_size = 16384;
    static constexpr size_t page_slots = 8;
    struct Page { uint64_t index; std::vector<uint8_t> data; };
    struct Cache { std::mutex mutex; std::deque<Page> pages; };
    uint8_t page_byte(size_t at) const
    {
        const uint64_t index = at / page_size;
        std::lock_guard<std::mutex> lock(m_cache->mutex);
        auto &pages = m_cache->pages;
        for (auto it = pages.begin(); it != pages.end(); ++it)
            if (it->index == index) {
                const uint8_t value = it->data[at - index * page_size];
                if (it != pages.begin()) std::rotate(pages.begin(), it, it + 1);
                return value;
            }
        const uint64_t begin = index * page_size;
        Page page{index, m_store->read(begin, std::min<uint64_t>(page_size, m_size - begin))};
        const uint8_t value = page.data[at - begin];
        pages.push_front(std::move(page));
        if (pages.size() > page_slots) pages.pop_back();
        return value;
    }
    const std::vector<uint8_t> *m_vector = nullptr;
    std::shared_ptr<const ByteStore> m_keep;
    const ByteStore *m_store = nullptr;
    uint64_t m_size = 0;
    std::shared_ptr<Cache> m_cache;
};

// An image being rewritten: the original (any NativeBytes) with point patches and appended data.
// Edits touch only table/header fields and the appended payloads, so nothing proportional to the
// file is held in RAM beyond the (spilling) tail. Also a ByteStore: a layout can verify it directly.
class PatchedImage : public ByteStore {
public:
    explicit PatchedImage(NativeBytes base) : m_base(std::move(base)), m_size(m_base.size()) {}
    uint64_t size() const { return m_size; }
    const NativeBytes &base() const { return m_base; }
    NativeBytes view() const { return NativeBytes(static_cast<const ByteStore &>(*this), m_size); }

    uint8_t get(uint64_t at) const { const auto v = read(at, 1); return v[0]; }
    struct ByteRef {
        PatchedImage &image; uint64_t at;
        operator uint8_t() const { return image.get(at); }
        ByteRef &operator=(uint8_t value) { image.set(at, value); return *this; }
    };
    ByteRef at(uint64_t index) { return {*this, index}; }
    void set(uint64_t at, uint8_t value) { check(at, 1); put(at, value); }
    void put32(uint64_t at, uint32_t value) { check(at, 4); for (unsigned n = 0; n < 4; ++n) put(at + n, uint8_t(value >> (8 * n))); }
    void put32_be(uint64_t at, uint32_t value) { check(at, 4); for (unsigned n = 0; n < 4; ++n) put(at + n, uint8_t(value >> (24 - 8 * n))); }
    void set_range(uint64_t at, const std::vector<uint8_t> &bytes) { check(at, bytes.size()); for (size_t n = 0; n < bytes.size(); ++n) put(at + n, bytes[n]); }
    uint32_t get32(uint64_t at) const { const auto v = read(at, 4); return uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24); }

    // Appends and returns the offset of the first appended byte.
    uint64_t append(const uint8_t *data, size_t length)
    {
        const uint64_t at = m_size;
        m_tail.insert(m_tail.end(), data, data + length);
        m_size += length;
        if (m_tail.size() > tail_resident_limit) flush_tail();
        return at;
    }
    uint64_t append(const std::vector<uint8_t> &bytes) { return append(bytes.data(), bytes.size()); }
    uint64_t append_from_base(uint64_t at, uint64_t length) // copy a range of the ORIGINAL image
    {
        uint64_t first = m_size;
        for (uint64_t done = 0; done < length;) {
            const uint64_t n = std::min<uint64_t>(length - done, uint64_t(4) << 20);
            const auto chunk = m_base.read(at + done, n);
            append(chunk);
            done += n;
        }
        return first;
    }
    // Copies a range of ANY image (e.g. the source of a rewrite whose layout moves: this image then is
    // built from an empty base, PatchedImage(NativeBytes())) in 4 MiB pieces; returns its new offset.
    uint64_t append_from(const NativeBytes &source, uint64_t at, uint64_t length)
    {
        const uint64_t first = m_size;
        for (uint64_t done = 0; done < length;) {
            const uint64_t n = std::min<uint64_t>(length - done, uint64_t(4) << 20);
            append(source.read(at + done, n));
            done += n;
        }
        return first;
    }
    uint64_t pending_tail() const { return m_size - m_base.size(); }

    std::vector<uint8_t> read(uint64_t at, uint64_t length) const override
    {
        if (at > m_size || length > m_size - at) throw RuntimeError("The rewritten container section lies outside the file");
        std::vector<uint8_t> out(static_cast<size_t>(length));
        const uint64_t base_size = m_base.size();
        if (at < base_size) {
            const uint64_t n = std::min<uint64_t>(length, base_size - at);
            const auto part = m_base.read(at, n);
            std::copy(part.begin(), part.end(), out.begin());
        }
        if (at + length > base_size) {
            const uint64_t from = std::max<uint64_t>(at, base_size);
            for (uint64_t cursor = from; cursor < at + length;) {
                const uint64_t tail_at = cursor - base_size;
                uint64_t n = at + length - cursor;
                if (tail_at < m_spilled) {
                    n = std::min<uint64_t>(n, m_spilled - tail_at);
                    const auto part = m_spill->read(tail_at, n);
                    std::copy(part.begin(), part.end(), out.begin() + (cursor - at));
                } else {
                    std::copy(m_tail.begin() + (tail_at - m_spilled), m_tail.begin() + (tail_at - m_spilled + n), out.begin() + (cursor - at));
                }
                cursor += n;
            }
        }
        for (auto it = m_patches.lower_bound(at); it != m_patches.end() && it->first < at + length; ++it)
            out[size_t(it->first - at)] = it->second;
        return out;
    }
    // Streams the whole image (patches applied) through `sink(const uint8_t *, size_t)`.
    template<class Sink> void stream(Sink &&sink, uint64_t chunk = uint64_t(4) << 20) const
    {
        for (uint64_t at = 0; at < m_size; at += chunk) {
            const auto part = read(at, std::min(chunk, m_size - at));
            sink(part.data(), part.size());
        }
    }
private:
    static constexpr size_t tail_resident_limit = size_t(64) << 20;
    void check(uint64_t at, uint64_t length) const
    {
        if (at > m_size || length > m_size - at) throw RuntimeError("Invalid edited container address");
    }
    void put(uint64_t at, uint8_t value)
    {
        const uint64_t base_size = m_base.size();
        if (at >= base_size && at - base_size >= m_spilled) { m_tail[size_t(at - base_size - m_spilled)] = value; return; }
        m_patches[at] = value;
    }
    void flush_tail()
    {
        if (!m_spill) m_spill = std::make_shared<HistorySpill>();
        m_spill->append(m_tail);
        m_spilled += m_tail.size();
        std::vector<uint8_t>().swap(m_tail);
    }
    NativeBytes m_base;
    uint64_t m_size = 0, m_spilled = 0;
    std::vector<uint8_t> m_tail;
    std::shared_ptr<HistorySpill> m_spill;
    std::map<uint64_t, uint8_t> m_patches;
};

// The layers a native save has to re-encode: `edited[i]` marks them and `pixels(i)` produces the edited
// mask on demand (the document's image), so a rewrite never holds every edited mask at once.
struct NativeEdits {
    std::vector<bool> edited;
    std::function<std::vector<uint8_t>(size_t)> pixels;
    bool is_edited(size_t layer) const { return layer < edited.size() && edited[layer]; }
};

// Layer payload derived from a native container: read(layer, _) decodes the layer's own RLE out of
// the source image and returns the editable PNG. Nothing is cached here (the document LRU does).
class DerivedLayerStore : public ByteStore {
public:
    using Produce = std::function<std::vector<uint8_t>(uint64_t index)>;
    explicit DerivedLayerStore(Produce produce) : m_produce(std::move(produce)) {}
    std::vector<uint8_t> read(uint64_t offset, uint64_t) const override { return m_produce(offset); }
private:
    Produce m_produce;
};
}
#endif
