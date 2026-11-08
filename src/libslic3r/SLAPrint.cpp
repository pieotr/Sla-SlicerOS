#include "SLA/ExportGeneration.hpp"
#include "SLA/ExportSnapshot.hpp"
#include "SLA/EncodedMaskReuse.hpp"
#include <typeinfo>
#include "SLA/ExportRequest.hpp"
#include "SLA/LayerPaletteProjection.hpp"
#include "SLA/LayerMaskValidation.hpp"
#include "SLA/ProjectedPNGMaskEncoder.hpp"
#include "SLA/ExportPrinterValidation.hpp"
#include "SLA/AnycubicSettingsPreflight.hpp"
#include "SLA/CrealityCXDLPv4MaskReader.hpp"
#include <chrono>
///|/ Copyright (c) Prusa Research 2018 - 2023 Tomáš Mészáros @tamasmeszaros, Lukáš Matěna @lukasmatena, Pavel Mikuš @Godrak, Oleksandra Iushchenko @YuSanka, Vojtěch Bubník @bubnikv, Roman Beránek @zavorka, Enrico Turri @enricoturri1966
///|/ Copyright (c) 2022 ole00 @ole00
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "SLAPrint.hpp"
#include "SLA/FreshLayerMaterialPricing.hpp"
#include "SLA/GeneratedLayerOwnership.hpp"
#include "SLA/ProjectArchive.hpp"
#include "SLA/ExposurePositionSchedule.hpp"
#include "SLA/ExposureMaskUnion.hpp"
#include <unordered_set>
#include "SLA/LayerParameterConversion.hpp"
#include "SLA/LayerConversionPlan.hpp"
#include "SLA/PrinterGeometry.hpp"
#include "SLA/LayerExportProfile.hpp"
#include "SLA/ExportPublication.hpp"
#include "SLA/ChituboxJobOverlay.hpp"
#include "SLA/ChituboxLayerParameters.hpp"
#include "SLA/CTBEncryptedMotion.hpp"
#include "SLA/CrealityCXDLPv4FreshExportPlan.hpp"
#include "SLA/SL1FreshExportPlan.hpp"
#include "SLA/CWSFreshExportPlan.hpp"
#include "SLA/CBDDLPFreshExportPlan.hpp"
#include "SLA/AnycubicFreshExportPlan.hpp"
#include "SLA/UVJFreshExportPlan.hpp"
#include "Format/OpenStandardsArchive.hpp"
#include <nlohmann/json.hpp>
#include "Format/ChituboxLayerLayout.hpp"
#include "Format/AnycubicLayerLayout.hpp"
#include "Format/UVToolsCompatArchive.hpp"
#include "Format/ZipperArchiveImport.hpp"
#include "SLA/FreshLayerStatistics.hpp"
#include "SLA/StatisticsPresentation.hpp"
#include "SLA/SL1FreshMaterialProfile.hpp"
#include "SLA/SL1FreshExportVerify.hpp"
#include "Format/SL1.hpp"
#include "SLA/CrealityCXDLPv4JobReader.hpp"
#include "SLA/CrealityCXDLPv4LayerParameters.hpp"
#include "Format/CrealityCXDLPv4.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include "SLAPrintSteps.hpp" // IWYU pragma: keep
#include "CSGMesh/CSGMeshCopy.hpp"
#include "CSGMesh/PerformCSGMeshBooleans.hpp"
#include "format.hpp"
#include "StaticMap.hpp"

#include "Format/SLAArchiveFormatRegistry.hpp"
#include "Format/AnycubicSLA.hpp"
#include "Format/ChituboxArchive.hpp"
#include "Format/CTBEncryptedArchive.hpp"
#include "Format/NanoDLPArchive.hpp"
#include "Format/KlipperArchive.hpp"
#include "Format/PhrozenArchive.hpp"
#include "Format/FDGArchive.hpp"
#include "Format/FlashforgeArchive.hpp"
#include "Format/LongerOrangeArchive.hpp"
#include "Format/MakerbaseArchive.hpp"
#include "Format/Emake3DArchive.hpp"
#include "Format/VoxeldanceArchive.hpp"
#include "Format/UniformationArchive.hpp"
#include "Format/ZCodeArchive.hpp"
#include "SLA/PrintParameters.hpp"

#include "Geometry.hpp"
#include "Thread.hpp"

#include <unordered_set>
#include <unordered_map>
#include <string_view>
#include <numeric>

#include <tbb/parallel_for.h>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/MultipleBeds.hpp"

// #define SLAPRINT_DO_BENCHMARK

#ifdef SLAPRINT_DO_BENCHMARK
#include <libnest2d/tools/benchmark.h>
#endif

#include "I18N.hpp"

//! macro used to mark string used at localization,
//! return same string
#define _u8L(s) Slic3r::I18N::translate(s)

