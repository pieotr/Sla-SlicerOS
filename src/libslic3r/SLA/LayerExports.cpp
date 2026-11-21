// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LayerExports.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "CancellablePNGEncoder.hpp"

namespace Slic3r::sla::layerexport {

using layerops::kLastLayer;

namespace {

struct Range { size_t first, last; };

Range resolve(size_t first, size_t last_requested, size_t count)
{
    if (count == 0) throw std::invalid_argument("There are no layers.");
    const size_t last = std::min(last_requested, count - 1);
    if (first > last) throw std::invalid_argument("The layer range is empty or outside of the layers.");
    return {first, last};
}

void check_uniform(const std::vector<Mask> &layers)
{
    for (const Mask &m : layers)
        if (m.width != layers.front().width || m.height != layers.front().height || m.px.size() != m.width * m.height)
            throw std::invalid_argument("All layers must have the same resolution.");
}

// Runs fn(i) for i in [0, n) on TBB with progress + cooperative cancel; throws LayerOperationCancelled afterwards.
template<class F> void parallel_for_n(size_t n, const LayerProgress &progress, F &&fn)
{
    check_layer_progress(progress, 0, n);
    if (n == 0) return;
    std::atomic<size_t> done{0};
    std::atomic<bool>   cancelled{false};
    std::mutex          mtx;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t> &r) {
        for (size_t i = r.begin(); i != r.end(); ++i) {
            if (cancelled.load()) return;
            fn(i);
            const size_t d = ++done;
            if (progress) {
                std::lock_guard<std::mutex> lock(mtx);
                if (!progress(d, n)) cancelled = true;
            }
        }
    });
    if (cancelled) throw LayerOperationCancelled{};
}

size_t digit_count(size_t v) { size_t d = 1; while (v >= 10) { v /= 10; ++d; } return d; }

// ROI of the selection, or the whole layer. Validates the rectangle.
Rect effective_roi(const LayerSelection &sel, size_t width, size_t height)
{
    if (sel.roi.empty()) return Rect{0, 0, int(width), int(height)};
    if (sel.roi.x < 0 || sel.roi.y < 0 || size_t(sel.roi.right()) > width || size_t(sel.roi.bottom()) > height)
        throw std::invalid_argument("The region of interest is outside of the layer.");
    return sel.roi;
}
Rect effective_roi(const LayerSelection &sel, const Mask &sample) { return effective_roi(sel, sample.width, sample.height); }

// Writes `<file>.part` for every entry and renames them all once every write succeeded (or removes them on error).
class StagedFiles {
public:
    void add(const std::string &final_path, const std::vector<uint8_t> &bytes)
    {
        namespace fs = std::filesystem;
        const fs::path target(final_path);
        if (target.has_parent_path()) fs::create_directories(target.parent_path());
        const std::string part = final_path + ".part";
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Cannot write " + part);
        m_parts.emplace_back(part, final_path); // registered first so a failed write is cleaned up too
        out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
        out.close();
        if (!out) throw std::runtime_error("Cannot write " + part);
    }
    void commit()
    {
        namespace fs = std::filesystem;
        for (auto &[part, final_path] : m_parts) fs::rename(part, final_path); // replaces an existing file
        m_parts.clear();
    }
    void rollback() noexcept
    {
        std::error_code ec;
        for (auto &p : m_parts) std::filesystem::remove(p.first, ec);
        m_parts.clear();
    }
    ~StagedFiles() { rollback(); }
private:
    std::vector<std::pair<std::string, std::string>> m_parts;
};

void write_one(const std::string &file, const std::vector<uint8_t> &bytes)
{
    StagedFiles s;
    s.add(file, bytes);
    s.commit();
}

// Orient + optional crop of one layer for exporting.
Mask prepare(const Mask &layer, const Rect &roi, bool crop, const Orientation &o)
{
    Mask m = crop ? layerops::crop_mask(layer, roi) : layer;
    return layerops::orient_mask(m, o.flip, o.rotate);
}

} // namespace

// ---- PNG --------------------------------------------------------------------------------------------

std::vector<uint8_t> encode_png(const Mask &m, const std::function<void()> &cancel)
{
    if (m.px.empty()) return {};
    EncodedRaster r = encode_png_raster(m.px.data(), m.width, m.height, 1, cancel);
    const uint8_t *d = static_cast<const uint8_t *>(r.data());
    return std::vector<uint8_t>(d, d + r.size());
}

// ---- Layer images -----------------------------------------------------------------------------------

std::string layer_image_filename(const ExportImageParams &p, size_t layer_index, size_t layer_count)
{
    std::string idx = std::to_string(layer_index);
    if (p.pad_layer_index) {
        const size_t digits = digit_count(layer_count);
        if (idx.size() < digits) idx.insert(0, digits - idx.size(), '0');
    }
    return p.filename_prefix + idx + (p.type == ImageType::Png ? ".png" : ".pgm");
}

namespace {
std::vector<uint8_t> encode_image(const Mask &m, ImageType t, const std::function<void()> &cancel)
{
    if (t == ImageType::Png) return encode_png(m, cancel);
    const std::string header = "P5\n" + std::to_string(m.width) + " " + std::to_string(m.height) + "\n255\n";
    std::vector<uint8_t> out(header.begin(), header.end());
    out.insert(out.end(), m.px.begin(), m.px.end());
    return out;
}
} // namespace

