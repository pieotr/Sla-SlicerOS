#include "LayerMemoryBudget.hpp"
#include "libslic3r/Exception.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Slic3r::sla {
namespace {
constexpr uint64_t MiB = uint64_t(1) << 20, GiB = uint64_t(1) << 30;
std::mutex g_override_mutex;
std::optional<LayerBudgetOverride> g_override;
std::atomic<size_t> g_spill_files{0};

uint64_t query_available_memory()
{
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? uint64_t(status.ullAvailPhys) : 0;
#else
    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    uint64_t value = 0;
    std::string unit;
    while (meminfo >> key >> value >> unit)
        if (key == "MemAvailable:") return value * 1024;
    const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && size > 0 ? uint64_t(pages) * uint64_t(size) / 2 : 0;
#endif
}

std::optional<uint64_t> env_mib(const char *name)
{
    const char *text = std::getenv(name);
    if (!text || !*text) return std::nullopt;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end) return std::nullopt;
    return uint64_t(value) * MiB;
}
std::optional<LayerBudgetOverride> current_override()
{
    std::lock_guard<std::mutex> lock(g_override_mutex);
    return g_override;
}
}

uint64_t layer_available_memory()
{
    // Budgets are queried per layer while loading; re-reading the OS counter every call is wasteful.
    static std::atomic<int64_t> stamp{0};
    static std::atomic<uint64_t> cached{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() + 1;
    if (const int64_t then = stamp.load(); then != 0 && now - then < 1000) return cached.load();
    const uint64_t value = query_available_memory();
    cached = value;
    stamp = now;
    return value;
}
uint64_t layer_editor_byte_budget()
{
    if (const auto o = current_override(); o && o->editor_bytes) return *o->editor_bytes;
    if (const auto e = env_mib("SLA_LAYER_EDITOR_BUDGET_MB")) return *e;
    return std::clamp(layer_available_memory() / 3, GiB, 64 * GiB);
}
uint64_t layer_decoded_budget()
{
    if (const auto o = current_override(); o && o->decoded_bytes) return *o->decoded_bytes;
    if (const auto e = env_mib("SLA_LAYER_DECODED_BUDGET_MB")) return *e;
    return std::clamp(layer_available_memory() / 3, GiB, 64 * GiB);
}
uint64_t layer_history_edit_budget()
{
    if (const auto o = current_override(); o && o->history_edit_bytes) return *o->history_edit_bytes;
    if (const auto e = env_mib("SLA_LAYER_HISTORY_EDIT_MB")) return *e;
    return std::clamp(layer_available_memory() / 8, 128 * MiB, 8 * GiB);
}
uint64_t layer_history_resident_budget()
{
    if (const auto o = current_override(); o && o->history_resident_bytes) return *o->history_resident_bytes;
    if (const auto e = env_mib("SLA_LAYER_HISTORY_RESIDENT_MB")) return *e;
    return 128 * MiB;
}
uint64_t layer_payload_resident_budget()
{
    if (const auto o = current_override(); o && o->payload_resident_bytes) return *o->payload_resident_bytes;
    if (const auto e = env_mib("SLA_LAYER_PAYLOAD_RESIDENT_MB")) return *e;
    // An eighth of the free RAM, 128 - 256 MiB: opening a multi-GiB archive keeps its peak well below its size.
    return std::clamp(layer_available_memory() / 8, 128 * MiB, 256 * MiB);
}
std::string layer_temp_directory()
{
    if (const auto o = current_override(); o && o->temp_directory) return *o->temp_directory;
    boost::system::error_code ec;
    const auto path = boost::filesystem::temp_directory_path(ec);
    return ec ? std::string() : path.string();
}
std::optional<uint64_t> layer_temp_space_available()
{
    if (const auto o = current_override(); o && o->temp_space_bytes) return *o->temp_space_bytes;
    const auto dir = layer_temp_directory();
    if (dir.empty()) return std::nullopt;
    boost::system::error_code ec;
    const auto info = boost::filesystem::space(boost::filesystem::path(dir), ec);
    if (ec) return std::nullopt;
    return uint64_t(info.available);
}
std::string layer_mib(uint64_t bytes) { return std::to_string((bytes + MiB / 2) / MiB) + " MiB"; }

ScopedLayerBudgets::ScopedLayerBudgets(LayerBudgetOverride value)
{
    std::lock_guard<std::mutex> lock(g_override_mutex);
    m_previous = g_override;
    g_override = std::move(value);
}
ScopedLayerBudgets::~ScopedLayerBudgets()
{
    std::lock_guard<std::mutex> lock(g_override_mutex);
    g_override = std::move(m_previous);
}
size_t layer_spill_files_alive() { return g_spill_files.load(); }

HistorySpill::HistorySpill()
{
    const auto dir = layer_temp_directory();
    if (dir.empty()) throw RuntimeError("No temporary directory is available for the undo history");
    boost::system::error_code ec;
    const auto path = boost::filesystem::path(dir) /
        boost::filesystem::unique_path("sla-layer-history-%%%%-%%%%-%%%%-%%%%.tmp", ec);
    if (ec) throw RuntimeError("Cannot name a temporary undo-history file in " + dir);
#ifdef _WIN32
    m_file = boost::nowide::fopen(path.string().c_str(), "w+bTD");
#else
    m_file = boost::nowide::fopen(path.string().c_str(), "w+b");
    if (m_file) boost::filesystem::remove(path, ec); // anonymous from now on: nothing to clean up after a crash
#endif
    if (!m_file) throw RuntimeError("Cannot create a temporary undo-history file in " + dir);
    ++g_spill_files;
}
HistorySpill::~HistorySpill()
{
    if (m_file) { std::fclose(m_file); --g_spill_files; }
}
uint64_t HistorySpill::append(const std::vector<uint8_t> &bytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t offset = m_size;
    if (bytes.empty()) return offset;
#ifdef _WIN32
    const bool seek_ok = _fseeki64(m_file, int64_t(offset), SEEK_SET) == 0;
#else
    const bool seek_ok = fseeko(m_file, off_t(offset), SEEK_SET) == 0;
#endif
    if (!seek_ok || std::fwrite(bytes.data(), 1, bytes.size(), m_file) != bytes.size() || std::fflush(m_file) != 0)
        throw RuntimeError("Not enough temporary disk space in " + layer_temp_directory() +
                           " to store the undo history (" + layer_mib(bytes.size()) + " needed)");
    m_size += bytes.size();
    return offset;
}
std::vector<uint8_t> HistorySpill::read(uint64_t offset, uint64_t length) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (offset > m_size || length > m_size - offset) throw RuntimeError("Undo history file is damaged");
    std::vector<uint8_t> result(static_cast<size_t>(length));
    if (result.empty()) return result;
#ifdef _WIN32
    const bool seek_ok = _fseeki64(m_file, int64_t(offset), SEEK_SET) == 0;
#else
    const bool seek_ok = fseeko(m_file, off_t(offset), SEEK_SET) == 0;
#endif
    if (!seek_ok || std::fread(result.data(), 1, result.size(), m_file) != result.size())
        throw RuntimeError("Cannot read the undo history from its temporary file");
    return result;
}

