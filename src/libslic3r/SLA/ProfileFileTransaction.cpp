// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProfileFileTransaction.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/uuid/detail/sha1.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <cerrno>
#include <cstdio>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Slic3r::sla {
namespace {
namespace fs = boost::filesystem;
using Json = nlohmann::json;
constexpr const char *journal_name = ".sla-profile-transaction";
constexpr size_t file_limit = 64 * 1024 * 1024;
constexpr size_t transaction_limit = 256 * 1024 * 1024;
std::mutex mutation_mutex;

[[noreturn]] void fail(const std::string &message) { throw std::runtime_error("Profile transaction: " + message); }

bool present(const fs::path &path) { return fs::exists(path) || fs::is_symlink(path); }

void regular(const fs::path &path)
{
    if (fs::is_symlink(path) || !fs::is_regular_file(path)) fail("not a regular file: " + path.string());
}

std::string read(const fs::path &path)
{
    regular(path);
    const auto size = fs::file_size(path);
    if (size > file_limit) fail("file exceeds 64 MiB: " + path.string());
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(boost::nowide::fopen(path.string().c_str(), "rb"), &std::fclose);
    if (!file) fail("cannot read " + path.string());
    std::string bytes(size, '\0');
    if (std::fread(bytes.data(), 1, bytes.size(), file.get()) != bytes.size() || std::fgetc(file.get()) != EOF || std::ferror(file.get()))
        fail("incomplete or changing file: " + path.string());
    return bytes;
}

std::optional<std::string> read_optional(const fs::path &path)
{
    return present(path) ? std::optional<std::string>(read(path)) : std::nullopt;
}

// Detect accidental journal/backup corruption; this is not authentication.
std::string digest(const std::string &bytes)
{
    boost::uuids::detail::sha1 hash;
    hash.process_bytes(bytes.data(), bytes.size());
    boost::uuids::detail::sha1::digest_type words;
    hash.get_digest(words);
    std::ostringstream text;
    for (const auto word : words) text << std::hex << std::setfill('0') << std::setw(8) << word;
    return text.str();
}

Json identity(const std::optional<std::string> &bytes)
{
    return bytes ? Json{{"size", bytes->size()}, {"sha1", digest(*bytes)}} : Json(nullptr);
}

void sync_directory(const fs::path &path)
{
#ifndef _WIN32
    const int fd = ::open(path.string().c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) fail("cannot open directory for synchronization: " + path.string());
    const int result = ::fsync(fd);
    ::close(fd);
    if (result != 0) fail("cannot synchronize directory: " + path.string());
#else
    // File contents are flushed with _commit. Windows rename_file uses its
    // platform replacement implementation; crash recovery still uses the journal.
    (void)path;
#endif
}

void write(const fs::path &path, const std::string &bytes)
{
    if (present(path)) fail("staging file already exists: " + path.string());
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(boost::nowide::fopen(path.string().c_str(), "wb"), &std::fclose);
    if (!file) fail("cannot write " + path.string());
    fs::permissions(path, fs::owner_read | fs::owner_write);
    if (std::fwrite(bytes.data(), 1, bytes.size(), file.get()) != bytes.size() || std::fflush(file.get()) != 0)
        fail("cannot flush " + path.string());
#ifdef _WIN32
    const int result = ::_commit(::_fileno(file.get()));
#else
    const int result = ::fsync(::fileno(file.get()));
#endif
    if (result != 0) fail("cannot synchronize " + path.string());
    if (std::fclose(file.release()) != 0) fail("cannot close " + path.string());
}

void replace(const fs::path &source, const fs::path &destination)
{
    const auto error = rename_file(source.string(), destination.string());
    if (error) fail("cannot publish " + destination.string() + ": " + error.message());
    sync_directory(destination.parent_path());
}

fs::path target(const fs::path &root, const std::string &relative)
{
    const fs::path path(relative);
    if (relative.empty() || path.is_absolute() || path.generic_string() != relative || path.extension() != ".ini")
        fail("invalid destination in journal");
    std::vector<std::string> parts;
    for (const auto &part : path) {
        const auto name = part.string();
        if (name.empty() || name == "." || name == ".." || name.find('\\') != std::string::npos || name.find('\0') != std::string::npos)
            fail("invalid destination component");
        parts.push_back(name);
    }
    const bool app = parts.size() == 1 && parts[0] == SLIC3R_APP_KEY ".ini";
    size_t role = !parts.empty() && parts[0] == "presets" ? 1 : 0;
    const bool preset = parts.size() == role + 2 &&
        (parts[role] == "printer" || parts[role] == "sla_material" || parts[role] == "sla_print");
    if (!app && !preset) fail("destination is outside SLA profile storage");
    auto parent = root;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        parent /= parts[i];
        if (fs::is_symlink(parent) || !fs::is_directory(parent)) fail("invalid profile directory: " + parent.string());
    }
    const auto result = root / path;
    if (present(result)) regular(result);
    return result;
}