std::vector<ExportedFile> render_layer_images(const std::vector<Mask> &layers, const ExportImageParams &p, const LayerProgress &progress)
{
    check_uniform(layers);
    return render_layer_images(layerops::batch_layout(layers), p, progress);
}

std::vector<ExportedFile> render_layer_images(const StreamLayout &layers, const ExportImageParams &p, const LayerProgress &progress)
{
    const Range r = resolve(p.selection.first_layer, p.selection.last_layer, layers.layer_count);
    const Rect roi = effective_roi(p.selection, layers.width, layers.height);
    std::vector<ExportedFile> out(r.last - r.first + 1);
    parallel_for_n(out.size(), progress, [&](size_t k) {
        const size_t i = r.first + k;
        out[k].name = layer_image_filename(p, i, layers.layer_count);
        out[k].data = encode_image(prepare(*layers.fetch(i), roi, p.crop_by_roi, p.orientation), p.type, {});
        if (out[k].data.empty()) throw std::runtime_error("Cannot encode layer " + std::to_string(i));
    });
    return out;
}

size_t export_layer_images(const std::vector<Mask> &layers, const ExportImageParams &p, const std::string &folder, const LayerProgress &progress)
{
    check_uniform(layers);
    return export_layer_images(layerops::batch_layout(layers), p, folder, progress);
}

size_t export_layer_images(const StreamLayout &layers, const ExportImageParams &p, const std::string &folder, const LayerProgress &progress)
{
    // Rendered in batches so memory stays bounded for thousands of layers; nothing becomes visible before commit().
    const Range r = resolve(p.selection.first_layer, p.selection.last_layer, layers.layer_count);
    StagedFiles staged;
    const size_t total = r.last - r.first + 1, batch = 64;
    size_t done = 0;
    check_layer_progress(progress, 0, total);
    for (size_t b = r.first; b <= r.last; b += batch) {
        ExportImageParams bp = p;
        bp.selection.first_layer = b;
        bp.selection.last_layer = std::min(r.last, b + batch - 1);
        const size_t base = done;
        const std::vector<ExportedFile> files = render_layer_images(layers, bp, [&](size_t d, size_t) { return !progress || progress(base + d, total); });
        for (const ExportedFile &f : files) staged.add((std::filesystem::path(folder) / f.name).string(), f.data);
        done += files.size();
        check_layer_progress(progress, done, total);
    }
    staged.commit();
    return total;
}

// ---- GIF --------------------------------------------------------------------------------------------

namespace {

// Variable width LZW as in GIF89a (min code size 8 for 256 colour frames). The dictionary is an open addressing
// hash on (prefix code, byte); a generation stamp avoids clearing it when the code table is reset.
class LzwWriter {
public:
    explicit LzwWriter(std::vector<uint8_t> &out) : m_out(out) {}

    void encode(const uint8_t *px, size_t n, int min_code_size)
    {
        m_out.push_back(uint8_t(min_code_size));
        const unsigned clear = 1u << min_code_size, eoi = clear + 1;
        reset(min_code_size);
        emit(clear);
        if (n > 0) {
            uint32_t cur = px[0];
            for (size_t i = 1; i < n; ++i) {
                const uint32_t c = px[i];
                const uint32_t key = (cur << 8) | c;
                const int found = lookup(key);
                if (found >= 0) { cur = uint32_t(found); continue; }
                emit(cur);
                insert(key, m_next++);
                // The decoder is one entry behind the encoder, so the width grows one code later than the table.
                if (m_next > (1u << m_size) && m_size < 12) ++m_size;
                if (m_next >= 4096) { // table full: tell the decoder to start over
                    emit(clear);
                    reset(min_code_size);
                }
                cur = c;
            }
            emit(cur);
            // The decoder adds an entry for this last code as well, which can widen its next code (the EOI).
            if (m_next + 1 > (1u << m_size) && m_size < 12) ++m_size;
        }
        emit(eoi);
        if (m_nbits > 0) push_byte(uint8_t(m_bits & 0xFFu));
        flush_block();
        m_out.push_back(0); // block terminator
    }

private:
    struct Slot { uint32_t key = 0; uint16_t code = 0; uint32_t gen = 0; };
    static constexpr size_t kSlots = 1u << 14;