SourceFileStore::SourceFileStore(const std::string &path) : m_path(path)
{
    boost::system::error_code ec;
    const auto size = boost::filesystem::file_size(path, ec);
    if (ec) throw RuntimeError("Cannot read the source file " + path);
    const auto mtime = boost::filesystem::last_write_time(path, ec);
    if (ec) throw RuntimeError("Cannot read the source file " + path);
    m_size = uint64_t(size);
    m_mtime = int64_t(mtime);
    m_file = boost::nowide::fopen(path.c_str(), "rb");
    if (!m_file) throw RuntimeError("Cannot open the source file " + path);
}
SourceFileStore::~SourceFileStore() { if (m_file) std::fclose(m_file); }
bool SourceFileStore::same_file(const std::string &other) const
{
    boost::system::error_code ec;
    return boost::filesystem::equivalent(m_path, other, ec) && !ec;
}
void SourceFileStore::detach() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_detached) return;
    validate();
#ifdef _WIN32
    auto copy = std::make_shared<HistorySpill>();
    std::vector<uint8_t> chunk;
    constexpr uint64_t step = uint64_t(8) << 20;
    for (uint64_t at = 0; at < m_size; at += step) {
        const uint64_t n = std::min(step, m_size - at);
        chunk.resize(size_t(n));
        _fseeki64(m_file, int64_t(at), SEEK_SET);
        if (std::fread(chunk.data(), 1, chunk.size(), m_file) != chunk.size()) throw RuntimeError("Cannot read the source file " + m_path);
        copy->append(chunk);
    }
    std::fclose(m_file); m_file = nullptr;
    m_copy = std::move(copy);
