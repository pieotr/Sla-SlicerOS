// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "libslic3r/PrintConfig.hpp"
#include <string>
#include <vector>

namespace Slic3r::sla {
// One setting a destination format cannot store exactly, together with the nearest safe value it can.
// The adjustment is data only: it is shown to the user, and once approved it is applied to the
// configuration the writer is built from. User presets are never modified.
struct RepresentationAdjustment {
    std::vector<std::string> keys; // config keys set to `value` (material/printer keys); normal+bottom share one entry when identical
    std::string label;             // "Lift speed"
    std::string from, to;          // display numbers or text, without unit
    std::string unit;              // "mm/min", "s", ""
    std::string reason;            // short, user facing
    std::string value;             // serialized value stored under every key
    // "Lift speed 80 mm/min -> 60 mm/min (CXDLP stores whole mm/s)"
    std::string describe() const;
};
using RepresentationApproval = std::vector<RepresentationAdjustment>;

struct RepresentationPlan {
    std::vector<RepresentationAdjustment> adjustments; // approved by the user (GUI) or refused (strict path)
    std::vector<std::string> blockers;                 // problems no adjustment can fix; always refused
    bool empty() const { return adjustments.empty() && blockers.empty(); }
    // Every problem at once. `adjustable_hint` is appended when only adjustments are pending.
    std::string strict_message(const std::string &format_name) const;
};

// True when `format` has representation rules (CXDLP v1/v2/v3 today).
bool has_representation_rules(const std::string &format);
// `effective` is the complete configuration the writer will see (material + printer keys).
RepresentationPlan plan_representation(const std::string &format, const DynamicPrintConfig &effective);
// Sets every adjusted key (those absent from `target` are ignored).
void apply_representation_adjustments(ConfigBase &target, const RepresentationApproval &adjustments);
// Creality CL/CT model whose native resolution equals the given one (UVtools machine list order), or "".
std::string creality_model_for_resolution(unsigned columns, unsigned rows);
// Log line for the written file.
std::string describe_adjustments(const RepresentationApproval &adjustments);
} // namespace Slic3r::sla
