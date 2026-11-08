#include "ExposureFinderTable.hpp"
#include "ProjectArchive.hpp"
#include "ExposurePositionSchedule.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem.hpp>
#include "libslic3r/Model.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Format/AnycubicLayerLayout.hpp"
#include "libslic3r/miniz_extension.hpp"
#include <array>
#include <boost/algorithm/string.hpp>
#include <boost/uuid/detail/sha1.hpp>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>

namespace Slic3r::sla {
// Version 2 persists the current entry graph rather than flattening it to PNGs.
// Native source and opaque ZIP entries are retained for lossless native saving.
struct ProjectDocumentAccess {
  static void keys(const nlohmann::json &value, std::initializer_list<const char *> expected) {
    if (!value.is_object() || value.size() != expected.size())
      throw RuntimeError("Invalid fresh project metadata members");
    for (const auto *key : expected)
      if (!value.contains(key)) throw RuntimeError("Missing fresh project metadata member");
  }
  static size_t integer(const nlohmann::json &value, size_t maximum) {
    if (!value.is_number_integer() || value < 0 || value > maximum)
      throw RuntimeError("Invalid fresh project integer");
    return value.get<size_t>();
  }
  static double number(const nlohmann::json &value) {
    if (!value.is_number()) throw RuntimeError("Invalid fresh project numeric parameter");
    const double result = value.get<double>();
    if (!std::isfinite(result)) throw RuntimeError("Nonfinite fresh project numeric parameter");
    return result;
  }
  static nlohmann::json parameters(const LayerParameters &row) {
    nlohmann::json value = nlohmann::json::object();
    for (const auto &[field, parameter] : row) value[std::to_string(int(field))] = parameter;
    return value;
  }
  static LayerParameters parameters(const nlohmann::json &value, bool with_z) {
    static_assert(int(LayerParameter::PositionZ) == 0 && int(LayerParameter::LightPWM) == 13,
                  "Fresh metadata v1 requires its stable fourteen-field schema");
    if (!value.is_object() || value.size() != (with_z ? 14 : 13))
      throw RuntimeError("Incomplete fresh project parameter row");
    LayerParameters result;
    for (int field = with_z ? 0 : 1; field <= 13; ++field)
      result.emplace(LayerParameter(field), number(value.at(std::to_string(field))));
    return result;
  }
  static nlohmann::json describe_fresh(const FreshLayerJob &job) {
    nlohmann::json rows = nlohmann::json::array();
    for (const auto &row : job.rows) rows.push_back(parameters(row));
    nlohmann::json result={{"version",1},{"producer",int(job.producer)},
      {"globals",{{"layer_height",job.globals.layer_height},{"bottom_layers",job.globals.bottom_layers},
        {"transition_layers",job.globals.transition_layers},{"normal",parameters(job.globals.normal)},
        {"bottom",parameters(job.globals.bottom)}}},
      {"geometry",{{"columns",job.geometry.columns},{"rows",job.geometry.rows},
        {"width_mm",job.geometry.width_mm},{"height_mm",job.geometry.height_mm}}},{"rows",rows}};
    if (job.parameter_origin==FreshLayerParameterOrigin::AutomaticModel) {
      result["version"]=2;result["parameter_origin"]=int(job.parameter_origin);
    }
    if (job.mask_precision!=LayerMaskPrecision::Gray8) {
      result["version"]=3;result["parameter_origin"]=int(job.parameter_origin);
      result["mask_precision"]=int(job.mask_precision);
    }
    if (job.position_policy == ExposurePositionPolicy::RepeatedExposureHeights) {
      result["version"]=4;result["parameter_origin"]=int(job.parameter_origin);
      result["mask_precision"]=int(job.mask_precision);
      result["position_policy"]=int(job.position_policy);
    }
    return result;
  }
  static FreshLayerJob restore_fresh(const nlohmann::json &value, int manifest_version) {
    const bool automatic_metadata=manifest_version==4 || (manifest_version==6 && value.at("version")==2);
    const bool sequence_metadata=manifest_version==7;
    const bool precision_metadata=manifest_version==5 || (manifest_version==6 && value.at("version")==3) || sequence_metadata;
    if (sequence_metadata) keys(value,{"version","producer","globals","geometry","rows","parameter_origin","mask_precision","position_policy"});
    else if (precision_metadata) keys(value,{"version","producer","globals","geometry","rows","parameter_origin","mask_precision"});
    else if (automatic_metadata) keys(value,{"version","producer","globals","geometry","rows","parameter_origin"});
    else keys(value,{"version","producer","globals","geometry","rows"});
    if (!value.at("version").is_number_integer() || value.at("version") != (sequence_metadata?4:precision_metadata?3:automatic_metadata?2:1))
      throw RuntimeError("Unsupported fresh project descriptor version");
    FreshLayerJob job;
    if (sequence_metadata) {
      job.position_policy=ExposurePositionPolicy(integer(value.at("position_policy"),1));
      if (job.position_policy!=ExposurePositionPolicy::RepeatedExposureHeights)
        throw RuntimeError("Sequence project must explicitly declare repeated exposure heights");
    }
    job.producer = FreshLayerProducer(integer(value.at("producer"),3));
    if (automatic_metadata || precision_metadata) {
      job.parameter_origin=FreshLayerParameterOrigin(integer(value.at("parameter_origin"),1));
      if (automatic_metadata && job.parameter_origin!=FreshLayerParameterOrigin::AutomaticModel)
        throw RuntimeError("Automatic project must explicitly own automatic model parameters");
    }
    if (precision_metadata)
      job.mask_precision=LayerMaskPrecision(integer(value.at("mask_precision"),18));
    const auto &globals = value.at("globals");
    keys(globals,{"layer_height","bottom_layers","transition_layers","normal","bottom"});
    job.globals.layer_height = number(globals.at("layer_height"));
    job.globals.bottom_layers = integer(globals.at("bottom_layers"),100000);
    job.globals.transition_layers = integer(globals.at("transition_layers"),100000);
    job.globals.normal = parameters(globals.at("normal"),false);
    job.globals.bottom = parameters(globals.at("bottom"),false);
    const auto &geometry = value.at("geometry");
    keys(geometry,{"columns","rows","width_mm","height_mm"});
    job.geometry = {integer(geometry.at("columns"),32768),integer(geometry.at("rows"),32768),
      number(geometry.at("width_mm")),number(geometry.at("height_mm"))};
    const auto &rows = value.at("rows");
    if (!rows.is_array() || rows.empty() || rows.size() > 100000)
      throw RuntimeError("Invalid fresh project parameter table size");
    for (const auto &row : rows) job.rows.push_back(parameters(row,true));
    return job;
  }
  static void validate_profile(const ProjectRasterState &state) {
    if (state.export_profile) validate_profile(state,*state.export_profile);
  }
  static void validate_profile(const ProjectRasterState &state,const LayerExportProfile &profile) {
    const auto &doc=state.document;
    const auto image=doc.image(0);profile.validate(image.cols,image.rows);
    // Fresh geometry is an owned double-precision intention, not a native
    // header approximation. Changing the export context must not resize it.
    if (doc.m_fresh_job) {
      const auto &geometry=doc.m_fresh_job->geometry;
      if (profile.columns!=geometry.columns || profile.rows!=geometry.rows ||
          profile.screen_width_mm!=geometry.width_mm ||
          profile.screen_height_mm!=geometry.height_mm)
        throw RuntimeError("Project export profile differs from fresh raster geometry");
    }
    if(!anycubic_available_versions(doc.m_archive_format).empty()) {
      const AnycubicLayerLayout source(doc.m_entries.at(0).bytes,doc.m_archive_format);
      if(const auto dimensions=source.physical_dimensions();dimensions &&
         (std::abs(dimensions->first-profile.screen_width_mm)>.005 ||
          std::abs(dimensions->second-profile.screen_height_mm)>.005))
        throw RuntimeError("Project export profile differs from the native machine screen");
    }
    for(size_t i=0;i<doc.layer_count();++i) {
      const auto parameters=doc.layer_parameters(i);
      if (!parameters.empty())
        profile.validate_height(parameters.at(LayerParameter::PositionZ));
      else if (!state.positions_z.empty())
        profile.validate_height(state.positions_z.at(i));
      // An imported image/legacy container may have no canonical Z. Do not
      // invent heights just to apply a physical/material estimate context.
    }
  }