namespace Slic3r {


bool is_zero_elevation(const SLAPrintObjectConfig &c)
{
    return c.pad_enable.getBool() && c.pad_around_object.getBool();
}

// Compile the argument for support creation from the static print config.
sla::SupportTreeConfig make_support_cfg(const SLAPrintObjectConfig& c)
{
    sla::SupportTreeConfig scfg;

    scfg.enabled = c.supports_enable.getBool();
    scfg.tree_type = c.support_tree_type.value;

    switch(scfg.tree_type) {
    case sla::SupportTreeType::Default: {
        scfg.head_front_radius_mm = 0.5*c.support_head_front_diameter.getFloat();
        double pillar_r = 0.5 * c.support_pillar_diameter.getFloat();
        scfg.head_back_radius_mm = pillar_r;
        scfg.head_fallback_radius_mm =
            0.01 * c.support_small_pillar_diameter_percent.getFloat() * pillar_r;
        scfg.head_penetration_mm = c.support_head_penetration.getFloat();
        scfg.head_width_mm = c.support_head_width.getFloat();
        scfg.object_elevation_mm = is_zero_elevation(c) ?
                                       0. : c.support_object_elevation.getFloat();
        scfg.bridge_slope = c.support_critical_angle.getFloat() * PI / 180.0 ;
        scfg.max_bridge_length_mm = c.support_max_bridge_length.getFloat();
        scfg.max_pillar_link_distance_mm = c.support_max_pillar_link_distance.getFloat();
        scfg.pillar_connection_mode = c.support_pillar_connection_mode.value;
        scfg.ground_facing_only = c.support_buildplate_only.getBool();
        scfg.pillar_widening_factor = c.support_pillar_widening_factor.getFloat();
        scfg.base_radius_mm = 0.5*c.support_base_diameter.getFloat();
        scfg.base_height_mm = c.support_base_height.getFloat();
        scfg.pillar_base_safety_distance_mm =
            c.support_base_safety_distance.getFloat() < EPSILON ?
                scfg.safety_distance_mm : c.support_base_safety_distance.getFloat();

        scfg.max_bridges_on_pillar = unsigned(c.support_max_bridges_on_pillar.getInt());
        scfg.max_weight_on_model_support = c.support_max_weight_on_model.getFloat();
        break;
    }
    case sla::SupportTreeType::Branching:
        [[fallthrough]];
    case sla::SupportTreeType::Organic:{
        scfg.head_front_radius_mm = 0.5*c.branchingsupport_head_front_diameter.getFloat();
        double pillar_r = 0.5 * c.branchingsupport_pillar_diameter.getFloat();
        scfg.head_back_radius_mm = pillar_r;
        scfg.head_fallback_radius_mm =
            0.01 * c.branchingsupport_small_pillar_diameter_percent.getFloat() * pillar_r;
        scfg.head_penetration_mm = c.branchingsupport_head_penetration.getFloat();
        scfg.head_width_mm = c.branchingsupport_head_width.getFloat();
        scfg.object_elevation_mm = is_zero_elevation(c) ?
                                       0. : c.branchingsupport_object_elevation.getFloat();
        scfg.bridge_slope = c.branchingsupport_critical_angle.getFloat() * PI / 180.0 ;
        scfg.max_bridge_length_mm = c.branchingsupport_max_bridge_length.getFloat();
        scfg.max_pillar_link_distance_mm = c.branchingsupport_max_pillar_link_distance.getFloat();
        scfg.pillar_connection_mode = c.branchingsupport_pillar_connection_mode.value;
        scfg.ground_facing_only = c.branchingsupport_buildplate_only.getBool();
        scfg.pillar_widening_factor = c.branchingsupport_pillar_widening_factor.getFloat();
        scfg.base_radius_mm = 0.5*c.branchingsupport_base_diameter.getFloat();
        scfg.base_height_mm = c.branchingsupport_base_height.getFloat();
        scfg.pillar_base_safety_distance_mm =
            c.branchingsupport_base_safety_distance.getFloat() < EPSILON ?
                scfg.safety_distance_mm : c.branchingsupport_base_safety_distance.getFloat();

        scfg.max_bridges_on_pillar = unsigned(c.branchingsupport_max_bridges_on_pillar.getInt());
        scfg.max_weight_on_model_support = c.branchingsupport_max_weight_on_model.getFloat();
        break;
    }
    }
    
    return scfg;
}

sla::PadConfig::EmbedObject builtin_pad_cfg(const SLAPrintObjectConfig& c)
{
    sla::PadConfig::EmbedObject ret;
    
    ret.enabled = is_zero_elevation(c);
    
    if(ret.enabled) {
        ret.everywhere           = c.pad_around_object_everywhere.getBool();
        ret.object_gap_mm        = c.pad_object_gap.getFloat();
        ret.stick_width_mm       = c.pad_object_connector_width.getFloat();
        ret.stick_stride_mm      = c.pad_object_connector_stride.getFloat();
        ret.stick_penetration_mm = c.pad_object_connector_penetration
                                       .getFloat();
    }
    
    return ret;
}

sla::PadConfig make_pad_cfg(const SLAPrintObjectConfig& c)
{
    sla::PadConfig pcfg;
    
    pcfg.wall_thickness_mm = c.pad_wall_thickness.getFloat();
    pcfg.wall_slope = c.pad_wall_slope.getFloat() * PI / 180.0;
    
    pcfg.max_merge_dist_mm = c.pad_max_merge_distance.getFloat();
    pcfg.wall_height_mm = c.pad_wall_height.getFloat();
    pcfg.brim_size_mm = c.pad_brim_size.getFloat();
    
    // set builtin pad implicitly ON
    pcfg.embed_object = builtin_pad_cfg(c);
    
    return pcfg;
}

bool validate_pad(const indexed_triangle_set &pad, const sla::PadConfig &pcfg)
{
    // An empty pad can only be created if embed_object mode is enabled
    // and the pad is not forced everywhere
    return !pad.empty() || (pcfg.embed_object.enabled && !pcfg.embed_object.everywhere);
}

void SLAPrint::set_calibration_recipe(std::shared_ptr<const sla::CalibrationRecipe> recipe)
{
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    if (!m_raster_job) throw RuntimeError("Calibration recipe requires a raster job");
    if (recipe) {
        recipe->validate();
        if (const auto &precision=m_layer_document->mask_contract_precision();
            precision && *precision!=recipe->precision)
            throw RuntimeError("Calibration recipe differs from its immutable source mask contract");
        if (const auto &fresh = m_layer_document->fresh_job(); fresh &&
            (fresh->producer != sla::FreshLayerProducer::Calibration || fresh->mask_precision != recipe->precision))
            throw RuntimeError("Calibration recipe does not match its source job");
    }
    // Copy the provenance so an external mutable shared owner cannot change it.
    auto replacement = recipe ? recipe->clone() : nullptr;
    if (!replacement && !m_calibration_recipe) return;
    // Invalidate before changing storage borrowed by an exporter. The commit
    // lock is released before stopping its background owner.
    advance_export_generation();
    this->call_cancel_callback();
    m_calibration_recipe = std::move(replacement);
}

sla::ExportGenerationGuard SLAPrint::export_generation_guard() const
{
    return m_frozen_export_guard ? *m_frozen_export_guard : sla::ExportGenerationGuard(m_export_generation);
}

void SLAPrint::throw_if_canceled() const
{
    PrintBase::throw_if_canceled();
    if (m_frozen_export_checkpoint) m_frozen_export_checkpoint();
    if (m_frozen_export_guard) m_frozen_export_guard->check();
}

const SLAPrintStatistics &SLAPrint::print_statistics() const
{
    if (m_raster_job_purpose != RasterJobPurpose::CompleteJob)
        throw RuntimeError("An internal encoding context has no complete job statistics");
    return m_print_statistics;
}

bool SLAPrint::can_capture_completed_cxdlpv4_encoding() const
{
    std::lock_guard<std::mutex> lock(m_completed_cxdlpv4_encoding_mutex);
    if (!sla::encoded_mask_reuse_detail::experiment_enabled() || m_raster_job ||
        !m_completed_cxdlpv4_encoding || !m_layer_document || !m_archiver ||
        typeid(*m_archiver) != typeid(CrealityCXDLPv4Archive) ||
        m_full_print_config.opt_string("sla_archive_format") != "cxdlpv4")
        return false;
    const auto &receipt = *m_completed_cxdlpv4_encoding;
    const auto &document = *m_layer_document;
    const auto &fresh = document.fresh_job();
    if (!receipt.finalized || !receipt.document_generation.current() ||
        !document.export_input_guard().same_source(receipt.document_generation) ||
        document.revision() != receipt.revision || receipt.revision != 0 ||
        document.can_undo() || document.can_redo() || document.mask_plane_count() != 1 ||
        document.layer_count() != receipt.layer_count || document.layer_count() != m_printer_input.size() ||
        !(m_full_print_config == receipt.configuration) || !fresh ||
        fresh->producer != sla::FreshLayerProducer::ModelSlice ||
        fresh->parameter_origin != sla::FreshLayerParameterOrigin::AutomaticModel ||
        fresh->mask_precision != sla::LayerMaskPrecision::NativeGray7)
        return false;
    const auto &geometry = fresh->geometry;
    return geometry.columns == receipt.geometry.columns && geometry.rows == receipt.geometry.rows &&
        geometry.width_mm == receipt.geometry.width_mm && geometry.height_mm == receipt.geometry.height_mm;
}

void SLAPrint::remember_completed_cxdlpv4_encoding()
{
    std::lock_guard<std::mutex> lock(m_completed_cxdlpv4_encoding_mutex);
    m_completed_cxdlpv4_encoding.reset();
    if (!sla::encoded_mask_reuse_detail::experiment_enabled() || m_raster_job ||
        !m_layer_document || !m_archiver || typeid(*m_archiver) != typeid(CrealityCXDLPv4Archive) ||
        m_full_print_config.opt_string("sla_archive_format") != "cxdlpv4")
        return;
    const auto &document = *m_layer_document;
    const auto &fresh = document.fresh_job();
    if (!fresh || fresh->producer != sla::FreshLayerProducer::ModelSlice ||
        fresh->parameter_origin != sla::FreshLayerParameterOrigin::AutomaticModel ||
        fresh->mask_precision != sla::LayerMaskPrecision::NativeGray7 || document.revision() != 0 ||
        document.mask_plane_count() != 1 || document.can_undo() || document.can_redo() ||
        document.layer_count() != m_printer_input.size() || m_archiver->m_layers.size() != document.layer_count())
        return;
    m_completed_cxdlpv4_encoding.emplace(CompletedCXDLPv4Encoding{
        document.export_generation_guard(), document.revision(), m_full_print_config,
        fresh->geometry, document.layer_count()});
}

void SLAPrint::reset_completed_cxdlpv4_encoding()
{
    std::lock_guard<std::mutex> lock(m_completed_cxdlpv4_encoding_mutex);
    m_completed_cxdlpv4_encoding.reset();
}

void SLAPrint::finalize_completed_cxdlpv4_encoding()
{
    // process() is the worker owner. Restored/replaced documents retired the
    // receipt already; failed statistics/cancellation cannot reach this point.
    std::lock_guard<std::mutex> lock(m_completed_cxdlpv4_encoding_mutex);
    if (m_completed_cxdlpv4_encoding)
        m_completed_cxdlpv4_encoding->finalized = true;
}

sla::ExportSnapshot SLAPrint::capture_export_snapshot(const ThumbnailsList &thumbnails,
    const sla::LayerProgress &progress) const
{
    return capture_export_snapshot_impl(thumbnails, progress, true);
}

sla::ExportSnapshot SLAPrint::capture_export_snapshot_impl(const ThumbnailsList &thumbnails,
    const sla::LayerProgress &progress, bool include_native_encoding) const
{
    const auto generation = export_generation_guard();
    // finished() uses state_mutex through milestone checks; never nest it
    // inside the coherent capture lock. The guard detects intervening changes.
    if (!finished()) throw ExportError("Finish slicing before capturing an export snapshot");
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    auto checkpoint = [&](size_t done, size_t total) {
        throw_if_canceled();
        generation.check();
        sla::check_layer_progress(progress, done, total);
    };
    checkpoint(0, m_printer_input.size());
    if (m_printer_input.empty()) throw ExportError("Export snapshot requires finalized layers");
    sla::ExportSnapshot result(generation);
    result.m_configuration = m_full_print_config;
    result.m_placeholders = m_placeholder_parser.config();
    result.m_statistics = print_statistics();
    result.m_recipe = m_calibration_recipe;
    result.m_raster_job = m_raster_job;
    if (m_layer_document) {
        result.m_document = m_layer_document->snapshot([&](size_t done, size_t total) {
            checkpoint(done, total); return true;
        });
        if (include_native_encoding && can_capture_completed_cxdlpv4_encoding()) {
            auto batch = sla::encoded_mask_reuse_detail::copy_cxdlpv4_batch(
                m_archiver->m_layers, m_layer_document->layer_count(),
                [&] { checkpoint(0, m_printer_input.size()); });
            if (batch) {
                result.m_cxdlpv4_encoding_bytes = batch->payload_bytes;
                result.m_cxdlpv4_encoding = std::make_shared<const std::vector<sla::EncodedRaster>>(
                    std::move(batch->layers));
            }
        }
    } else {
        result.m_vector_layers.reserve(m_printer_input.size());
    }
    result.m_levels.reserve(m_printer_input.size());
    for (size_t i = 0; i < m_printer_input.size(); ++i) {
        checkpoint(i, m_printer_input.size());
        const auto &layer = m_printer_input[i];
        result.m_levels.push_back(layer.level());
        if (!m_layer_document)
            result.m_vector_layers.push_back({layer.level(), layer.transformed_slices()});
    }
    result.m_objects.reserve(m_objects.size());
    for (const auto *object : m_objects) {
        checkpoint(result.m_objects.size(), m_objects.size());
        result.m_objects.push_back({object->model_object()->name, object->config()});
    }
    result.m_thumbnails.reserve(thumbnails.size());
    for (const auto &thumbnail : thumbnails) {
        checkpoint(result.m_thumbnails.size(), thumbnails.size());
        result.m_thumbnails.push_back(thumbnail);
    }
    checkpoint(m_printer_input.size(), m_printer_input.size());
    return result;
}

void SLAPrint::write_export_snapshot(const sla::ExportSnapshot &snapshot,
    const sla::ExportGenerationGuard &generation, const std::string &format,
    const std::string &path, const std::string &projectname, bool palette_approved,
    const std::function<void()> &checkpoint)
{
    const auto started = std::chrono::steady_clock::now();
    const auto trace = [&](const char *phase) {
        if (std::getenv("SLA_TRACE_FROZEN_EXPORT"))
            BOOST_LOG_TRIVIAL(info) << "Frozen export " << phase << ": " <<
                std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count() << " s";
    };
    auto check = [&] { generation.check(); if (checkpoint) checkpoint(); generation.check(); };
    check();
    auto configuration = snapshot.configuration();
    std::optional<sla::LayerDocument> projected;
    const auto precision = sla::generated_mask_precision(format,
        configuration.opt_int("sla_cbddlp_aa"), configuration.opt_string("sla_anycubic_version"));
    if (snapshot.document() && snapshot.document()->fresh_job() &&
        precision != sla::LayerMaskPrecision::Gray8 && precision != snapshot.document()->fresh_job()->mask_precision) {
        const auto progress = [&](size_t, size_t) { check(); return true; };
        const auto loss = sla::layer_palette_loss(*snapshot.document(), precision, progress);
        if (loss.changed_pixels && !palette_approved)
            throw ExportError("Export requires explicit approval of a palette conversion copy");
        projected = sla::project_layer_palette_copy(*snapshot.document(), precision, progress);
    }
    std::unique_ptr<SLAPrint> job;
    const auto *document = projected ? &*projected : snapshot.document() ? &*snapshot.document() : nullptr;
    trace("projection ready");
    if (document) {
        std::vector<double> positions; positions.reserve(snapshot.levels().size());
        for (auto level : snapshot.levels()) positions.push_back(double(level) * SCALING_FACTOR);
        job = create_raster_job_impl(configuration, *document, positions, std::nullopt,
            document->fresh_job() ? document->fresh_job()->position_policy : sla::ExposurePositionPolicy::IncreasingLayers,
            &snapshot.statistics(), &generation, checkpoint);
        // The snapshot's exact config and integer grid must survive the
        // temporary raster factory's first-height/coordinate normalization.
        job->m_full_print_config = configuration;
        job->m_material_config.apply(configuration, true);
        trace("raster context ready");
    } else {
        configuration.set("sla_archive_format", format);
        job = std::make_unique<SLAPrint>(); job->apply(Model{}, configuration);
        const auto reason = job->validate(); if (!reason.empty()) throw ExportError(reason);
    }
    job->m_printer_input.clear();
    for (auto level : snapshot.levels()) job->m_printer_input.emplace_back(level);
    job->m_print_statistics = snapshot.statistics();
    job->m_placeholder_parser.apply_config(snapshot.placeholders());
    job->m_calibration_recipe = snapshot.calibration_recipe();
    // Owned metadata objects have no meshes and never point into the source.
    for (const auto &metadata : snapshot.objects()) {
        check();
        auto *object = job->m_model.add_object(); object->name = metadata.name;
        auto *descriptor = new SLAPrintObject(job.get(), object);
        job->m_objects.push_back(descriptor); descriptor->config_apply(metadata.configuration);
    }
    job->m_frozen_export_guard = generation;
    job->m_frozen_export_checkpoint = checkpoint;
    job->m_frozen_export_ready = true;
    if (!projected && document && snapshot.m_cxdlpv4_encoding && format == "cxdlpv4" &&
        snapshot.configuration().opt_string("sla_archive_format") == format) {
        job->m_frozen_cxdlpv4_document = document;
        job->m_frozen_cxdlpv4_configuration = &snapshot.configuration();
        job->m_frozen_cxdlpv4_encoding = snapshot.m_cxdlpv4_encoding;
    }
    trace("metadata ready");
    if (!document) {
        if (snapshot.vector_layers().size() != snapshot.levels().size())
            throw ExportError("Vector snapshot has incomplete contours");
        std::vector<sla::EncodedRaster> masks;
        auto canonical = precision == sla::LayerMaskPrecision::Gray8 ? sla::RasterEncoder(sla::PNGRasterEncoder{}) :
            sla::RasterEncoder(sla::ProjectedPNGMaskEncoder{precision, check});
        job->m_archiver->draw_layers(snapshot.levels().size(), [&](sla::RasterBase &raster, size_t index) {
            check(); for (const auto &polygon : snapshot.vector_layers()[index].contours) raster.draw(polygon);
        }, [&] { check(); return false; }, ex_tbb, &masks, nullptr, canonical);
        if (std::all_of(masks.begin(), masks.end(), [](const auto &mask) { return mask.size() > 0; })) {
            auto owned = job->make_fresh_layer_job(sla::FreshLayerProducer::ModelSlice);
            owned.mask_precision = precision;
            job->m_layer_document = std::make_unique<sla::LayerDocument>(sla::LayerDocument::from_fresh_rasters(
                std::move(masks), std::move(owned), [&](size_t, size_t) { check(); return true; }));
            document = job->m_layer_document.get();
        }
    }
    check();
    if (document) {
        const char *borrow_png_flag = std::getenv("SLA_EXPERIMENT_SL1_BORROW_PNG");
        if ((!borrow_png_flag || std::strcmp(borrow_png_flag, "1") == 0) &&
            (format == "SL1" || format == "SL1S") && !projected &&
            snapshot.document() && document == &*snapshot.document() && job->m_layer_document) {
            // The factory already made this private owned copy. No callback
            // can reach the local job; borrow only from it, never from a public
            // export argument or the editor's independently mutable document.
            document = job->m_layer_document.get();
            job->m_frozen_sl1_document = document;
        }
        job->export_layer_document(*document, format, path, snapshot.thumbnails(), projectname);
    } else {
        // Keep the explicitly selected vector writer; do not re-infer a
        // different schema from the last component of an alias filename.
        sla::publish_export(path, [&](const std::string &temporary) {
            job->m_archiver->export_print(temporary, *job, snapshot.thumbnails(), projectname);
        }, check, get_default_extension(format.c_str()), &generation);
    }
    trace("publication complete");
}

bool sla::ExportGenerationGuard::current() const
{
    // A moved-from guard is empty and must behave as an expired source.
    return m_state && m_state->generation.load(std::memory_order_acquire)==m_generation;
}

void sla::ExportInputGuard::check() const
{
    if (!current()) throw CanceledException{};
}

void sla::ExportGenerationGuard::check() const
{
    if (!current() || cancelled()) throw CanceledException{};
}

sla::ExportGenerationGuard sla::ExportGenerationGuard::with_cancellation() const
{
    check();
    auto result = *this;
    result.m_cancelled = std::make_shared<std::atomic<bool>>(false);
    return result;
}
void sla::ExportGenerationGuard::cancel() const
{
    if (!m_state || !m_cancelled) throw RuntimeError("Cancellation requires an export request guard");
    std::lock_guard<std::mutex> lock(m_state->commit_mutex);
    m_cancelled->store(true, std::memory_order_release);
}

void sla::ExportGenerationGuard::publish(const std::string &staged_path, const std::string &target_path) const
{
    if (!m_state) throw CanceledException{};
    std::lock_guard<std::mutex> lock(m_state->commit_mutex);
    check();
    const auto error = rename_file(staged_path, target_path);
    if (error) throw FileIOError("Cannot replace SLA output: " + error.message());
}

void SLAPrint::advance_export_generation(bool input_changed)
{
    reset_completed_cxdlpv4_encoding();
    // Process-wide IDs also distinguish two jobs with equal local revisions.
    static std::atomic<uint64_t> next{1};
    std::lock_guard<std::mutex> lock(m_export_generation->commit_mutex);
    const auto generation = next.fetch_add(1,std::memory_order_relaxed);
    m_export_generation->generation.store(generation,std::memory_order_release);
    if (input_changed) m_export_generation->input_generation.store(generation,std::memory_order_release);
}

void SLAPrint::reset_layer_document_cache()
{
    advance_export_generation();
    m_layer_document.reset();
}

void SLAPrint::clear()
{
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    advance_export_generation();
    // Raster jobs have no Started/Done slicing milestones. Their exporters
    // still borrow masks and the writer, so milestone invalidation alone does
    // not guarantee that the background owner has stopped.
    this->call_cancel_callback();
    // The following call should stop background processing if it is running.
    this->invalidate_all_steps();
    reset_layer_document_cache();
    m_printer_input.clear();
    m_archiver.reset();
    m_print_statistics.clear();
    m_raster_job_purpose = RasterJobPurpose::CompleteJob;
    m_raster_job = false;
    m_calibration_recipe.reset();
    m_pending_project_layers.reset();
    m_project_layer_restore_failed = false;
    for (SLAPrintObject *object : m_objects)
        delete object;
    m_objects.clear();
    m_model.clear_objects();
}

// Transformation without rotation around Z and without a shift by X and Y.
Transform3d SLAPrint::sla_trafo(const ModelObject &model_object) const
{
    ModelInstance &model_instance = *model_object.instances.front();
    auto trafo = Transform3d::Identity();
    trafo.translate(Vec3d{ 0., 0., model_instance.get_offset().z() * this->relative_correction().z() });
    trafo.linear() = Eigen::DiagonalMatrix<double, 3, 3>(this->relative_correction()) * model_instance.get_matrix().linear();
    if (model_instance.is_left_handed())
        trafo = Eigen::Scaling(Vec3d(-1., 1., 1.)) * trafo;
    return trafo;
}

// List of instances, where the ModelInstance transformation is a composite of sla_trafo and the transformation defined by SLAPrintObject::Instance.
static std::vector<SLAPrintObject::Instance> sla_instances(const ModelObject &model_object)
{
    std::vector<SLAPrintObject::Instance> instances;
    assert(! model_object.instances.empty());
    if (! model_object.instances.empty()) {
        const Transform3d& trafo0 = model_object.instances.front()->get_matrix();
        for (ModelInstance *model_instance : model_object.instances)
            if (model_instance->is_printable()) {
                instances.emplace_back(
                    model_instance->id(),
                    Point::new_scale(model_instance->get_offset(X), model_instance->get_offset(Y)),
                    float(Geometry::rotation_diff_z(trafo0, model_instance->get_matrix())));
            }
    }
    return instances;
}

std::vector<ObjectID> SLAPrint::print_object_ids() const 
{ 
    std::vector<ObjectID> out;
    // Reserve one more for the caller to append the ID of the Print itself.
    out.reserve(m_objects.size() + 1);
    for (const SLAPrintObject *print_object : m_objects)
        out.emplace_back(print_object->id());
    return out;
}

static t_config_option_keys print_config_diffs(const StaticPrintConfig     &current_config,
                                               const DynamicPrintConfig &new_full_config,
                                               DynamicPrintConfig       &material_overrides)
{
    using namespace std::string_view_literals;

    static const constexpr StaticSet overriden_keys = {
        "support_head_front_diameter"sv,
        "support_head_penetration"sv,
        "support_head_width"sv,
        "support_pillar_diameter"sv,
        "branchingsupport_head_front_diameter"sv,
        "branchingsupport_head_penetration"sv,
        "branchingsupport_head_width"sv,
        "branchingsupport_pillar_diameter"sv,
        "support_points_density_relative"sv,
        "elefant_foot_compensation"sv,
        "absolute_correction"sv,
    };

    static constexpr auto material_ow_prefix = "material_ow_";

    t_config_option_keys           print_diff;
    for (const t_config_option_key &opt_key : current_config.keys()) {
        const ConfigOption *opt_old = current_config.option(opt_key);
        assert(opt_old != nullptr);
        const ConfigOption *opt_new = new_full_config.option(opt_key);
        // assert(opt_new != nullptr);
        if (opt_new == nullptr)
            //FIXME This may happen when executing some test cases.
            continue;
        const ConfigOption *opt_new_override = std::binary_search(overriden_keys.begin(), overriden_keys.end(), opt_key) ? new_full_config.option(material_ow_prefix + opt_key) : nullptr;
        if (opt_new_override != nullptr && ! opt_new_override->is_nil()) {
            // An override is available at some of the material presets.
            bool overriden = opt_new->overriden_by(opt_new_override);
            if (overriden || *opt_old != *opt_new) {
                auto opt_copy = opt_new->clone();
                opt_copy->apply_override(opt_new_override);
                bool changed = *opt_old != *opt_copy;
                if (changed)
                    print_diff.emplace_back(opt_key);
                if (changed || overriden) {
                    // overrides will be applied to the placeholder parser, which layers these parameters over full_print_config.
                    material_overrides.set_key_value(opt_key, opt_copy);
                } else
                    delete opt_copy;
            }
        } else if (*opt_new != *opt_old)
            print_diff.emplace_back(opt_key);
    }

    return print_diff;
}


SLAPrint::ApplyStatus SLAPrint::apply(const Model &model, DynamicPrintConfig config, std::vector<std::string> *warnings)
{
    const bool had_layer_edits = finished() && m_layer_document && m_layer_document->revision() != 0;
    // Applying a model/config starts a normal slicing job. Generated jobs are
    // separate documents; their masks cannot survive a geometry replacement.
    if (m_raster_job) clear();
#ifdef _DEBUG
    check_model_ids_validity(model);
#endif /* _DEBUG */

    // Normalize the config.
    config.option("sla_print_settings_id",        true);
    config.option("sla_material_settings_id",     true);
    config.option("printer_settings_id",          true);
    config.option("physical_printer_settings_id", true);
    // Collect changes to print config.
    DynamicPrintConfig mat_overrides;
    t_config_option_keys print_diff    = m_print_config.diff(config);
    t_config_option_keys printer_diff  = print_config_diffs(m_printer_config, config, mat_overrides);
    t_config_option_keys material_diff = m_material_config.diff(config);
    t_config_option_keys object_diff   = print_config_diffs(m_default_object_config, config, mat_overrides);
    t_config_option_keys placeholder_parser_diff = m_placeholder_parser.config_diff(config);

    config.apply(mat_overrides, true);

    // Do not use the ApplyStatus as we will use the max function when updating apply_status.
    unsigned int apply_status = APPLY_STATUS_UNCHANGED;
    auto update_apply_status = [&apply_status](bool invalidated)
        { apply_status = std::max<unsigned int>(apply_status, invalidated ? APPLY_STATUS_INVALIDATED : APPLY_STATUS_CHANGED); };
    if (! (print_diff.empty() && printer_diff.empty() && material_diff.empty() && object_diff.empty()))
        update_apply_status(false);

    // Grab the lock for the Print / PrintObject milestones.
    std::scoped_lock<std::mutex> lock(this->state_mutex());

    // Exporters borrow configuration, including fields that do not invalidate
    // geometry (printer identity, material pricing, filenames, thumbnails).
    // Stop their background owner before replacing any of that storage.
    const bool full_config_changed = !(m_full_print_config == config);
    if (full_config_changed) {
        advance_export_generation();
        this->call_cancel_callback();
        update_apply_status(false);
    }

    // The following call may stop the background processing.
    bool invalidate_all_model_objects = false;
    if (! print_diff.empty())
        update_apply_status(this->invalidate_state_by_config_options(print_diff, invalidate_all_model_objects));
    if (! printer_diff.empty())
        update_apply_status(this->invalidate_state_by_config_options(printer_diff, invalidate_all_model_objects));
    if (! material_diff.empty())
        update_apply_status(this->invalidate_state_by_config_options(material_diff, invalidate_all_model_objects));

    // Multiple beds hack: We currently use one SLAPrint for all beds. It must be invalidated
    // when beds are switched. If not done explicitly, supports from previously sliced object
    // might end up with wrong offset.
    static int last_bed_idx = s_multiple_beds.get_active_bed();
    int current_bed = s_multiple_beds.get_active_bed();
    if (current_bed != last_bed_idx) {
        invalidate_all_model_objects = true;
        last_bed_idx = current_bed;
    }

    // Apply variables to placeholder parser. The placeholder parser is currently used
    // only to generate the output file name.
    if (! placeholder_parser_diff.empty()) {
        // update_apply_status(this->invalidate_step(slapsRasterize));
        m_placeholder_parser.apply_config(config);
        // Set the profile aliases for the PrintBase::output_filename()
        m_placeholder_parser.set("print_preset",            config.option("sla_print_settings_id")->clone());
        m_placeholder_parser.set("material_preset",         config.option("sla_material_settings_id")->clone());
        m_placeholder_parser.set("printer_preset",          config.option("printer_settings_id")->clone());
        m_placeholder_parser.set("physical_printer_preset", config.option("physical_printer_settings_id")->clone());
    }

    // It is also safe to change m_config now after this->invalidate_state_by_config_options() call.
    m_print_config.apply_only(config, print_diff, true);
    m_printer_config.apply_only(config, printer_diff, true);
    if (!printer_diff.empty() && m_printer_config.sla_archive_format.value == "SL1SVG")
        reset_layer_document_cache(); // Pixel edits cannot be encoded as vector contours.
    // Handle changes to material config.
    m_material_config.apply_only(config, material_diff, true);
    // Handle changes to object config defaults
    m_default_object_config.apply_only(config, object_diff, true);

    if (!m_archiver || !printer_diff.empty()) {
        const auto &selected = m_printer_config.sla_archive_format.value;
        // A printer with an unfinished container can still slice and edit PNG
        // masks. Export independently checks its actual destination codec.
        const auto raster_format = archive_export_unavailable_reason(selected.c_str()).empty() ? selected : std::string("SL1");
        m_archiver = SLAArchiveWriter::create(raster_format, m_printer_config);
    }

    struct ModelObjectStatus {
        enum Status {
            Unknown,
            Old,
            New,
            Moved,
            Deleted,
        };
        ModelObjectStatus(ObjectID id, Status status = Unknown) : id(id), status(status) {}
        ObjectID                id;
        Status                  status;
        // Search by id.
        bool operator<(const ModelObjectStatus &rhs) const { return id < rhs.id; }
    };
    std::set<ModelObjectStatus> model_object_status;

    // 1) Synchronize model objects.
    if (model.id() != m_model.id() || invalidate_all_model_objects) {
        // Kill everything, initialize from scratch.
        // Stop background processing.
        this->call_cancel_callback();
        update_apply_status(this->invalidate_all_steps());
        reset_layer_document_cache();
        for (SLAPrintObject *object : m_objects) {
            model_object_status.emplace(object->model_object()->id(), ModelObjectStatus::Deleted);
            update_apply_status(object->invalidate_all_steps());
            delete object;
        }
        m_objects.clear();
        m_model.assign_copy(model);
        for (const ModelObject *model_object : m_model.objects)
            model_object_status.emplace(model_object->id(), ModelObjectStatus::New);
    } else {
        if (model_object_list_equal(m_model, model)) {
            // The object list did not change.
            for (const ModelObject *model_object : m_model.objects)
                model_object_status.emplace(model_object->id(), ModelObjectStatus::Old);
        } else if (model_object_list_extended(m_model, model)) {
            // Add new objects. Their volumes and configs will be synchronized later.
            update_apply_status(this->invalidate_step(slapsMergeSlicesAndEval));
            for (const ModelObject *model_object : m_model.objects)
                model_object_status.emplace(model_object->id(), ModelObjectStatus::Old);
            for (size_t i = m_model.objects.size(); i < model.objects.size(); ++ i) {
                model_object_status.emplace(model.objects[i]->id(), ModelObjectStatus::New);
                m_model.objects.emplace_back(ModelObject::new_copy(*model.objects[i]));
                m_model.objects.back()->set_model(&m_model);
            }
        } else {
            // Reorder the objects, add new objects.
            // First stop background processing before shuffling or deleting the PrintObjects in the object list.
            this->call_cancel_callback();
            update_apply_status(this->invalidate_step(slapsMergeSlicesAndEval));
            // Second create a new list of objects.
            std::vector<ModelObject*> model_objects_old(std::move(m_model.objects));
            m_model.objects.clear();
            m_model.objects.reserve(model.objects.size());
            auto by_id_lower = [](const ModelObject *lhs, const ModelObject *rhs){ return lhs->id() < rhs->id(); };
            std::sort(model_objects_old.begin(), model_objects_old.end(), by_id_lower);
            for (const ModelObject *mobj : model.objects) {
                auto it = std::lower_bound(model_objects_old.begin(), model_objects_old.end(), mobj, by_id_lower);
                if (it == model_objects_old.end() || (*it)->id() != mobj->id()) {
                    // New ModelObject added.
                    m_model.objects.emplace_back(ModelObject::new_copy(*mobj));
                    m_model.objects.back()->set_model(&m_model);
                    model_object_status.emplace(mobj->id(), ModelObjectStatus::New);
                } else {
                    // Existing ModelObject re-added (possibly moved in the list).
                    m_model.objects.emplace_back(*it);
                    model_object_status.emplace(mobj->id(), ModelObjectStatus::Moved);
                }
            }
            bool deleted_any = false;
            for (ModelObject *&model_object : model_objects_old) {
                if (model_object_status.find(ModelObjectStatus(model_object->id())) == model_object_status.end()) {
                    model_object_status.emplace(model_object->id(), ModelObjectStatus::Deleted);
                    deleted_any = true;
                } else
                    // Do not delete this ModelObject instance.
                    model_object = nullptr;
            }
            if (deleted_any) {
                // Delete PrintObjects of the deleted ModelObjects.
                std::vector<SLAPrintObject*> print_objects_old = std::move(m_objects);
                m_objects.clear();
                m_objects.reserve(print_objects_old.size());
                for (SLAPrintObject *print_object : print_objects_old) {
                    auto it_status = model_object_status.find(ModelObjectStatus(print_object->model_object()->id()));
                    assert(it_status != model_object_status.end());
                    if (it_status->status == ModelObjectStatus::Deleted) {
                        update_apply_status(print_object->invalidate_all_steps());
                        delete print_object;
                    } else
                        m_objects.emplace_back(print_object);
                }
                for (ModelObject *model_object : model_objects_old)
                    delete model_object;
            }
        }
    }

    // 2) Map print objects including their transformation matrices.
    struct PrintObjectStatus {
        enum Status {
            Unknown,
            Deleted,
            Reused,
            New
        };
        PrintObjectStatus(SLAPrintObject *print_object, Status status = Unknown) :
            id(print_object->model_object()->id()),
            print_object(print_object),
            trafo(print_object->trafo()),
            status(status) {}
        PrintObjectStatus(ObjectID id) : id(id), print_object(nullptr), trafo(Transform3d::Identity()), status(Unknown) {}
        // ID of the ModelObject & PrintObject
        ObjectID         id;
        // Pointer to the old PrintObject
        SLAPrintObject  *print_object;
        // Trafo generated with model_object->world_matrix(true)
        Transform3d      trafo;
        Status           status;
        // Search by id.
        bool operator<(const PrintObjectStatus &rhs) const { return id < rhs.id; }
    };
    std::multiset<PrintObjectStatus> print_object_status;
    for (SLAPrintObject *print_object : m_objects)
        print_object_status.emplace(PrintObjectStatus(print_object));

    // 3) Synchronize ModelObjects & PrintObjects.
    std::vector<SLAPrintObject*> print_objects_new;
    print_objects_new.reserve(std::max(m_objects.size(), m_model.objects.size()));
    bool new_objects = false;
    for (size_t idx_model_object = 0; idx_model_object < model.objects.size(); ++ idx_model_object) {
        ModelObject &model_object = *m_model.objects[idx_model_object];
        auto it_status = model_object_status.find(ModelObjectStatus(model_object.id()));
        assert(it_status != model_object_status.end());
        assert(it_status->status != ModelObjectStatus::Deleted);
        // PrintObject for this ModelObject, if it exists.
        auto it_print_object_status = print_object_status.end();
        if (it_status->status != ModelObjectStatus::New) {
            // Update the ModelObject instance, possibly invalidate the linked PrintObjects.
            assert(it_status->status == ModelObjectStatus::Old || it_status->status == ModelObjectStatus::Moved);
            const ModelObject &model_object_new       = *model.objects[idx_model_object];
            it_print_object_status = print_object_status.lower_bound(PrintObjectStatus(model_object.id()));
            if (it_print_object_status != print_object_status.end() && it_print_object_status->id != model_object.id())
                it_print_object_status = print_object_status.end();
            // Check whether a model part volume was added or removed, their transformations or order changed.
            bool model_parts_differ =
                model_volume_list_changed(model_object, model_object_new,
                                          {ModelVolumeType::MODEL_PART,
                                           ModelVolumeType::NEGATIVE_VOLUME,
                                           ModelVolumeType::SUPPORT_ENFORCER,
                                           ModelVolumeType::SUPPORT_BLOCKER});
            bool sla_trafo_differs  =
                model_object.instances.empty() != model_object_new.instances.empty() ||
                (! model_object.instances.empty() &&
                  (! sla_trafo(model_object).isApprox(sla_trafo(model_object_new)) ||
                    model_object.instances.front()->is_left_handed() != model_object_new.instances.front()->is_left_handed()));
            if (model_parts_differ || sla_trafo_differs) {
                // The very first step (the slicing step) is invalidated. One may freely remove all associated PrintObjects.
                if (it_print_object_status != print_object_status.end()) {
                    update_apply_status(it_print_object_status->print_object->invalidate_all_steps());
                    const_cast<PrintObjectStatus&>(*it_print_object_status).status = PrintObjectStatus::Deleted;
                }
                // Copy content of the ModelObject including its ID, do not change the parent.
                model_object.assign_copy(model_object_new);
            } else {
                // Synchronize Object's config.
                bool object_config_changed = ! model_object.config.timestamp_matches(model_object_new.config);
                if (object_config_changed)
                    model_object.config.assign_config(model_object_new.config);
                if (! object_diff.empty() || object_config_changed) {
                    SLAPrintObjectConfig new_config = m_default_object_config;
                    new_config.apply(model_object.config.get(), true);
                    if (it_print_object_status != print_object_status.end()) {
                        t_config_option_keys diff = it_print_object_status->print_object->config().diff(new_config);
                        if (! diff.empty()) {
                            update_apply_status(it_print_object_status->print_object->invalidate_state_by_config_options(diff));
                            it_print_object_status->print_object->config_apply_only(new_config, diff, true);
                        }
                    }
                }

                bool old_user_modified = model_object.sla_points_status == sla::PointsStatus::UserModified;
                bool new_user_modified = model_object_new.sla_points_status == sla::PointsStatus::UserModified;
                if ((old_user_modified && ! new_user_modified) || // switching to automatic supports from manual supports
                    (! old_user_modified && new_user_modified) || // switching to manual supports from automatic supports
                    (new_user_modified && model_object.sla_support_points != model_object_new.sla_support_points)) {
                    if (it_print_object_status != print_object_status.end())
                        update_apply_status(it_print_object_status->print_object->invalidate_step(slaposSupportPoints));

                    model_object.sla_support_points = model_object_new.sla_support_points;
                }
                model_object.sla_points_status = model_object_new.sla_points_status;
                
                // Invalidate hollowing if drain holes have changed
                if (model_object.sla_drain_holes != model_object_new.sla_drain_holes)
                {
                    model_object.sla_drain_holes = model_object_new.sla_drain_holes;
                    update_apply_status(it_print_object_status->print_object->invalidate_step(slaposDrillHoles));
                }

                // Copy the ModelObject name, input_file and instances. The instances will compared against PrintObject instances in the next step.
                model_object.name       = model_object_new.name;
                model_object.input_file = model_object_new.input_file;
                model_object.clear_instances();
                model_object.instances.reserve(model_object_new.instances.size());
                for (const ModelInstance *model_instance : model_object_new.instances) {
                    model_object.instances.emplace_back(new ModelInstance(*model_instance));
                    model_object.instances.back()->set_model_object(&model_object);
                }
            }
        }

        std::vector<SLAPrintObject::Instance> new_instances = sla_instances(model_object);
        if (it_print_object_status != print_object_status.end() && it_print_object_status->status != PrintObjectStatus::Deleted) {
            // The SLAPrintObject is already there.
            if (new_instances.empty()) {
                const_cast<PrintObjectStatus&>(*it_print_object_status).status = PrintObjectStatus::Deleted;
            } else {
                if (new_instances != it_print_object_status->print_object->instances()) {
                    // Instances changed.
                    it_print_object_status->print_object->set_instances(new_instances);
                    update_apply_status(this->invalidate_step(slapsMergeSlicesAndEval));
                }
                print_objects_new.emplace_back(it_print_object_status->print_object);
                const_cast<PrintObjectStatus&>(*it_print_object_status).status = PrintObjectStatus::Reused;
            }
        } else if (! new_instances.empty()) {
            auto print_object = new SLAPrintObject(this, &model_object);

            // FIXME: this invalidates the transformed mesh in SLAPrintObject
            // which is expensive to calculate (especially the raw_mesh() call)
            print_object->set_trafo(sla_trafo(model_object), model_object.instances.front()->is_left_handed());

            print_object->set_instances(std::move(new_instances));

            print_object->config_apply(m_default_object_config, true);
            print_object->config_apply(model_object.config.get(), true);
            print_objects_new.emplace_back(print_object);
            new_objects = true;
        }
    }

    if (m_objects != print_objects_new) {
        this->call_cancel_callback();
        update_apply_status(this->invalidate_all_steps());
        reset_layer_document_cache();
        m_objects = print_objects_new;
        // Delete the PrintObjects marked as Unknown or Deleted.
        for (auto &pos : print_object_status)
            if (pos.status == PrintObjectStatus::Unknown || pos.status == PrintObjectStatus::Deleted) {
                update_apply_status(pos.print_object->invalidate_all_steps());
                delete pos.print_object;
            }
        if (new_objects)
            update_apply_status(false);
    }

    if(m_objects.empty()) {
        m_printer_input = {};
        m_print_statistics = {};
    }

#ifdef _DEBUG
    check_model_ids_equal(m_model, model);
#endif /* _DEBUG */

    // A no-op apply must keep option addresses valid for an active exporter.
    if (full_config_changed)
        m_full_print_config = std::move(config);

    if (m_layer_document && std::any_of(material_diff.begin(), material_diff.end(), [](const auto &key) {
            return key == "material_density" || key == "bottle_cost" || key == "bottle_volume";
        })) {
        // Pricing does not invalidate slicing. Refresh its cached totals now,
        // so the sidebar and filename placeholders need no further process().
        // Reuse the measured volume instead of inflating every mask again.
        const auto &material = m_material_config;
        const double ml = (m_print_statistics.objects_used_material + m_print_statistics.support_used_material) / 1000.;
        const double density = material.material_density.value;
        const double cost = material.bottle_cost.value, volume = material.bottle_volume.value;
        const double weight = ml * density;
        const double price = volume > 0 ? ml * cost / volume : 0.;
        // Invalid drafts are rejected by validation/export. Keep the last
        // finite totals until then; never introduce NaN/overflow into the cache.
        if (std::isfinite(ml) && ml >= 0 && std::isfinite(density) && density >= 0 &&
            std::isfinite(cost) && cost >= 0 && std::isfinite(volume) && volume >= 0 &&
            std::isfinite(weight) && weight >= 0 && std::isfinite(price) && price >= 0) {
            m_print_statistics.total_weight = weight;
            m_print_statistics.total_cost = price;
        }
    }

    if (had_layer_edits && !m_layer_document && warnings)
        warnings->push_back(_u8L("The model or raster settings changed. Previously applied pixel edits were discarded; the layers must be sliced again."));

    return static_cast<ApplyStatus>(apply_status);
}

// Generate a recommended output file name based on the format template, default extension, and template parameters
// (timestamps, object placeholders derived from the model, current placeholder prameters and print statistics.
// Use the final print statistics if available, or just keep the print statistics placeholders if not available yet (before the output is finalized).
std::string SLAPrint::output_filename(const std::string &filename_base) const
{
    DynamicConfig config = this->finished() ? this->print_statistics().config() : this->print_statistics().placeholders();
    std::string default_ext = get_default_extension(m_printer_config.sla_archive_format.value.c_str());
    if (default_ext.empty())
        default_ext = "sl1";

    default_ext.insert(default_ext.begin(), '.');

    config.set_key_value("default_output_extension",
                         new ConfigOptionString(default_ext));

    const auto filename = this->PrintBase::output_filename(m_print_config.output_filename_format.value, default_ext, filename_base, &config);
    // A decimal statistic at the end of a template is part of its basename,
    // not an archive extension. Supply the printer suffix before save dialogs
    // separate stem/extension, otherwise e.g. 16.8 silently becomes 16.
    const auto suffix = boost::filesystem::path(filename).extension().string();
    return get_archive_entry_by_extension(suffix.c_str()) ? filename : filename + default_ext;
}

std::string SLAPrint::validate(std::vector<std::string>*) const
{
    // Report container capability errors before the expensive slicing job.
    if (m_printer_config.sla_archive_format.value == "photons") {
        try { ChituboxPhotonSArchive::validate_configuration(*this); }
        catch (const std::exception &error) { return error.what(); }
    }

    const auto &format=m_printer_config.sla_archive_format.value;
    if (!anycubic_available_versions(format).empty()) {
        try {
            const auto version=sla::generated_anycubic_version(format,m_printer_config.sla_anycubic_version.value);
            if (version && *version>=516 && *version<=518 && !m_model.objects.empty()) {
                // This is a defaults-only preflight. Actual Z, per-row edits,
                // palette and publication are still validated during export.
                // Imported/raster jobs own their document intent; before it is
                // attached, ambient material defaults cannot validate that job.
                const auto error=sla::anycubic_generated_settings_error(m_material_config);
                if (!error.empty()) return error;
            }
        } catch (const std::exception &error) { return error.what(); }
    }
    if (format=="cbddlp" || format=="photon" || format=="cbddlp_v1" || format=="photon_v1") {
        try { ChituboxCBDDLPArchive::validate_configuration(*this,format=="cbddlp_v1" || format=="photon_v1" ? 1 : 2); }
        catch (const std::exception &error) { return error.what(); }
    }
    if (m_printer_config.sla_archive_format.value == "ctb_v1" || m_printer_config.sla_archive_format.value == "cbt") {
        try { ChituboxCTBv1Archive::validate_configuration(*this); }
        catch (const std::exception &error) { return error.what(); }
    }
    if (m_printer_config.sla_archive_format.value == "ctb_encrypted" ||
        m_printer_config.sla_archive_format.value == "ctb_encrypted_v4") {
        try { CTBEncryptedArchive::validate_configuration(*this); }
        catch (const std::exception &error) { return error.what(); }
    }
    if (m_printer_config.sla_archive_format.value == "ctb_v3" ||
        m_printer_config.sla_archive_format.value == "ctb_v4" ||
        m_printer_config.sla_archive_format.value == "ctb_v5" ||
        m_printer_config.sla_archive_format.value == "ctb_gktwo") {
        const auto &format = m_printer_config.sla_archive_format.value;
        const unsigned version = format == "ctb_v3" ? 3 : format == "ctb_v5" ? 5 : 4;
        try { ChituboxCTBModernArchive::validate_configuration(*this, version); }
        catch (const std::exception &error) { return error.what(); }
    }
    if (m_printer_config.sla_archive_format.value == "nanodlp" ||
        m_printer_config.sla_archive_format.value == "nanodlp_v1" ||
        m_printer_config.sla_archive_format.value == "nanodlp_rgb" ||
        m_printer_config.sla_archive_format.value == "nanodlp_rgb_v1") {
        try { NanoDLPArchive::validate_configuration(*this); }
        catch (const std::exception &error) { return error.what(); }
    }
    if (m_printer_config.sla_archive_format.value == "fdg") {
        try { FDGArchive::validate_configuration(*this); }
        catch (const std::exception &error) { return error.what(); }
    }
    if (m_printer_config.sla_archive_format.value == "svgx") {
        try { FlashforgeSVGXArchive::validate_configuration(*this); }
        catch (const std::exception &error) { return error.what(); }
    }
    if (m_printer_config.sla_archive_format.value == "phz") {
        try { PhrozenPHZArchive::validate_configuration(*this); }
        catch (const std::exception &error) { return error.what(); }
    }
    if (const uint32_t model = LongerOrangeArchive::model_for_format(m_printer_config.sla_archive_format.value, m_printer_config.printer_model.value)) {
        try { LongerOrangeArchive::validate_configuration(*this, model); }
        catch (const std::exception &error) { return error.what(); }
    }
    if (m_printer_config.sla_archive_format.value == "gr1" || m_printer_config.sla_archive_format.value == "mdlp") {
        try { MakerbaseArchive::validate_configuration(*this, m_printer_config.sla_archive_format.value == "mdlp"); }
        catch (const std::exception &error) { return error.what(); }
    }
    if (m_printer_config.sla_archive_format.value == "jxs") {
        try { UniformationGKoneArchive::validate_configuration(*this); }
        catch (const std::exception &error) { return error.what(); }
    }
    if (m_printer_config.sla_archive_format.value == "zcode") {
        try { ZCodeArchive::validate_configuration(*this); }
        catch (const std::exception &error) { return error.what(); }
    }
    if (m_printer_config.sla_archive_format.value == "zcodex") {
        try { ZCodexArchive::validate_configuration(*this); }
        catch (const std::exception &error) { return error.what(); }
    }
    if (m_printer_config.sla_archive_format.value == "vdt") {
        try { VoxeldanceTangoArchive::validate_configuration(*this); }
        catch (const std::exception &error) { return error.what(); }
    }
    if (m_printer_config.sla_archive_format.value == "klipper" || m_printer_config.sla_archive_format.value == "klipper_rgb") {
        try { KlipperArchive::validate_configuration(*this); }
        catch (const std::exception &error) { return error.what(); }
    }
    if (m_printer_config.sla_archive_format.value == "qdt") {
        try { Emake3DGalaxyArchive::validate_configuration(*this); }
        catch (const std::exception &error) { return error.what(); }
    }
    for (const auto &setting : sla::machine_settings) {
        const auto *option = m_material_config.option(setting.key);
        if (!option) continue;
        const double value = setting.integer ? option->getInt() : option->getFloat();
        if (!std::isfinite(value) || value < 0 ||
            (std::string(setting.key).find("light_pwm") != std::string::npos && value > 255))
            return std::string("Invalid LCD setting: ") + setting.label;
    }
    for (const bool bottom : {false, true}) {
        const std::string prefix = bottom ? "sla_bottom_" : "sla_";
        auto value = [&](const char *key) { return m_material_config.option(prefix + key)->getFloat(); };
        const double height = value("lift_height"), height2 = value("lift_height2");
        if ((height > 0 && value("lift_speed") <= 0) || (height2 > 0 && value("lift_speed2") <= 0) ||
            (height + height2 > 0 && value("retract_speed") <= 0) ||
            (value("retract_height2") > 0 && value("retract_speed2") <= 0))
            return "LCD motion speeds must be greater than zero for enabled moves";
        if (value("retract_height2") > height + height2)
            return "The second retract height cannot exceed the total lift height";
    }
    for(SLAPrintObject * po : m_objects) {

        const ModelObject *mo = po->model_object();
        bool supports_en = po->config().supports_enable.getBool();

        if(supports_en &&
           mo->sla_points_status == sla::PointsStatus::UserModified &&
           mo->sla_support_points.empty())
            return _u8L("Cannot proceed without support points! "
                     "Add support points or disable support generation.");

        sla::SupportTreeConfig cfg = make_support_cfg(po->config());

        double elv = cfg.object_elevation_mm;
        
        sla::PadConfig padcfg = make_pad_cfg(po->config());
        sla::PadConfig::EmbedObject &builtinpad = padcfg.embed_object;
        
        if(supports_en && !builtinpad.enabled && elv < cfg.head_fullwidth())
            return _u8L(
                "Elevation is too low for object. Use the \"Pad around "
                "object\" feature to print the object without elevation.");
        
        if(supports_en && builtinpad.enabled &&
           cfg.pillar_base_safety_distance_mm < builtinpad.object_gap_mm) {
            return _u8L(
                "The endings of the support pillars will be deployed on the "
                "gap between the object and the pad. 'Support base safety "
                "distance' has to be greater than the 'Pad object gap' "
                "parameter to avoid this.");
        }
        
        std::string pval = padcfg.validate();
        if (!pval.empty()) return pval;
    }

    double expt_max = m_printer_config.max_exposure_time.getFloat();
    double expt_min = m_printer_config.min_exposure_time.getFloat();
    double expt_cur = m_material_config.exposure_time.getFloat();

    if (expt_cur < expt_min || expt_cur > expt_max)
        return _u8L("Exposition time is out of printer profile bounds.");

    double iexpt_max = m_printer_config.max_initial_exposure_time.getFloat();
    double iexpt_min = m_printer_config.min_initial_exposure_time.getFloat();
    double iexpt_cur = m_material_config.initial_exposure_time.getFloat();

    if (iexpt_cur < iexpt_min || iexpt_cur > iexpt_max)
        return _u8L("Initial exposition time is out of printer profile bounds.");

    for (const std::string &prefix : { std::string(), std::string("branching") }) {

        double head_penetration = m_full_print_config.opt_float(prefix + "support_head_penetration");
        double head_width       = m_full_print_config.opt_float(prefix + "support_head_width");

        if (head_penetration > head_width) {
            return _u8L("Invalid Head penetration\n"
                        "Head penetration should not be greater than the Head width.\n"
                        "Please check value of Head penetration in Print Settings or Material Overrides.");
        }

        double pinhead_d = m_full_print_config.opt_float(prefix + "support_head_front_diameter");
        double pillar_d  = m_full_print_config.opt_float(prefix + "support_pillar_diameter");

        if (pinhead_d > pillar_d) {
            return _u8L("Invalid pinhead diameter\n"
                        "Pinhead front diameter should be smaller than the Pillar diameter.\n"
                        "Please check value of Pinhead front diameter in Print Settings or Material Overrides.");
        }
    }

    if ((!m_material_config.use_tilt.get_at(0) && is_approx(m_material_config.tower_hop_height.get_at(0), 0.))
        || (!m_material_config.use_tilt.get_at(1) && is_approx(m_material_config.tower_hop_height.get_at(1), 0.)))
        return _u8L("Disabling the 'Use tilt' function causes the object to separate away from the film in the "
                    "vertical direction only. Therefore, it is necessary to set the 'Tower hop height' parameter "
                    "to reasonable value. The recommended value is 5 mm.");

    return "";
}

namespace {
void validate_layer_document(const SLAPrint &print, const sla::LayerDocument &document,
                             bool transferring_parameters = false, const std::function<void()> &checkpoint = {})
{
    if (checkpoint) checkpoint();
    if (!transferring_parameters && !document.fresh_job() && !document.parameter_capabilities().empty())
        throw RuntimeError("A native parameter table requires a preserving conversion; use Export layers or save the native archive");
    if (document.mask_plane_count()>1)
        throw RuntimeError("Multi-plane layer documents must use their native archive writer; raster conversion cannot preserve secondary masks yet");
    if (!document.layer_count() || document.layer_count() != print.print_layers().size())
        throw RuntimeError("Layer document does not match the sliced result");
    const auto first = document.image(0);
    if (checkpoint) checkpoint();
    const auto &config = print.printer_config();
    const bool portrait = config.display_orientation.value == sladoPortrait;
    const auto width = portrait ? config.display_pixels_y.value : config.display_pixels_x.value;
    const auto height = portrait ? config.display_pixels_x.value : config.display_pixels_y.value;
    if (first.cols != size_t(width) || first.rows != size_t(height))
        throw RuntimeError("Edited layers must retain the printer's oriented raster dimensions");
}
void validate_fresh_statistic_inputs(const SLAPrint &print, double raster_area)
{
    const auto &material = print.material_config();
    sla::finite_fresh_statistic(raster_area, "raster area");
    sla::finite_fresh_statistic(material.material_density.value, "material density");
    sla::finite_fresh_statistic(material.bottle_cost.value, "bottle cost");
    sla::finite_fresh_statistic(material.bottle_volume.value, "bottle volume");
}

SLAPrintStatistics raster_job_statistics(const SLAPrint &print, const sla::LayerDocument &document,
    const std::function<void()> &check = {})
{
    const bool profile = std::getenv("SLA_PROFILE_MASK_STATISTICS") != nullptr;
    const auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto checkpoint = [&] { if (check) check(); };
    validate_layer_document(print, document, false, checkpoint);
    const auto &fresh = document.fresh_job();
    if (fresh) fresh->validate(document.layer_count(), [&](size_t, size_t) {
        checkpoint(); return true;
    });
    const auto &printer = print.printer_config();
    const bool portrait = printer.display_orientation.value == sladoPortrait;
    const size_t columns = fresh ? fresh->geometry.columns : size_t(portrait ? printer.display_pixels_y.value : printer.display_pixels_x.value);
    const size_t rows = fresh ? fresh->geometry.rows : size_t(portrait ? printer.display_pixels_x.value : printer.display_pixels_y.value);
    const double raster_area = fresh ? fresh->geometry.width_mm * fresh->geometry.height_mm :
        printer.display_width.value * printer.display_height.value;
    const double pixel_area = raster_area / double(columns * rows);
    SLAPrintStatistics statistics;
    statistics.raster_area_mm2 = raster_area;
    const auto &material = print.material_config();
    if (fresh) {
        statistics.time_basis = SLAPrintStatistics::TimeBasis::OwnedNeutralSchedule;
        validate_fresh_statistic_inputs(print, raster_area);
    }
    // Equality compares the complete PNG bytes, so hash collisions cannot
    // conflate masks. Keys borrow immutable bytes for this calculation only.
    std::unordered_map<std::string_view, size_t> repeated_masks;
    std::vector<size_t> representative(document.layer_count()), unique;
    std::vector<double> areas(document.layer_count());
    for (size_t i=0;i<document.layer_count();++i) {
        checkpoint();
        const auto encoded=document.encoded_layer(i);
        const std::string_view key{static_cast<const char *>(encoded.buf),encoded.sz};
        const auto found=repeated_masks.find(key);
        if (found!=repeated_masks.end()) representative[i]=found->second;
        else {
            representative[i]=i; unique.push_back(i);
            if (repeated_masks.size()<1024) repeated_masks.emplace(key,i);
        }
    }
    // Reuse only a fully validated count owned by these exact PNG bytes.
    // Edits and project restores take the decoding fallback. Repeated-Z
    // groups below still decode their actual union when masks differ.
    std::vector<size_t> pending;
    pending.reserve(unique.size());
    for (const auto i : unique) {
        checkpoint();
        if (const auto cached = document.validated_mask_statistics(i)) {
            if (cached->columns != columns || cached->rows != rows)
                throw RuntimeError("Raster job masks must match the oriented printer resolution");
            areas[i] = double(cached->nonzero_pixels) * pixel_area;
        } else pending.push_back(i);
    }
    // Full-resolution PNG inflation dominates this step. Bound concurrent
    // decoded buffers to the same 512 MiB budget as rasterization/encoding.
    const int workers=int(std::clamp<uint64_t>((uint64_t(512)<<20)/
        std::max<uint64_t>(1,uint64_t(columns)*rows*4),1,8));
    tbb::task_arena arena(workers);
    // Keep arbitrary request/UI checkpoints on the coordinator. A bounded
    // batch limits cancellation latency without invoking callbacks on workers.
    for (size_t first=0; first<pending.size(); first+=size_t(workers)) {
      checkpoint();
      const auto end=std::min(first+size_t(workers),pending.size());
      arena.execute([&] { execution::for_each(ExecutionTBB{},first,end,[&](size_t index) {
        const auto i=pending[index];
        const auto mask=document.image(i);
        if (mask.cols!=columns || mask.rows!=rows)
            throw RuntimeError("Raster job masks must match the oriented printer resolution");
        areas[i]=double(std::count_if(mask.buf.begin(),mask.buf.end(),[](uint8_t value){return value!=0;}))*pixel_area;
      }); });
      checkpoint();
    }
    double previous_z = 0;
    const bool repeated = fresh && fresh->position_policy == sla::ExposurePositionPolicy::RepeatedExposureHeights;
    for (size_t i = 0; i < document.layer_count(); ++i) {
        checkpoint();
        const double area=areas[representative[i]];
        const double z = fresh ? fresh->rows[i].at(sla::LayerParameter::PositionZ) :
            double(print.print_layers()[i].level()) * SCALING_FACTOR;
        statistics.layers_areas.push_back(area);
        if (!repeated) statistics.objects_used_material += area * (z-previous_z);
        statistics.estimated_print_time += fresh ? sla::fresh_layer_schedule_time(fresh->rows[i]) : sla::lcd_layer_time(print, i);
        if (fresh) {
            sla::finite_fresh_statistic(statistics.objects_used_material, "material volume");
            sla::finite_fresh_statistic(statistics.estimated_print_time, "cumulative schedule time");
        }
        statistics.layers_times_running_total.push_back(statistics.estimated_print_time);
        previous_z = z;
    }
    if (repeated) {
        std::vector<double> positions;
        positions.reserve(fresh->rows.size());
        for (const auto &row : fresh->rows) positions.push_back(row.at(sla::LayerParameter::PositionZ));
        std::unique_ptr<sla::ExposureMaskUnion> occupied;
        for (const auto &group : sla::group_exposure_positions(positions, fresh->position_policy)) {
            checkpoint();
            std::unordered_set<size_t> masks;
            for (size_t i = group.first_record; i < group.past_last_record; ++i) masks.insert(representative[i]);
            double area = areas[*masks.begin()];
            if (masks.size() > 1) {
                if (!occupied) occupied = std::make_unique<sla::ExposureMaskUnion>(columns * rows);
                occupied->reset();
                for (size_t index : masks) {
                    checkpoint();
                    occupied->append(document.image(index).buf);
                }
                area = double(occupied->nonzero_pixels()) * pixel_area;
            }
            statistics.objects_used_material = sla::finite_fresh_statistic(
                statistics.objects_used_material + area * group.thickness, "physical exposure union volume");
        }
    }
    statistics.slow_layers_count = fresh ? fresh->globals.bottom_layers : sla::bottom_layers(print, document.layer_count());
    statistics.fast_layers_count = document.layer_count()-statistics.slow_layers_count;
    const double milliliters = statistics.objects_used_material / 1000.;
    statistics.total_weight = milliliters * material.material_density.value;
    if (material.bottle_volume.value > 0)
        statistics.total_cost = milliliters * material.bottle_cost.value / material.bottle_volume.value;
    // A zero-volume price has no usable unit price; source statistics retain
    // cost0. Native exporters enforce their own stricter material contracts.
    if (fresh) {
        sla::finite_fresh_statistic(statistics.total_weight, "material weight");
        sla::finite_fresh_statistic(statistics.total_cost, "material cost");
    }
    if (profile)
        BOOST_LOG_TRIVIAL(info) << "SLA mask statistics: unique=" << unique.size()
            << "; reused=" << unique.size() - pending.size() << "; decoded=" << pending.size()
            << "; seconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return statistics;
}


}

sla::FreshLayerJob SLAPrint::make_fresh_layer_job(sla::FreshLayerProducer producer,
    sla::ExposurePositionPolicy position_policy) const
{
    std::vector<double> positions;
    positions.reserve(print_layers().size());
    for (const auto &layer : print_layers())
        positions.push_back(double(layer.level()) * SCALING_FACTOR);
    return sla::make_fresh_layer_job(printer_config(), material_config(),
        default_object_config().layer_height.value, positions, producer, position_policy);
}

std::unique_ptr<SLAPrint> SLAPrint::create_raster_job(const DynamicPrintConfig &configuration,
    const sla::LayerDocument &document, const std::vector<double> &positions_z,
    std::optional<sla::FreshLayerProducer> producer, sla::ExposurePositionPolicy position_policy)
{
    return create_raster_job_impl(configuration, document, positions_z, producer, position_policy, nullptr, nullptr, {});
}

std::unique_ptr<SLAPrint> SLAPrint::create_raster_job_impl(const DynamicPrintConfig &configuration,
    const sla::LayerDocument &document, const std::vector<double> &positions_z,
    std::optional<sla::FreshLayerProducer> producer, sla::ExposurePositionPolicy position_policy,
    const SLAPrintStatistics *statistics, const sla::ExportGenerationGuard *generation,
    const std::function<void()> &checkpoint, RasterJobPurpose purpose)
{
    // This mode is private and requires a complete format-owned export plan
    // at its only call site. It must never masquerade as a public raster job
    // or consume captured statistics for a potentially different edited mask.
    if (purpose == RasterJobPurpose::ValidatedFreshEncoding &&
        (statistics || producer || !generation || !checkpoint ||
         (configuration.opt_string("sla_archive_format") != "cxdlpv4" &&
          configuration.opt_string("sla_archive_format") != "SL1" &&
          configuration.opt_string("sla_archive_format") != "SL1S")))
        throw RuntimeError("Invalid internal fresh encoding context");
    if (purpose == RasterJobPurpose::PlannedConversionDispatch &&
        (statistics || producer || !generation || !checkpoint ||
         position_policy != sla::ExposurePositionPolicy::IncreasingLayers))
        throw RuntimeError("Invalid internal conversion dispatch context");
    // Repeated heights require explicit ownership; an imported raster is not
    // silently reclassified by inspecting equal coordinates.
    if (position_policy == sla::ExposurePositionPolicy::RepeatedExposureHeights && !producer &&
        (!document.fresh_job() || document.fresh_job()->position_policy != position_policy))
        throw RuntimeError("Repeated exposure production requires explicit intentions");
    if (!document.layer_count() || positions_z.size() != document.layer_count())
        throw RuntimeError("A raster job requires one Z position per mask");
    DynamicPrintConfig config;
    config.apply(SLAFullPrintConfig{}); config.apply(configuration);
    if (config.option("printer_technology")->getInt() != ptSLA)
        throw RuntimeError("A raster job requires an SLA printer profile");
    const auto *format_entry = get_archive_entry(config.opt_string("sla_archive_format").c_str());
    if (!format_entry) throw RuntimeError("Unknown raster job archive format");
    config.set("sla_archive_format", std::string(format_entry->id));
    if (config.opt_string("sla_archive_format") == "SL1SVG")
        throw RuntimeError("A generated raster job requires a pixel-based format");
    auto job = std::make_unique<SLAPrint>();
    if (generation) {
        job->m_frozen_export_guard = *generation;
        job->m_frozen_export_checkpoint = checkpoint;
    }
    job->throw_if_canceled();
    job->apply(Model{}, config);
    const auto validation = job->validate();
    if (!validation.empty()) throw RuntimeError(validation);
    for (const char *key : {"exposure_time", "initial_exposure_time", "layer_height"}) {
        const double value = config.opt_float(key);
        if (!std::isfinite(value) || value <= 0) throw RuntimeError(std::string("Invalid raster job setting: ") + key);
    }
    const auto &printer = job->printer_config();
    if (printer.display_pixels_x.value <= 0 || printer.display_pixels_y.value <= 0 ||
        !std::isfinite(printer.display_width.value) || !std::isfinite(printer.display_height.value) ||
        printer.display_width.value <= 0 || printer.display_height.value <= 0 ||
        !std::isfinite(printer.max_print_height.value) || printer.max_print_height.value <= 0)
        throw RuntimeError("Invalid raster job printer geometry");
    sla::ExposurePositionValidator positions(position_policy);
    for (size_t i = 0; i < positions_z.size(); ++i) {
        job->throw_if_canceled();
        const double z = positions_z[i];
        const bool new_height = positions.append(z);
        if (z > printer.max_print_height.value ||
            z / SCALING_FACTOR > double(std::numeric_limits<coord_t>::max()))
            throw RuntimeError("Raster job Z must increase within the printer height");
        const coord_t level = scaled<coord_t>(z);
        if (!job->m_printer_input.empty() &&
            (level < job->m_printer_input.back().level() || (new_height && level == job->m_printer_input.back().level())))
            throw RuntimeError("Raster job Z increments are below coordinate precision");
        job->m_printer_input.emplace_back(level);
    }
    // Synchronize the first layer height with the actual first raster level;
    // encoders must not reuse an unrelated process preset's initial height.
    job->m_material_config.initial_layer_height.value = positions_z.front();
    job->m_full_print_config.set("initial_layer_height", positions_z.front());
    if (producer) {
        // Explicit production is distinct from importing/converting masks.
        // Never replace an existing native or owned job's independent intent.
        if (document.fresh_job() || !document.archive_format().empty() ||
            !document.parameter_capabilities().empty() || document.mask_plane_count() != 1)
            throw RuntimeError("Fresh production requires unowned canonical masks");
        auto fresh = job->make_fresh_layer_job(*producer, position_policy);
        fresh.mask_precision=sla::generated_mask_precision(job->m_printer_config.sla_archive_format.value,
            job->m_printer_config.sla_cbddlp_aa.value, job->m_printer_config.sla_anycubic_version.value);
        for (size_t i = 0; i < positions_z.size(); ++i)
            fresh.rows[i][sla::LayerParameter::PositionZ] = positions_z[i];
        fresh.validate(document.layer_count());
        std::vector<sla::EncodedRaster> masks;
        masks.reserve(document.layer_count());
        for (size_t i = 0; i < document.layer_count(); ++i) {
            const auto bytes = document.encoded_layer(i);
            const auto *begin = static_cast<const uint8_t *>(bytes.buf);
            masks.emplace_back(std::vector<uint8_t>(begin, begin + bytes.sz), "png");
        }
        job->m_layer_document = std::make_unique<sla::LayerDocument>(
            sla::LayerDocument::from_generated_rasters(std::move(masks), std::move(fresh)));
    } else {
        if (generation)
            job->m_layer_document = std::make_unique<sla::LayerDocument>(document.snapshot(
                [&](size_t, size_t) { job->throw_if_canceled(); return true; }));
        else
            job->m_layer_document = std::make_unique<sla::LayerDocument>(document);
    }
    job->advance_export_generation();
    job->m_encoded_revision = std::numeric_limits<uint64_t>::max();
    job->m_raster_job = true;
    job->m_raster_job_purpose = purpose;
    if (statistics || purpose != RasterJobPurpose::CompleteJob) {
        // Reuse captured values or omit unused temporary-context statistics,
        // while retaining the factory's native-table,
        // secondary-plane, dimensions and owned-parameter validation gates.
        job->throw_if_canceled();
        validate_layer_document(*job, *job->m_layer_document,
            purpose == RasterJobPurpose::PlannedConversionDispatch, [&] { job->throw_if_canceled(); });
        job->throw_if_canceled();
        if (const auto &fresh = job->m_layer_document->fresh_job()) {
            fresh->validate(document.layer_count(), [&](size_t, size_t) { job->throw_if_canceled(); return true; });
            validate_fresh_statistic_inputs(*job, fresh->geometry.width_mm * fresh->geometry.height_mm);
        }
        if (statistics) job->m_print_statistics = *statistics;
        // A dispatch context is private to the already planned conversion;
        // its own statistics are never read. The final context and writer
        // still validate every mask and derive the destination's metrics.
        // Fresh encoding contexts instead consume the format-owned plan's
        // exact metrics. Neither needs neutral totals on this temporary grid.
    } else {
        job->m_print_statistics = raster_job_statistics(*job, *job->m_layer_document, [&] { job->throw_if_canceled(); });
    }
    job->throw_if_canceled();
    return job;
}

// Edited masks cannot use a thumbnail rendered from the unchanged 3D mesh.
// Show the first layer in raster coordinates, preserving aspect ratio. The
// thumbnail buffer uses OpenGL's bottom-up rows, as all archive writers expect.
static ThumbnailsList layer_thumbnails(const sla::LayerDocument &document, const ThumbnailsList &requested)
{
    const auto mask = document.image(0);
    if (mask.cols == 0 || mask.rows == 0) throw ExportError("Cannot preview an empty layer mask");
    std::vector<std::pair<unsigned, unsigned>> sizes;
    for (const auto &thumbnail : requested)
        if (thumbnail.is_valid() && thumbnail.width <= 2048 && thumbnail.height <= 2048 && sizes.size() < 8)
            sizes.emplace_back(thumbnail.width, thumbnail.height);
    if (sizes.empty()) sizes = {{400, 400}, {800, 480}};
    ThumbnailsList result;
    for (const auto &[width, height] : sizes) {
        ThumbnailData thumbnail;
        thumbnail.set(width, height);
        const double scale = std::min(double(width) / mask.cols, double(height) / mask.rows);
        const unsigned columns = std::max(1u, unsigned(std::round(mask.cols * scale)));
        const unsigned rows = std::max(1u, unsigned(std::round(mask.rows * scale)));
        const unsigned left = (width - columns) / 2, top = (height - rows) / 2;
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x) {
                uint8_t gray = 0;
                if (x >= left && x < left + columns && y >= top && y < top + rows) {
                    const size_t sx = std::min(mask.cols - 1, size_t(x - left) * mask.cols / columns);
                    const size_t sy = std::min(mask.rows - 1, size_t(y - top) * mask.rows / rows);
                    gray = mask.buf[sy * mask.cols + sx];
                }
                const size_t offset = (size_t(height - 1 - y) * width + x) * 4;
                thumbnail.pixels[offset] = thumbnail.pixels[offset + 1] = thumbnail.pixels[offset + 2] = gray;
                thumbnail.pixels[offset + 3] = 255;
            }
        result.push_back(std::move(thumbnail));
    }
    return result;
}