    void reset(int min_code_size)
    {
        ++m_gen;
        m_size = min_code_size + 1;
        m_next = (1u << min_code_size) + 2;
        if (m_slots.empty()) m_slots.resize(kSlots);
    }
    int lookup(uint32_t key) const
    {
        size_t h = (key * 2654435761u) >> 18 & (kSlots - 1);
        while (m_slots[h].gen == m_gen) {
            if (m_slots[h].key == key) return m_slots[h].code;
            h = (h + 1) & (kSlots - 1);
        }
        return -1;
    }
    void insert(uint32_t key, uint32_t code)
    {
        size_t h = (key * 2654435761u) >> 18 & (kSlots - 1);
        while (m_slots[h].gen == m_gen) h = (h + 1) & (kSlots - 1);
        m_slots[h] = {key, uint16_t(code), m_gen};
    }
    void emit(uint32_t code)
    {
        m_bits |= uint64_t(code) << m_nbits;
        m_nbits += m_size;
        while (m_nbits >= 8) { push_byte(uint8_t(m_bits & 0xFFu)); m_bits >>= 8; m_nbits -= 8; }
    }
    void push_byte(uint8_t b)
    {
        m_block.push_back(b);
        if (m_block.size() == 255) flush_block();
    }
    void flush_block()
    {
        if (m_block.empty()) return;
        m_out.push_back(uint8_t(m_block.size()));
        m_out.insert(m_out.end(), m_block.begin(), m_block.end());
        m_block.clear();
    }

    std::vector<uint8_t> &m_out;
    std::vector<uint8_t>  m_block;
    std::vector<Slot>     m_slots;
    uint32_t m_gen = 0, m_next = 0;
    int      m_size = 9, m_nbits = 0;
    uint64_t m_bits = 0;
};

void put16(std::vector<uint8_t> &o, uint16_t v) { o.push_back(uint8_t(v & 0xFF)); o.push_back(uint8_t(v >> 8)); }

} // namespace

std::vector<uint8_t> encode_gif(const std::vector<Mask> &frames, uint16_t delay_cs, uint16_t repeats)
{
    if (frames.empty()) throw std::invalid_argument("A GIF needs at least one frame.");
    const size_t w = frames.front().width, h = frames.front().height;
    if (w == 0 || h == 0 || w > 65535 || h > 65535) throw std::invalid_argument("Invalid GIF frame size.");
    for (const Mask &f : frames) if (f.width != w || f.height != h || f.px.size() != w * h) throw std::invalid_argument("All GIF frames must have the same size.");

    std::vector<uint8_t> o;
    const char sig[] = "GIF89a";
    o.insert(o.end(), sig, sig + 6);
    put16(o, uint16_t(w)); put16(o, uint16_t(h));
    o.push_back(0xF7); // global colour table, 8 bit colour resolution, 256 entries
    o.push_back(0);    // background colour index
    o.push_back(0);    // pixel aspect ratio
    for (int i = 0; i < 256; ++i) { o.push_back(uint8_t(i)); o.push_back(uint8_t(i)); o.push_back(uint8_t(i)); } // grey ramp
    // NETSCAPE2.0 application extension: loop count (0 = forever).
    const uint8_t app[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01};
    o.insert(o.end(), app, app + sizeof(app));
    put16(o, repeats); o.push_back(0);
    for (const Mask &f : frames) {
        // Graphic control extension: disposal 2 (restore to background), no transparency.
        const uint8_t gce[] = {0x21, 0xF9, 0x04, 0x08};
        o.insert(o.end(), gce, gce + sizeof(gce));
        put16(o, delay_cs); o.push_back(0); o.push_back(0);
        o.push_back(0x2C); put16(o, 0); put16(o, 0); put16(o, uint16_t(w)); put16(o, uint16_t(h));
        o.push_back(0); // no local colour table, not interlaced
        LzwWriter(o).encode(f.px.data(), f.px.size(), 8);
    }
    o.push_back(0x3B);
    return o;
}

size_t gif_frame_count(const ExportGifParams &p, size_t layer_count)
{
    if (layer_count == 0) return 0;
    const size_t last = std::min(p.selection.last_layer, layer_count - 1);
    if (p.selection.first_layer > last) return 0;
    return size_t(float(last - p.selection.first_layer + 1) / float(p.skip + 1)); // UVtools: (uint)(count / (float)(skip + 1))
}

std::string validate_export_gif(const ExportGifParams &p, size_t layer_count)
{
    std::string sb;
    if (gif_frame_count(p, layer_count) == 0) sb += "There are no layers to pack, please adjust the configurations.\n";
    if (p.fps == 0 || p.fps > 100) sb += "FPS must be between 1 and 100.\n";
    if (p.scale_percent <= 0) sb += "Scale must be positive.\n";
    return sb;
}

std::vector<uint8_t> render_layer_gif(const std::vector<Mask> &layers, const ExportGifParams &p, const LayerProgress &progress)
{
    check_uniform(layers);
    return render_layer_gif(layerops::batch_layout(layers), p, progress);
}

std::vector<uint8_t> render_layer_gif(const StreamLayout &layers, const ExportGifParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_export_gif(p, layers.layer_count); !err.empty()) throw std::invalid_argument(err);
    const Range r = resolve(p.selection.first_layer, p.selection.last_layer, layers.layer_count);
    const Rect roi = p.clip_by_volume_bounds ? layerops::scan_model_bounds(layers) : effective_roi(p.selection, layers.width, layers.height);
    if (roi.empty()) throw std::invalid_argument("The model has no pixels to clip to.");
    const size_t frames = gif_frame_count(p, layers.layer_count);
    const size_t fw = std::max<size_t>(1, size_t(double(roi.w) * p.scale_percent / 100.0)), fh = std::max<size_t>(1, size_t(double(roi.h) * p.scale_percent / 100.0));
    std::vector<Mask> out(frames);
    parallel_for_n(frames, progress, [&](size_t k) {
        Mask m = layerops::crop_mask(*layers.fetch(r.first + k * (p.skip + 1)), roi);
        if (p.scale_percent != 100) m = layerops::resize_mask_bilinear(m, fw, fh);
        out[k] = layerops::orient_mask(m, p.orientation.flip, p.orientation.rotate);
    });
    const unsigned ms = 1000u / std::max(1u, p.fps);
    return encode_gif(out, uint16_t(ms / 10), uint16_t(p.repeats));
}