  static nlohmann::json describe(const LayerDocument &doc) {
    doc.validate_persisted_state();
    nlohmann::json names=nlohmann::json::array();
    for(const auto &entry:doc.m_entries)names.push_back(entry.name);
    nlohmann::json metadata{{"archive_format",doc.m_archive_format},{"entries",names},
            {"main",doc.m_layers},{"secondary",doc.m_secondary_layers},
            {"parameters",doc.m_layer_parameters}};
    if (doc.m_fresh_job) metadata["fresh_job"] = describe_fresh(*doc.m_fresh_job);
    if (doc.m_mask_precision) metadata["mask_contract"]={{"version",1},{"precision",int(*doc.m_mask_precision)}};
    return metadata;
  }
  static size_t size(const LayerDocument &doc){return doc.m_entries.size();}
  static png::ReadBuf entry(const LayerDocument &doc,size_t i) {
    const auto &bytes=doc.m_entries.at(i).bytes;return {bytes.data(),bytes.size()};
  }
  static LayerDocument restore(const nlohmann::json &metadata,
                               std::vector<std::vector<uint8_t>> bytes, int manifest_version) {
    LayerDocument doc;
    doc.m_archive_format=metadata.at("archive_format").get<std::string>();
    if(doc.m_archive_format.size()>128)throw RuntimeError("Invalid persisted archive format");
    const auto names=metadata.at("entries").get<std::vector<std::string>>();
    if(names.size()!=bytes.size())throw RuntimeError("Persisted document entries mismatch");
    for(size_t i=0;i<names.size();++i) {
      if(names[i].empty() || names[i].size()>4096 || names[i].find('\0')!=std::string::npos)
        throw RuntimeError("Invalid persisted entry name");
      doc.m_entries.push_back({names[i],std::move(bytes[i])});
    }
    auto indices=[&](const char *key) {
      const auto &list=metadata.at(key);
      if(!list.is_array() || list.size()>100000)throw RuntimeError("Invalid persisted index list");
      std::vector<size_t> result;
      for(const auto &index:list) {
        if(!index.is_number_integer() || index.get<int64_t>()<0 || index.get<uint64_t>()>=names.size())
          throw RuntimeError("Invalid persisted entry index");
        result.push_back(index.get<size_t>());
      }
      return result;
    };
    doc.m_layers=indices("main");doc.m_secondary_layers=indices("secondary");
    doc.m_layer_parameters=indices("parameters");
    if (manifest_version==8) {
      // The bounded v8 contract is independent of native/fresh ownership.
      // Reject shadow descriptors rather than interpreting their child schema
      // through a manifest version that never described FreshLayerJob.
      keys(metadata,{"archive_format","entries","main","secondary","parameters","mask_contract"});
      const auto &contract=metadata.at("mask_contract");keys(contract,{"version","precision"});
      if(integer(contract.at("version"),1)!=1)
        throw RuntimeError("Unsupported source mask contract version");
      const auto precision=LayerMaskPrecision(integer(contract.at("precision"),18));
      if(precision!=LayerMaskPrecision::NativeGray4 && precision!=LayerMaskPrecision::NativeBitplanes1)
        throw RuntimeError("Unsupported independent source mask precision");
      doc.m_mask_precision=precision;
    } else {
      if(metadata.contains("mask_contract"))
        throw RuntimeError("Source mask contract requires project manifest version 8");
      if (manifest_version>=3 && !(manifest_version==6 && !metadata.contains("fresh_job"))) {
        keys(metadata,{"archive_format","entries","main","secondary","parameters","fresh_job"});
        doc.m_fresh_job = restore_fresh(metadata.at("fresh_job"),manifest_version);
      } else if (metadata.contains("fresh_job"))
        throw RuntimeError("Fresh intentions require project manifest version 3");
    }
    doc.validate_persisted_state();doc.mark_restored_edit();return doc;
  }
};
namespace {
 double finder_number(const nlohmann::json &value) {
  if (!value.is_number()) throw RuntimeError("Invalid ExposureFinder recipe number");
  const double result=value.get<double>();
  if (!std::isfinite(result) || std::abs(result)>1000000.)
    throw RuntimeError("ExposureFinder recipe number exceeds its finite range");
  return result;
 }
 bool finder_boolean(const nlohmann::json &value) {
  if (!value.is_boolean()) throw RuntimeError("Invalid ExposureFinder recipe boolean");
  return value.get<bool>();
 }
 nlohmann::json describe_finder_schedule(const ExposureFinderScheduleParameters &p) {
  return {{"base_height",p.base_height},{"features_height",p.features_height},
    {"custom_base_exposure",p.custom_base_exposure},{"bottom_layers",p.bottom_layers},
    {"multiple_exposures",p.multiple_exposures},{"multiple_brightness",p.multiple_brightness},
    {"brightness",p.brightness},{"base_mode",int(p.base_mode)},
    {"brightness_exclusion",int(p.brightness_exclusion)},{"repeated_settings",p.repeated_settings},
    {"repeated_lift_enabled",p.repeated_lift_enabled},{"repeated_wait_enabled",p.repeated_wait_enabled},
    {"repeated_lift_height",p.repeated_lift_height},{"repeated_wait_before",p.repeated_wait_before}};
 }
 ExposureFinderScheduleParameters restore_finder_schedule(const nlohmann::json &data) {
  ProjectDocumentAccess::keys(data,{"base_height","features_height","custom_base_exposure","bottom_layers",
    "multiple_exposures","multiple_brightness","brightness","base_mode","brightness_exclusion",
    "repeated_settings","repeated_lift_enabled","repeated_wait_enabled","repeated_lift_height","repeated_wait_before"});
  ExposureFinderScheduleParameters p;
  p.base_height=finder_number(data.at("base_height"));p.features_height=finder_number(data.at("features_height"));
  p.custom_base_exposure=finder_number(data.at("custom_base_exposure"));
  p.bottom_layers=ProjectDocumentAccess::integer(data.at("bottom_layers"),65535);
  p.multiple_exposures=finder_boolean(data.at("multiple_exposures"));
  p.multiple_brightness=finder_boolean(data.at("multiple_brightness"));
  p.base_mode=ExposureFinderBaseMode(ProjectDocumentAccess::integer(data.at("base_mode"),4));
  p.brightness_exclusion=ExposureFinderBrightnessExclusion(ProjectDocumentAccess::integer(data.at("brightness_exclusion"),2));
  p.repeated_settings=finder_boolean(data.at("repeated_settings"));
  p.repeated_lift_enabled=finder_boolean(data.at("repeated_lift_enabled"));
  p.repeated_wait_enabled=finder_boolean(data.at("repeated_wait_enabled"));
  p.repeated_lift_height=finder_number(data.at("repeated_lift_height"));
  p.repeated_wait_before=finder_number(data.at("repeated_wait_before"));
  const auto &values=data.at("brightness");
  if (!values.is_array() || values.size()>256) throw RuntimeError("Invalid ExposureFinder schedule brightness list");
  p.brightness.clear();p.brightness.reserve(values.size());
  for (const auto &value:values)p.brightness.push_back(uint8_t(ProjectDocumentAccess::integer(value,255)));
  return p;
 }
 int finder_signed(const nlohmann::json &value,int minimum,int maximum) {
  if(!value.is_number_integer())throw RuntimeError("Invalid ExposureFinder signed integer");
  if(value.is_number_unsigned() && value.get<uint64_t>()>uint64_t(maximum))throw RuntimeError("ExposureFinder signed integer overflow");
  const int64_t number=value.get<int64_t>();
  if(number<minimum || number>maximum)throw RuntimeError("ExposureFinder signed integer outside range");
  return int(number);
 }
 nlohmann::json describe_finder_generation(const ExposureFinderGenerationSettings &settings) {
  settings.validate();const auto &p=settings.pattern;const auto &l=settings.layout;
  return {{"version",1},{"bottom_erosion",settings.bottom_erosion},{"chamfer_layers",settings.chamfer_layers},
    {"pattern",{{"holes",p.holes},{"square_holes",p.square_holes},{"bars",p.bars},{"bullseyes",p.bullseyes},
      {"feature_margin_mm",p.feature_margin_mm},{"bar_length_mm",p.bar_length_mm},{"bar_spacing_mm",p.bar_spacing_mm},
      {"bar_splitter",p.bar_splitter},{"bar_fence",p.bar_fence},{"bar_fence_offset",p.bar_fence_offset},
      {"bullseye_fence",p.bullseye_fence},{"bullseye_fence_offset",p.bullseye_fence_offset},
      {"invert_bullseye_quadrants",p.invert_bullseye_quadrants},{"counter_triangles",p.counter_triangles},
      {"triangle_fence",p.triangle_fence},{"triangle_tip_offset",p.triangle_tip_offset},
      {"staircase_thickness",p.staircase_thickness},{"antialias",p.antialias},{"text",p.text},
      {"font_face",p.font_face},{"text_thickness",p.text_thickness},{"text_scale",p.text_scale}}},
    {"layout",{{"cell_columns",l.cell_columns},{"cell_rows",l.cell_rows},{"margin_x_mm",l.margin_x_mm},
      {"margin_y_mm",l.margin_y_mm},{"gap_x_mm",l.gap_x_mm},{"gap_y_mm",l.gap_y_mm},
      {"center",l.center},{"mirror_x",l.mirror_x},{"mirror_y",l.mirror_y}}}};
 }
 ExposureFinderGenerationSettings restore_finder_generation(const nlohmann::json &data) {
  ProjectDocumentAccess::keys(data,{"version","pattern","layout","bottom_erosion","chamfer_layers"});
  if(ProjectDocumentAccess::integer(data.at("version"),1)!=1)throw RuntimeError("Unsupported ExposureFinder generation settings");
  ExposureFinderGenerationSettings result;auto &p=result.pattern;auto &l=result.layout;
  result.bottom_erosion=ProjectDocumentAccess::integer(data.at("bottom_erosion"),255);
  result.chamfer_layers=ProjectDocumentAccess::integer(data.at("chamfer_layers"),255);
  const auto &v=data.at("pattern");
  ProjectDocumentAccess::keys(v,{"holes","square_holes","bars","bullseyes","feature_margin_mm","bar_length_mm","bar_spacing_mm",
    "bar_splitter","bar_fence","bar_fence_offset","bullseye_fence","bullseye_fence_offset","invert_bullseye_quadrants",
    "counter_triangles","triangle_fence","triangle_tip_offset","staircase_thickness","antialias","text","font_face","text_thickness","text_scale"});
  const auto feature_list=[&](const char *name) {
    const auto &list=v.at(name);if(!list.is_array() || list.size()>500)throw RuntimeError("Invalid ExposureFinder feature list");
    std::vector<unsigned> values;for(const auto &value:list)values.push_back(ProjectDocumentAccess::integer(value,500));return values;
  };
  p.holes=feature_list("holes");p.bars=feature_list("bars");
  const auto &rings=v.at("bullseyes");if(!rings.is_array() || rings.size()>500)throw RuntimeError("Invalid ExposureFinder rings");
  p.bullseyes.clear();for(const auto &ring:rings) {
    if(!ring.is_array() || ring.size()!=2)throw RuntimeError("Invalid ExposureFinder ring tuple");
    p.bullseyes.push_back({unsigned(ProjectDocumentAccess::integer(ring.at(0),500)),unsigned(ProjectDocumentAccess::integer(ring.at(1),500))});
  }
  p.square_holes=finder_boolean(v.at("square_holes"));p.invert_bullseye_quadrants=finder_boolean(v.at("invert_bullseye_quadrants"));
  p.counter_triangles=finder_boolean(v.at("counter_triangles"));p.triangle_fence=finder_boolean(v.at("triangle_fence"));p.antialias=finder_boolean(v.at("antialias"));
  p.feature_margin_mm=finder_number(v.at("feature_margin_mm"));p.bar_length_mm=finder_number(v.at("bar_length_mm"));p.bar_spacing_mm=finder_number(v.at("bar_spacing_mm"));
  p.text_scale=finder_number(v.at("text_scale"));p.bar_splitter=finder_signed(v.at("bar_splitter"),-127,127);
  p.bar_fence_offset=finder_signed(v.at("bar_fence_offset"),-127,127);p.bullseye_fence_offset=finder_signed(v.at("bullseye_fence_offset"),-127,127);
  p.triangle_tip_offset=finder_signed(v.at("triangle_tip_offset"),-127,127);
  p.bar_fence=ProjectDocumentAccess::integer(v.at("bar_fence"),255);p.bullseye_fence=ProjectDocumentAccess::integer(v.at("bullseye_fence"),255);
  p.staircase_thickness=ProjectDocumentAccess::integer(v.at("staircase_thickness"),4096);
  p.font_face=ProjectDocumentAccess::integer(v.at("font_face"),23);p.text_thickness=ProjectDocumentAccess::integer(v.at("text_thickness"),20);
  const auto &text=v.at("text");if(!text.is_string() || text.get_ref<const std::string &>().size()>4096)throw RuntimeError("Invalid ExposureFinder text");p.text=text.get<std::string>();
  const auto &layout=data.at("layout");ProjectDocumentAccess::keys(layout,{"cell_columns","cell_rows","margin_x_mm","margin_y_mm","gap_x_mm","gap_y_mm","center","mirror_x","mirror_y"});
  l.cell_columns=ProjectDocumentAccess::integer(layout.at("cell_columns"),32768);l.cell_rows=ProjectDocumentAccess::integer(layout.at("cell_rows"),32768);
  l.margin_x_mm=finder_number(layout.at("margin_x_mm"));l.margin_y_mm=finder_number(layout.at("margin_y_mm"));
  l.gap_x_mm=finder_number(layout.at("gap_x_mm"));l.gap_y_mm=finder_number(layout.at("gap_y_mm"));
  l.center=finder_boolean(layout.at("center"));l.mirror_x=finder_boolean(layout.at("mirror_x"));l.mirror_y=finder_boolean(layout.at("mirror_y"));
  result.validate();return result;
 }
 nlohmann::json describe_finder(const ExposureFinderRecipe &recipe) {
  const auto &p=recipe.table_parameters;
  nlohmann::json rows=nlohmann::json::array();
  for (const auto &row:recipe.rows)
    rows.push_back({{"layer_height",row.layer_height},{"bottom_exposure",row.bottom_exposure},
      {"normal_exposure",row.normal_exposure},{"brightness",row.brightness}});
  nlohmann::json data={{"version",1},{"table_origin",int(recipe.table_origin)},{"parameters",{
    {"layer_height",p.layer_height},{"maximum_layer_height",p.maximum_layer_height},
    {"layer_height_step",p.layer_height_step},{"bottom_exposure",p.bottom_exposure},
    {"normal_exposure",p.normal_exposure},{"bottom_step",p.bottom_step},{"normal_step",p.normal_step},
    {"tests",p.tests},{"multiple_layer_heights",p.multiple_layer_heights},{"include_base",p.include_base},
    {"series",int(p.series)}}},{"rows",std::move(rows)}};
  if (recipe.schedule) {
    data["version"]=2;
    data["schedule"]=describe_finder_schedule(*recipe.schedule);
  }
  if(recipe.generation) {
    data["version"]=3;data["generation"]=describe_finder_generation(*recipe.generation);
  }
  return data;
 }
 std::shared_ptr<const ExposureFinderRecipe> restore_finder(const nlohmann::json &data) {
  const size_t version=ProjectDocumentAccess::integer(data.at("version"),3);
  if (version!=1 && version!=2 && version!=3) throw RuntimeError("Unsupported ExposureFinder recipe version");
  if (version==3) ProjectDocumentAccess::keys(data,{"version","table_origin","parameters","rows","schedule","generation"});
  else if (version==2) ProjectDocumentAccess::keys(data,{"version","table_origin","parameters","rows","schedule"});
  else ProjectDocumentAccess::keys(data,{"version","table_origin","parameters","rows"});
  ExposureFinderRecipe recipe;
  if (version>=2) recipe.schedule=restore_finder_schedule(data.at("schedule"));
  if (version==3) recipe.generation=restore_finder_generation(data.at("generation"));
  recipe.table_origin=ExposureFinderTableOrigin(ProjectDocumentAccess::integer(data.at("table_origin"),1));
  const auto &values=data.at("parameters");
  ProjectDocumentAccess::keys(values,{"layer_height","maximum_layer_height","layer_height_step",
    "bottom_exposure","normal_exposure","bottom_step","normal_step","tests",
    "multiple_layer_heights","include_base","series"});
  auto &p=recipe.table_parameters;
  p.layer_height=finder_number(values.at("layer_height"));
  p.maximum_layer_height=finder_number(values.at("maximum_layer_height"));
  p.layer_height_step=finder_number(values.at("layer_height_step"));
  p.bottom_exposure=finder_number(values.at("bottom_exposure"));
  p.normal_exposure=finder_number(values.at("normal_exposure"));
  p.bottom_step=finder_number(values.at("bottom_step"));p.normal_step=finder_number(values.at("normal_step"));
  p.tests=ProjectDocumentAccess::integer(values.at("tests"),255);
  p.multiple_layer_heights=finder_boolean(values.at("multiple_layer_heights"));
  p.include_base=finder_boolean(values.at("include_base"));
  p.series=ExposureFinderSeries(ProjectDocumentAccess::integer(values.at("series"),1));
  const auto &rows=data.at("rows");
  if (!rows.is_array() || rows.empty() || rows.size()>100000)
    throw RuntimeError("Invalid ExposureFinder recipe row count");
  recipe.rows.reserve(rows.size());
  for (const auto &row:rows) {
    ProjectDocumentAccess::keys(row,{"layer_height","bottom_exposure","normal_exposure","brightness"});
    recipe.rows.push_back({finder_number(row.at("layer_height")),finder_number(row.at("bottom_exposure")),
      finder_number(row.at("normal_exposure")),uint8_t(ProjectDocumentAccess::integer(row.at("brightness"),255))});
  }
  return std::make_shared<const ExposureFinderRecipe>(std::move(recipe));
 }
 nlohmann::json describe_recipe(const CalibrationRecipe &recipe) {
  recipe.validate(); const auto &p=recipe.parameters;
  nlohmann::json data={{"version",1},{"pattern",int(p.pattern)},{"size_x",p.size_x},{"size_y",p.size_y},
    {"height",p.height},{"margin",p.margin},{"grayscale_steps",p.grayscale_steps},
    {"grayscale_first",p.grayscale_first},{"grayscale_last",p.grayscale_last},
    {"precision",int(recipe.precision)},{"requested",recipe.requested},{"effective",recipe.effective}};
  if (recipe.exposure_finder) {
    data["version"]=3;
    data["exposure_finder"]=describe_finder(*recipe.exposure_finder);
    return data;
  }
  // Keep existing recipes byte-compatible in schema 1. A reader that knows
  // only schema 1 must reject new taper provenance rather than silently lose it.
  if (p.lift_taper_step || p.lift_taper_min != 10) {
    data["version"]=2;
    data["lift_taper_step"]=p.lift_taper_step;
    data["lift_taper_min"]=p.lift_taper_min;
  }
  return data;
 }
 std::shared_ptr<const CalibrationRecipe> restore_recipe(const nlohmann::json &data) {
  if (!data.at("version").is_number_integer() || (data.at("version")!=1 && data.at("version")!=2 && data.at("version")!=3))
    throw RuntimeError("Unsupported calibration recipe version");
  const bool taper_schema=data.at("version")==2;
  const bool finder_schema=data.at("version")==3;
  if (finder_schema)
    ProjectDocumentAccess::keys(data,{"version","pattern","size_x","size_y","height","margin",
      "grayscale_steps","grayscale_first","grayscale_last","precision","requested","effective","exposure_finder"});
  else if (taper_schema)
    ProjectDocumentAccess::keys(data,{"version","pattern","size_x","size_y","height","margin",
      "grayscale_steps","grayscale_first","grayscale_last","precision","requested","effective",
      "lift_taper_step","lift_taper_min"});
  else
    ProjectDocumentAccess::keys(data,{"version","pattern","size_x","size_y","height","margin",
      "grayscale_steps","grayscale_first","grayscale_last","precision","requested","effective"});
  CalibrationRecipe recipe;auto &p=recipe.parameters;
  p.pattern=CalibrationPattern(ProjectDocumentAccess::integer(data.at("pattern"),finder_schema?3:2));
  p.size_x=ProjectDocumentAccess::number(data.at("size_x"));p.size_y=ProjectDocumentAccess::number(data.at("size_y"));
  p.height=ProjectDocumentAccess::number(data.at("height"));p.margin=ProjectDocumentAccess::number(data.at("margin"));
  p.grayscale_steps=ProjectDocumentAccess::integer(data.at("grayscale_steps"),256);
  p.grayscale_first=ProjectDocumentAccess::integer(data.at("grayscale_first"),255);
  p.grayscale_last=ProjectDocumentAccess::integer(data.at("grayscale_last"),255);
  if (taper_schema) {
    p.lift_taper_step=ProjectDocumentAccess::integer(data.at("lift_taper_step"),99);
    p.lift_taper_min=ProjectDocumentAccess::integer(data.at("lift_taper_min"),99);
  }
  recipe.precision=LayerMaskPrecision(ProjectDocumentAccess::integer(data.at("precision"),18));
  auto levels=[&](const char *key) {
    const auto &values=data.at(key);
    if (!values.is_array() || values.size()>256) throw RuntimeError("Invalid calibration recipe levels");
    std::vector<uint8_t> result;
    for (const auto &value:values) result.push_back(ProjectDocumentAccess::integer(value,255));
    return result;
  };
  recipe.requested=levels("requested");recipe.effective=levels("effective");
  if (finder_schema) recipe.exposure_finder=restore_finder(data.at("exposure_finder"));
  recipe.validate();
  return std::make_shared<const CalibrationRecipe>(std::move(recipe));
 }
constexpr uint64_t document_budget = uint64_t(1) << 30;
constexpr const char *manifest_name = "Metadata/SLA/layers.json";
constexpr const char *layer_prefix = "Metadata/SLA/layers/";
std::string document_name(size_t index) {
  std::ostringstream stream;
  stream << "Metadata/SLA/document/" << std::setw(8) << std::setfill('0') << index << ".bin";
  return stream.str();
}
std::string layer_name(size_t index) {
  std::ostringstream stream;
  stream << layer_prefix << std::setw(8) << std::setfill('0') << index
         << ".png";
  return stream.str();
}
class Fingerprint {
  boost::uuids::detail::sha1 hash;

public:
  void integer(uint64_t value) {
    std::array<unsigned char, 8> bytes{};
    for (unsigned i = 0; i < 8; ++i)
      bytes[i] = static_cast<unsigned char>(value >> (8 * i));
    hash.process_bytes(bytes.data(), bytes.size());
  }
  void text(const std::string &value) {
    integer(value.size());
    hash.process_bytes(value.data(), value.size());
  }
  static int64_t coordinate(double value) {
    if (!std::isfinite(value) || std::abs(value) > 1e7)
      throw RuntimeError("Invalid project geometry coordinate");
    // 3MF saves decimal mesh coordinates. Quantization avoids differences
    // caused solely by decimal parsing and vertex indexing on reload.
    return int64_t(std::llround(value * 100000.));
  }
  void point(const Vec3d &point) {
    for (int axis = 0; axis < 3; ++axis)
      integer(uint64_t(coordinate(point[axis])));
  }
  std::string finish() {
    boost::uuids::detail::sha1::digest_type digest;
    hash.get_digest(digest);
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto word : digest)
      stream << std::setw(8) << word;
    return stream.str();
  }
};
void geometry_config(Fingerprint &hash, const ConfigBase &config) {
  SLAPrintObjectConfig normalized;
  normalized.apply(config, true);
  auto keys = normalized.keys();
  std::sort(keys.begin(), keys.end());
  for (const auto &key : keys) {
    hash.text(key);
    hash.text(normalized.opt_serialize(key));
  }
}
const char *origin_name(ProjectLayerOrigin origin) {
  switch (origin) {
  case ProjectLayerOrigin::ModelSlice: return "model_slice";
  case ProjectLayerOrigin::ImportedArchive: return "imported_archive";
  case ProjectLayerOrigin::GeneratedRaster: return "generated_raster";
  }
  throw RuntimeError("Unknown project layer origin");
}
ProjectLayerOrigin parse_origin(const nlohmann::json &metadata) {
  if (!metadata.contains("source_kind")) {
    if (metadata.at("version") >= 3) throw RuntimeError("Fresh project requires explicit origin");
    return ProjectLayerOrigin::ModelSlice;
  }
  const auto name = metadata.at("source_kind").get<std::string>();
  if (name == "model_slice") return ProjectLayerOrigin::ModelSlice;
  if (metadata.at("version") >= 2) {
    if (name == "imported_archive") return ProjectLayerOrigin::ImportedArchive;
    if (name == "generated_raster") return ProjectLayerOrigin::GeneratedRaster;
  }
  throw RuntimeError("Unsupported project layer origin");
}
void validate_metadata(const ProjectRasterState &state, size_t count, const FreshLayerJob *declared_fresh = nullptr) {
  if (!count || count > 100000)
    throw RuntimeError("Invalid saved project layer count");
  // Also rejects invalid enum values supplied by API callers.
  origin_name(state.origin);
  const auto *fresh = state.document.fresh_job() ? &*state.document.fresh_job() : declared_fresh;
  if (fresh) {
    fresh->validate(count);
    if (state.origin == ProjectLayerOrigin::ImportedArchive ||
        (fresh->producer == FreshLayerProducer::ModelSlice) != (state.origin == ProjectLayerOrigin::ModelSlice))
      throw RuntimeError("Fresh producer conflicts with project origin");
    if (state.positions_z.size() != count) throw RuntimeError("Fresh project is missing actual layer positions");
    for (size_t i = 0; i < count; ++i)
      if (state.positions_z[i] != fresh->rows[i].at(LayerParameter::PositionZ))
        throw RuntimeError("Fresh project positions differ from owned parameter rows");
  }
  if (state.origin == ProjectLayerOrigin::ImportedArchive) {
    if (!state.source_fingerprint.empty() || !state.positions_z.empty())
      throw RuntimeError("Imported layers cannot carry a model fingerprint or invented baseline heights");
    return;
  }
  if (state.positions_z.size() != count)
    throw RuntimeError("Invalid saved project layer count");
  if (state.origin == ProjectLayerOrigin::ModelSlice) {
    if (state.source_fingerprint.size() != 40 ||
        !std::all_of(state.source_fingerprint.begin(), state.source_fingerprint.end(),
                     [](unsigned char c) {
                       return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                     }))
      throw RuntimeError("Invalid project raster fingerprint");
  } else if (!state.source_fingerprint.empty())
    throw RuntimeError("Generated layers cannot carry a model fingerprint");
  ExposurePositionValidator positions(fresh ? fresh->position_policy : ExposurePositionPolicy::IncreasingLayers);
  for (const auto z : state.positions_z) positions.append(z);
}
void validate_settings(const ProjectRasterState &state, const ProjectSLASettings &settings) {
  const auto config=restore_project_settings(settings);
  LayerExportProfile physical;
  physical.columns=config.option("display_pixels_x")->getInt();physical.rows=config.option("display_pixels_y")->getInt();
  physical.screen_width_mm=config.option("display_width")->getFloat();physical.screen_height_mm=config.option("display_height")->getFloat();
  if (config.option("display_orientation")->getInt()==sladoPortrait) {
    std::swap(physical.columns,physical.rows);std::swap(physical.screen_width_mm,physical.screen_height_mm);
  }
  physical.density_g_ml=config.option("material_density")->getFloat();
  physical.cost_per_ml=config.option("bottle_cost")->getFloat()/config.option("bottle_volume")->getFloat();
  physical.max_print_height_mm=config.option("max_print_height")->getFloat();
  ProjectDocumentAccess::validate_profile(state,physical);
}
void validate_state(const ProjectRasterState &state) {
  validate_metadata(state, state.document.layer_count());
  ProjectDocumentAccess::validate_profile(state);
  if (state.settings) validate_settings(state,*state.settings);
  if (state.calibration_recipe) {
    state.calibration_recipe->validate();
    if (state.origin!=ProjectLayerOrigin::GeneratedRaster || !state.settings)
      throw RuntimeError("Calibration recipe requires a generated project with settings");
    const auto &fresh=state.document.fresh_job();
    if (const auto &precision=state.document.mask_contract_precision();
        precision && *precision!=state.calibration_recipe->precision)
      throw RuntimeError("Calibration recipe differs from its immutable source mask contract");
    if ((fresh && (fresh->producer!=FreshLayerProducer::Calibration || fresh->mask_precision!=state.calibration_recipe->precision)) ||
        // Historical generation provenance is independent of a subsequently
        // selected export target. Four-bit ZIP and one-plane Chitubox v1
        // producers have proven pixel contracts without owned parameters. Keep all
        // other nonfresh precision domains rejected; never invent a FreshJob.
        (!fresh && state.calibration_recipe->precision!=LayerMaskPrecision::Gray8 &&
         state.calibration_recipe->precision!=LayerMaskPrecision::NativeGray4 &&
         state.calibration_recipe->precision!=LayerMaskPrecision::NativeBitplanes1))
      throw RuntimeError("Calibration recipe does not match its source document");
  }
}
void capture_actual_positions(ProjectRasterState &state, const SLAPrint &print) {
  if (const auto &fresh = state.document.fresh_job()) {
    fresh->validate(state.document.layer_count());
    // Engine coordinates are a geometry grid, not the authoritative DOUBLE
    // positions of an owned document. Both project origins preserve its Z.
    for (const auto &row : fresh->rows)
      state.positions_z.push_back(row.at(LayerParameter::PositionZ));
  } else {
    for (const auto &layer : print.print_layers())
      state.positions_z.push_back(double(layer.level()) * SCALING_FACTOR);
  }
}
void validate_png(const png::ReadBuf &bytes, size_t columns, size_t rows) {
  if (bytes.sz < 33 || !png::is_png(bytes))
    throw RuntimeError("Invalid saved project PNG");
  const auto *data = static_cast<const uint8_t *>(bytes.buf);
  auto u32 = [&](size_t offset) {
    return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) |
           (uint32_t(data[offset + 2]) << 8) | data[offset + 3];
  };
  if (u32(16) != columns || u32(20) != rows || data[24] != 8 || data[25] != 0)
    throw RuntimeError(
        "Project layer PNG does not match its grayscale screen geometry");
  png::ImageGreyscale image;
  if (!png::decode_png(bytes, image) || image.cols != columns ||
      image.rows != rows)
    throw RuntimeError("Cannot decode saved project layer PNG");
}
} // namespace

