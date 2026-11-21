// Manual benchmark of save_layer_project on a large layer document (Fala 45, D10).
// Hidden from the default CTest run; driven by tests/perf/project_save_benchmark.py.
//   SLA_PERF_LAYERS (256) SLA_PERF_COLS (8192) SLA_PERF_ROWS (4096)
//   SLA_PERF_KIND (shape|noise) SLA_PERF_DIR (temp dir) SLA_PERF_REPEAT (3)
// Prints one "PERF key=value ..." line per measurement.
#include <catch2/catch_test_macros.hpp>
#include <boost/filesystem.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
using namespace Slic3r;

namespace {
size_t env_size(const char *name, size_t fallback)
{
    const char *value = std::getenv(name);
    return value && *value ? size_t(std::strtoull(value, nullptr, 10)) : fallback;
}
double seconds_since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
// Model-like layer: a few ellipses/rings with anti-aliased (gray) edges, different on every layer.
std::vector<uint8_t> shape_layer(size_t cols, size_t rows, size_t index)
{
    std::vector<uint8_t> px(cols * rows, 0);
    const double cx = cols * (0.35 + 0.3 * std::fmod(index * 0.173, 1.0)), cy = rows * 0.5;
    for (int part = 0; part < 4; ++part) {
        const double ox = cx + part * cols * 0.1, rx = cols * (0.06 + 0.01 * part) + index % 17, ry = rows * 0.25;
        const double inner = (part & 1) ? 0.6 : 0.0;
        const size_t y0 = size_t(std::max(0.0, cy - ry - 2)), y1 = size_t(std::min<double>(rows, cy + ry + 2));
        for (size_t y = y0; y < y1; ++y) {
            const double dy = (double(y) - cy) / ry;
            for (size_t x = size_t(std::max(0.0, ox - rx - 2)); x < size_t(std::min<double>(cols, ox + rx + 2)); ++x) {
                const double dx = (double(x) - ox) / rx, r = std::sqrt(dx * dx + dy * dy);
                if (r <= 1.0 && r >= inner) px[y * cols + x] = 255;
                else if (r > 1.0 && r < 1.0 + 1.5 / rx) px[y * cols + x] = 128;
            }
        }
    }
    return px;
}
std::vector<uint8_t> noise_layer(size_t cols, size_t rows, size_t index)
{
    std::vector<uint8_t> px(cols * rows);
    std::mt19937 rng(uint32_t(index + 1));
    for (auto &p : px) p = uint8_t(rng());
    return px;
}
uint64_t fnv(uint64_t h, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 1099511628211ull;
    return h;
}
}

