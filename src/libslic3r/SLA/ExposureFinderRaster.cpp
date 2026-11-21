// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ExposureFinderRaster.hpp"
#include "CancellablePNGEncoder.hpp"
#include "ExposurePositionSchedule.hpp"
#include "RasterRounding.hpp"
#include <boost/multiprecision/cpp_dec_float.hpp>
#include <charconv>
#include <set>
#include <algorithm>

namespace Slic3r::sla {
namespace {
constexpr size_t part_budget = 100000, placement_budget = 1000000;
void validate_screen(const LayerRasterGeometry &g) {
    g.validate();
    if (g.columns>32768 || g.rows>32768 || uint64_t(g.columns)*g.rows>(uint64_t(128)<<20))
        throw RuntimeError("ExposureFinder LCD exceeds its raster budget");
}
size_t spacing_pixels(double mm, double span, size_t pixels) {
    if (!std::isfinite(mm) || mm<0) throw RuntimeError("ExposureFinder spacing must be finite and nonnegative");
    using Decimal = boost::multiprecision::cpp_dec_float_50;
    const auto decimal = [](double value) {
        char buffer[64];const auto text=std::to_chars(buffer,buffer+sizeof(buffer),value);
        if(text.ec!=std::errc{})throw RuntimeError("Cannot encode ExposureFinder physical spacing");
        return Decimal(std::string(buffer,text.ptr));
    };
    const Decimal count=ceil(decimal(mm)*pixels/decimal(span));
    if(count>pixels)throw RuntimeError("ExposureFinder spacing exceeds the LCD axis");
    return count.convert_to<size_t>();
}
void validate_schedule(const ExposureFinderSchedule &plan, const std::function<void()> &check) {
    if(plan.parts.empty() || plan.parts.size()>part_budget || plan.exposures.empty() || plan.exposures.size()>part_budget)
        throw RuntimeError("ExposureFinder composition requires 1-100000 parts and exposure records");
    for(const auto &part:plan.parts) {
        check();
        for(double value:{part.layer_height,part.bottom_exposure,part.normal_exposure,part.label_bottom_exposure,part.label_normal_exposure})
            if(!std::isfinite(value) || value<0)throw RuntimeError("Invalid ExposureFinder raster part");
        if(part.layer_height<=0 || !part.requested_brightness)throw RuntimeError("Invalid ExposureFinder part height or brightness");
    }
    ExposurePositionValidator positions(ExposurePositionPolicy::RepeatedExposureHeights);
    std::vector<bool> used(plan.parts.size(),false);
    size_t physical=0,bottom=0,placements=0;
    for(const auto &record:plan.exposures) {
        check();
        if(positions.append(record.position_z))++physical;
        if(record.physical_layer!=physical-1 || !std::isfinite(record.layer_height) || record.layer_height<=0 ||
            !std::isfinite(record.exposure) || record.exposure<=0 || record.parts.empty() ||
            record.parts.size()!=record.mask_brightness.size())throw RuntimeError("Invalid ExposureFinder raster exposure");
        if(record.parts.size()>placement_budget-placements)throw RuntimeError("ExposureFinder composition exceeds its placement budget");
        placements+=record.parts.size();
        if(record.bottom)++bottom;
        std::set<size_t> seen;
        for(size_t i=0;i<record.parts.size();++i) {
            check();const size_t id=record.parts[i];
            if(id>=plan.parts.size() || !seen.insert(id).second || !record.mask_brightness[i] ||
                plan.parts[id].layer_height!=record.layer_height)throw RuntimeError("Invalid ExposureFinder part assignment");
            used[id]=true;
        }
    }
    if(physical!=plan.physical_layers || bottom!=plan.bottom_records ||
        std::find(used.begin(),used.end(),false)!=used.end())
        throw RuntimeError("ExposureFinder schedule counts differ or a part is never exposed");
}
} // namespace
ExposureFinderRasterLayout plan_exposure_finder_layout(const LayerRasterGeometry &geometry,size_t count,
    const ExposureFinderLayoutParameters &parameters,const LayerProgress &progress) {
    const auto g=geometry;const auto p=parameters;ScopedNearestRounding rounding;
    check_layer_progress(progress,0,count);validate_screen(g);
    if(!count || count>part_budget || !p.cell_columns || !p.cell_rows || p.cell_columns>g.columns || p.cell_rows>g.rows)
        throw RuntimeError("ExposureFinder cell footprint or part count is invalid for the LCD");
    const size_t mx=spacing_pixels(p.margin_x_mm,g.width_mm,g.columns),my=spacing_pixels(p.margin_y_mm,g.height_mm,g.rows);
    const size_t gx=spacing_pixels(p.gap_x_mm,g.width_mm,g.columns),gy=spacing_pixels(p.gap_y_mm,g.height_mm,g.rows);
    if(2*mx>=g.columns || 2*my>=g.rows)throw RuntimeError("ExposureFinder margins leave no printable LCD area");
    const size_t columns=(g.columns-2*mx+gx)/(p.cell_columns+gx);
    const size_t rows=(g.rows-2*my+gy)/(p.cell_rows+gy);
    if(!columns || !rows || count>uint64_t(columns)*rows)
        throw RuntimeError("All ExposureFinder parts must fit the LCD; reduce tests, spacing or cell size");
    const size_t used_columns=std::min(columns,count),used_rows=(count+columns-1)/columns;
    const size_t width=used_columns*p.cell_columns+(used_columns-1)*gx;
    const size_t height=used_rows*p.cell_rows+(used_rows-1)*gy;
    const size_t first_x=p.center?(g.columns-width)/2:mx,first_y=p.center?(g.rows-height)/2:my;
    ExposureFinderRasterLayout result{g,p,{}};result.grid_columns=columns;result.grid_rows=rows;result.origins.reserve(count);
    for(size_t i=0;i<count;++i) {
        check_layer_progress(progress,i,count);
        size_t x=first_x+(i%columns)*(p.cell_columns+gx),y=first_y+(i/columns)*(p.cell_rows+gy);
        if(p.mirror_x)x=g.columns-p.cell_columns-x;
        if(p.mirror_y)y=g.rows-p.cell_rows-y;
        result.origins.push_back({x,y});
    }
    check_layer_progress(progress,count,count);return result;
}
LayerDocument compose_exposure_finder_layers(const ExposureFinderSchedule &schedule,const ExposureFinderRasterLayout &layout,
    const ExposureFinderCellRenderer &renderer,const LayerProgress &progress) {
    if(schedule.parts.size()>part_budget || schedule.exposures.size()>part_budget || layout.origins.size()>part_budget)
        throw RuntimeError("ExposureFinder composition input exceeds its budget");
    const auto plan=schedule;const auto grid=layout;const auto render=renderer;
    ScopedNearestRounding rounding;
    size_t current=0;const size_t total=plan.exposures.size();
    const auto check=[&]{check_layer_progress(progress,current,total);};check();
    if(!render)throw RuntimeError("ExposureFinder requires its native cell renderer");
    validate_schedule(plan,check);
    const auto expected=plan_exposure_finder_layout(grid.geometry,plan.parts.size(),grid.parameters,
        [&](size_t,size_t){check();return true;});
    if(expected.origins!=grid.origins)throw RuntimeError("ExposureFinder layout differs from its declared physical settings");
    const size_t columns=grid.geometry.columns,rows=grid.geometry.rows;
    const size_t cw=grid.parameters.cell_columns,ch=grid.parameters.cell_rows;
    std::vector<uint8_t> canvas(columns*rows,0);
    std::vector<EncodedRaster> masks;masks.reserve(total);
    constexpr size_t encoded_budget=size_t(1)<<30;size_t encoded_bytes=0;
    for(current=0;current<total;++current) {
        check();std::fill(canvas.begin(),canvas.end(),uint8_t(0));check();
        const auto &record=plan.exposures[current];
        for(size_t i=0;i<record.parts.size();++i) {
            check();const size_t id=record.parts[i];
            const auto cell=render(plan.parts[id],record,check);check();
            if(cell.cols!=cw || cell.rows!=ch || cell.buf.size()!=cw*ch)
                throw RuntimeError("ExposureFinder rendered cell differs from its planned footprint");
            const auto &origin=grid.origins[id];const unsigned subtract=255-record.mask_brightness[i];
            for(size_t y=0;y<ch;++y) {
                check();const size_t target_y=origin.row+(grid.parameters.mirror_y?ch-1-y:y);
                for(size_t x=0;x<cw;++x) {
                    const unsigned value=cell.buf[y*cw+x];
                    const size_t target_x=origin.column+(grid.parameters.mirror_x?cw-1-x:x);
                    // Reference subtracts (255-brightness), saturating AA edges
                    // at zero. Multiplication would produce a different dose.
                    canvas[target_y*columns+target_x]=value>subtract?uint8_t(value-subtract):uint8_t(0);
                }
            }
        }
        auto png=encode_png_raster(canvas.data(),columns,rows,1,check);
        if(!png.size() || png.size()>encoded_budget-encoded_bytes)
            throw RuntimeError("ExposureFinder encoded masks exceed the memory budget or PNG encoding failed");
        encoded_bytes+=png.size();masks.push_back(std::move(png));
    }
    // Keep completion for the final check, including owned-document validation.
    current=total-1;
    std::vector<uint8_t>().swap(canvas);
    auto result=LayerDocument::from_rasters(std::move(masks),[&](size_t,size_t){check();return true;});
    current=total;check();return result;
}
} // namespace Slic3r::sla
