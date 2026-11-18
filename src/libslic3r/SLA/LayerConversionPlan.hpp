// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerJobParameters.hpp"
#include "LayerProgress.hpp"
#include "LayerRasterGeometry.hpp"
#include "RepresentationAdjustment.hpp"
#include <optional>
#include <string>
#include <vector>
#include <memory>

namespace Slic3r::sla {
class LayerDocument;
struct CXDLPv4FreshExportPlan;
struct SL1FreshExportPlan;
struct CWSFreshExportPlan;
struct CBDDLPFreshExportPlan;
struct AnycubicFreshExportPlan;
struct UVJFreshExportPlan;
// Owned, read-only parameter intent prepared before copying/encoding masks.
// Printer geometry and fixed motion are preflight checks; encoded pixels and
// the native table remain independently verified publication gates.
struct LayerConversionPlan {
    std::string destination;
    LayerParameterCapabilities schema;
    std::vector<LayerParameters> rows;
    std::optional<LayerJobParameters> globals;
    std::optional<LayerRasterGeometry> source_geometry;
    std::shared_ptr<const CXDLPv4FreshExportPlan> cxdlpv4_fresh;
    std::shared_ptr<const SL1FreshExportPlan> sl1_fresh;
    std::shared_ptr<const CWSFreshExportPlan> cws_fresh;
    std::shared_ptr<const CBDDLPFreshExportPlan> cbddlp_fresh;
    std::shared_ptr<const AnycubicFreshExportPlan> anycubic_fresh;
    std::shared_ptr<const UVJFreshExportPlan> uvj_fresh;
    // Fresh job exported through the destination's own direct writer (the one
    // the CLI uses for sla_archive_format=X). `globals`/`rows` describe the job
    // that writer must reproduce; direct_fresh_export_config() proves that and
    // yields the configuration to build the writer with.
    bool direct_fresh = false;
};
LayerConversionPlan plan_layer_conversion(const LayerDocument &document,
                                          const std::string &destination,
                                          const LayerProgress &progress = {});
// Direct-writer route for fresh jobs. Returns `base` with the job's globals applied and
// destination `format`, after proving the one-parameter-set-per-job writer reproduces every
// layer row of `document`; otherwise throws naming the layer/parameter and what to change.
DynamicPrintConfig direct_fresh_export_config(const LayerDocument &document,
                                              const std::string &format,
                                              const DynamicPrintConfig &base);
// Everything the destination writer cannot store exactly for this job, as adjustable items plus
// blockers. For a fresh job the effective configuration is the direct-writer one (globals applied,
// per-layer fidelity proven or refused); otherwise `config` itself. Empty when the format has no
// representation rules. Pure preflight: nothing is written.
RepresentationPlan plan_export_representation(const std::string &format, const DynamicPrintConfig &config,
                                              const LayerDocument *document);
// Check target axes and actual planned Z before copying masks. Unknown source
// dimensions are not proof of scale preservation and remain an open contract.
void require_layer_conversion_geometry(const LayerConversionPlan &plan,
                                       const DynamicPrintConfig &printer);
// Reject incompatible fixed-motion targets before copying/encoding masks.
void require_layer_conversion_motion(const LayerConversionPlan &plan,
                                     const DynamicPrintConfig &printer);
} // namespace Slic3r::sla