TEST_CASE("Project save of a large layer document", "[.][sla_perf_project_save]")
{
    const size_t layers = env_size("SLA_PERF_LAYERS", 256), cols = env_size("SLA_PERF_COLS", 8192),
                 rows = env_size("SLA_PERF_ROWS", 4096), repeat = env_size("SLA_PERF_REPEAT", 3);
    const std::string kind = std::getenv("SLA_PERF_KIND") ? std::getenv("SLA_PERF_KIND") : "shape";
    const boost::filesystem::path dir = std::getenv("SLA_PERF_DIR") ? boost::filesystem::path(std::getenv("SLA_PERF_DIR"))
                                                                   : boost::filesystem::temp_directory_path();
    boost::filesystem::create_directories(dir);

    auto t0 = std::chrono::steady_clock::now();
    std::vector<sla::EncodedRaster> rasters;
    rasters.reserve(layers);
    size_t encoded_bytes = 0;
    for (size_t i = 0; i < layers; ++i) {
        const auto px = kind == "noise" ? noise_layer(cols, rows, i) : shape_layer(cols, rows, i);
        rasters.push_back(sla::PNGRasterEncoder{}(px.data(), cols, rows, 1));
        encoded_bytes += rasters.back().size();
    }
    const double fixture_s = seconds_since(t0);
    std::vector<double> z;
    for (size_t i = 0; i < layers; ++i) z.push_back(0.05 * double(i + 1));
    sla::ProjectRasterState state{sla::LayerDocument::from_rasters(std::move(rasters)), z, "", {},
                                  sla::ProjectLayerOrigin::GeneratedRaster};
    std::printf("PERF fixture layers=%zu cols=%zu rows=%zu kind=%s png_bytes=%zu build_s=%.3f\n", layers, cols, rows,
                kind.c_str(), encoded_bytes, fixture_s);

    // Cost of the validation alone: one full decode of every layer (what write_project_layers does per layer).
    t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < layers; ++i) {
        png::ImageGreyscale image;
        REQUIRE(png::decode_png(state.document.encoded_layer(i), image));
    }
    std::printf("PERF decode_all_serial_s=%.3f\n", seconds_since(t0));

    const auto output = dir / "perf.sla-project";
    for (size_t r = 0; r < repeat; ++r) {
        boost::filesystem::remove(output);
        t0 = std::chrono::steady_clock::now();
        sla::save_layer_project(output.string(), state);
        const double save_s = seconds_since(t0);
        std::printf("PERF save run=%zu save_s=%.3f file_bytes=%llu\n", r, save_s,
                    (unsigned long long) boost::filesystem::file_size(output));
    }
    t0 = std::chrono::steady_clock::now();
    auto restored = sla::read_project_layers(output.string());
    const double read_s = seconds_since(t0);
    REQUIRE(restored.has_value());
    REQUIRE(restored->document.layer_count() == layers);
    // Identity evidence: hash of the decoded pixels and of the positions after the round trip.
    uint64_t pixel_hash = 1469598103934665603ull;
    for (size_t i = 0; i < layers; ++i) {
        const auto image = restored->document.image(i);
        pixel_hash = fnv(pixel_hash, image.buf.data(), image.buf.size());
    }
    std::printf("PERF read_s=%.3f pixel_hash=%016llx\n", read_s, (unsigned long long) pixel_hash);
    boost::filesystem::remove(output);
}

// Per-layer cost split of the raster stage (draw / lit count / PNG encode); manual, hidden.
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/SLA/LayerMaskValidation.hpp"
TEST_CASE("Raster stage cost split of one layer", "[.][sla_perf_raster_layer]")
{
    const size_t cols = env_size("SLA_PERF_COLS", 7680), rows = env_size("SLA_PERF_ROWS", 4320), repeat = env_size("SLA_PERF_REPEAT", 5);
    const double pitch = 218.88 / double(cols);
    ExPolygons polys;
    for (int k = 0; k < 6; ++k) {
        Polygon p;
        const double cx = (30 + 25 * k) * 1e6, cy = 61e6, r = 14e6;
        for (int a = 0; a < 90; ++a)
            p.points.emplace_back(coord_t(cx + r * std::cos(a * 6.2831853 / 90)), coord_t(cy + r * 0.8 * std::sin(a * 6.2831853 / 90)));
        polys.emplace_back(p);
    }
    for (size_t r = 0; r < repeat; ++r) {
        auto t = std::chrono::steady_clock::now();
        auto raster = sla::create_raster_grayscale_aa(sla::Resolution{cols, rows}, sla::PixelDim{pitch, pitch}, 1.0);
        const double t_create = seconds_since(t); t = std::chrono::steady_clock::now();
        for (const auto &poly : polys) raster->draw(poly);
        const double t_draw = seconds_since(t); t = std::chrono::steady_clock::now();
        double t_count = 0, t_png = 0;
        raster->encode([&](const void *ptr, size_t w, size_t h, size_t n) {
            auto t1 = std::chrono::steady_clock::now();
            const auto lit = sla::validate_and_count_mask_chunk(static_cast<const uint8_t *>(ptr), w * h, sla::LayerMaskPrecision::Gray8).nonzero_pixels;
            t_count = seconds_since(t1); t1 = std::chrono::steady_clock::now();
            auto png = sla::PNGRasterEncoder{}(ptr, w, h, n);
            t_png = seconds_since(t1);
            std::printf("PERF raster run=%zu lit=%zu png_bytes=%zu\n", r, size_t(lit), png.size());
            return png;
        });
        std::printf("PERF raster run=%zu create_s=%.4f draw_s=%.4f count_s=%.4f png_s=%.4f total_s=%.4f\n", r, t_create, t_draw, t_count, t_png, seconds_since(t));
    }
}
