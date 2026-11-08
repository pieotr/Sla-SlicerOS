// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ExposureFinderDraft.hpp"
#include "LayerJobParameters.hpp"
#include "FreshLayerStatistics.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <limits>
#include <sstream>
#include <string_view>
namespace Slic3r::sla {
namespace {
bool space(char c) { return c==' ' || c=='\t' || c=='\n' || c=='\r'; }
std::string_view trim(std::string_view value) {
    while(!value.empty() && space(value.front()))value.remove_prefix(1);
    while(!value.empty() && space(value.back()))value.remove_suffix(1);
    return value;
}
unsigned integer(std::string_view value,unsigned maximum) {
    if(value.empty() || value.front()<'0' || value.front()>'9')throw RuntimeError("Expected an unsigned decimal integer");
    unsigned result=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
    if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || result>maximum)
        throw RuntimeError("ExposureFinder integer exceeds its range or has invalid syntax");
    return result;
}
std::vector<std::string_view> tokens(const std::string &text,size_t maximum) {
    if(text.size()>65536 || text.find('\0')!=std::string::npos)throw RuntimeError("ExposureFinder list input exceeds its text budget or contains NUL");
    std::vector<std::string_view> result;size_t i=0;bool value_required=false;
    while(i<text.size()) {
        if(space(text[i])){++i;continue;}
        if(text[i]==',' || text[i]==';') {
            if(result.empty() || value_required)throw RuntimeError("ExposureFinder list contains an empty value");
            value_required=true;++i;continue;
        }
        const size_t start=i;
        while(i<text.size() && !space(text[i]) && text[i]!=',' && text[i]!=';')++i;
        if(result.size()==maximum)throw RuntimeError("ExposureFinder list exceeds its item budget");
        result.emplace_back(text.data()+start,i-start);value_required=false;
    }
    if(value_required)throw RuntimeError("ExposureFinder list ends with an empty value");
    return result;
}
double number(std::string_view value) {
    value=trim(value);double result=0.;
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result,std::chars_format::general);
    if(value.empty() || parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || !std::isfinite(result) || std::abs(result)>1000000.)
        throw RuntimeError("ExposureFinder table requires finite decimal values within +/-1000000");
    return result;
}
}
std::vector<unsigned> parse_exposure_finder_features(const std::string &text) {
    std::vector<unsigned> result;
    for(const auto value:tokens(text,500)) {
        const auto n=integer(value,500);if(!n)throw RuntimeError("ExposureFinder feature sizes require 1-500 pixels");result.push_back(n);
    }
    std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());return result;
}
std::vector<std::array<unsigned,2>> parse_exposure_finder_rings(const std::string &text) {
    std::vector<std::array<unsigned,2>> result;
    for(const auto value:tokens(text,500)) {
        const auto colon=value.find(':');
        if(colon==std::string_view::npos || value.find(':',colon+1)!=std::string_view::npos)throw RuntimeError("ExposureFinder ring requires diameter:thickness");
        const auto diameter=integer(value.substr(0,colon),500),thickness=integer(value.substr(colon+1),500);
        if(!diameter || !thickness)throw RuntimeError("ExposureFinder ring sizes require 1-500 pixels");
        result.push_back({diameter,thickness});
    }
    std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());
    for(size_t i=1;i<result.size();++i)if(result[i-1][0]==result[i][0])throw RuntimeError("ExposureFinder ring diameter has conflicting thicknesses");
    return result;
}
std::vector<uint8_t> parse_exposure_finder_brightness(const std::string &text) {
    std::vector<uint8_t> result;for(const auto value:tokens(text,256))result.push_back(uint8_t(integer(value,255)));return result;
}
std::vector<ExposureFinderTableRow> parse_exposure_finder_manual_rows(const std::string &text) {
    if(text.size()>(size_t(16)<<20) || text.find('\0')!=std::string::npos)throw RuntimeError("ExposureFinder manual CSV exceeds its text budget or contains NUL");
    std::vector<ExposureFinderTableRow> result;size_t start=0;
    while(start<text.size()) {
        const auto end=text.find('\n',start);auto line=trim(std::string_view(text).substr(start,end==std::string::npos?text.size()-start:end-start));
        start=end==std::string::npos?text.size():end+1;if(line.empty())continue;
        if(result.size()==100000)throw RuntimeError("ExposureFinder manual table exceeds 100000 rows");
        std::array<std::string_view,4> values;
        for(size_t i=0;i<4;++i) {
            const auto comma=line.find(',');
            if((i<3 && comma==std::string_view::npos) || (i==3 && comma!=std::string_view::npos))throw RuntimeError("ExposureFinder manual row requires height,bottom,normal,brightness");
            values[i]=trim(line.substr(0,comma));if(i<3)line.remove_prefix(comma+1);
        }
        result.push_back({number(values[0]),number(values[1]),number(values[2]),uint8_t(integer(values[3],255))});
    }
    return result;
}
LayerJobParameters validate_exposure_finder_configuration(const DynamicPrintConfig &configuration) {
    DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});config.apply(configuration);
    if(config.option("printer_technology")->getInt()!=ptSLA)throw RuntimeError("ExposureFinder requires an SLA printer");
    const auto *format=get_archive_entry(config.opt_string("sla_archive_format").c_str());
    if(!format || std::string(format->id)=="SL1SVG")throw RuntimeError("ExposureFinder requires a known pixel archive format");
    SLAPrint context;context.set_status_callback([](const PrintBase::SlicingStatus &){});context.apply(Model{},config);
    const auto error=context.validate();if(!error.empty())throw RuntimeError(error);
    for(const char *name:{"exposure_time","initial_exposure_time","layer_height"})
        if(!std::isfinite(config.opt_float(name)) || config.opt_float(name)<=0)throw RuntimeError(std::string("Invalid ExposureFinder preset ")+name);
    // These are the same typed neutral motion intentions used by fresh jobs.
    // Validate them through the central neutral validator, not codec-specific
    // rounding or assumptions about which exposure rows happen to move.
    LayerJobParameters globals;globals.layer_height=config.opt_float("layer_height");
    using F=LayerParameter;
    for(const bool bottom:{false,true}) {
        auto &row=bottom?globals.bottom:globals.normal;const std::string prefix=bottom?"sla_bottom_":"sla_";
        row[F::Exposure]=config.opt_float(bottom?"initial_exposure_time":"exposure_time");row[F::LightOff]=0.;
        for(const auto &[field,key]:std::map<F,std::string>{{F::LiftDistance,"lift_height"},{F::LiftDistance2,"lift_height2"},
            {F::LiftSpeed,"lift_speed"},{F::LiftSpeed2,"lift_speed2"},{F::RetractSpeed,"retract_speed"},{F::RetractSpeed2,"retract_speed2"},
            {F::RetractDistance2,"retract_height2"},{F::WaitBefore,"wait_before_cure"},{F::WaitAfter,"wait_after_cure"},{F::WaitAfterLift,"wait_after_lift"}})
            row[field]=config.opt_float(prefix+key);
        row[F::LightPWM]=config.opt_int(prefix+"light_pwm");
    }
    globals.validate(1);return globals;
}
std::vector<LayerParameters> make_exposure_finder_rows(const LayerJobParameters &source_globals,
    const ExposureFinderSchedule &source_schedule,const LayerProgress &progress) {
    const auto globals=source_globals;const auto schedule=source_schedule;const auto callback=progress;
    globals.validate(std::numeric_limits<size_t>::max());
    if(schedule.exposures.empty() || schedule.exposures.size()>100000)throw RuntimeError("ExposureFinder row plan requires 1-100000 records");
    std::vector<LayerParameters> rows;rows.reserve(schedule.exposures.size());
    double total_time=0.;
    for(const auto &exposure:schedule.exposures) {
        check_layer_progress(callback,rows.size(),schedule.exposures.size());
        auto row=exposure.bottom?globals.bottom:globals.normal;
        row[LayerParameter::Exposure]=exposure.exposure;
        if(exposure.lift_height_override) {
            row[LayerParameter::LiftDistance]=*exposure.lift_height_override;row[LayerParameter::LiftDistance2]=0.;
            row[LayerParameter::RetractDistance2]=std::min(row.at(LayerParameter::RetractDistance2),*exposure.lift_height_override);
        }
        if(exposure.wait_before_override)row[LayerParameter::WaitBefore]=*exposure.wait_before_override;
        // The neutral global validator owns all thirteen phase/finite/PWM
        // rules. PositionZ is structural and is added after this validation.
        LayerJobParameters effective;effective.layer_height=exposure.layer_height;
        effective.normal=row;effective.bottom=row;effective.validate(1);
        row[LayerParameter::PositionZ]=exposure.position_z;
        if(!std::isfinite(exposure.position_z) || exposure.position_z<=0)throw RuntimeError("ExposureFinder row requires finite positive Z");
        total_time=finite_fresh_statistic(total_time+fresh_layer_schedule_time(row),"ExposureFinder schedule time");
        rows.push_back(std::move(row));
    }
    check_layer_progress(callback,rows.size(),schedule.exposures.size());return rows;
}
ExposureFinderGenerationPlan plan_exposure_finder_generation(const DynamicPrintConfig &configuration,
    const ExposureFinderGenerationParameters &parameters,const LayerProgress &progress) {
    const auto request=parameters;const auto callback=progress;DynamicPrintConfig config;config.apply(SLAFullPrintConfig{});config.apply(configuration);
    size_t phase=0;const auto check=[&]{check_layer_progress(callback,phase,100);};check();
    ExposureFinderGenerationSettings{request.pattern,request.layout,request.bottom_erosion,request.chamfer_layers}.validate();
    if(!request.recipe.schedule)throw RuntimeError("ExposureFinder requires an explicit schedule");
    const int columns=config.opt_int("display_pixels_x"),rows=config.opt_int("display_pixels_y");
    if(columns<=0 || rows<=0 || columns>32768 || rows>32768 || uint64_t(columns)*rows>(uint64_t(128)<<20))throw RuntimeError("Invalid ExposureFinder printer resolution");
    ExposureFinderGenerationPlan result;
    result.geometry={size_t(columns),size_t(rows),config.opt_float("display_width"),config.opt_float("display_height")};
    if(config.option("display_orientation")->getInt()==sladoPortrait){std::swap(result.geometry.columns,result.geometry.rows);std::swap(result.geometry.width_mm,result.geometry.height_mm);}
    result.geometry.validate();
    phase=10;check();
    const auto schedule=plan_exposure_finder_schedule(request.recipe,*request.recipe.schedule,[&](size_t,size_t){check();return true;});
    config.set("layer_height",schedule.exposures.front().layer_height);
    config.set("sla_bottom_layers",int(schedule.bottom_records));config.set("sla_transition_layers",0);
    const auto globals=validate_exposure_finder_configuration(config);
    (void)make_exposure_finder_rows(globals,schedule,[&](size_t,size_t){check();return true;});
    result.max_z=schedule.exposures.back().position_z;
    if(!std::isfinite(config.opt_float("max_print_height")) || config.opt_float("max_print_height")<=0 || result.max_z>config.opt_float("max_print_height"))throw RuntimeError("ExposureFinder exceeds the printer build height");
    phase=35;check();const auto pattern=make_exposure_finder_pattern(result.geometry,request.pattern,check);
    auto layout=request.layout;
    if((layout.cell_columns && layout.cell_columns!=pattern.base.cols) || (layout.cell_rows && layout.cell_rows!=pattern.base.rows))throw RuntimeError("ExposureFinder layout cell differs from the generated pattern");
    layout.cell_columns=pattern.base.cols;layout.cell_rows=pattern.base.rows;
    layout.mirror_x=layout.mirror_x!=config.opt_bool("display_mirror_x");layout.mirror_y=layout.mirror_y!=config.opt_bool("display_mirror_y");
    phase=70;check();const auto grid=plan_exposure_finder_layout(result.geometry,schedule.parts.size(),layout,[&](size_t,size_t){check();return true;});
    result.requested_pattern=request.pattern;result.effective_pattern=pattern.parameters;
    result.cell_columns=pattern.base.cols;result.cell_rows=pattern.base.rows;
    result.cell_width_mm=result.cell_columns*result.geometry.width_mm/result.geometry.columns;
    result.cell_height_mm=result.cell_rows*result.geometry.height_mm/result.geometry.rows;
    result.grid_columns=grid.grid_columns;result.grid_rows=grid.grid_rows;
    result.spatial_parts=schedule.parts.size();result.exposure_records=schedule.exposures.size();result.physical_layers=schedule.physical_layers;
    result.raster_pixels=uint64_t(result.geometry.columns)*result.geometry.rows;
    result.logical_mask_bytes=result.raster_pixels*result.exposure_records;
    phase=100;check();return result;
}
std::string describe_exposure_finder_generation_plan(const ExposureFinderGenerationPlan &p) {
    std::ostringstream text;text.imbue(std::locale::classic());text<<std::setprecision(9);
    text<<"LCD: "<<p.geometry.columns<<" x "<<p.geometry.rows<<" px; "<<p.geometry.width_mm<<" x "<<p.geometry.height_mm<<" mm\n"
        <<"Cell: "<<p.cell_columns<<" x "<<p.cell_rows<<" px; "<<p.cell_width_mm<<" x "<<p.cell_height_mm<<" mm\n"
        <<"Fit: "<<p.grid_columns<<" columns x "<<p.grid_rows<<" rows; "<<p.spatial_parts<<" spatial parts\n"
        <<"Exposure records: "<<p.exposure_records<<"; physical Z heights: "<<p.physical_layers<<"; maximum Z: "<<p.max_z<<" mm\n"
        <<"Feature margin requested/effective: "<<p.requested_pattern.feature_margin_mm<<" / "<<p.effective_pattern.feature_margin_mm<<" mm\n"
        <<"Text scale requested/effective: "<<p.requested_pattern.text_scale<<" / "<<p.effective_pattern.text_scale<<"\n"
        <<"Logical raster work: "<<p.logical_mask_bytes<<" pixel-bytes (not peak memory or compressed size)\n"
        <<"Encoded-mask runtime limit: "<<p.encoded_mask_limit_bytes<<" bytes; final export compatibility is checked separately.";
    return text.str();
}
} // namespace Slic3r::sla