void export_layer_gif(const std::vector<Mask> &layers, const ExportGifParams &p, const std::string &file, const LayerProgress &progress)
{
    write_one(file, render_layer_gif(layers, p, progress));
}
void export_layer_gif(const StreamLayout &layers, const ExportGifParams &p, const std::string &file, const LayerProgress &progress)
{
    write_one(file, render_layer_gif(layers, p, progress));
}

// ---- Heat map ---------------------------------------------------------------------------------------

std::string validate_export_heat_map(const ExportHeatMapParams &p, size_t layer_count)
{
    if (layer_count == 0) return "There are no layers.\n";
    const size_t last = std::min(p.selection.last_layer, layer_count - 1);
    if (p.selection.first_layer >= last) return "To generate a heat map at least two layers are required.\n";
    return {};
}

Mask render_heat_map(const std::vector<Mask> &layers, const std::vector<double> &z_mm, const ExportHeatMapParams &p, const LayerProgress &progress)
{
    check_uniform(layers);
    return render_heat_map(layerops::batch_layout(layers, &z_mm), z_mm, p, progress);
}

Mask render_heat_map(const StreamLayout &layers, const std::vector<double> &z_mm, const ExportHeatMapParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_export_heat_map(p, layers.layer_count); !err.empty()) throw std::invalid_argument(err);
    const Range r = resolve(p.selection.first_layer, p.selection.last_layer, layers.layer_count);
    const Rect roi = effective_roi(p.selection, layers.width, layers.height);

    // Layers that share a Z (e.g. phased exposure copies) count once, merged with a per-pixel maximum.
    struct Item { size_t first, last; };
    std::vector<Item> items;
    if (p.merge_same_positioned_layers) {
        if (z_mm.size() != layers.layer_count) throw std::invalid_argument("Merging same positioned layers needs the Z of every layer.");
        for (size_t i = r.first; i <= r.last;) {
            size_t e = i;
            while (e + 1 < layers.layer_count && z_mm[e + 1] == z_mm[i]) ++e; // UVtools merges past the range end as well
            items.push_back({i, e});
            i = e + 1;
        }
    } else {
        for (size_t i = r.first; i <= r.last; ++i) items.push_back({i, i});
    }

    std::vector<uint32_t> sum(size_t(roi.w) * size_t(roi.h), 0);
    std::mutex mtx;
    parallel_for_n(items.size(), progress, [&](size_t k) {
        std::vector<uint8_t> merged(sum.size(), 0);
        for (size_t li = items[k].first; li <= items[k].last; ++li) {
            const auto layer = layers.fetch(li);
            for (int y = 0; y < roi.h; ++y)
                for (int x = 0; x < roi.w; ++x) {
                    uint8_t &dst = merged[size_t(y) * size_t(roi.w) + size_t(x)];
                    dst = std::max(dst, layer->px[size_t(roi.y + y) * layer->width + size_t(roi.x + x)]);
                }
        }
        std::lock_guard<std::mutex> lock(mtx);
        for (size_t i = 0; i < sum.size(); ++i) sum[i] += merged[i];
    });

    Mask heat(size_t(roi.w), size_t(roi.h));
    const double scale = 1.0 / double(items.size());
    for (size_t i = 0; i < sum.size(); ++i) heat.px[i] = uint8_t(std::min(255L, std::lrint(double(sum[i]) * scale))); // cv: round half to even
    // The ROI was already applied; crop_by_roi=false pastes the result back into a full size canvas.
    if (!p.crop_by_roi && !p.selection.roi.empty()) {
        Mask full(layers.width, layers.height);
        for (int y = 0; y < roi.h; ++y)
            std::memcpy(&full.px[size_t(roi.y + y) * full.width + size_t(roi.x)], &heat.px[size_t(y) * heat.width], heat.width);
        heat = std::move(full);
    }
    return layerops::orient_mask(heat, p.orientation.flip, p.orientation.rotate);
}

void export_heat_map(const std::vector<Mask> &layers, const std::vector<double> &z_mm, const ExportHeatMapParams &p, const std::string &file,
                     const LayerProgress &progress)
{
    write_one(file, encode_png(render_heat_map(layers, z_mm, p, progress)));
}
void export_heat_map(const StreamLayout &layers, const std::vector<double> &z_mm, const ExportHeatMapParams &p, const std::string &file,
                     const LayerProgress &progress)
{
    write_one(file, encode_png(render_heat_map(layers, z_mm, p, progress)));
}

// ---- Skeleton ---------------------------------------------------------------------------------------

