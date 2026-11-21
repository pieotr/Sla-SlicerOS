// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "FreshLayerJob.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Format/AnycubicSLA.hpp"
#include <optional>
#include <string>

namespace Slic3r::sla {
// Use the registry factory's default, rather than assuming all Anycubic
// printers use PWC516. Legacy defaults remain legacy when "auto" is selected.
inline std::optional<uint16_t> generated_anycubic_version(
    const std::string &format, const std::string &requested = "auto")
{
    const auto *entry = get_archive_entry(format.c_str());
    if (!entry || anycubic_available_versions(entry->id).empty()) return std::nullopt;
    const auto writer = entry->wrfactoryfn(SLAPrinterConfig{});
    const auto *anycubic = dynamic_cast<const AnycubicSLAArchive *>(writer.get());
    if (!anycubic) throw std::logic_error("Anycubic format has no native writer factory");
    return anycubic_resolve_version(entry->id, requested, anycubic->default_version());
}
// Some established legacy writers have a proven pixel alphabet before their
// full owned parameter contract exists. This capability never enables ownership.
inline std::optional<LayerMaskPrecision> independent_generated_mask_precision(const std::string &format)
{
    const auto *entry=get_archive_entry(format.c_str());
    if(!entry)return std::nullopt;
    const std::string id=entry->id;
    if(id=="pm4u" || id=="pm7" || id=="pm7m" || id=="pwsz")
        return LayerMaskPrecision::NativeGray4;
    // These v1 writers use exactly one exposure plane with threshold255.
    // PWS threshold128 and v2 non-power AA require distinct source contracts.
    if(id=="cbddlp_v1" || id=="photon_v1")
        return LayerMaskPrecision::NativeBitplanes1;
    return std::nullopt;
}
// Versioned legacy capability is explicit at each producer call site. The
// one-argument domains above stay unchanged for model/PCB until their own gates.
inline std::optional<LayerMaskPrecision> independent_generated_mask_precision(
    const std::string &format, const std::string &version)
{
    if (const auto precision = independent_generated_mask_precision(format))
        return precision;
    const auto *entry = get_archive_entry(format.c_str());
    if (!entry) return std::nullopt;
    const std::string id = entry->id;
    // PM4n's default515 uses the same PW0 alphabet. Canonicalize new
    // model/calibration/PCB masks before editing; restored Gray8 documents
    // keep their recorded precision and are never retroactively quantized.
    if (id != "pmx2" && id != "pwmb" && id != "pm4n") return std::nullopt;
    const auto resolved = generated_anycubic_version(id, version);
    if (resolved && *resolved == ANYCUBIC_SLA_FORMAT_VERSION_515)
        return LayerMaskPrecision::NativeGray4;
    return std::nullopt;
}
inline LayerMaskPrecision generated_mask_precision(const std::string &format,unsigned planes=1,
                                                  const std::string &version="auto")
{
    const auto *entry = get_archive_entry(format.c_str());
    if (!entry) return LayerMaskPrecision::Gray8;
    const std::string id = entry->id;
    if(const auto precision=independent_generated_mask_precision(id,version))return *precision;
    if(id=="pwc")return LayerMaskPrecision::NativeGray4;
    if (const auto resolved=generated_anycubic_version(id,version);
        resolved && *resolved>=516 && *resolved<=518) return LayerMaskPrecision::NativeGray4;
    if(id=="cbddlp" || id=="photon")return bitplane_mask_precision(planes);
    if(id=="cbddlp_v1" || id=="photon_v1")return bitplane_mask_precision(1);
    return id == "cxdlpv4" || id == "ctb_v4" || id == "ctb_v5" ||
        id == "ctb_encrypted_v4" || id == "ctb_encrypted"
        ? LayerMaskPrecision::NativeGray7 : LayerMaskPrecision::Gray8;
}
// Migration boundary, not the final format capability matrix. These codecs
// have preserving fresh-job writers. Other generators retain their current
// export path until their own neutral parameter contracts are implemented.
// Do not remove ownership when an existing job later selects another target.
inline std::optional<FreshLayerProducer> generated_layer_ownership(
    const std::string &format, FreshLayerProducer producer,const std::string &anycubic_version="auto")
{
    const auto *entry = get_archive_entry(format.c_str());
    if (!entry) return std::nullopt; // The job factory reports unknown formats.
    const std::string id = entry->id;
    if (const auto version=generated_anycubic_version(id,anycubic_version)) {
        // Preserve legacy paths until their own parameter contracts exist.
        if(*version>=516 && *version<=518)return producer;
        return std::nullopt;
    }
    if (id == "SL1" || id == "SL1S" || id == "cxdlpv4" || id == "cws" || id == "uvj" ||
        id == "ctb_v4" || id == "ctb_v5" ||
        id == "ctb_encrypted_v4" || id == "ctb_encrypted")
        return producer;
    return std::nullopt;
}
} // namespace Slic3r::sla