void SLAPrint::export_print(const std::string &fname, const ThumbnailsList &thumbnails, const std::string &projectname)
{
    const auto generation = export_generation_guard();
    throw_if_canceled();
    if (!finished()) throw ExportError("Slice the model before exporting");
    const auto suffix = boost::filesystem::path(fname).extension().string();
    const auto *requested = get_archive_entry_by_extension(suffix.c_str());
    // Compound extensions identify independent schemas (for example v1.cxdlp).
    // A filesystem's final suffix alone loses that version information.
    size_t suffix_length=suffix.size();
    for(const auto &entry:registered_sla_archives())
        for(const auto &extension:get_extensions(entry))
            if(extension.size()+1>suffix_length && boost::algorithm::iends_with(fname,"."+extension)) {
                requested=&entry;suffix_length=extension.size()+1;
            }
    // A bare shared suffix (ZIP, CTB, NanoDLP, CWS) keeps the printer's
    // selected schema/version. A more specific registered compound suffix
    // selects its own schema and must not be overridden by a shorter match.
    if (const auto *selected = get_archive_entry(m_printer_config.sla_archive_format.value.c_str()))
        for (const auto &extension : get_extensions(*selected))
            if ((extension.size() + 1 == suffix_length && boost::algorithm::iends_with(fname, "." + extension)) ||
                (suffix_length == suffix.size() && boost::algorithm::iends_with(extension, suffix))) {
                requested = selected;
                break;
            }
    if (!requested) throw ExportError("Unknown SLA output extension: " + suffix);
    const auto reason = archive_export_unavailable_reason(requested->id);
    if (!reason.empty()) throw ExportError(reason);
    if (requested->id != m_printer_config.sla_archive_format.value ||
        (m_layer_document && m_layer_document->fresh_job())) {
        if (!m_layer_document)
            throw ExportError("This vector result cannot be converted to a raster container without slicing again");
        export_layer_document(*m_layer_document, requested->id, fname, thumbnails, projectname);
        return;
    }
    if (m_archiver) {
        if (m_layer_document && m_layer_document->revision() != m_encoded_revision) {
            m_archiver->encode_layers(*m_layer_document, [this, &generation] { throw_if_canceled(); generation.check(); });
            m_encoded_revision = m_layer_document->revision();
        }
        const bool has_preview = std::any_of(thumbnails.begin(), thumbnails.end(), [](const auto &image) { return image.is_valid(); });
        const auto previews = m_layer_document && (m_layer_document->revision() != 0 || (m_raster_job && !has_preview))
            ? layer_thumbnails(*m_layer_document, thumbnails) : thumbnails;
        sla::publish_export(fname, [&](const std::string &temporary) { m_archiver->export_print(temporary, *this, previews, projectname); },
            [this, &generation] { throw_if_canceled(); generation.check(); }, "", &generation);
    }
    else {
        throw ExportError(format(_u8L("Unknown archive format: %s"), m_printer_config.sla_archive_format.value));
    }
}