Mask skeletonize(const Mask &src, const std::function<void()> &cancel)
{
    Mask skeleton(src.width, src.height);
    Mask image = src;
    while (true) {
        if (cancel) cancel();
        const Mask eroded = layerops::erode_mask(image, 1);
        const Mask opened = layerops::dilate_mask(eroded, 1);
        // skeleton |= image - open(image): the thin parts the opening removes
        for (size_t i = 0; i < skeleton.px.size(); ++i) skeleton.px[i] |= uint8_t(image.px[i] > opened.px[i] ? image.px[i] - opened.px[i] : 0);
        bool any = false;
        for (uint8_t v : eroded.px) if (v) { any = true; break; }
        if (!any) break;
        // The erosion treats the outside as "not eroding", so a mask that fills the whole frame never shrinks;
        // UVtools would spin forever there, we stop as soon as an iteration changes nothing.
        if (eroded == image) break;
        image = eroded;
    }
    return skeleton;
}

Mask render_skeleton(const std::vector<Mask> &layers, const ExportSkeletonParams &p, const LayerProgress &progress)
{
    check_uniform(layers);
    return render_skeleton(layerops::batch_layout(layers), p, progress);
}

Mask render_skeleton(const StreamLayout &layers, const ExportSkeletonParams &p, const LayerProgress &progress)
{
    const Range r = resolve(p.selection.first_layer, p.selection.last_layer, layers.layer_count);
    const Rect roi = effective_roi(p.selection, layers.width, layers.height);
    Mask sum(size_t(roi.w), size_t(roi.h));
    std::mutex mtx;
    std::atomic<bool> cancelled{false};
    parallel_for_n(r.last - r.first + 1, progress, [&](size_t k) {
        const Mask sk = skeletonize(layerops::crop_mask(*layers.fetch(r.first + k), roi), [&]() { if (cancelled) throw LayerOperationCancelled{}; });
        std::lock_guard<std::mutex> lock(mtx);
        for (size_t i = 0; i < sum.px.size(); ++i) sum.px[i] = uint8_t(std::min(255, sum.px[i] + sk.px[i])); // CvInvoke.Add saturates
    });
    if (!p.crop_by_roi && !p.selection.roi.empty()) {
        Mask full(layers.width, layers.height);
        for (int y = 0; y < roi.h; ++y)
            std::memcpy(&full.px[size_t(roi.y + y) * full.width + size_t(roi.x)], &sum.px[size_t(y) * sum.width], sum.width);
        return full;
    }
    return sum;
}

void export_skeleton(const std::vector<Mask> &layers, const ExportSkeletonParams &p, const std::string &file, const LayerProgress &progress)
{
    write_one(file, encode_png(render_skeleton(layers, p, progress)));
}
void export_skeleton(const StreamLayout &layers, const ExportSkeletonParams &p, const std::string &file, const LayerProgress &progress)
{
    write_one(file, encode_png(render_skeleton(layers, p, progress)));
}

// ---- HTML -------------------------------------------------------------------------------------------

std::string html_escape(const std::string &s)
{
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\'': o += "&#39;"; break;
        default: o += c;
        }
    }
    return o;
}

std::string base64_encode(const std::vector<uint8_t> &d)
{
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((d.size() + 2) / 3 * 4);
    for (size_t i = 0; i < d.size(); i += 3) {
        const uint32_t v = (uint32_t(d[i]) << 16) | (i + 1 < d.size() ? uint32_t(d[i + 1]) << 8 : 0) | (i + 2 < d.size() ? d[i + 2] : 0);
        o += tbl[(v >> 18) & 63]; o += tbl[(v >> 12) & 63];
        o += i + 1 < d.size() ? tbl[(v >> 6) & 63] : '=';
        o += i + 2 < d.size() ? tbl[v & 63] : '=';
    }
    return o;
}

namespace {
std::string num(double v, int decimals = 2)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}
} // namespace

std::string render_html(const std::vector<Mask> &layers, const std::vector<double> &z_mm, const std::vector<LayerParams> &params,
                        const ExportHtmlParams &p, const LayerProgress &progress)
{
    check_uniform(layers);
    return render_html(layerops::batch_layout(layers), z_mm, params, p, progress);
}