std::string raster_source_fingerprint(const Model &model,
                                      const ConfigBase &configuration) {
  Fingerprint hash;
  hash.text("SLA-project-raster-source-v1");
  geometry_config(hash, configuration);
  SLAFullPrintConfig full;
  full.apply(configuration, true);
  for (const char *key : {"initial_layer_height",   "material_correction",
                          "material_correction_x",  "material_correction_y",
                          "material_correction_z",  "material_print_speed",
                          "relative_correction",    "relative_correction_x",
                          "relative_correction_y",  "relative_correction_z",
                          "absolute_correction",    "elefant_foot_compensation",
                          "elefant_foot_min_width", "zcorrection_layers",
                          "display_width",          "display_height",
                          "display_pixels_x",       "display_pixels_y",
                          "display_mirror_x",       "display_mirror_y",
                          "display_orientation",    "gamma_correction",
                          "sla_output_precision"}) {
    if (full.has(key)) {
      hash.text(key);
      hash.text(full.opt_serialize(key));
    }
  }
  // Material overrides change support geometry and XY compensation even
  // when the process preset itself stays unchanged.
  auto full_keys = full.keys();
  std::sort(full_keys.begin(), full_keys.end());
  for (const auto &key : full_keys)
    if (key.rfind("material_ow_", 0) == 0) {
      hash.text(key);
      hash.text(full.opt_serialize(key));
    }
  hash.integer(model.objects.size());
  for (const auto *object : model.objects) {
    hash.integer(object->printable);
    geometry_config(hash, object->config.get());
    hash.integer(object->instances.size());
    for (const auto *instance : object->instances) {
      hash.integer(instance->printable);
      const auto transform = instance->get_matrix();
      hash.integer(object->sla_support_points.size());
      for (const auto &support : object->sla_support_points) {
        hash.point(transform * support.pos.cast<double>());
        hash.integer(
            uint64_t(Fingerprint::coordinate(support.head_front_radius)));
        hash.integer(support.is_island());
      }
      hash.integer(object->sla_drain_holes.size());
      for (const auto &hole : object->sla_drain_holes) {
        hash.point(transform * hole.pos.cast<double>());
        hash.point(transform.linear() * hole.normal.cast<double>());
        hash.integer(uint64_t(Fingerprint::coordinate(hole.radius)));
        hash.integer(uint64_t(Fingerprint::coordinate(hole.height)));
      }
      hash.integer(object->volumes.size());
      for (const auto *volume : object->volumes) {
        hash.integer(unsigned(volume->type()));
        geometry_config(hash, volume->config.get());
        const auto &mesh = volume->mesh().its;
        // Bound temporary sorting storage independently of PNG storage.
        if (mesh.indices.size() > 3500000)
          throw RuntimeError(
              "Project fingerprint exceeds the 256 MiB geometry budget");
        std::vector<std::array<int64_t, 9>> faces;
        faces.reserve(mesh.indices.size());
        const auto combined = transform * volume->get_matrix();
        for (const auto &face : mesh.indices) {
          std::array<std::array<int64_t, 3>, 3> points;
          for (int vertex = 0; vertex < 3; ++vertex) {
            const Vec3d point =
                combined *
                mesh.vertices.at(size_t(face[vertex])).cast<double>();
            for (int axis = 0; axis < 3; ++axis)
              points[vertex][axis] = Fingerprint::coordinate(point[axis]);
          }
          // Cyclic canonicalization preserves winding while ignoring
          // vertex/triangle indexing changes from 3MF serialization.
          const size_t first = size_t(
              std::min_element(points.begin(), points.end()) - points.begin());
          std::array<int64_t, 9> values;
          for (size_t vertex = 0; vertex < 3; ++vertex)
            for (size_t axis = 0; axis < 3; ++axis)
              values[vertex * 3 + axis] = points[(first + vertex) % 3][axis];
          faces.push_back(values);
        }
        std::sort(faces.begin(), faces.end());
        hash.integer(faces.size());
        for (const auto &face : faces)
          for (const auto value : face)
            hash.integer(uint64_t(value));
      }
    }
  }
  return hash.finish();
}

