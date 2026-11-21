// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "GeneratedLayerOwnership.hpp"

namespace Slic3r::sla {
// Preset edits are a draft until saved. They drive the proposed output, but
// must not overwrite another printer's durable last-used format on Save As.
inline std::string preferred_export_format(const std::string &saved,
    const std::string &edited, const std::string &stored, const std::string &anycubic_version="auto")
{
    if (edited!=saved) return edited;
    const auto *entry=get_archive_entry(stored.c_str());
    if (entry && archive_export_unavailable_reason(entry->id).empty()) {
        try {
            // A persisted format can become incompatible after changing the
            // printer's version, e.g. PM5S with an explicit version517.
            generated_anycubic_version(entry->id,anycubic_version);
            return entry->id;
        } catch (const std::invalid_argument &) {}
    }
    return edited;
}
} // namespace Slic3r::sla
