// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerJobParameters.hpp"
#include "LayerProgress.hpp"
#include "LayerRasterGeometry.hpp"
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
};
LayerConversionPlan plan_layer_conversion(const LayerDocument &document,
                                          const std::string &destination,
                                          const LayerProgress &progress = {});
// Check target axes and actual planned Z before copying masks. Unknown source
// dimensions are not proof of scale preservation and remain an open contract.
void require_layer_conversion_geometry(const LayerConversionPlan &plan,
                                       const DynamicPrintConfig &printer);
// Reject incompatible fixed-motion targets before copying/encoding masks.
void require_layer_conversion_motion(const LayerConversionPlan &plan,
                                     const DynamicPrintConfig &printer);
} // namespace Slic3r::sla