ProjectRasterState capture_project_layers(const Model &model,
                                          const ConfigBase &config,
                                          const SLAPrint &print) {
  if (print.is_raster_job() || !print.finished() || !print.layer_document())
    throw RuntimeError("A project raster cache requires a completed model slice");
  const auto source=raster_source_fingerprint(model,config);
  if (source!=raster_source_fingerprint(print.model(),print.full_print_config()))
    throw RuntimeError("Slice the current model and settings before saving its layer edits");
  // Persist the current masks/revision; editor history remains with its owner.
  ProjectRasterState state{print.layer_document()->snapshot(), {}, source};
  capture_actual_positions(state, print);
  state.settings=capture_project_settings(config);
  validate_state(state);
  return state;
}

ProjectRasterState capture_generated_project(const SLAPrint &print, const LayerDocument &document) {
  if (!print.is_raster_job() || !print.finished())
    throw RuntimeError("A generated project requires a completed raster job");
  ProjectRasterState state{document, {}, "", {}, ProjectLayerOrigin::GeneratedRaster,
                           capture_project_settings(print.full_print_config())};
  state.calibration_recipe=print.calibration_recipe();
  capture_actual_positions(state, print);
  validate_state(state);
  return state;
}
std::unique_ptr<SLAPrint> restore_generated_project(const ProjectRasterState &state) {
  if (state.origin != ProjectLayerOrigin::GeneratedRaster || !state.settings)
    throw RuntimeError("A generated layer project requires its saved SLA settings");
  validate_state(state);
  // The job factory also validates export capabilities. In particular it must
  // continue rejecting unsupported parameter overrides and secondary masks.
  auto job=SLAPrint::create_raster_job(restore_project_settings(*state.settings),
      state.document, state.positions_z, std::nullopt,
      state.document.fresh_job() ? state.document.fresh_job()->position_policy : ExposurePositionPolicy::IncreasingLayers);
  if (state.calibration_recipe) job->set_calibration_recipe(state.calibration_recipe);
  return job;
}

