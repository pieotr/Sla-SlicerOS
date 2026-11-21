// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// Resin inventory behind "I printed this file" (UVtools OperationIPrintedThisFile + Material.Consume).
// A small JSON store of bottles per material. Corrupt or unreadable files are reported, never replaced by an
// empty inventory, and saving publishes atomically (staging file + rename).
#include "libslic3r/Exception.hpp"
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::sla {

class ResinStockError final : public RuntimeError {
public:
    using RuntimeError::RuntimeError;
};

struct ResinMaterial {
    std::string name;
    double bottle_volume_ml = 1000.;   // UVtools BottleVolume
    double density = 1.;               // g/ml
    double bottle_cost = 30.;
    int    bottles_in_stock = 1;       // may go negative: UVtools lets the stock run below zero
    double bottle_remaining_ml = 1000.;
    double consumed_ml = 0.;
    double print_time_h = 0.;
    int    print_count = 0;

    // Exactly UVtools Material.Consume. No-op (false is not returned) for ml <= 0 or a zero bottle volume.
    // Returns whether at least one bottle is left in stock.
    bool consume(double ml, double seconds);
};

class ResinStock {
public:
    std::vector<ResinMaterial> materials;

    ResinMaterial       *find(const std::string &name);
    const ResinMaterial *find(const std::string &name) const;
    // Rejects an empty name and a duplicate (case-sensitive) name, and non-finite or negative quantities.
    void add(ResinMaterial material);
    bool remove(const std::string &name);

    // A missing file is an empty stock; an existing file that is not valid stock JSON throws ResinStockError that
    // names the file and the problem. The caller must not overwrite it afterwards without the user's decision.
    static ResinStock load(const std::string &path);
    // Atomic: staging file next to the target, readback check, then rename. The previous file survives any failure.
    void save(const std::string &path) const;
    // "<data_dir>/resin_stock.json"
    static std::string default_path();
};

// Validation shared by the store and the GUI editor; returns an empty string when the material is valid.
std::string validate_resin_material(const ResinMaterial &material);

} // namespace Slic3r::sla