std::string render_html(const StreamLayout &layers, const std::vector<double> &z_mm, const std::vector<LayerParams> &params,
                        const ExportHtmlParams &p, const LayerProgress &progress)
{
    std::string h;
    h += "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    h += "<title>" + html_escape(p.title) + "</title>\n<style>\n"
         "body{font:14px/1.4 system-ui,sans-serif;margin:0;padding:16px;background:#fff;color:#222}\n"
         "h1{font-size:20px}h2{font-size:16px;margin-top:24px}\n"
         "table{border-collapse:collapse}td,th{border:1px solid #ccc;padding:2px 8px;text-align:right}th{background:#eee}td:first-child,th:first-child{text-align:left}\n"
         ".wrap{overflow-x:auto;max-width:100%}pre{background:#f4f4f4;padding:8px;overflow:auto;max-height:400px}\n"
         "#view img{max-width:100%;image-rendering:pixelated;background:#000}\n"
         "@media (prefers-color-scheme:dark){body{background:#111;color:#ddd}th{background:#222}td,th{border-color:#444}pre{background:#1c1c1c}}\n"
         "</style>\n</head>\n<body>\n<h1>" + html_escape(p.title) + "</h1>\n";

    if (!p.thumbnails_png.empty()) {
        h += "<div>";
        for (const auto &t : p.thumbnails_png)
            h += "<img alt=\"" + html_escape(t.first) + "\" src=\"data:image/png;base64," + base64_encode(t.second) + "\"> ";
        h += "</div>\n";
    }
    if (p.export_properties && !p.properties.empty()) {
        h += "<h2>Properties</h2>\n<div class=\"wrap\"><table>\n";
        for (const auto &kv : p.properties) h += "<tr><td>" + html_escape(kv.first) + "</td><td>" + html_escape(kv.second) + "</td></tr>\n";
        h += "</table></div>\n";
    }
    if (p.export_layer_settings && !params.empty()) {
        if (z_mm.size() != params.size()) throw std::invalid_argument("Every layer needs a Z position.");
        h += "<h2>Layers</h2>\n<div class=\"wrap\"><table>\n<tr><th>Layer</th><th>Z (mm)</th><th>Exposure (s)</th><th>Wait before cure (s)</th>"
             "<th>Wait after cure (s)</th><th>Lift (mm)</th><th>Lift speed</th><th>Retract speed</th><th>Wait after lift (s)</th><th>PWM</th></tr>\n";
        for (size_t i = 0; i < params.size(); ++i) {
            const LayerParams &l = params[i];
            h += "<tr><td>" + std::to_string(i) + (l.is_bottom ? " (bottom)" : "") + "</td><td>" + num(z_mm[i], 3) + "</td><td>" + num(l.exposure_s) + "</td><td>" +
                 num(l.wait_before_cure_s) + "</td><td>" + num(l.wait_after_cure_s) + "</td><td>" + num(l.lift_height_total()) + "</td><td>" + num(l.lift_speed) +
                 "</td><td>" + num(l.retract_speed) + "</td><td>" + num(l.wait_after_lift_s) + "</td><td>" + std::to_string(l.light_pwm) + "</td></tr>\n";
        }
        h += "</table></div>\n";
    }
    if (p.export_gcode && !p.gcode.empty()) h += "<h2>GCode</h2>\n<pre>" + html_escape(p.gcode) + "</pre>\n";

    if (p.export_layer_preview && layers.layer_count != 0) {
        const Range r = resolve(p.preview_first, p.preview_last, layers.layer_count);
        const size_t step = std::max<size_t>(1, p.preview_step);
        const Rect bounds = layerops::scan_model_bounds(layers); // crop to the model so the previews stay small
        std::vector<size_t> picked;
        for (size_t i = r.first; i <= r.last; i += step) picked.push_back(i);
        std::vector<std::string> encoded(picked.size());
        parallel_for_n(picked.size(), progress, [&](size_t k) {
            const auto layer = layers.fetch(picked[k]);
            Mask m = bounds.empty() ? Mask(*layer) : layerops::crop_mask(*layer, bounds);
            if (p.preview_scale_percent != 100)
                m = layerops::resize_mask_bilinear(m, std::max<size_t>(1, size_t(double(m.width) * p.preview_scale_percent / 100.0)),
                                                   std::max<size_t>(1, size_t(double(m.height) * p.preview_scale_percent / 100.0)));
            m = layerops::orient_mask(m, p.orientation.flip, p.orientation.rotate);
            encoded[k] = base64_encode(encode_png(m));
        });
        h += "<h2>Preview</h2>\n<div id=\"view\"><input id=\"slider\" type=\"range\" min=\"0\" max=\"" + std::to_string(picked.size() - 1) +
             "\" value=\"0\" style=\"width:100%\"><div>Layer <span id=\"idx\"></span></div><img id=\"img\" alt=\"layer preview\"></div>\n<script>\nconst L=[";
        for (size_t k = 0; k < picked.size(); ++k) h += (k ? "," : "") + std::string("[") + std::to_string(picked[k]) + ",\"" + encoded[k] + "\"]";
        h += "];\nconst s=document.getElementById('slider'),i=document.getElementById('img'),n=document.getElementById('idx');\n"
             "function show(){const e=L[+s.value];i.src='data:image/png;base64,'+e[1];n.textContent=e[0];}\n"
             "s.addEventListener('input',show);show();\n</script>\n";
    }
    h += "</body>\n</html>\n";
    return h;
}

void export_html(const std::vector<Mask> &layers, const std::vector<double> &z_mm, const std::vector<LayerParams> &params,
                 const ExportHtmlParams &p, const std::string &file, const LayerProgress &progress)
{
    const std::string html = render_html(layers, z_mm, params, p, progress);
    write_one(file, std::vector<uint8_t>(html.begin(), html.end()));
}
void export_html(const StreamLayout &layers, const std::vector<double> &z_mm, const std::vector<LayerParams> &params,
                 const ExportHtmlParams &p, const std::string &file, const LayerProgress &progress)
{
    const std::string html = render_html(layers, z_mm, params, p, progress);
    write_one(file, std::vector<uint8_t>(html.begin(), html.end()));
}