void SLAPrint::queue_project_layers(std::shared_ptr<const sla::ProjectRasterState> state)
{
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    if (state == m_pending_project_layers) return;
    // Selecting a different saved result is operator input. Its later worker
    // adoption is output-only and must not expire a newly accepted intent.
    advance_export_generation();
    this->call_cancel_callback();
    m_pending_project_layers = std::move(state);
    m_project_layer_restore_failed = false;
}

void SLAPrint::replace_layer_document(const sla::LayerDocument &document)
{
    if (!finished() || document.layer_count() != m_printer_input.size())
        throw RuntimeError("Layer edits do not match the current sliced result");
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    // Invalidation callbacks use this lock to stop/join the background worker.
    // Do not release its borrowed document until that callback has completed.
    this->call_cancel_callback();
    replace_layer_document_locked(document);
}

void SLAPrint::replace_layer_document(const sla::LayerDocument &document,
                                     const sla::ExportGenerationGuard &expected)
{
    const auto check = [&] {
        if (!export_input_guard().same_source(expected) || !expected.current())
            throw RuntimeError("The sliced result changed. Reopen its layers before applying edits.");
        expected.check();
    };
    check();
    if (!finished())
        throw RuntimeError("Finish slicing before applying layer edits");
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    check();
    // Applying edits to the captured source cannot replace its immutable
    // palette provenance. Whole-document adoption and historical cache restore
    // intentionally use their separate, unguarded replacement paths.
    if (m_layer_document &&
        m_layer_document->mask_contract_precision()!=document.mask_contract_precision())
        throw RuntimeError("Layer edits cannot change the source mask contract. Reopen the sliced result.");
    this->call_cancel_callback();
    check();
    replace_layer_document_locked(document);
}

