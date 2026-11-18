#ifndef SLIC3R_SLA_LAYER_MEMORY_BUDGET_HPP
#define SLIC3R_SLA_LAYER_MEMORY_BUDGET_HPP
// Memory/disk budgets of the layer editor. Masks are stored PNG-compressed, so the
// "document" budget bounds compressed bytes, the "decoded" budget bounds whole-document
// operations that need every layer decoded at once, and undo history beyond its
// resident budget is spilled to an unlinked temporary file (HistorySpill).
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::sla {

// Bytes of RAM the process can still use; 0 when the platform cannot tell.
uint64_t layer_available_memory();
// Compressed bytes of all entries (a container is read in full, so this also bounds the file).
uint64_t layer_editor_byte_budget();
// Decoded 8-bit pixels held at once by a whole-document operation.
uint64_t layer_decoded_budget();
// Bytes one edit may add to history (before + after copies of the changed layers).
uint64_t layer_history_edit_budget();
// Bytes of history kept in RAM; older edits are spilled to disk instead of dropped.
uint64_t layer_history_resident_budget();
// Free bytes in the directory spill files are created in; nullopt when unknown.
std::optional<uint64_t> layer_temp_space_available();
std::string layer_temp_directory();
std::string layer_mib(uint64_t bytes);

// Test/diagnostic overrides. Environment SLA_LAYER_EDITOR_BUDGET_MB, SLA_LAYER_DECODED_BUDGET_MB,
// SLA_LAYER_HISTORY_EDIT_MB and SLA_LAYER_HISTORY_RESIDENT_MB act as defaults for the same fields.
struct LayerBudgetOverride {
    std::optional<uint64_t> editor_bytes, decoded_bytes, history_edit_bytes, history_resident_bytes, payload_resident_bytes;
    std::optional<std::string> temp_directory;
    std::optional<uint64_t> temp_space_bytes; // pretend this much space is free (insufficient-space tests)
};
struct ScopedLayerBudgets {
    explicit ScopedLayerBudgets(LayerBudgetOverride value);
    ~ScopedLayerBudgets();
    ScopedLayerBudgets(const ScopedLayerBudgets &) = delete;
    ScopedLayerBudgets &operator=(const ScopedLayerBudgets &) = delete;
private:
    std::optional<LayerBudgetOverride> m_previous;
};
// Compressed layer payloads kept in RAM; the rest lives in a spill file or is read from the source
// archive on demand (StoredBytes). Never above the document budget.
uint64_t layer_payload_resident_budget();

// Live spill files (test observable: must return to its previous value after documents die).
size_t layer_spill_files_alive();

// Random-access byte container stored bytes can live in.
class ByteStore {
public:
    virtual ~ByteStore() = default;
    virtual std::vector<uint8_t> read(uint64_t offset, uint64_t length) const = 0;
};

// Append-only temporary file, removed with its last owner (unlinked on POSIX, delete-on-close on
// Windows) so a crash never leaves it behind. Reads and appends are serialised.
class HistorySpill : public ByteStore {
public:
    // Throws RuntimeError with a user-facing message when the file cannot be created.
    HistorySpill();
    ~HistorySpill();
    HistorySpill(const HistorySpill &) = delete;
    HistorySpill &operator=(const HistorySpill &) = delete;
    uint64_t append(const std::vector<uint8_t> &bytes); // returns the offset; throws on a full disk
    std::vector<uint8_t> read(uint64_t offset, uint64_t length) const override;
    uint64_t size() const { return m_size; }
private:
    mutable std::mutex m_mutex;
    std::FILE *m_file = nullptr;
    uint64_t m_size = 0;
};

// Read-only view of a file that is not ours (the archive a document was opened from). Size and
// modification time are captured at open and re-checked on every read: a file that changed on disk
// gives a clear error instead of garbage.
class SourceFileStore : public ByteStore {
public:
    explicit SourceFileStore(const std::string &path); // throws when the file cannot be opened
    ~SourceFileStore();
    SourceFileStore(const SourceFileStore &) = delete;
    SourceFileStore &operator=(const SourceFileStore &) = delete;
    std::vector<uint8_t> read(uint64_t offset, uint64_t length) const override;
    const std::string &path() const { return m_path; }
    uint64_t size() const { return m_size; }
    // True when `other` names the same file (used to detach before the file is overwritten).
    bool same_file(const std::string &other) const;
    // Called right before our file is replaced by a save: from now on reads no longer compare the
    // path (it names the NEW file). POSIX keeps reading the old inode through the open handle;
    // Windows (cannot replace an open file) first copies the content to an anonymous temp file.
    void detach() const;
private:
    void validate() const;
    mutable std::mutex m_mutex;
    mutable bool m_detached = false;
    mutable std::shared_ptr<class HistorySpill> m_copy;
    std::string m_path;
    std::FILE *m_file = nullptr;
    uint64_t m_size = 0;
    int64_t m_mtime = 0;
};

// Location of bytes that left RAM: (store, offset, length). Copies share the store, so snapshots
// and history stay cheap. Content at a location never changes.
struct StoredBytes {
    std::shared_ptr<ByteStore> store;
    uint64_t at = 0, len = 0;
    explicit operator bool() const { return bool(store); }
};

// Small LRU of loaded stored bytes, keyed by (store, offset): content at a location never changes.
std::shared_ptr<const std::vector<uint8_t>> layer_payload_cached_load(const StoredBytes &stored);
}
#endif
