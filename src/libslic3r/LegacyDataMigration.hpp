// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef slic3r_LegacyDataMigration_hpp_
#define slic3r_LegacyDataMigration_hpp_

#include <string>
#include <vector>

namespace Slic3r {

// One-time import of the SLA part of a PrusaSlicer data directory (~/.config/PrusaSlicer, the location the
// application used before it got its own SLA-Slicer data directory) into a fresh SLA-Slicer data directory.
//
// The legacy directory is only ever READ (no write, rename, lock or mtime change). Only what SLA-Slicer uses is copied:
//   sla_print/*.ini, sla_material/*.ini, printer/*.ini with printer_technology = SLA (and the bed texture / model files
//   they reference), vendor/*.ini bundles that define SLA printers (plus their vendor/<id>/ folder), shapes/,
//   resin_stock.json, and a filtered SLA-Slicer.ini (only SLA selections / SLA vendor models, no FFF keys).
//   Not copied: print/, filament/, physical_printer/ (not used by this application), FFF printers, cache/, snapshots/.
struct LegacyMigrationResult
{
    // True when the new data directory was created from the legacy one by this call.
    bool                     performed { false };
    // Non-empty when nothing was done (or what went wrong): "disabled", "explicit-datadir", "new-datadir-exists",
    // "no-legacy-datadir", "same-directory", or "error: ...".
    std::string              skipped_reason;
    size_t                   sla_prints { 0 };
    size_t                   sla_materials { 0 };
    size_t                   printers { 0 };
    size_t                   vendors { 0 };
    size_t                   other_files { 0 };      // shapes, resin stock, bed textures / models, vendor sub-folders
    bool                     config_written { false };
    // Relative paths (below the new directory) of everything that was written, sorted.
    std::vector<std::string> copied;

    // The number reported to the user: SLA process profiles + resins + printer profiles.
    size_t profile_count() const { return sla_prints + sla_materials + printers; }
};

// Pure migration: copy the SLA part of legacy_dir into new_dir. new_dir must not exist (it is created atomically:
// everything is staged next to it and renamed on success). Does not look at environment variables.
LegacyMigrationResult migrate_legacy_data_dir(const std::string &legacy_dir, const std::string &new_dir);

// Policy wrapper used at GUI start: does nothing when SLA_SLICER_NO_MIGRATION=1, when a --datadir was given explicitly,
// when new_dir already exists or when legacy_dir does not exist. Remembers the result (see last_legacy_migration()).
LegacyMigrationResult migrate_legacy_data_dir_if_needed(const std::string &legacy_dir, const std::string &new_dir, bool datadir_explicit);

// The legacy default data directory that belongs to a default new data directory (the sibling "PrusaSlicer").
std::string legacy_prusaslicer_data_dir(const std::string &default_new_dir);

// Result of the last migrate_legacy_data_dir_if_needed() call in this process (for the GUI notification).
const LegacyMigrationResult& last_legacy_migration();

} // namespace Slic3r

#endif // slic3r_LegacyDataMigration_hpp_