void SLAPrint::replace_layer_document_locked(const sla::LayerDocument &document, bool input_changed)
{
    // finished() locks state_mutex through the milestone checks. Both callers
    // establish completion before taking this lock; do not lock it twice.
    if (document.layer_count() != m_printer_input.size())
        throw RuntimeError("Layer edits do not match the current sliced result");
    // The previous reader is joined but may still carry its cancellation flag.
    // Validate replacement data without adopting that retired worker's state.
    validate_layer_document(*this, document);
    // Compute into a temporary before replacing masks, so failures preserve
    // both the old document and its statistics.
    // Once pixels have been edited, polygon/model statistics no longer
    // describe the job. Derive material use and timing from the actual masks.
    auto statistics = raster_job_statistics(*this, document);
    auto replacement = std::make_unique<sla::LayerDocument>(document);
    advance_export_generation(input_changed);
    m_layer_document = std::move(replacement);
    m_print_statistics = std::move(statistics);
    // A document can come from a different editing history with the same revision.
    m_encoded_revision = std::numeric_limits<uint64_t>::max();
}

// Native float32 representation is an export-plan property. The fresh
// descriptor retains the original doubles for projects and future edits.
static sla::LayerParameters fresh_chitubox_row(const sla::LayerParameters &source,
                                               const std::string &destination,
                                               const std::string &scope)
{
    using F=sla::LayerParameter;
    auto fail=[&](const std::string &field) {
        throw RuntimeError("Destination " + destination + " cannot represent fresh " + scope + " " + field);
    };
    auto represented=source;
    for(auto &[field,value]:represented) {
        const double native=double(float(value));
        if(!std::isfinite(native) || (value>0 && native<=0))
            fail("parameter " + std::to_string(int(field)));
        value=native;
    }
    const double total=double(float(source.at(F::LiftDistance)+source.at(F::LiftDistance2)));
    represented[F::LiftDistance]=total-represented.at(F::LiftDistance2);
    if(!std::isfinite(total) || represented.at(F::LiftDistance)<0 ||
       (source.at(F::LiftDistance)>0 && represented.at(F::LiftDistance)<=0))
        fail("first lift phase");
    const double first_retract=total-represented.at(F::RetractDistance2);
    if(source.at(F::LiftDistance)+source.at(F::LiftDistance2)-source.at(F::RetractDistance2)>0 && first_retract<=0)
        fail("first retract phase");
    sla::validate_chitubox_parameters(represented);
    return represented;
}

sla::LayerConversionPlan sla::plan_layer_conversion(const LayerDocument &document,
                                                    const std::string &destination,
                                                    const LayerProgress &progress)
{
    check_layer_progress(progress, 0, document.layer_count());
    if (const auto &fresh=document.fresh_job()) {
        fresh->validate(document.layer_count(), [&](size_t, size_t) {
            check_layer_progress(progress, 0, document.layer_count()); return true;
        });
        if (fresh->position_policy == ExposurePositionPolicy::RepeatedExposureHeights && destination != "uvj" && destination != "cxdlpv4")
            throw RuntimeError("Destination " + destination + " has no verified preserving exporter for repeated-height exposure sequences");
        if(destination=="uvj") {
            auto typed=std::make_shared<UVJFreshExportPlan>(uvj_fresh_export_plan(document));
            LayerConversionPlan plan;plan.destination=destination;plan.source_geometry=fresh->geometry;
            plan.globals=typed->globals;plan.uvj_fresh=std::move(typed);return plan;
        }
        const auto &anycubic_versions=anycubic_available_versions(destination);
        if(std::any_of(anycubic_versions.begin(),anycubic_versions.end(),
                       [](uint16_t version){return version>=516 && version<=518;})) {
            auto typed=std::make_shared<AnycubicFreshExportPlan>(anycubic_fresh_export_plan(document));
            LayerConversionPlan plan;plan.destination=destination;plan.source_geometry=fresh->geometry;
            plan.globals=typed->globals;plan.anycubic_fresh=std::move(typed);return plan;
        }
        if(destination=="cbddlp" || destination=="photon") {
            auto typed=std::make_shared<CBDDLPFreshExportPlan>(cbddlp_fresh_export_plan(document));
            LayerConversionPlan plan;plan.destination=destination;plan.source_geometry=fresh->geometry;
            plan.globals=fresh->globals;plan.cbddlp_fresh=std::move(typed);return plan;
        }
        if(destination=="cws") {
            auto typed=std::make_shared<CWSFreshExportPlan>(cws_fresh_export_plan(document));
            LayerConversionPlan plan;plan.destination=destination;plan.source_geometry=fresh->geometry;
            plan.globals=typed->represented.globals;plan.cws_fresh=std::move(typed);return plan;
        }
        if (destination == "SL1" || destination == "SL1S") {
            auto typed = std::make_shared<SL1FreshExportPlan>(sl1_fresh_export_plan(document, progress));
            LayerConversionPlan plan;
            plan.destination = destination; plan.source_geometry = fresh->geometry;
            plan.globals = typed->globals; plan.sl1_fresh = std::move(typed); return plan;
        }
        if (destination == "cxdlpv4") {
            auto typed = std::make_shared<CXDLPv4FreshExportPlan>(cxdlpv4_fresh_export_plan(document, progress));
            LayerConversionPlan plan;
            plan.destination = destination;
            plan.source_geometry = fresh->geometry;
            plan.globals = typed->globals;
            plan.cxdlpv4_fresh = std::move(typed);
            return plan;
        }
        // A matching row schema alone does not prove global/header support.
        if(destination!="ctb_v3" && destination!="ctb_v4" && destination!="ctb_v5" &&
           destination!="ctb_encrypted_v4" && destination!="ctb_encrypted")
            throw RuntimeError("Destination " + destination + " has no parameter-preserving fresh-job exporter yet");
        const auto validate_globals = destination == "ctb_v3" ? validate_chitubox_v3_job : validate_chitubox_job;
        validate_globals(fresh->globals,document.layer_count());
        auto represented=*fresh;
        represented.globals.normal=fresh_chitubox_row(fresh->globals.normal,destination,"normal defaults");
        represented.globals.bottom=fresh_chitubox_row(fresh->globals.bottom,destination,"bottom defaults");
        for(size_t i=0;i<represented.rows.size();++i)
            represented.rows[i]=fresh_chitubox_row(fresh->rows[i],destination,"layer " + std::to_string(i+1));
        // Native rounding may collapse Z or a moving phase. Reject it before
        // copying/decoding masks; never remove such rows or phases implicitly.
        represented.validate(document.layer_count());
        validate_globals(represented.globals,document.layer_count());
        LayerConversionPlan plan;
        plan.destination=destination;
        plan.schema=parameter_conversion_schema(destination);
        plan.source_geometry=fresh->geometry;
        // Printer motion gates and native overlays consume the same
        // representation as rows, including named .05f protocol values.
        plan.globals=std::move(represented.globals);
        plan.rows=std::move(represented.rows);
        if (destination == "ctb_v3") {
            // V3 has no second motion phase or explicit waits. A fresh job
            // carries every neutral field; omit only representable zeroes,
            // never silently discard nonzero printing instructions.
            for (auto &row : plan.rows) {
                for (auto field = row.begin(); field != row.end();) {
                    if (std::none_of(plan.schema.begin(), plan.schema.end(),
                            [&](const auto &capability) { return capability.field == field->first; })) {
                        if (field->second != 0)
                            throw RuntimeError("CTB v3 cannot preserve second-phase motion or explicit layer waits");
                        field = row.erase(field);
                    } else ++field;
                }
            }
        }
        return plan;
    }
    const auto capabilities = document.parameter_capabilities();
    if (capabilities.empty())
        throw RuntimeError("This imported format does not expose a complete parameter table for conversion yet");
    if (document.mask_plane_count() > 1)
        throw RuntimeError("Conversion cannot preserve secondary masks yet; save the native archive");
    LayerConversionPlan plan;
    plan.destination = destination;
    plan.schema = parameter_conversion_schema(destination);
    if (plan.schema.empty())
        throw RuntimeError("This destination has no implemented parameter-preserving conversion yet");
    for (const auto &source : capabilities)
        if (std::none_of(plan.schema.begin(), plan.schema.end(),
                        [&](const auto &target) { return target.field == source.field; }))
            throw RuntimeError("Destination " + destination + " cannot preserve source field: " +
                               source.label + " (" + source.unit + ")");
    plan.source_geometry = document.raster_geometry();
    plan.globals = document.job_parameters();
    // These currently implemented global-transfer destinations are CTB/AES.
    // A null descriptor does not prove complete global-default preservation.
    if (plan.globals) validate_chitubox_job(*plan.globals, document.layer_count());
    const bool anycubic = !anycubic_available_versions(document.archive_format()).empty();
    plan.rows.reserve(document.layer_count());
    double previous_z = 0;
    for (size_t i = 0; i < document.layer_count(); ++i) {
        check_layer_progress(progress, i, document.layer_count());
        auto row = document.layer_parameters(i);
        if (anycubic) row = anycubic_parameters_for_ctb(row, plan.schema);
        if (document.archive_format() == "ctb_v3") {
            // These fields are explicitly absent in the v3 protocol, rather
            // than unknown. Preserve their neutral behavior in newer codecs.
            using F = LayerParameter;
            for (const auto field : {F::LiftDistance2, F::LiftSpeed2, F::RetractDistance2,
                    F::RetractSpeed2, F::WaitBefore, F::WaitAfter, F::WaitAfterLift})
                if (std::any_of(plan.schema.begin(), plan.schema.end(),
                        [field](const auto &capability) { return capability.field == field; }))
                    row.emplace(field, 0.);
        }
        const double z = row.at(LayerParameter::PositionZ);
        if (z <= previous_z)
            throw RuntimeError("Destination float precision cannot preserve strictly increasing layer Z");
        previous_z = z;
        plan.rows.push_back(std::move(row));
    }
    return plan;
}

void sla::require_layer_conversion_geometry(const LayerConversionPlan &plan,
                                             const DynamicPrintConfig &printer)
{
    const auto error = printer_geometry_error(printer);
    if (!error.empty()) throw RuntimeError(error);
    LayerRasterGeometry target{size_t(printer.opt_int("display_pixels_x")),
        size_t(printer.opt_int("display_pixels_y")), printer.opt_float("display_width"),
        printer.opt_float("display_height")};
    if (printer.option("display_orientation")->getInt() == sladoPortrait) {
        std::swap(target.columns, target.rows);
        std::swap(target.width_mm, target.height_mm);
    }
    if (plan.source_geometry) plan.source_geometry->require_same_scale(target);
    const double limit = printer.opt_float("max_print_height");
    const auto &rows = plan.cxdlpv4_fresh ? plan.cxdlpv4_fresh->rows : plan.sl1_fresh ? plan.sl1_fresh->rows :
        plan.cws_fresh ? plan.cws_fresh->source.rows : plan.cbddlp_fresh ? plan.cbddlp_fresh->source.rows :
        plan.anycubic_fresh ? plan.anycubic_fresh->source.rows : plan.uvj_fresh ? plan.uvj_fresh->source.rows : plan.rows;
    for (const auto &row : rows) {
        const double z = row.at(LayerParameter::PositionZ);
        if (z <= 0 || !layer_z_within_native_limit(z, limit))
            throw RuntimeError("Source layer Z exceeds the selected printer height");
    }
}

void sla::require_layer_conversion_motion(const LayerConversionPlan &plan,
                                           const DynamicPrintConfig &printer)
{
    if (plan.destination != "ctb_encrypted" && plan.destination != "ctb_encrypted_v4") return;
    // The named destination owns fixed protocol motion independently of the
    // source nominal height. Anonymous numeric identity remains a separate
    // contract: temporary rounded defaults must not decide printer semantics.
    if (!ctb_encrypted_named_tilting_vat(printer.opt_string("printer_model"))) return;
    auto require_motion=[&](const LayerParameters &row, const std::string &scope) {
        for (const auto &capability:plan.schema) {
            const auto fixed=ctb_encrypted_fixed_motion().find(capability.field);
            if (fixed == ctb_encrypted_fixed_motion().end()) continue;
            const auto value=row.find(capability.field);
            if (value == row.end() || value->second != fixed->second)
                throw RuntimeError("Destination " + plan.destination + " has fixed motion: " +
                    scope + " " + capability.label + " (" + capability.unit + ") differs from the printer protocol");
        }
    };
    if (plan.globals) {
        require_motion(plan.globals->normal,"normal defaults");
        require_motion(plan.globals->bottom,"bottom defaults");
    }
    for (size_t i=0;i<plan.rows.size();++i) require_motion(plan.rows[i],"layer " + std::to_string(i+1));
}

void SLAPrint::convert_layer_archive(const DynamicPrintConfig &config, const sla::LayerDocument &document,
                                    const std::string &format, const std::string &path)
{
    sla::NativeArchiveConversionRequest(document, config, format, path).write();
}

void SLAPrint::convert_layer_archive_owned(const DynamicPrintConfig &config,
    const sla::LayerDocument &document, const std::string &format, const std::string &path,
    const sla::ExportGenerationGuard &generation, const sla::LayerProgress &progress)
{
    const auto checkpoint = [&] {
        generation.check();
        sla::check_layer_progress(progress);
        generation.check();
    };
    checkpoint();
    if (format=="cxdlpv4")
        sla::require_cxdlpv4_printer_identifier(config.opt_string("printer_model"),bool(document.fresh_job()));
    const auto plan = sla::plan_layer_conversion(document, format, progress);
    checkpoint();
    sla::require_layer_conversion_geometry(plan, config);
    sla::require_layer_conversion_motion(plan, config);
    // Keep the owned native document: the final writer validates its masks and
    // derives statistics, so dispatch needs neither an image-only copy nor a
    // second statistics pass. An explicit flag retains the reference path for
    // equivalence and performance regression checks.
    const char *dispatch_flag = std::getenv("SLA_EXPERIMENT_CONVERSION_DISPATCH");
    const bool planned_dispatch = !dispatch_flag || std::strcmp(dispatch_flag, "1") == 0;
    std::vector<sla::EncodedRaster> rasters;std::vector<double> levels;
    if (!planned_dispatch) rasters.reserve(document.layer_count());
    levels.reserve(document.layer_count());
    for(size_t i=0;i<document.layer_count();++i) {
        generation.check();sla::check_layer_progress(progress, i, document.layer_count());generation.check();
        if (!planned_dispatch) {
            const auto bytes=document.encoded_layer(i);const auto *begin=static_cast<const uint8_t *>(bytes.buf);
            rasters.emplace_back(std::vector<uint8_t>(begin,begin+bytes.sz),"png");
        }
        levels.push_back(double(i+1)/1000.);
    }
    auto configuration=config;configuration.set("sla_archive_format",format);
    if (plan.globals) {
        if (plan.cxdlpv4_fresh || plan.sl1_fresh || plan.cws_fresh || plan.cbddlp_fresh || plan.anycubic_fresh || plan.uvj_fresh) plan.globals->apply_to(configuration);
        else sla::apply_chitubox_container_baseline(*plan.globals, document.layer_count(), configuration);
    }
    std::optional<sla::LayerDocument> masks;
    if (!planned_dispatch) masks=sla::LayerDocument::from_rasters(std::move(rasters), progress);
    auto context=create_raster_job_impl(configuration,masks ? *masks : document,levels,std::nullopt,
        sla::ExposurePositionPolicy::IncreasingLayers,nullptr,&generation,checkpoint,
        planned_dispatch ? RasterJobPurpose::PlannedConversionDispatch : RasterJobPurpose::CompleteJob);
    context->export_layer_document_planned(document,format,path,{},"converted", &plan);
}
void SLAPrint::export_layer_document(const sla::LayerDocument &document, const std::string &format,
                                   const std::string &path, const ThumbnailsList &thumbnails,
                                   const std::string &projectname,
                                   const std::function<void()> &checkpoint,
                                   const sla::ExportGenerationGuard *request_generation) const
{
    export_layer_document_impl(document, format, path, thumbnails, projectname,
        checkpoint, request_generation, LayerExportInput::CallerOwned);
}