bool refresh_model_project_settings(ProjectRasterState &state, const Model &model, const ConfigBase &config) {
  if (state.origin!=ProjectLayerOrigin::ModelSlice ||
      raster_source_fingerprint(model,config)!=state.source_fingerprint) return false;
  validate_state(state);
  auto settings=capture_project_settings(config);
  // Validate before committing, without copying the potentially large mask
  // graph. Non-geometric metadata changes retain the exact edited pixels/Z.
  validate_settings(state,settings);
  if (const auto &fresh = state.document.fresh_job(); fresh &&
      fresh->parameter_origin == FreshLayerParameterOrigin::AutomaticModel) {
    SLAPrinterConfig printer;
    SLAMaterialConfig material;
    SLAPrintObjectConfig process;
    printer.apply(config, true);
    material.apply(config, true);
    process.apply(config, true);
    auto schedule = make_fresh_layer_job(printer, material,
        process.layer_height.value, state.positions_z,
        FreshLayerProducer::ModelSlice, fresh->position_policy);
    schedule.parameter_origin = FreshLayerParameterOrigin::AutomaticModel;
    // Validate and rebase automatic metadata before publishing the settings.
    // Explicit per-layer edits keep their own schedule; PNGs and Z never change.
    state.document.refresh_automatic_model_parameters(std::move(schedule));
  }
  state.settings=std::move(settings);
  return true;
}