// ---- Mesh -------------------------------------------------------------------------------------------

std::string validate_export_mesh(const ExportMeshParams &p, size_t layer_count)
{
    std::string sb;
    if (layer_count == 0 || p.selection.first_layer >= layer_count) sb += "The layer range is empty or outside of the layers.\n";
    if (p.quality == 0) sb += "Quality must be at least 1.\n";
    return sb;
}

VoxelMesh build_voxel_mesh(const std::vector<Mask> &layers, const std::vector<double> &z_mm, const ExportMeshParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_export_mesh(p, layers.size()); !err.empty()) throw std::invalid_argument(err);
    check_uniform(layers);
    return build_voxel_mesh(layerops::batch_layout(layers), z_mm, p, progress);
}

VoxelMesh build_voxel_mesh(const StreamLayout &layers, const std::vector<double> &z_mm, const ExportMeshParams &p, const LayerProgress &progress)
{
    if (const std::string err = validate_export_mesh(p, layers.layer_count); !err.empty()) throw std::invalid_argument(err);
    if (z_mm.size() != layers.layer_count) throw std::invalid_argument("Every layer needs a Z position.");
    const Range r = resolve(p.selection.first_layer, p.selection.last_layer, layers.layer_count);
    const unsigned q = p.quality;
    const double pw = (p.pixel_width_mm > 0 ? p.pixel_width_mm : 0.035) * q, ph = (p.pixel_height_mm > 0 ? p.pixel_height_mm : 0.035) * q;

    // Layers with the same Z form one slab (their pixels are merged with a maximum). A slab mask is rebuilt from the
    // layers whenever it is needed (twice: model bounds, then occupancy), so only the occupancy grid stays in memory.
    struct Slab { size_t first, last; double top; };
    std::vector<Slab> slabs;
    for (size_t i = r.first; i <= r.last;) {
        size_t e = i;
        while (e + 1 <= r.last && z_mm[e + 1] == z_mm[i]) ++e;
        slabs.push_back({i, e, z_mm[i]});
        i = e + 1;
    }
    const auto slab_mask = [&](const Slab &s) {
        Mask m = layerops::orient_mask(*layers.fetch(s.first), p.orientation.flip, p.orientation.rotate);
        for (size_t k = s.first + 1; k <= s.last; ++k) {
            const Mask o = layerops::orient_mask(*layers.fetch(k), p.orientation.flip, p.orientation.rotate);
            for (size_t j = 0; j < m.px.size(); ++j) m.px[j] = std::max(m.px[j], o.px[j]);
        }
        return m;
    };
    const auto half = [&](size_t base) { return [&progress, base, n = slabs.size()](size_t d, size_t) { return !progress || progress(base + d, n * 2); }; };
    check_layer_progress(progress, 0, slabs.size() * 2);

    // Model bounds over all slabs.
    Rect bounds;
    {
        std::mutex mtx;
        parallel_for_n(slabs.size(), half(0), [&](size_t k) {
            const Rect b = layerops::bounding_rect(slab_mask(slabs[k]));
            if (b.empty()) return;
            std::lock_guard<std::mutex> lock(mtx);
            if (bounds.empty()) { bounds = b; return; }
            const int x0 = std::min(bounds.x, b.x), y0 = std::min(bounds.y, b.y), x1 = std::max(bounds.right(), b.right()), y1 = std::max(bounds.bottom(), b.bottom());
            bounds = {x0, y0, x1 - x0, y1 - y0};
        });
    }
    VoxelMesh mesh;
    if (bounds.empty()) return mesh;

    const size_t nx = (size_t(bounds.w) + q - 1) / q, ny = (size_t(bounds.h) + q - 1) / q, nz = slabs.size();
    std::vector<uint8_t> occ(nx * ny * nz, 0);
    auto at = [&](long x, long y, long z) -> bool {
        return x >= 0 && y >= 0 && z >= 0 && size_t(x) < nx && size_t(y) < ny && size_t(z) < nz && occ[(size_t(z) * ny + size_t(y)) * nx + size_t(x)];
    };
    std::atomic<size_t> voxels{0};
    parallel_for_n(nz, half(nz), [&](size_t k) {
        const Mask c = layerops::crop_mask(slab_mask(slabs[k]), bounds);
        size_t lit = 0;
        for (size_t y = 0; y < ny; ++y)
            for (size_t x = 0; x < nx; ++x) {
                // Box (area) average of the q x q block, solid when more than half lit. Image y points down, mesh y up.
                unsigned sum = 0;
                for (size_t dy = 0; dy < q && y * q + dy < c.height; ++dy)
                    for (size_t dx = 0; dx < q && x * q + dx < c.width; ++dx) sum += c.px[(y * q + dy) * c.width + x * q + dx];
                if (double(sum) / double(q * q) > 127.0) { occ[(k * ny + (ny - 1 - y)) * nx + x] = 1; ++lit; }
            }
        voxels += lit;
    });
    mesh.voxels = voxels;

    // Z of the voxel boundaries: bottom of the first slab is the Z of the layer below the range (0 for layer 0).
    std::vector<double> zb(nz + 1);
    zb[0] = r.first == 0 ? 0.0 : z_mm[r.first - 1];
    for (size_t k = 0; k < nz; ++k) zb[k + 1] = slabs[k].top;

    const size_t dims[3] = {nx, ny, nz};
    auto coord = [&](int axis, size_t i) -> double { return axis == 0 ? double(i) * pw : axis == 1 ? double(i) * ph : zb[i]; };
    // For the face axis a, (w,u,v) = (a, a+1, a+2 mod 3): u x v = +w so (p00,p10,p11) is counter clockwise seen from +w.
    for (int a = 0; a < 3; ++a) {
        const int ua = (a + 1) % 3, va = (a + 2) % 3;
        const size_t U = dims[ua], V = dims[va], W = dims[a];
        for (int sign = 1; sign >= -1; sign -= 2) {
            for (size_t w = 0; w <= W; ++w) {
                std::vector<uint8_t> cell(U * V, 0);
                bool any = false;
                for (size_t v = 0; v < V; ++v)
                    for (size_t u = 0; u < U; ++u) {
                        long c[3];
                        auto voxel = [&](long dw) { c[a] = long(w) + dw; c[ua] = long(u); c[va] = long(v); return at(c[0], c[1], c[2]); };
                        const bool below = voxel(-1), above = voxel(0);
                        // +w face: solid below the plane, empty above. -w face: the opposite.
                        const bool face = sign > 0 ? (below && !above) : (!below && above);
                        if (face) { cell[v * U + u] = 1; any = true; }
                    }
                if (!any) continue;
                std::vector<uint8_t> used(U * V, 0);
                for (size_t v = 0; v < V; ++v)
                    for (size_t u = 0; u < U; ++u) {
                        if (!cell[v * U + u] || used[v * U + u]) continue;
                        size_t uw = 1, vh = 1;
                        if (p.merge_faces) {
                            while (u + uw < U && cell[v * U + u + uw] && !used[v * U + u + uw]) ++uw;
                            for (bool grow = true; grow && v + vh < V;) {
                                for (size_t k = 0; k < uw; ++k) if (!cell[(v + vh) * U + u + k] || used[(v + vh) * U + u + k]) { grow = false; break; }
                                if (grow) ++vh;
                            }
                        }
                        for (size_t dv = 0; dv < vh; ++dv) for (size_t du = 0; du < uw; ++du) used[(v + dv) * U + u + du] = 1;
                        const double u0 = coord(ua, u), u1 = coord(ua, u + uw), v0 = coord(va, v), v1 = coord(va, v + vh), wc = coord(a, w);
                        auto vert = [&](double uu, double vv, float *out) { double c[3]; c[a] = wc; c[ua] = uu; c[va] = vv; for (int i = 0; i < 3; ++i) out[i] = float(c[i]); };
                        float p00[3], p10[3], p11[3], p01[3];
                        vert(u0, v0, p00); vert(u1, v0, p10); vert(u1, v1, p11); vert(u0, v1, p01);
                        Triangle t1{}, t2{};
                        t1.n[a] = t2.n[a] = float(sign);
                        auto set = [](Triangle &t, const float *x, const float *y, const float *z) { std::memcpy(t.v[0], x, 12); std::memcpy(t.v[1], y, 12); std::memcpy(t.v[2], z, 12); };
                        if (sign > 0) { set(t1, p00, p10, p11); set(t2, p00, p11, p01); }
                        else          { set(t1, p00, p11, p10); set(t2, p00, p01, p11); }
                        mesh.triangles.push_back(t1); mesh.triangles.push_back(t2);
                    }
            }
        }
        check_layer_progress(progress, slabs.size() + (slabs.size() * size_t(a + 1)) / 3, slabs.size() * 2);
    }
    return mesh;
}