void SLAPrint::export_layer_document_snapshot(const sla::LayerDocument &document, std::string format,
    std::string path, ThumbnailsList thumbnails, std::string projectname,
    const std::function<void()> &checkpoint, const sla::ExportGenerationGuard *request_generation) const
{
    throw_if_canceled();
    if (request_generation && !export_input_guard().same_source(*request_generation))
        throw RuntimeError("Layer export request belongs to a different context");
    const auto generation = request_generation ? *request_generation : export_generation_guard();
    const auto captured = document.snapshot([&](size_t, size_t) {
        throw_if_canceled(); generation.check();
        if (checkpoint) checkpoint();
        generation.check(); return true;
    });
    // Copy the supplied editor masks, never the context's earlier sliced result.
    // No callback receives a reference to this local immutable document.
    export_layer_document_impl(captured, format, path, thumbnails, projectname,
        checkpoint, &generation, LayerExportInput::PrivateSnapshot);
}

void SLAPrint::export_layer_document_impl(const sla::LayerDocument &document, const std::string &format,
    const std::string &path, const ThumbnailsList &thumbnails, const std::string &projectname,
    const std::function<void()> &checkpoint, const sla::ExportGenerationGuard *request_generation,
    LayerExportInput input) const
{
    const auto started=std::chrono::steady_clock::now();
    throw_if_canceled();
    if (format=="cxdlpv4")
        sla::require_cxdlpv4_printer_identifier(m_printer_config.printer_model.value,bool(document.fresh_job()));
    if (request_generation && !export_input_guard().same_source(*request_generation))
        throw RuntimeError("Layer export request belongs to a different context");
    const auto generation = request_generation ? *request_generation : export_generation_guard();
    const auto check = [&] {
        throw_if_canceled(); generation.check();
        if (checkpoint) checkpoint();
        generation.check();
    };
    check();
    const auto plan = !document.fresh_job() && document.parameter_capabilities().empty() ? std::nullopt :
        std::optional<sla::LayerConversionPlan>(sla::plan_layer_conversion(document, format,
            [&](size_t, size_t) { check(); return true; }));
    check();
    if(std::getenv("SLA_TRACE_EXPORT_STAGES")) BOOST_LOG_TRIVIAL(info) << "Layer export plan: " <<
        std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count() << " s";
    export_layer_document_planned(document, format, path, thumbnails, projectname,
        plan ? &*plan : nullptr, &generation, checkpoint, input);
}