void write_project_layers(mz_zip_archive &archive,
                          const ProjectRasterState &state, const LayerProgress &progress) {
  validate_state(state);
  const auto image = state.document.image(0);
  if (!image.cols || !image.rows || image.cols > 32768 || image.rows > 32768 ||
      image.cols * image.rows > 128 * 1024 * 1024)
    throw RuntimeError("Invalid saved project screen geometry");
  nlohmann::json manifest{{"version", state.document.fresh_job() ? (state.document.fresh_job()->mask_precision!=LayerMaskPrecision::Gray8?5:state.document.fresh_job()->parameter_origin==FreshLayerParameterOrigin::AutomaticModel?4:3) : 2},
                          {"document", ProjectDocumentAccess::describe(state.document)},
                          {"source_kind", origin_name(state.origin)},
                          {"source_fingerprint", state.source_fingerprint},
                          {"columns", image.cols},
                          {"rows", image.rows},
                          {"positions_z", state.positions_z},
                          {"layers", state.document.layer_count()}};
  if (state.calibration_recipe) {
    manifest["version"]=6;manifest["calibration_recipe"]=describe_recipe(*state.calibration_recipe);
  }
  if (state.document.fresh_job() && state.document.fresh_job()->position_policy == ExposurePositionPolicy::RepeatedExposureHeights)
    manifest["version"]=7;
  if (state.document.mask_contract_precision()) manifest["version"]=8;
  if(state.export_profile) {
    const auto &profile=*state.export_profile;
    manifest["export_profile"]={{"columns",profile.columns},{"rows",profile.rows},
      {"screen_width_mm",profile.screen_width_mm},{"screen_height_mm",profile.screen_height_mm},
      {"density_g_ml",profile.density_g_ml},{"cost_per_ml",profile.cost_per_ml},
      {"max_print_height_mm",profile.max_print_height_mm}};
  }
  if (state.settings) {
    const auto &settings=*state.settings;
    const bool has_metadata=settings.printer.metadata || settings.material.metadata || settings.process.metadata;
    auto role=[has_metadata](const ProjectPresetSettings &preset) {
      nlohmann::json value{{"name",preset.name},{"values",preset.values}};
      if(has_metadata) value["metadata"]=preset.metadata ? nlohmann::json(*preset.metadata) : nlohmann::json(nullptr);
      return value;
    };
    manifest["settings"]={{"version",has_metadata ? 2 : 1},{"printer",role(settings.printer)},
      {"material",role(settings.material)},{"process",role(settings.process)}};
  }
  const size_t total_steps = ProjectDocumentAccess::size(state.document) +
                             state.document.layer_count() * state.document.mask_plane_count();
  size_t done = 0;
  check_layer_progress(progress, done, total_steps);
  const auto metadata = manifest.dump();
  if (metadata.size()>32*1024*1024) throw RuntimeError("SLA project manifest exceeds the 32 MiB budget");
  if (!mz_zip_writer_add_mem(&archive, manifest_name, metadata.data(),
                             metadata.size(), MZ_BEST_SPEED))
    throw FileIOError("Cannot write project layer manifest");
  uint64_t total = 0;
  for(size_t i=0;i<state.document.layer_count();++i)
    for(size_t plane=0;plane<state.document.mask_plane_count();++plane) {
      validate_png(state.document.encoded_layer(i,plane?LayerMaskPlane::Secondary:LayerMaskPlane::Main),image.cols,image.rows);
      check_layer_progress(progress, ++done, total_steps);
    }
  for (size_t i=0;i<ProjectDocumentAccess::size(state.document);++i) {
    const auto bytes=ProjectDocumentAccess::entry(state.document,i);
    if(bytes.sz>document_budget-total)throw RuntimeError("Saved project document exceeds the 1 GiB budget");
    total+=bytes.sz;
    if(!mz_zip_writer_add_mem(&archive,document_name(i).c_str(),bytes.buf,bytes.sz,MZ_BEST_SPEED))
      throw FileIOError("Cannot write project document entry");
    check_layer_progress(progress, ++done, total_steps);
  }
}