std::vector<uint8_t> encode_binary_stl(const VoxelMesh &m, const std::string &header)
{
    std::vector<uint8_t> o(80, 0);
    std::memcpy(o.data(), header.data(), std::min<size_t>(80, header.size()));
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i))); }; // little endian on any host
    auto f32 = [&](float f) { uint32_t v; std::memcpy(&v, &f, 4); u32(v); };
    u32(uint32_t(m.triangles.size()));
    o.reserve(o.size() + m.triangles.size() * 50);
    for (const Triangle &t : m.triangles) {
        for (int i = 0; i < 3; ++i) f32(t.n[i]);
        for (int v = 0; v < 3; ++v) for (int i = 0; i < 3; ++i) f32(t.v[v][i]);
        o.push_back(0); o.push_back(0); // attribute byte count
    }
    return o;
}

void export_mesh(const StreamLayout &layers, const std::vector<double> &z_mm, const ExportMeshParams &p, const std::string &file, const LayerProgress &progress)
{
    const VoxelMesh mesh = build_voxel_mesh(layers, z_mm, p, progress);
    write_one(file, encode_binary_stl(mesh));
}

void export_mesh(const std::vector<Mask> &layers, const std::vector<double> &z_mm, const ExportMeshParams &p, const std::string &file, const LayerProgress &progress)
{
    write_one(file, encode_binary_stl(build_voxel_mesh(layers, z_mm, p, progress)));
}

} // namespace Slic3r::sla::layerexport