void SLAPrint::export_layer_document_planned(const sla::LayerDocument &document, const std::string &format,
    const std::string &path, const ThumbnailsList &thumbnails, const std::string &projectname,
    const sla::LayerConversionPlan *plan, const sla::ExportGenerationGuard *request_generation,
    const std::function<void()> &checkpoint, LayerExportInput input) const
{
    const auto started=std::chrono::steady_clock::now();
    const auto trace=[&](const char *phase) {
        if(std::getenv("SLA_TRACE_EXPORT_STAGES")) BOOST_LOG_TRIVIAL(info) << "Layer export " << phase << ": " <<
            std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count() << " s";
    };
    const auto generation = request_generation ? *request_generation : export_generation_guard();
    const auto check = [&] {
        throw_if_canceled(); generation.check();
        if (checkpoint) checkpoint();
        generation.check();
    };
    check();
    throw_if_canceled();
    if (!finished()) throw ExportError("Slice the model before exporting edited layers");
    const auto unavailable = archive_export_unavailable_reason(format.c_str());
    if (!unavailable.empty()) throw ExportError(unavailable);
    if (document.layer_count() != m_printer_input.size())
        throw RuntimeError("Layer document does not match the sliced result");
    const bool transferring_parameters = plan != nullptr;
    if (plan) {
        sla::require_layer_conversion_geometry(*plan, m_full_print_config);
        sla::require_layer_conversion_motion(*plan, m_full_print_config);
    }
    if (plan && plan->cxdlpv4_fresh) {
        const auto &material = material_config();
        sla::cxdlpv4_fresh_material_metrics(plan->cxdlpv4_fresh->material_ml,
            material.material_density.value, material.bottle_cost.value, material.bottle_volume.value);
    }
    if (plan && plan->sl1_fresh) sla::sl1_fresh_material_profile(*plan->sl1_fresh,material_config());
    if(plan && (plan->cbddlp_fresh || plan->anycubic_fresh)) {
        const auto &m=material_config();
        sla::cbddlp_material_totals(0,m.material_density.value,m.bottle_cost.value,m.bottle_volume.value);
    }
    std::optional<sla::FreshLayerMaterialPricing> fresh_pricing;
    if (document.fresh_job() && (format=="ctb_v3" || format=="ctb_v4" || format=="ctb_v5" ||
            format=="ctb_encrypted" || format=="ctb_encrypted_v4")) {
        const auto &material=material_config();
        fresh_pricing=sla::FreshLayerMaterialPricing{material.material_density.value,
            material.bottle_cost.value, material.bottle_volume.value};
        fresh_pricing->validate(); // Before mask copying/encoding or any output.
    }
    validate_layer_document(*this, document, transferring_parameters, check);
    auto writer = SLAArchiveWriter::create(format, m_printer_config);
    if (!writer) throw RuntimeError("Unknown SLA archive format: " + format);
    // Export without Apply must use statistics for this exact document for
    // mesh jobs as well as generated jobs. It must not mutate the source.
    std::vector<double> levels;
    for (size_t i=0;i<m_printer_input.size();++i)
        // Temporary grid levels construct the container only. The final native
        // table restores every Z together, including sub-grid layer spacing.
        levels.push_back(transferring_parameters ? double(i+1)/1000. :
            double(m_printer_input[i].level()) * SCALING_FACTOR);
    // The explicitly chosen destination owns the export context. A source
    // preset may use another container, an unfinished codec, or a label saved
    // by an older GUI; none of those should override this valid destination.
    auto export_config = m_full_print_config;
    export_config.set("sla_archive_format", format);
    // Preserve source job defaults separately from its per-layer overrides.
    // Printer geometry and destination identity remain in the chosen context.
    const auto source_globals = plan ? plan->globals : std::nullopt;
    if (source_globals) {
        if (plan->cxdlpv4_fresh || plan->sl1_fresh || plan->cws_fresh || plan->cbddlp_fresh || plan->anycubic_fresh || plan->uvj_fresh) source_globals->apply_to(export_config);
        else sla::apply_chitubox_container_baseline(*source_globals, document.layer_count(), export_config);
    }
    if(plan && plan->cbddlp_fresh) {
        export_config.set("sla_retract_speed",double(plan->cbddlp_fresh->retract_speed));
        export_config.set("sla_bottom_retract_speed",double(plan->cbddlp_fresh->retract_speed));
    }
    // Preset identifiers live in the placeholder context, outside the typed
    // slicing configuration. Carry them into the new destination's context.
    for (const auto &keys : {std::make_pair("sla_material_settings_id", "material_preset"),
                             std::make_pair("sla_print_settings_id", "print_preset"),
                             std::make_pair("printer_settings_id", "printer_preset")})
        if (const auto *value = m_placeholder_parser.config().option(keys.second))
            export_config.set_key_value(keys.first, value->clone());
    // The writer constructs the destination container from canonical masks.
    // Native tables are transferred below through the destination's own
    // capability/validation contract before atomic publication. They must
    // never masquerade as a uniform job using the target preset's defaults.
    std::optional<sla::LayerDocument> masks;
    if(transferring_parameters) {
        std::vector<sla::EncodedRaster> rasters;rasters.reserve(document.layer_count());
        for(size_t i=0;i<document.layer_count();++i) {
            check();
            const auto encoded=document.encoded_layer(i);
            const auto *begin=static_cast<const uint8_t *>(encoded.buf);
            rasters.emplace_back(std::vector<uint8_t>(begin,begin+encoded.sz),"png");
        }
        masks=sla::LayerDocument::from_rasters(std::move(rasters), [&](size_t, size_t) {
            check();return true;
        });
    }
    const auto context_purpose = plan && (plan->cxdlpv4_fresh || plan->sl1_fresh) ?
        RasterJobPurpose::ValidatedFreshEncoding : RasterJobPurpose::CompleteJob;
    auto raster_context = create_raster_job_impl(export_config, masks ? *masks : document, levels,
        std::nullopt, sla::ExposurePositionPolicy::IncreasingLayers, nullptr, &generation,
        check, context_purpose);
    trace("context ready");
    if(transferring_parameters) {
        // Avoid floating-point truncation when constructing the exact temporary
        // grid required by the writer. This is not the exported native Z table.
        raster_context->m_printer_input.clear();
        const coord_t step=scaled<coord_t>(.001);
        for(size_t i=0;i<document.layer_count();++i)
            raster_context->m_printer_input.emplace_back(step*coord_t(i+1));
    }
    // Full planning, geometry/plane/metadata validation and output readback
    // remain mandatory. Borrow either the snapshot API's local const copy or
    // a frozen job's privately marked owned document. A ready editor context
    // with an arbitrary public argument still cannot grant this capability.
    const char *borrow_png_flag = std::getenv("SLA_EXPERIMENT_SL1_BORROW_PNG");
    const bool borrow_sl1_png = (!borrow_png_flag || std::strcmp(borrow_png_flag, "1") == 0) &&
        plan && plan->sl1_fresh && (format == "SL1" || format == "SL1S") &&
        typeid(*writer) == typeid(SL1Archive) &&
        (input == LayerExportInput::PrivateSnapshot ||
            (m_frozen_sl1_document == &document && m_layer_document.get() == &document));
    // Native v4 reuse retains its separately owned immutable encoded batch.
    std::shared_ptr<const std::vector<sla::EncodedRaster>> reused;
    if (sla::encoded_mask_reuse_detail::experiment_enabled() && plan && plan->cxdlpv4_fresh &&
        format == "cxdlpv4" && typeid(*writer) == typeid(CrealityCXDLPv4Archive) &&
        m_frozen_cxdlpv4_document == &document && m_frozen_cxdlpv4_encoding &&
        m_frozen_cxdlpv4_configuration && m_full_print_config == *m_frozen_cxdlpv4_configuration &&
        sla::encoded_mask_reuse_detail::cxdlpv4_batch_payload_size(
            *m_frozen_cxdlpv4_encoding, document.layer_count(), check))
        reused = m_frozen_cxdlpv4_encoding;
    if (borrow_sl1_png) {
        check();
        trace("PNG encoding bypass selected");
    } else if (reused) {
        check();
        trace("masks reused");
    } else {
        writer->encode_layers(document, check);
        trace("masks encoded");
    }
    // SL1 records whether any source object was hollowed. Retain that object
    // configuration without duplicating large meshes or slicing them again.
    // These lightweight descriptors borrow source model pointers only during
    // this synchronous export; their destructor does not own model objects.
    raster_context->m_objects.reserve(m_objects.size());
    for (auto *object : m_objects) {
        auto *descriptor = new SLAPrintObject(raster_context.get(), object->model_object());
        raster_context->m_objects.push_back(descriptor);
        descriptor->config_apply(object->config());
    }
    const bool has_preview = std::any_of(thumbnails.begin(), thumbnails.end(), [](const auto &image) { return image.is_valid(); });
    const auto previews = document.revision() != 0 || !has_preview ? layer_thumbnails(document, thumbnails) : thumbnails;
    trace("previews ready");
    sla::publish_export(path, [&](const std::string &temporary) {
        check();
        if(plan && plan->uvj_fresh) {
            const auto &fresh=*plan->uvj_fresh;
            auto *uvj=dynamic_cast<UVJArchive *>(writer.get());
            if(!uvj || format!="uvj")throw RuntimeError("UVJ plan does not match writer");
            uvj->export_fresh_print(temporary,*raster_context,previews,fresh);
            const auto archive=read_zipper_archive(temporary,{"json"},{});
            if(archive.entries.size()!=1)throw RuntimeError("UVJ requires one metadata record");
            const auto metadata=nlohmann::json::parse(archive.entries[0].buf);
            using F=sla::LayerParameter;
            const std::map<F,const char *> names{{F::Exposure,"LightOnTime"},{F::LightOff,"LightOffTime"},
                {F::WaitBefore,"WaitTimeBeforeCure"},{F::WaitAfter,"WaitTimeAfterCure"},{F::WaitAfterLift,"WaitTimeAfterLift"},
                {F::LiftDistance,"LiftHeight"},{F::LiftDistance2,"LiftHeight2"},{F::LiftSpeed,"LiftSpeed"},
                {F::LiftSpeed2,"LiftSpeed2"},{F::RetractDistance2,"RetractHeight2"},
                {F::RetractSpeed,"RetractSpeed"},{F::RetractSpeed2,"RetractSpeed2"},{F::LightPWM,"LightPWM"}};
            auto check_row=[&](const auto &json,const sla::LayerParameters &row) {
                for(auto [field,name]:names)if(json.at(name).template get<double>()!=row.at(field))
                    throw RuntimeError(std::string("UVJ projected parameter differs: ")+name);
                const double primary=double(float(row.at(F::LiftDistance)+row.at(F::LiftDistance2)-row.at(F::RetractDistance2)));
                if(json.at("RetractHeight").template get<double>()!=primary)throw RuntimeError("UVJ primary retract differs");
            };
            const auto &properties=metadata.at("Properties");const auto &size=properties.at("Size");
            if(size.at("X").get<size_t>()!=fresh.geometry.columns || size.at("Y").get<size_t>()!=fresh.geometry.rows ||
                size.at("Layers").get<size_t>()!=fresh.rows.size() || size.at("LayerHeight").get<double>()!=fresh.globals.layer_height ||
                size.at("Millimeter").at("X").get<double>()!=fresh.geometry.width_mm ||
                size.at("Millimeter").at("Y").get<double>()!=fresh.geometry.height_mm ||
                properties.at("Bottom").at("Count").get<uint32_t>()!=fresh.globals.bottom_layers ||
                metadata.at("Layers").size()!=fresh.rows.size())throw RuntimeError("UVJ metadata differs from owned geometry/counts");
            check_row(properties.at("Exposure"),fresh.globals.normal);check_row(properties.at("Bottom"),fresh.globals.bottom);
            const auto verified=sla::LayerDocument::load_archive(temporary, [&](size_t,size_t) {check();return true;});
            for(size_t i=0;i<fresh.rows.size();++i) {
                check();
                const auto &json=metadata.at("Layers").at(i);
                if(json.at("Z").get<double>()!=fresh.rows[i].at(F::PositionZ))throw RuntimeError("UVJ projected Z differs");
                check_row(json.at("Exposure"),fresh.rows[i]);
                if(verified.image(i).buf!=document.image(i).buf)throw RuntimeError("UVJ mask differs from source");
            }
            return;
        }
        if(plan && plan->anycubic_fresh) {
            const auto &fresh=*plan->anycubic_fresh;
            auto *any_writer=dynamic_cast<AnycubicSLAArchive *>(writer.get());
            if(!any_writer)throw RuntimeError("Anycubic plan does not match destination writer");
            any_writer->export_fresh_print(temporary,*raster_context,previews,fresh);
            std::ifstream input(temporary,std::ios::binary);const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
            const AnycubicLayerLayout layout(bytes,format);
            const auto h=layout.header;
            using F=sla::LayerParameter;
            const auto &normal=fresh.globals.normal;const auto &bottom=fresh.globals.bottom;
            const auto requested=anycubic_resolve_version(format,export_config.opt_string("sla_anycubic_version"),any_writer->default_version());
            if(layout.version<516 || layout.version>518 || layout.count!=document.layer_count() ||
                layout.version!=requested || layout.integer(h+84)!=fresh.estimated_print_time ||
                layout.number(h+24)!=float(normal.at(F::Exposure)) || layout.number(h+32)!=float(bottom.at(F::Exposure)) ||
                layout.number(h+40)!=float(normal.at(F::LiftDistance)+normal.at(F::LiftDistance2)) ||
                layout.number(h+44)!=float(normal.at(F::LiftSpeed)/60.) ||
                layout.number(h+48)!=float(normal.at(F::RetractSpeed)/60.) ||
                layout.columns!=fresh.geometry.columns || layout.rows!=fresh.geometry.rows ||
                layout.number(h+20)!=fresh.nominal_height || layout.number(h+28)!=fresh.wait_before ||
                layout.number(h+36)!=float(fresh.bottom_layers) || layout.integer(h+88)!=fresh.transition_layers)
                throw RuntimeError("Modern Anycubic header differs from owned projection");
            const auto extra=layout.integer(40);
            for(bool is_bottom:{true,false}) {
                const auto &group=is_bottom?bottom:normal;
                const size_t at=extra+(is_bottom?20:48);
                if(layout.number(at)!=float(group.at(F::LiftDistance)) ||
                    layout.number(at+4)!=float(group.at(F::LiftSpeed)/60.) ||
                    layout.number(at+8)!=float(group.at(F::RetractSpeed2)/60.) ||
                    layout.number(at+12)!=float(group.at(F::LiftDistance2)) ||
                    layout.number(at+16)!=float(group.at(F::LiftSpeed2)/60.) ||
                    layout.number(at+20)!=float(group.at(F::RetractSpeed)/60.))
                    throw RuntimeError("Modern Anycubic global motion differs from owned projection");
            }
            const auto machine=layout.integer(44);
            if(layout.number(machine+136)!=float(fresh.geometry.width_mm) || layout.number(machine+140)!=float(fresh.geometry.height_mm))
                throw RuntimeError("Modern Anycubic machine geometry differs from source");
            for(size_t i=0;i<fresh.rows.size();++i) {
                check();
                const auto at=layout.record(i);const auto &row=fresh.rows[i];const auto source=document.image(i);
                if(layout.number(at+8)!=row.lift || layout.number(at+12)!=row.speed_mms ||
                    layout.number(at+16)!=row.exposure || layout.number(at+20)!=row.height)
                    throw RuntimeError("Modern Anycubic native row differs from owned projection");
                if(layout.decode(i)!=source.buf || (layout.subtable && layout.decode_subimage(i)!=source.buf))
                    throw RuntimeError("Modern Anycubic grayscale cannot preserve source pixels");
            }
            return;
        }
        if(plan && plan->cbddlp_fresh) {
            const auto &fresh=*plan->cbddlp_fresh;
            auto *cb_writer=dynamic_cast<UVToolsChituboxArchive *>(writer.get());
            if(!cb_writer || (format!="cbddlp" && format!="photon"))throw RuntimeError("CBDDLP plan does not match destination writer");
            cb_writer->export_fresh_print(temporary,*raster_context,previews,fresh);
            std::ifstream input(temporary,std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
            const sla::ChituboxLayerLayout layout(bytes);
            if(!layout.binary || layout.version!=2 || layout.count!=document.layer_count() ||
                layout.columns!=fresh.geometry.columns || layout.rows!=fresh.geometry.rows ||
                layout.number(8)!=float(fresh.geometry.width_mm) || layout.number(12)!=float(fresh.geometry.height_mm) ||
                layout.number(28)!=fresh.rows.back().z || layout.number(32)!=fresh.nominal_height ||
                layout.number(36)!=fresh.default_exposure[0] || layout.number(40)!=fresh.default_exposure[1] ||
                layout.integer(48)!=fresh.bottom_layers || layout.integer(76)!=fresh.estimated_print_time ||
                (uint16_t(bytes[96])|(uint16_t(bytes[97])<<8))!=fresh.pwm[0] ||
                (uint16_t(bytes[98])|(uint16_t(bytes[99])<<8))!=fresh.pwm[1])
                throw RuntimeError("CBDDLP native header differs from owned projection");
            const auto p=layout.parameters;
            if(!p || layout.number(p)!=fresh.lift_height[1] || layout.number(p+4)!=fresh.lift_speed[1] ||
                layout.number(p+8)!=fresh.lift_height[0] || layout.number(p+12)!=fresh.lift_speed[0] ||
                layout.number(p+16)!=fresh.retract_speed || layout.integer(p+40)!=fresh.bottom_layers ||
                layout.number(p+32)!=fresh.default_light_off[1] || layout.number(p+36)!=fresh.default_light_off[0])
                throw RuntimeError("CBDDLP native motion/defaults differ from owned projection");
            double ml=0,previous=0;
            const double area=fresh.geometry.width_mm/fresh.geometry.columns*fresh.geometry.height_mm/fresh.geometry.rows;
            for(size_t i=0;i<fresh.rows.size();++i) {
                check();
                const auto source=document.image(i);const auto decoded=layout.decode(i);
                if(source.cols!=layout.columns || source.rows!=layout.rows || source.buf!=decoded)
                    throw RuntimeError("CBDDLP target AA cannot preserve source pixels; convert the source palette explicitly");
                for(unsigned plane=0;plane<layout.planes;++plane) {
                    const auto at=layout.record(i,plane);
                    if(layout.number(at)!=fresh.rows[i].z || layout.number(at+4)!=fresh.rows[i].exposure ||
                        layout.number(at+8)!=fresh.rows[i].light_off)
                        throw RuntimeError("CBDDLP native AA row differs from owned projection");
                }
                ml+=std::count_if(decoded.begin(),decoded.end(),[](uint8_t value){return value!=0;})*
                    area*(double(fresh.rows[i].z)-previous);previous=fresh.rows[i].z;
            }
            const auto &m=material_config();const auto totals=sla::cbddlp_material_totals(ml/1000.,m.material_density.value,m.bottle_cost.value,m.bottle_volume.value);
            for(size_t i=0;i<3;++i)if(layout.number(p+20+i*4)!=totals[i])
                throw RuntimeError("CBDDLP native material total differs from current pixels and source profile");
            return;
        }
        if(plan && plan->cws_fresh) {
            const auto &fresh=*plan->cws_fresh;
            auto *cws_writer=dynamic_cast<UVToolsCWSArchive *>(writer.get());
            if(!cws_writer || format!="cws")throw RuntimeError("CWS fresh plan does not match destination writer");
            cws_writer->export_fresh_print(temporary,*raster_context,fresh);
            const auto command_archive=read_zipper_archive(temporary,{".gcode"},{});
            if(command_archive.entries.size()!=1 || command_archive.entries[0].fname!="sla_job.gcode")
                throw RuntimeError("CWS requires one authoritative exposure command entry");
            const auto &bytes=command_archive.entries[0].buf;
            sla::cws_require_command_intent(fresh.represented,sla::cws_decode_command_rows(std::string(bytes.begin(),bytes.end())));
            const auto verified=sla::LayerDocument::load_archive(temporary, [&](size_t,size_t) {check();return true;});
            if(verified.archive_format()!="cws" || verified.layer_count()!=document.layer_count())
                throw RuntimeError("CWS native mask count or dialect differs from source");
            for(size_t i=0;i<document.layer_count();++i) {
                check();
                const auto before=document.image(i),after=verified.image(i);
                if(before.cols!=after.cols || before.rows!=after.rows || before.buf!=after.buf)
                    throw RuntimeError("CWS exported mask pixels differ from source");
            }
            return;
        }
        if (plan && plan->sl1_fresh) {
            auto *sl1_writer = dynamic_cast<SL1Archive *>(writer.get());
            if (!sl1_writer || (format != "SL1" && format != "SL1S"))
                throw RuntimeError("SL1 fresh plan does not match the destination writer");
            const auto job_name = projectname.empty() ? boost::filesystem::path(path).stem().string() : projectname;
            if (borrow_sl1_png) {
                sl1_writer->export_fresh_document(temporary, *raster_context, previews, job_name,
                    *plan->sl1_fresh, document);
                trace("PNG document consumed");
            } else
                sl1_writer->export_fresh_print(temporary, *raster_context, previews, job_name, *plan->sl1_fresh);
            const char *compare_flag = std::getenv("SLA_EXPERIMENT_SL1_COMPARE_ENCODED");
            // A complete plan alone does not protect a callback-mutable public
            // document. Reuse only the writer's exact private ownership proof.
            const bool compare_encoded = borrow_sl1_png && (!compare_flag || std::strcmp(compare_flag, "1") == 0);
            sla::SL1FreshArchiveVerifier::verify(temporary, *plan->sl1_fresh, document,
                raster_context->material_config(), check, compare_encoded);
            return;
        }
        if (plan && plan->cxdlpv4_fresh) {
            const auto &fresh = *plan->cxdlpv4_fresh;
            auto *cx_writer = dynamic_cast<CrealityCXDLPv4Archive *>(writer.get());
            if (!cx_writer || format != "cxdlpv4")
                throw RuntimeError("CXDLPv4 fresh plan does not match the destination writer");
            if (reused)
                cx_writer->export_fresh_print_with_encoding(temporary, *raster_context, previews, fresh, *reused);
            else
                cx_writer->export_fresh_print(temporary, *raster_context, previews, fresh);
            trace("CXDLP written");
            // Verify native payloads directly. Importing an editable document
            // would decode and re-encode every layer to PNG, then decode it
            // again below solely to compare the same pixels.
            check();
            std::ifstream input(temporary, std::ios::binary);
            input.seekg(0,std::ios::end);const auto length=input.tellg();
            if(length<19 || length>std::streamoff(uint64_t(1)<<30))
                throw RuntimeError("CXDLPv4 readback exceeds the memory budget or is truncated");
            std::vector<uint8_t> bytes(static_cast<size_t>(length));input.seekg(0);
            for(size_t offset=0;offset<bytes.size();) {
                check();const auto count=std::min(size_t(65536),bytes.size()-offset);
                if(!input.read(reinterpret_cast<char *>(bytes.data()+offset),std::streamsize(count)))
                    throw FileIOError("Cannot read back CXDLPv4 output");
                offset+=count;
            }
            const sla::CrealityCXDLPv4Layout layout(bytes);
            trace("CXDLP loaded");
            const auto view = sla::cxdlpv4_reference_bottom_view(layout,
                [&](size_t i) { return sla::cxdlpv4_layer_image(layout, i, check); },
                [&](size_t, size_t) { check(); return true; });
            if (view.native.layer_height != fresh.globals.layer_height ||
                view.native.normal != fresh.globals.normal ||
                view.native.bottom_layers != fresh.globals.bottom_layers ||
                view.native.parameter_bottom_layers != fresh.globals.bottom_layers ||
                view.native.transition_layers != fresh.globals.transition_layers ||
                view.native.raw_per_layer_mode != 1 ||
                view.meaningful_bottom_layer != fresh.meaningful_bottom_layer)
                throw RuntimeError("CXDLPv4 native globals/counts cannot preserve fresh job intent");
            using F = sla::LayerParameter;
            auto bottom = fresh.globals.bottom;
            for (F field : {F::WaitBefore,F::WaitAfter,F::WaitAfterLift,F::RetractSpeed,F::RetractDistance2,F::RetractSpeed2}) {
                if (view.virtual_bottom.at(field) != bottom.at(field))
                    throw RuntimeError("CXDLPv4 virtual bottom defaults cannot preserve fresh job intent");
                bottom.erase(field);
            }
            if (view.native.bottom != bottom)
                throw RuntimeError("CXDLPv4 native bottom defaults cannot preserve fresh job intent");

            if (layout.number(layout.base+4) != float(fresh.geometry.width_mm) ||
                layout.number(layout.base+8) != float(fresh.geometry.height_mm) ||
                layout.number(layout.base+16) != float(fresh.rows.back().at(F::PositionZ)))
                throw RuntimeError("CXDLPv4 native geometry cannot preserve fresh job intent");
            const auto &material = raster_context->material_config();
            const auto material_metrics = sla::cxdlpv4_fresh_material_metrics(fresh.material_ml,
                material.material_density.value, material.bottle_cost.value, material.bottle_volume.value);
            if (layout.integer(layout.base+44) != fresh.expected_print_time ||
                layout.number(layout.integer(layout.base+52)+20) != fresh.material_ml ||
                layout.number(layout.integer(layout.base+52)+24) != material_metrics[0] ||
                layout.number(layout.integer(layout.base+52)+28) != material_metrics[1])
                throw RuntimeError("CXDLPv4 time/material accounting cannot preserve fresh job intent");
            for (size_t i=0;i<fresh.rows.size();++i) {
                check();
                if (sla::cxdlpv4_layer_parameters(layout,i) != fresh.rows[i])
                    throw RuntimeError("CXDLPv4 native rows cannot preserve fresh job intent");
            }
            trace("CXDLP metadata verified");
            const int workers=int(std::clamp<uint64_t>((uint64_t(512)<<20)/
                std::max<uint64_t>(1,uint64_t(layout.columns)*layout.rows*4),1,8));
            tbb::task_arena verification_arena(workers);
            // User checkpoints stay on the coordinator. Workers inspect only
            // thread-safe cancellation state, never arbitrary UI callbacks.
            const auto worker_check=[this,&generation] {
                PrintBase::throw_if_canceled();generation.check();
            };
            for(size_t first=0;first<fresh.rows.size();first+=size_t(workers)) {
                check();
                const auto end=std::min(first+size_t(workers),fresh.rows.size());
                verification_arena.execute([&] {execution::for_each(ExecutionTBB{},first,end,[&](size_t i) {
                    worker_check();
                    const auto encoded=sla::cxdlpv4_layer_image(layout,i,worker_check),source=document.image(i);
                    if(encoded.cols!=source.cols || encoded.rows!=source.rows || encoded.buf!=source.buf)
                        throw RuntimeError("CXDLPv4 grayscale pixels cannot preserve fresh job intent");
                    worker_check();
                });});
                check();
            }
            trace("CXDLP verified");
            return; // Already written and proven by the format-owned contract.
        }
        writer->export_print(temporary, *raster_context, previews, projectname);
        if(transferring_parameters) {
            const auto progress=[&check](size_t,size_t) {
                check();return true;
            };
            const bool aes_transfer=format=="ctb_encrypted" || format=="ctb_encrypted_v4";
            const bool plain_ctb_transfer=format=="ctb_v3" || format=="ctb_v4" || format=="ctb_v5";
            std::vector<sla::LayerMaskStatistics> compared_masks;
            auto destination=plain_ctb_transfer ?
                sla::LayerDocument::load_converted_chitubox(temporary,format,document,compared_masks,progress) :
                sla::LayerDocument::load_archive(temporary,progress);
            const auto capabilities=destination.parameter_capabilities();
            std::optional<sla::LayerDocument::ChituboxConversionMasks> verified_masks;
            if (plain_ctb_transfer) {
                verified_masks.emplace(destination);
                verified_masks->layers=std::move(compared_masks);
            }
            std::vector<sla::LayerParameters> table;table.reserve(document.layer_count());
            for(size_t i=0;i<document.layer_count();++i) {
                progress(i,document.layer_count());
                // Plain CTB already compared every native mask during import.
                // Other formats keep their complete document-pixel comparison.
                if (!plain_ctb_transfer) {
                    const auto encoded=destination.image(i),source_mask=document.image(i);
                    if(encoded.cols!=source_mask.cols || encoded.rows!=source_mask.rows || encoded.buf!=source_mask.buf)
                        throw RuntimeError("Destination grayscale precision cannot preserve the source exposure pixels");
                }
                const auto &source = plan->rows[i];
                const auto before=destination.layer_parameters(i);
                auto row=before;
                for(const auto &[field,value]:source) {
                    const auto capability=std::find_if(capabilities.begin(),capabilities.end(),
                        [&](const auto &candidate){return candidate.field==field;});
                    if(capability==capabilities.end() || !before.count(field))
                        throw RuntimeError("Destination cannot preserve a source per-layer field");
                    if(!capability->editable && !aes_transfer) {
                        if(before.at(field)!=value)
                            throw RuntimeError("Destination motion is fixed and differs from the source per-layer field");
                    } else row[field]=value;
                }
                table.push_back(std::move(row));
            }
            if (aes_transfer)
                // Import complete parameter intent into a newly generated
                // container, not an editor patch against its bootstrap rows.
                destination.save_converted_ctb_encrypted(temporary,table,source_globals ? &*source_globals : nullptr,
                    fresh_pricing ? &*fresh_pricing : nullptr,progress);
            else if (plain_ctb_transfer) {
                destination.save_converted_chitubox(temporary,table,source_globals ? &*source_globals : nullptr,
                    fresh_pricing ? &*fresh_pricing : nullptr,std::move(*verified_masks),progress);
            } else {
                if (fresh_pricing)
                    throw RuntimeError("Fresh material pricing requires a supported native transfer");
                destination.replace_layer_parameter_table(table);
                progress(0,table.size());
                destination.save_archive(temporary, progress, nullptr, source_globals ? &*source_globals : nullptr);
            }
            if (plain_ctb_transfer) {
                destination.verify_converted_chitubox(temporary, table,
                    source_globals ? &*source_globals : nullptr, progress);
            } else {
                const auto verified=sla::LayerDocument::load_archive(temporary,progress);
                if (source_globals) verified.require_job_parameters(*source_globals);
                for (size_t i=0;i<table.size();++i) {
                    progress(i,table.size());
                    if (verified.layer_parameters(i)!=table[i])
                        throw RuntimeError("Native destination cannot preserve the complete converted parameter table");
                }
            }
        }
    }, check,
        transferring_parameters ? get_default_extension(format.c_str()) : "", &generation);
}

bool SLAPrint::is_prusa_print(const std::string& printer_model)
{
    static const std::vector<std::string> prusa_printer_models = { "SL1", "SL1S", "M1", "SLX" };
    for (const std::string& model : prusa_printer_models)
        if (model == printer_model)
            return true;

    return false;
}

bool SLAPrint::invalidate_step(SLAPrintStep step)
{
    bool invalidated = Inherited::invalidate_step(step);
    // Invalidation cancels background work before releasing its raster cache.
    reset_layer_document_cache();

    // propagate to dependent steps
    if (step == slapsMergeSlicesAndEval) {
        invalidated |= this->invalidate_all_steps();
    }

    return invalidated;
}

void SLAPrint::process()
{
    if (m_objects.empty())
        return;

    name_tbb_thread_pool_threads_set_locale();

    // Assumption: at this point the print objects should be populated only with
    // the model objects we have to process and the instances are also filtered
    
    Steps printsteps(this);

    // We want to first process all objects...
    std::vector<SLAPrintObjectStep> level1_obj_steps = {
        slaposAssembly, slaposHollowing, slaposDrillHoles, slaposObjectSlice, slaposSupportPoints, slaposSupportTree, slaposPad
    };

    // and then slice all supports to allow preview to be displayed ASAP
    std::vector<SLAPrintObjectStep> level2_obj_steps = {
        slaposSliceSupports
    };

    SLAPrintStep print_steps[] = { slapsMergeSlicesAndEval, slapsRasterize };
    
    double st = Steps::min_objstatus;

    BOOST_LOG_TRIVIAL(info) << "Start slicing process.";

#ifdef SLAPRINT_DO_BENCHMARK
    Benchmark bench;
#else
    struct {
        void start() {} void stop() {} double getElapsedSec() { return .0; }
    } bench;
#endif

    std::array<double, slaposCount + slapsCount> step_times {};

    auto apply_steps_on_objects =
        [this, &st, &printsteps, &step_times, &bench]
        (const std::vector<SLAPrintObjectStep> &steps)
    {
        double incr = 0;
        for (SLAPrintObject *po : m_objects) {
            for (SLAPrintObjectStep step : steps) {

                // Cancellation checking. Each step will check for
                // cancellation on its own and return earlier gracefully.
                // Just after it returns execution gets to this point and
                // throws the canceled signal.
                throw_if_canceled();

                st += incr;

                if (po->set_started(step)) {
                    m_report_status(*this, st, printsteps.label(step));
                    bench.start();
                    printsteps.execute(step, *po);
                    bench.stop();
                    step_times[step] += bench.getElapsedSec();
                    throw_if_canceled();
                    po->set_done(step);
                }
                
                incr = printsteps.progressrange(step);
            }
        }
    };

    apply_steps_on_objects(level1_obj_steps);
    apply_steps_on_objects(level2_obj_steps);

    st = Steps::max_objstatus;
    for(SLAPrintStep currentstep : print_steps) {
        throw_if_canceled();

        if (set_started(currentstep)) {
            m_report_status(*this, st, printsteps.label(currentstep));
            bench.start();
            printsteps.execute(currentstep);
            bench.stop();
            step_times[slaposCount + currentstep] += bench.getElapsedSec();
            throw_if_canceled();
            set_done(currentstep);
        }
        
        st += printsteps.progressrange(currentstep);
    }

    if (m_pending_project_layers) {
        auto saved = std::move(m_pending_project_layers);
        try {
            m_project_layer_restore_failed = !sla::project_layers_match(*this, m_model, m_full_print_config, *saved);
            if (!m_project_layer_restore_failed) {
                // This is the processing worker itself. The public mutation
                // path stops/joins an external worker and would deadlock here.
                std::scoped_lock<std::mutex> lock(this->state_mutex());
                replace_layer_document_locked(saved->document, false);
            }
        } catch (const std::exception &error) {
            m_project_layer_restore_failed = true;
            BOOST_LOG_TRIVIAL(warning) << "Cannot restore saved SLA masks: " << error.what();
        }
    }

    if (m_layer_document && (m_layer_document->revision() != 0 || m_layer_document->fresh_job())) {
        m_report_status(*this, 99, _u8L("Generating statistics"));
        m_print_statistics = raster_job_statistics(*this, *m_layer_document, [this] { throw_if_canceled(); });
    }

    // Completion is visible only after final statistics and the last cancel check.
    throw_if_canceled();
    m_report_status(*this, 100, _u8L("Slicing done"));

#ifdef SLAPRINT_DO_BENCHMARK
    std::string csvbenchstr;
    for (size_t i = 0; i < size_t(slaposCount); ++i)
        csvbenchstr += printsteps.label(SLAPrintObjectStep(i)) + ";";

    for (size_t i = 0; i < size_t(slapsCount); ++i)
        csvbenchstr += printsteps.label(SLAPrintStep(i)) + ";";

    csvbenchstr += "\n";
    for (double t : step_times) csvbenchstr += std::to_string(t) + ";";

    std::cout << "Performance stats: \n" << csvbenchstr << std::endl;
#endif
    // Promote only at successful process exit: callbacks and optional
    // benchmark formatting above may still throw before reaching this point.
    finalize_completed_cxdlpv4_encoding();
}

bool SLAPrint::invalidate_state_by_config_options(const std::vector<t_config_option_key> &opt_keys, bool &invalidate_all_model_objects)
{
    using namespace std::string_view_literals;

    if (opt_keys.empty())
        return false;

    static constexpr StaticSet steps_full = {
        "initial_layer_height"sv,
        "material_correction"sv,
        "material_correction_x"sv,
        "material_correction_y"sv,
        "material_correction_z"sv,
        "material_print_speed"sv,
        "relative_correction"sv,
        "relative_correction_x"sv,
        "relative_correction_y"sv,
        "relative_correction_z"sv,
        "absolute_correction"sv,
        "elefant_foot_compensation"sv,
        "elefant_foot_min_width"sv,
        "zcorrection_layers"sv,
    };

    // Cache the plenty of parameters, which influence the final rasterization only,
    // or they are only notes not influencing the rasterization step.
    static constexpr StaticSet steps_rasterize = {
        "min_exposure_time"sv,
        "max_exposure_time"sv,
        "exposure_time"sv,
        "min_initial_exposure_time"sv,
        "max_initial_exposure_time"sv,
        "initial_exposure_time"sv,
        "display_width"sv,
        "display_height"sv,
        "display_pixels_x"sv,
        "display_pixels_y"sv,
        "display_mirror_x"sv,
        "display_mirror_y"sv,
        "display_orientation"sv,
        "sla_archive_format"sv,
        "sla_anycubic_version"sv,
        "sla_cbddlp_aa"sv,
        "sla_phz_encryption_key"sv,
        "nanodlp_device_profile"sv,
        "sla_output_precision"sv,
        // tilt params
        "delay_before_exposure"sv,
        "delay_after_exposure"sv,
        "tower_hop_height"sv,
        "tower_speed"sv,
        "use_tilt"sv,
        "tilt_down_initial_speed"sv,
        "tilt_down_offset_steps"sv,
        "tilt_down_offset_delay"sv,
        "tilt_down_finish_speed"sv,
        "tilt_down_cycles"sv,
        "tilt_down_delay"sv,
        "tilt_up_initial_speed"sv,
        "tilt_up_offset_steps"sv,
        "tilt_up_offset_delay"sv,
        "tilt_up_finish_speed"sv,
        "tilt_up_cycles"sv,
        "tilt_up_delay"sv,
        "area_fill"sv,
    };

    static StaticSet steps_ignore = {
        "bed_shape"sv,
        "max_print_height"sv,
        "printer_technology"sv,
        "output_filename_format"sv,
        "fast_tilt_time"sv,
        "slow_tilt_time"sv,
        "high_viscosity_tilt_time"sv,
        "bottle_cost"sv,
        "bottle_volume"sv,
        "bottle_weight"sv,
        "material_density"sv,
        "material_ow_support_pillar_diameter"sv,
        "material_ow_support_head_front_diameter"sv,
        "material_ow_support_head_penetration"sv,
        "material_ow_support_head_width"sv,
        "material_ow_branchingsupport_pillar_diameter"sv,
        "material_ow_branchingsupport_head_front_diameter"sv,
        "material_ow_branchingsupport_head_penetration"sv,
        "material_ow_branchingsupport_head_width"sv,
        "material_ow_elefant_foot_compensation"sv,
        "material_ow_support_points_density_relative"sv,
        "material_ow_absolute_correction"sv,
        "printer_model"sv,
    };

    std::vector<SLAPrintStep> steps;
    std::vector<SLAPrintObjectStep> osteps;
    bool invalidated = false;

    for (std::string_view opt_key : opt_keys) {
        if (opt_key == "gamma_correction") {
            // Gamma changes antialiasing only. Preserve supports, mesh slices
            // and merged polygons; rebuild the encoded raster masks.
            steps.emplace_back(slapsRasterize);
        } else if (steps_rasterize.find(opt_key) != steps_rasterize.end()) {
            // These options only affect the final rasterization, or they are just notes without influence on the output,
            // so there is nothing to invalidate.
            steps.emplace_back(slapsMergeSlicesAndEval);
        } else if (opt_key.find("sla_") == 0 && std::any_of(std::begin(sla::machine_settings), std::end(sla::machine_settings),
                   [&](const auto &setting) { return opt_key == setting.key; })) {
            // Machine motion does not affect polygon slicing.
            steps.emplace_back(slapsMergeSlicesAndEval);
        } else if (steps_ignore.find(opt_key) != steps_ignore.end()) {
            // These steps have no influence on the output. Just ignore them.
        } else if (steps_full.find(opt_key) != steps_full.end()) {
            steps.emplace_back(slapsMergeSlicesAndEval);
            osteps.emplace_back(slaposObjectSlice);
            invalidate_all_model_objects = true;
        } else {
            // All values should be covered.
            assert(false);
        }
    }

    // Exposure, motion and codec/AA changes preserve canonical edited masks.
    // Keep those masks through statistics/encoding updates. Geometry, display,
    // orientation and gamma changes still invalidate them normally.
    const bool metadata_only = std::all_of(opt_keys.begin(), opt_keys.end(), [](std::string_view key) {
        return key == "exposure_time" || key == "initial_exposure_time" ||
            key == "min_exposure_time" || key == "max_exposure_time" ||
            key == "min_initial_exposure_time" || key == "max_initial_exposure_time" ||
            key == "sla_archive_format" || key == "sla_anycubic_version" || key == "sla_cbddlp_aa" || key == "sla_phz_encryption_key" || key == "nanodlp_device_profile" ||
            std::any_of(std::begin(sla::machine_settings), std::end(sla::machine_settings),
                        [&](const auto &setting) { return key == setting.key; });
    });
    std::unique_ptr<sla::LayerDocument> preserved;
    if (metadata_only) {
        this->call_cancel_callback();
        preserved = std::move(m_layer_document);
    }
    sort_remove_duplicates(steps);
    for (SLAPrintStep step : steps)
        invalidated |= this->invalidate_step(step);
    sort_remove_duplicates(osteps);
    for (SLAPrintObjectStep ostep : osteps)
        for (SLAPrintObject *object : m_objects)
            invalidated |= object->invalidate_step(ostep);
    if (preserved) {
        m_layer_document = std::move(preserved);
        m_encoded_revision = std::numeric_limits<uint64_t>::max();
    }
    return invalidated;
}

// Returns true if an object step is done on all objects and there's at least one object.
bool SLAPrint::is_step_done(SLAPrintObjectStep step) const
{
    if (m_objects.empty())
        return false;
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    for (const SLAPrintObject *object : m_objects)
        if (! object->is_step_done_unguarded(step))
            return false;
    return true;
}

SLAPrintObject::SLAPrintObject(SLAPrint *print, ModelObject *model_object)
    : Inherited(print, model_object)
{}

SLAPrintObject::~SLAPrintObject() {}

// Called by SLAPrint::apply().
// This method only accepts SLAPrintObjectConfig option keys.
bool SLAPrintObject::invalidate_state_by_config_options(const std::vector<t_config_option_key> &opt_keys)
{
    if (opt_keys.empty())
        return false;

    std::vector<SLAPrintObjectStep> steps;
    bool invalidated = false;
    for (const t_config_option_key &opt_key : opt_keys) {
        if (   opt_key == "hollowing_enable"
            || opt_key == "hollowing_min_thickness"
            || opt_key == "hollowing_quality"
            || opt_key == "hollowing_closing_distance"
            ) {
            steps.emplace_back(slaposHollowing);
        } else if (
               opt_key == "layer_height"
            || opt_key == "faded_layers"
            || opt_key == "pad_enable"
            || opt_key == "pad_wall_thickness"
            || opt_key == "supports_enable"
            || opt_key == "support_tree_type"
            || opt_key == "support_object_elevation"
            || opt_key == "branchingsupport_object_elevation"
            || opt_key == "pad_around_object"
            || opt_key == "pad_around_object_everywhere"
            || opt_key == "slice_closing_radius"
            || opt_key == "slicing_mode") {
            steps.emplace_back(slaposObjectSlice);
        } else if (
               opt_key == "support_points_density_relative"
            || opt_key == "support_enforcers_only"
            ) {
            steps.emplace_back(slaposSupportPoints);
        } else if (
               opt_key == "support_head_front_diameter"
            || opt_key == "support_head_penetration"
            || opt_key == "support_head_width"
            || opt_key == "support_pillar_diameter"
            || opt_key == "support_pillar_widening_factor"
            || opt_key == "support_small_pillar_diameter_percent"
            || opt_key == "support_max_weight_on_model"
            || opt_key == "support_max_bridges_on_pillar"
            || opt_key == "support_pillar_connection_mode"
            || opt_key == "support_buildplate_only"
            || opt_key == "support_base_diameter"
            || opt_key == "support_base_height"
            || opt_key == "support_critical_angle"
            || opt_key == "support_max_bridge_length"
            || opt_key == "support_max_pillar_link_distance"
            || opt_key == "support_base_safety_distance"
            || opt_key == "branchingsupport_head_front_diameter"
            || opt_key == "branchingsupport_head_penetration"
            || opt_key == "branchingsupport_head_width"
            || opt_key == "branchingsupport_pillar_diameter"
            || opt_key == "branchingsupport_pillar_widening_factor"
            || opt_key == "branchingsupport_small_pillar_diameter_percent"
            || opt_key == "branchingsupport_max_weight_on_model"
            || opt_key == "branchingsupport_max_bridges_on_pillar"
            || opt_key == "branchingsupport_pillar_connection_mode"
            || opt_key == "branchingsupport_buildplate_only"
            || opt_key == "branchingsupport_base_diameter"
            || opt_key == "branchingsupport_base_height"
            || opt_key == "branchingsupport_critical_angle"
            || opt_key == "branchingsupport_max_bridge_length"
            || opt_key == "branchingsupport_max_pillar_link_distance"
            || opt_key == "branchingsupport_base_safety_distance"
            || opt_key == "pad_object_gap"
            ) {
            steps.emplace_back(slaposSupportTree);
        } else if (
               opt_key == "pad_wall_height"
            || opt_key == "pad_brim_size"
            || opt_key == "pad_max_merge_distance"
            || opt_key == "pad_wall_slope"
            || opt_key == "pad_edge_radius"
            || opt_key == "pad_object_connector_stride"
            || opt_key == "pad_object_connector_width"
            || opt_key == "pad_object_connector_penetration"
            ) {
            steps.emplace_back(slaposPad);
        } else {
            // All keys should be covered.
            assert(false);
        }
    }

    sort_remove_duplicates(steps);
    for (SLAPrintObjectStep step : steps)
        invalidated |= this->invalidate_step(step);
    return invalidated;
}

bool SLAPrintObject::invalidate_step(SLAPrintObjectStep step)
{
    bool invalidated = Inherited::invalidate_step(step);
    // propagate to dependent steps
    if (step == slaposAssembly) {
        invalidated |= this->invalidate_all_steps();
    } else if (step == slaposHollowing) {
        invalidated |= invalidated |= this->invalidate_steps({ slaposDrillHoles, slaposObjectSlice, slaposSupportPoints, slaposSupportTree, slaposPad, slaposSliceSupports });
    } else if (step == slaposDrillHoles) {
        invalidated |= this->invalidate_steps({ slaposObjectSlice, slaposSupportPoints, slaposSupportTree, slaposPad, slaposSliceSupports });
        invalidated |= m_print->invalidate_step(slapsMergeSlicesAndEval);
    } else if (step == slaposObjectSlice) {
        invalidated |= this->invalidate_steps({ slaposSupportPoints, slaposSupportTree, slaposPad, slaposSliceSupports });
        invalidated |= m_print->invalidate_step(slapsMergeSlicesAndEval);
    } else if (step == slaposSupportPoints) {
        invalidated |= this->invalidate_steps({ slaposSupportTree, slaposPad, slaposSliceSupports });
        invalidated |= m_print->invalidate_step(slapsMergeSlicesAndEval);
    } else if (step == slaposSupportTree) {
        invalidated |= this->invalidate_steps({ slaposPad, slaposSliceSupports });
        invalidated |= m_print->invalidate_step(slapsMergeSlicesAndEval);
    } else if (step == slaposPad) {
        invalidated |= this->invalidate_steps({slaposSliceSupports});
        invalidated |= m_print->invalidate_step(slapsMergeSlicesAndEval);
    } else if (step == slaposSliceSupports) {
        invalidated |= m_print->invalidate_step(slapsMergeSlicesAndEval);
    }
    return invalidated;
}

bool SLAPrintObject::invalidate_all_steps()
{
    // Both caches must be invalidated even when the first call returns true.
    const bool object_invalidated = Inherited::invalidate_all_steps();
    const bool print_invalidated = m_print->invalidate_step(slapsMergeSlicesAndEval);
    return object_invalidated || print_invalidated;
}

double SLAPrintObject::get_elevation() const {
    if (is_zero_elevation(m_config)) return 0.;

    bool en = m_config.supports_enable.getBool();

    double ret = en ? m_config.support_object_elevation.getFloat() : 0.;

    if(m_config.pad_enable.getBool()) {
        // Normally the elevation for the pad itself would be the thickness of
        // its walls but currently it is half of its thickness. Whatever it
        // will be in the future, we provide the config to the get_pad_elevation
        // method and we will have the correct value
        sla::PadConfig pcfg = make_pad_cfg(m_config);
        if(!pcfg.embed_object) ret += pcfg.required_elevation();
    }

    return ret;
}

double SLAPrintObject::get_current_elevation() const
{
    if (is_zero_elevation(m_config)) return 0.;

    bool has_supports = is_step_done(slaposSupportTree);
    bool has_pad      = is_step_done(slaposPad);

    if(!has_supports && !has_pad)
        return 0;
    else if(has_supports && !has_pad) {
        return m_config.support_object_elevation.getFloat();
    }

    return get_elevation();
}

Vec3d SLAPrint::relative_correction() const
{
    Vec3d corr(1., 1., 1.);

    if(printer_config().relative_correction.values.size() >= 2) {
        corr.x() = printer_config().relative_correction_x.value;
        corr.y() = printer_config().relative_correction_y.value;
        corr.z() = printer_config().relative_correction_z.value;
    }

    if(material_config().material_correction.values.size() >= 2) {
        corr.x() *= material_config().material_correction_x.value;
        corr.y() *= material_config().material_correction_y.value;
        corr.z() *= material_config().material_correction_z.value;
    }

    return corr;
}

namespace { // dummy empty static containers for return values in some methods
const std::vector<ExPolygons> EMPTY_SLICES;
const TriangleMesh EMPTY_MESH;
const indexed_triangle_set EMPTY_TRIANGLE_SET;
const ExPolygons EMPTY_SLICE;
const std::vector<sla::SupportPoint> EMPTY_SUPPORT_POINTS;
}

const SliceRecord SliceRecord::EMPTY(0, std::nanf(""), 0.f);

const std::vector<sla::SupportPoint>& SLAPrintObject::get_support_points() const
{
    return m_supportdata? m_supportdata->input.pts : EMPTY_SUPPORT_POINTS;
}

const std::vector<ExPolygons> &SLAPrintObject::get_support_slices() const
{
    // assert(is_step_done(slaposSliceSupports));
    if (!m_supportdata) return EMPTY_SLICES;
    return m_supportdata->support_slices;
}

const ExPolygons &SliceRecord::get_slice(SliceOrigin o) const
{
    size_t idx = o == soModel ? m_model_slices_idx : m_support_slices_idx;

    if(m_po == nullptr) return EMPTY_SLICE;

    const std::vector<ExPolygons>& v = o == soModel? m_po->get_model_slices() :
                                                     m_po->get_support_slices();

    return idx >= v.size() ? EMPTY_SLICE : v[idx];
}

const TriangleMesh& SLAPrintObject::support_mesh() const
{
    if (m_config.supports_enable.getBool() &&
        is_step_done(slaposSupportTree) &&
        m_supportdata)
        return m_supportdata->tree_mesh;

    return EMPTY_MESH;
}

const TriangleMesh& SLAPrintObject::pad_mesh() const
{
    if(m_config.pad_enable.getBool() && is_step_done(slaposPad) && m_supportdata)
        return m_supportdata->pad_mesh;

    return EMPTY_MESH;
}

const std::shared_ptr<const indexed_triangle_set> &
SLAPrintObject::get_mesh_to_print() const
{
    int s = last_completed_step();

    while (s > 0 && ! m_preview_meshes[s])
        --s;

    return m_preview_meshes[s];
}

std::vector<csg::CSGPart> SLAPrintObject::get_parts_to_slice() const
{
    return get_parts_to_slice(slaposCount);
}

std::vector<csg::CSGPart>
SLAPrintObject::get_parts_to_slice(SLAPrintObjectStep untilstep) const
{
    auto laststep = last_completed_step();
    SLAPrintObjectStep s = std::min(untilstep, laststep);

    if (s == slaposCount)
        return {};

    std::vector<csg::CSGPart> ret;

    for (unsigned int step = 0; step <= s; ++step) {
        auto r = m_mesh_to_slice.equal_range(SLAPrintObjectStep(step));
        csg::copy_csgrange_shallow(Range{r.first, r.second}, std::back_inserter(ret));
    }

    return ret;
}

sla::SupportPoints SLAPrintObject::transformed_support_points() const
{
    assert(model_object());
    auto spts = model_object()->sla_support_points;
    Transform3f tr = trafo().cast<float>();
    for (sla::SupportPoint &suppt : spts) {
        suppt.pos = tr * suppt.pos;
    }
    return spts;
}

sla::DrainHoles SLAPrintObject::transformed_drainhole_points() const
{
    assert(model_object());

    return sla::transformed_drainhole_points(*model_object(), trafo());
}

DynamicConfig SLAPrintStatistics::config() const
{
    DynamicConfig config;
    const std::string print_time = sla::statistics_time(this->estimated_print_time, true);
    config.set_key_value("print_time", new ConfigOptionString(print_time));
    config.set_key_value("objects_used_material", new ConfigOptionFloat(this->objects_used_material));
    config.set_key_value("support_used_material", new ConfigOptionFloat(this->support_used_material));
    config.set_key_value("total_cost", new ConfigOptionFloat(this->total_cost));
    config.set_key_value("total_weight", new ConfigOptionFloat(this->total_weight));
    return config;
}

DynamicConfig SLAPrintStatistics::placeholders()
{
    DynamicConfig config;
    for (const char *key : {
        "print_time", "total_cost", "total_weight",
        "objects_used_material", "support_used_material" })
        config.set_key_value(key, new ConfigOptionString(std::string("{") + key + "}"));

    return config;
}

std::string SLAPrintStatistics::finalize_output_path(const std::string &path_in) const
{
    std::string final_path;
    try {
        boost::filesystem::path path(path_in);
        DynamicConfig cfg = this->config();
        PlaceholderParser pp;
        std::string new_stem = pp.process(path.stem().string(), 0, &cfg);
        final_path = (path.parent_path() / (new_stem + path.extension().string())).string();
    }
    catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Failed to apply the print statistics to the export file name: " << ex.what();
        final_path = path_in;
    }
    return final_path;
}

void SLAPrint::StatusReporter::operator()(SLAPrint &         p,
                                          double             st,
                                          const std::string &msg,
                                          unsigned           flags,
                                          const std::string &logmsg)
{
    m_st = st;
    BOOST_LOG_TRIVIAL(info)
        << st << "% " << msg << (logmsg.empty() ? "" : ": ") << logmsg
        << log_memory_info();

    p.set_status(int(std::round(st)), msg, flags);
}

namespace csg {

MeshBoolean::cgal::CGALMeshPtr get_cgalmesh(const CSGPartForStep &part)
{
    if (!part.cgalcache && csg::get_mesh(part)) {
        part.cgalcache = csg::get_cgalmesh(static_cast<const csg::CSGPart&>(part));
    }

    return part.cgalcache? clone(*part.cgalcache) : nullptr;
}

} // namespace csg

} // namespace Slic3r