void save_layer_project(const std::string &path, const ProjectRasterState &state,
                        const LayerProgress &progress) {
  validate_state(state);
  check_layer_progress(progress);
  const boost::filesystem::path output(path);
  const auto temporary = output.parent_path() / boost::filesystem::unique_path(
      "." + output.filename().string() + ".%%%%-%%%%.tmp");
  struct Writer {
    mz_zip_archive archive{};
    ~Writer() { if (archive.m_pState) close_zip_writer(&archive); }
  } writer;
  try {
    if (!open_zip_writer(&writer.archive, temporary.string()))
      throw FileIOError("Cannot create layer project staging file");
    write_project_layers(writer.archive, state, progress);
    if (!mz_zip_writer_finalize_archive(&writer.archive) || !close_zip_writer(&writer.archive))
      throw FileIOError("Cannot finalize layer project");
    check_layer_progress(progress, 1, 1);
    if (const auto error = rename_file(temporary.string(), output.string()); error)
      throw FileIOError("Cannot publish layer project: " + error.message());
  } catch (...) {
    // Close before cleanup (also required on platforms that lock open files).
    if (writer.archive.m_pState) close_zip_writer(&writer.archive);
    boost::system::error_code ignored;
    boost::filesystem::remove(temporary, ignored);
    throw;
  }
}

