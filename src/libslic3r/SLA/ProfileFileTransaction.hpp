// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef slic3r_sla_ProfileFileTransaction_hpp_
#define slic3r_sla_ProfileFileTransaction_hpp_

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::sla {

// Paths are relative to the application data directory. A missing value means
// the file must be absent (expected) or removed (replacement).
struct ProfileFileChange {
    std::string path;
    std::optional<std::string> expected;
    std::optional<std::string> replacement;
};

enum class ProfileTransactionStage { Prepared, Published, BeforeCommit, Committed };
using ProfileTransactionObserver = std::function<void(ProfileTransactionStage, size_t)>;

// Publish a coherent group of profile files. No caller state may be changed
// until this returns. A pre-commit exception rolls the files back. An abrupt
// process exit leaves a journal which recovery rolls back, or retires if its
// durable commit marker exists. Observer is synchronous, for diagnostics and
// deterministic interruption tests; exceptions after commit cannot undo it.
void commit_profile_files(const std::string &directory, const std::vector<ProfileFileChange> &changes,
                          const ProfileTransactionObserver &observer = {});
// Must run before loading application settings and profiles. Reject damaged
// journals or independently changed destinations instead of overwriting them.
void recover_profile_files(const std::string &directory);

} // namespace Slic3r::sla
#endif