// A stable lock file coordinates processes; the mutex also coordinates threads
// because POSIX advisory file locks are owned by the process, not by its threads.
struct Lock {
    std::unique_lock<std::mutex> local{mutation_mutex, std::try_to_lock};
    std::unique_ptr<boost::interprocess::file_lock> file;
    explicit Lock(const fs::path &root) {
        if (!local.owns_lock()) fail("another profile change is in progress");
        const auto path = root / ".sla-profile-transaction.lock";
        if (present(path)) regular(path);
        std::FILE *created = boost::nowide::fopen(path.string().c_str(), "ab");
        if (!created) fail("cannot open profile lock");
        std::fclose(created);
        file = std::make_unique<boost::interprocess::file_lock>(path.string().c_str());
        if (!file->try_lock()) fail("another application is changing profiles");
    }
    ~Lock() { if (file) { try { file->unlock(); } catch (...) {} } }
};

void retire(const fs::path &root)
{
    const auto journal = root / journal_name;
    const auto garbage = root / fs::unique_path(".sla-profile-transaction.cleanup-%%%%-%%%%");
    // Never delete a commit marker inside an active journal: a crash midway
    // through remove_all could otherwise turn a committed change into rollback.
    fs::rename(journal, garbage);
    sync_directory(root);
    boost::system::error_code ignored;
    fs::remove_all(garbage, ignored);
}

void recover_locked(const fs::path &root)
{
    const auto journal = root / journal_name;
    if (!present(journal)) return;
    if (fs::is_symlink(journal) || !fs::is_directory(journal)) fail("invalid journal directory");
    const auto manifest = journal / "prepared.json";
    if (!present(manifest)) { retire(root); return; } // No publication can precede this marker.
    const auto text = read(manifest);
    const auto data = Json::parse(text);
    if (!data.is_object() || data.size() != 2 || data.at("version") != 1 || !data.at("files").is_array() || data.at("files").empty() || data.at("files").size() > 10000)
        fail("invalid prepared journal");
    const bool committed = present(journal / "committed");
    if (committed && read(journal / "committed") != digest(text)) fail("invalid commit marker");
    struct Entry { fs::path path; Json before, after; bool restore; };
    std::vector<Entry> entries;
    std::set<std::string> paths;
    size_t total = 0;
    for (const auto &row : data.at("files")) {
        if (!row.is_object() || row.size() != 3) fail("invalid journal entry");
        const auto relative = row.at("path").get<std::string>();
        if (!paths.insert(relative).second) fail("duplicate journal destination");
        auto path = target(root, relative);
        const auto actual = identity(read_optional(path));
        const auto before = row.at("before"), after = row.at("after");
        for (const auto &id : {before, after}) if (!id.is_null()) {
            if (!id.is_object() || id.size() != 2 || !id.at("size").is_number_unsigned() || !id.at("sha1").is_string() || id.at("sha1").get<std::string>().size() != 40)
                fail("invalid journal file identity");
            const auto size = id.at("size").get<size_t>();
            if (size > file_limit || size > transaction_limit - total) fail("journal exceeds recovery budget");
            total += size;
        }
        if (committed ? actual != after : actual != before && actual != after)
            fail("destination was changed independently; recovery stopped: " + relative);
        const bool restore = !committed && actual != before;
        if (restore && !before.is_null() && identity(read(journal / (std::to_string(entries.size()) + ".before"))) != before)
            fail("damaged rollback copy: " + relative);
        entries.push_back({path, before, after, restore});
    }
    // Validate every required backup before restoring any file. Recovery is
    // idempotent: an already restored file no longer needs its moved backup.
    for (size_t i = 0; i < entries.size(); ++i) if (entries[i].restore) {
        const auto &entry = entries[i];
        if (entry.before.is_null()) {
            fs::remove(entry.path);
            sync_directory(entry.path.parent_path());
        } else replace(journal / (std::to_string(i) + ".before"), entry.path);
    }
    retire(root);
}
} // namespace