std::optional<ProjectRasterState> read_project_layers(const std::string &path, const LayerProgress &progress) {
  check_layer_progress(progress);
  struct Reader : MZ_Archive {
    explicit Reader(const std::string &path) {
      if (!open_zip_reader(&arch, path))
        throw FileIOError(get_errorstr());
    }
    ~Reader() { close_zip_reader(&arch); }
  } zip(path);
  std::map<std::string, mz_uint> entries;
  for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip.arch); ++i) {
    check_layer_progress(progress, i, mz_zip_reader_get_num_files(&zip.arch));
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip.arch, i, &stat))
      throw FileIOError(zip.get_errorstr());
    const auto name =
        boost::algorithm::to_lower_copy(std::string(stat.m_filename));
    if (name.rfind("metadata/sla/", 0) == 0 && !entries.emplace(name, i).second)
      throw RuntimeError("Duplicate SLA project entry");
  }
  const auto found =
      entries.find(boost::algorithm::to_lower_copy(std::string(manifest_name)));
  if (found == entries.end())
    return std::nullopt;
  auto extract = [&](mz_uint index, uint64_t limit) {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip.arch, index, &stat) ||
        stat.m_uncomp_size > limit)
      throw RuntimeError("SLA project entry exceeds its memory budget");
    std::vector<uint8_t> bytes(size_t(stat.m_uncomp_size));
    if (!mz_zip_reader_extract_to_mem(&zip.arch, index, bytes.data(),
                                      bytes.size(), 0))
      throw FileIOError(zip.get_errorstr());
    return bytes;
  };
  // A DOM alone collapses duplicate keys. Reject ambiguous input during
  // parsing, before version dispatch or restoration can discard intentions.
  std::vector<std::set<std::string>> object_keys;
  const auto unique_keys = [&](int, nlohmann::json::parse_event_t event,
                               nlohmann::json &value) {
    using Event=nlohmann::json::parse_event_t;
    if (event==Event::object_start) object_keys.emplace_back();
    else if (event==Event::key) {
      if (object_keys.empty() || !object_keys.back().insert(value.get<std::string>()).second)
        throw RuntimeError("Duplicate SLA project metadata key");
    } else if (event==Event::object_end) object_keys.pop_back();
    return true;
  };
  const auto metadata = nlohmann::json::parse(
      extract(found->second, 32 * 1024 * 1024), unique_keys);
  if (!metadata.at("version").is_number_integer() ||
      (metadata.at("version") != 1 && metadata.at("version") != 2 && metadata.at("version") != 3 && metadata.at("version") != 4 && metadata.at("version") != 5 && metadata.at("version") != 6 && metadata.at("version") != 7 && metadata.at("version") != 8))
    throw RuntimeError("Unsupported SLA project layer version");
  if ((metadata.at("version")==6 && !metadata.contains("calibration_recipe")) ||
      (metadata.contains("calibration_recipe") && metadata.at("version")!=6 && metadata.at("version")!=7 && metadata.at("version")!=8))
    throw RuntimeError("Calibration recipe requires project manifest version 6");
  if (metadata.at("version")==8) {
    if (!metadata.contains("document") || !metadata.at("document").is_object() ||
        !metadata.at("document").contains("mask_contract") || metadata.at("document").contains("fresh_job"))
      throw RuntimeError("Project version 8 requires independent source masks without fresh shadow intentions");
  } else if (metadata.contains("document") && metadata.at("document").is_object() &&
             metadata.at("document").contains("mask_contract"))
    throw RuntimeError("Source mask contract requires project manifest version 8");
  if (metadata.at("version")<3 && metadata.contains("document") &&
      metadata.at("document").is_object() && metadata.at("document").contains("fresh_job"))
    throw RuntimeError("Fresh SLA project intentions require manifest version 3");
  auto integer = [&](const char *key, int64_t maximum) {
    const auto &value = metadata.at(key);
    if (!value.is_number_integer() || value.get<int64_t>() <= 0 ||
        value.get<int64_t>() > maximum)
      throw RuntimeError("Invalid SLA project layer geometry/count");
    return size_t(value.get<int64_t>());
  };
  const size_t columns = integer("columns", 32768),
               rows = integer("rows", 32768), count = integer("layers", 100000);
  if (columns * rows > 128 * 1024 * 1024)
    throw RuntimeError("Saved project screen exceeds the pixel budget");
  ProjectRasterState state;
  if (metadata.contains("calibration_recipe"))state.calibration_recipe=restore_recipe(metadata.at("calibration_recipe"));
  state.origin = parse_origin(metadata);
  state.source_fingerprint =
      metadata.at("source_fingerprint").get<std::string>();
  state.positions_z = metadata.at("positions_z").get<std::vector<double>>();
  if(metadata.contains("export_profile") && !metadata.at("export_profile").is_null()) {
    const auto &data=metadata.at("export_profile");LayerExportProfile profile;
    auto dimension=[&](const char *key) {
      const auto &value=data.at(key);
      if(!value.is_number_integer() || value.get<int64_t>()<=0 || value.get<int64_t>()>32768)
        throw RuntimeError("Invalid project export profile resolution");
      return value.get<size_t>();
    };
    profile.columns=dimension("columns");profile.rows=dimension("rows");
    profile.screen_width_mm=data.at("screen_width_mm").get<double>();
    profile.screen_height_mm=data.at("screen_height_mm").get<double>();
    profile.density_g_ml=data.at("density_g_ml").get<double>();profile.cost_per_ml=data.at("cost_per_ml").get<double>();
    profile.max_print_height_mm=data.value("max_print_height_mm",0.);
    profile.validate(columns,rows);
    state.export_profile=profile;
  }

  if (metadata.contains("settings") && !metadata.at("settings").is_null()) {
    const auto &data=metadata.at("settings");
    if (!data.is_object() || data.size()!=4) throw RuntimeError("Invalid project SLA settings members");
    if (!data.at("version").is_number_integer() || (data.at("version")!=1 && data.at("version")!=2))
      throw RuntimeError("Unsupported project SLA settings version");
    const bool has_metadata=data.at("version")==2;
    auto role=[&](const char *name) {
      const auto &value=data.at(name);
      if (!value.is_object() || value.size()!=(has_metadata ? 3 : 2)) throw RuntimeError("Invalid project SLA preset members");
      ProjectPresetSettings result{value.at("name").get<std::string>(),
          value.at("values").get<std::map<std::string,std::string>>()};
      if(has_metadata && !value.at("metadata").is_null())
          result.metadata=value.at("metadata").get<std::map<std::string,std::string>>();
      return result;
    };
    state.settings=ProjectSLASettings{role("printer"),role("material"),role("process")};
    restore_project_settings(*state.settings);
  }

  // Validate metadata before allocating and decoding all masks.
  {
    // Validate declared intentions before PNG allocation, including the mode
    // required to interpret equal Z. Release this temporary table before
    // restoring the full document graph.
    std::optional<FreshLayerJob> declared;
    if (metadata.at("version") >= 3 && metadata.contains("document") &&
        metadata.at("document").contains("fresh_job"))
      declared = ProjectDocumentAccess::restore_fresh(metadata.at("document").at("fresh_job"), metadata.at("version").get<int>());
    validate_metadata(state, count, declared ? &*declared : nullptr);
  }
  if(metadata.at("version")>=2) {
    const auto &descriptor=metadata.at("document");const auto &names=descriptor.at("entries");
    if(!names.is_array() || names.empty() || names.size()>500000)
      throw RuntimeError("Invalid persisted document entry count");
    std::vector<std::vector<uint8_t>> payloads;payloads.reserve(names.size());
    std::set<std::string> expected{boost::algorithm::to_lower_copy(std::string(manifest_name))};
    uint64_t total=0;
    for(size_t i=0;i<names.size();++i) {
      const auto name=boost::algorithm::to_lower_copy(document_name(i));expected.insert(name);
      const auto found=entries.find(name);
      if(found==entries.end())throw RuntimeError("Missing persisted document entry");
      check_layer_progress(progress, i, names.size());
      auto bytes=extract(found->second,document_budget-total);total+=bytes.size();payloads.push_back(std::move(bytes));
    }
    for(const auto &entry:entries)
      if(!expected.count(entry.first))throw RuntimeError("Unexpected persisted document entry");
    state.document=ProjectDocumentAccess::restore(descriptor,std::move(payloads),metadata.at("version").get<int>());
    if(state.document.layer_count()!=count)throw RuntimeError("Persisted layer count mismatch");
    for(size_t i=0;i<count;++i)
      for(size_t plane=0;plane<state.document.mask_plane_count();++plane) {
        check_layer_progress(progress, i, count);
        validate_png(state.document.encoded_layer(i,plane?LayerMaskPlane::Secondary:LayerMaskPlane::Main),columns,rows);
      }
    validate_state(state);check_layer_progress(progress, count, count);return state;
  }
  std::vector<EncodedRaster> layers;
  layers.reserve(count);
  uint64_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    check_layer_progress(progress, i, count);
    const auto entry =
        entries.find(boost::algorithm::to_lower_copy(layer_name(i)));
    if (entry == entries.end())
      throw RuntimeError("Missing saved project layer PNG");
    auto bytes = extract(entry->second, document_budget - total);
    total += bytes.size();
    validate_png(png::ReadBuf{bytes.data(), bytes.size()}, columns, rows);
    layers.emplace_back(std::move(bytes), "png");
  }
  // Reject orphan/extra masks as well as gaps in the manifest's sequence.
  for (const auto &entry : entries)
    if (entry.first.rfind("metadata/sla/layers/", 0) == 0) {
      if (entry.first.size() != std::string(layer_prefix).size() + 12)
        throw RuntimeError("Invalid saved project layer name");
      const auto number =
          entry.first.substr(std::string(layer_prefix).size(), 8);
      if (!std::all_of(number.begin(), number.end(),
                       [](unsigned char c) { return c >= '0' && c <= '9'; }) ||
          entry.first.substr(entry.first.size() - 4) != ".png" ||
          std::stoul(number) >= count)
        throw RuntimeError("Unexpected saved project layer entry");
    }
  state.document = LayerDocument::from_rasters(std::move(layers));
  state.document.mark_restored_edit();
  validate_state(state);
  check_layer_progress(progress, count, count);
  return state;
}

bool project_layers_match(const SLAPrint &print, const Model &model,
                            const ConfigBase &config,
                            const ProjectRasterState &state) {
  validate_state(state);
  if (state.origin != ProjectLayerOrigin::ModelSlice) return false;
  if (print.is_raster_job() || !print.finished() || !print.layer_document() ||
      raster_source_fingerprint(print.model(),print.full_print_config())!=state.source_fingerprint ||
      state.document.layer_count() != print.print_layers().size() ||
      raster_source_fingerprint(model, config) != state.source_fingerprint)
    return false;
  for (size_t i = 0; i < state.positions_z.size(); ++i)
    if (std::abs(state.positions_z[i] -
                 double(print.print_layers()[i].level()) * SCALING_FACTOR) >
        SCALING_FACTOR)
      return false;
  return true;
}
bool restore_project_layers(SLAPrint &print, const Model &model,
                            const ConfigBase &config, const ProjectRasterState &state) {
  if (!project_layers_match(print, model, config, state)) return false;
  print.replace_layer_document(state.document);
  return true;
}
} // namespace Slic3r::sla