#endif
    m_detached = true;
}
void SourceFileStore::validate() const
{
    if (m_detached) return;
    boost::system::error_code ec;
    const auto size = boost::filesystem::file_size(m_path, ec);
    const auto mtime = ec ? std::time_t(0) : boost::filesystem::last_write_time(m_path, ec);
    if (ec || uint64_t(size) != m_size || int64_t(mtime) != m_mtime)
        throw RuntimeError("The source file " + m_path + " was changed or removed by another program while it is open. "
                           "Reopen it (unsaved edits of the layers it supplied cannot be read any more).");
}
std::vector<uint8_t> SourceFileStore::read(uint64_t offset, uint64_t length) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    validate();
    if (offset > m_size || length > m_size - offset) throw RuntimeError("The source file " + m_path + " is damaged");
    if (m_copy) return m_copy->read(offset, length);
    std::vector<uint8_t> result(static_cast<size_t>(length));
    if (result.empty()) return result;
#ifdef _WIN32
    const bool seek_ok = _fseeki64(m_file, int64_t(offset), SEEK_SET) == 0;
#else
    const bool seek_ok = fseeko(m_file, off_t(offset), SEEK_SET) == 0;
#endif
    if (!seek_ok || std::fread(result.data(), 1, result.size(), m_file) != result.size())
        throw RuntimeError("Cannot read the source file " + m_path);
    return result;
}

namespace {
struct CacheSlot { const ByteStore *store; uint64_t at, len; std::shared_ptr<const std::vector<uint8_t>> data; std::weak_ptr<ByteStore> alive; };
std::mutex g_cache_mutex;
std::vector<CacheSlot> g_cache; // most recent last; at most 16 slots
}
std::shared_ptr<const std::vector<uint8_t>> layer_payload_cached_load(const StoredBytes &payload)
{
    const ByteStore *store = payload.store.get();
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
            if (it->store == store && it->at == payload.at && it->len == payload.len && !it->alive.expired()) {
                auto data = it->data;
                std::rotate(it, it + 1, g_cache.end());
                return data;
            }
    }
    auto data = std::make_shared<const std::vector<uint8_t>>(payload.store->read(payload.at, payload.len));
    // A whole container image (project serialisation of a native document) must not stay pinned by the cache.
    if (data->size() > (uint64_t(32) << 20)) return data;
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    // Slots of dead stores (a new store may reuse the address) are dropped first.
    g_cache.erase(std::remove_if(g_cache.begin(), g_cache.end(), [](const CacheSlot &s) { return s.alive.expired(); }), g_cache.end());
    g_cache.push_back({store, payload.at, payload.len, data, payload.store});
    if (g_cache.size() > 16) g_cache.erase(g_cache.begin());
    return data;
}
}