void recover_profile_files(const std::string &directory)
{
    const fs::path root = fs::canonical(directory);
    if (!present(root / journal_name)) return;
    Lock lock(root);
    recover_locked(root);
}

void commit_profile_files(const std::string &directory, const std::vector<ProfileFileChange> &changes,
                          const ProfileTransactionObserver &observer)
{
    if (changes.empty()) return;
    const fs::path root = fs::canonical(directory);
    Lock lock(root);
    recover_locked(root);
    if (changes.size() > 10000) fail("too many dependent profiles");
    std::set<std::string> names;
    size_t total = 0;
    std::vector<fs::path> paths;
    for (const auto &change : changes) {
        if (!names.insert(change.path).second) fail("duplicate profile change");
        paths.push_back(target(root, change.path));
        for (const auto *bytes : {&change.expected, &change.replacement}) if (*bytes) {
            if ((*bytes)->size() > file_limit || (*bytes)->size() > transaction_limit - total) fail("profile change exceeds storage budget");
            total += (*bytes)->size();
        }
        if (read_optional(paths.back()) != change.expected) fail("profile changed while preparing rename: " + change.path);
    }
    const auto journal = root / journal_name;
    if (!fs::create_directory(journal)) fail("journal already exists");
    fs::permissions(journal, fs::owner_all);
    bool committed = false;
    try {
        Json files = Json::array();
        for (size_t i = 0; i < changes.size(); ++i) {
            const auto &change = changes[i];
            const auto prefix = std::to_string(i);
            if (change.expected) {
                write(journal / (prefix + ".before"), *change.expected);
                fs::permissions(journal / (prefix + ".before"), fs::status(paths[i]).permissions());
            }
            if (change.replacement) {
                write(journal / (prefix + ".after"), *change.replacement);
                if (change.expected) fs::permissions(journal / (prefix + ".after"), fs::status(paths[i]).permissions());
            }
            files.push_back({{"path", change.path}, {"before", identity(change.expected)}, {"after", identity(change.replacement)}});
        }
        const auto manifest = Json{{"version", 1}, {"files", files}}.dump();
        write(journal / "prepared.tmp", manifest);
        replace(journal / "prepared.tmp", journal / "prepared.json");
        sync_directory(root);
        if (observer) observer(ProfileTransactionStage::Prepared, 0);
        for (size_t i = 0; i < changes.size(); ++i) {
            if (read_optional(paths[i]) != changes[i].expected) fail("profile changed during publication: " + changes[i].path);
            if (changes[i].replacement) replace(journal / (std::to_string(i) + ".after"), paths[i]);
            else { fs::remove(paths[i]); sync_directory(paths[i].parent_path()); }
            if (observer) observer(ProfileTransactionStage::Published, i);
        }
        if (observer) observer(ProfileTransactionStage::BeforeCommit, changes.size());
        write(journal / "commit.tmp", digest(manifest));
        replace(journal / "commit.tmp", journal / "committed");
        committed = true;
        if (observer) observer(ProfileTransactionStage::Committed, changes.size());
        retire(root);
    } catch (...) {
        if (committed || present(journal / "committed")) return; // Durable success; cleanup may be retried at startup.
        const auto error = std::current_exception();
        recover_locked(root); // Failure here deliberately retains the journal and stops the caller.
        std::rethrow_exception(error);
    }
}
} // namespace Slic3r::sla
