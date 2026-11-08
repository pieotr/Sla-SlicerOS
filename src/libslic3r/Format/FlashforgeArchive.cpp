///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "FlashforgeArchive.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
#include "SVGXFile.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"

namespace Slic3r {

std::unique_ptr<sla::RasterBase> FlashforgeSVGXArchive::create_raster() const
{
    return sla::create_printer_raster(m_cfg);
}

sla::RasterEncoder FlashforgeSVGXArchive::get_encoder() const {
    return svgx::encode_contours;
}

void FlashforgeSVGXArchive::validate_configuration(const SLAPrint &print)
{
    const auto &cfg=print.printer_config();const auto &material=print.material_config();
    if(cfg.display_pixels_x.value<=0||cfg.display_pixels_y.value<=0||
       uint64_t(cfg.display_pixels_x.value)*cfg.display_pixels_y.value>(uint64_t(128)<<20))
        throw ExportError("Invalid SVGX screen dimensions");
    svgx::pixel_coordinates(uint32_t(cfg.display_pixels_x.value),float(cfg.display_width.value));
    svgx::pixel_coordinates(uint32_t(cfg.display_pixels_y.value),float(cfg.display_height.value));
    svgx::pixel_size_microns(float(cfg.display_width.value),uint32_t(cfg.display_pixels_x.value));
    svgx::pixel_size_microns(float(cfg.display_height.value),uint32_t(cfg.display_pixels_y.value));
    if(!std::isfinite(float(cfg.max_print_height.value))||cfg.max_print_height.value<=0)
        throw ExportError("Invalid SVGX machine height");
    if(cfg.display_mirror_x.value||!cfg.display_mirror_y.value)
        throw ExportError("SVGX uses a fixed vertical display mirror");
    const double height=print.default_object_config().layer_height.value;
    if(!std::isfinite(height)||height<.001||height>cfg.max_print_height.value||!std::isfinite(material.initial_layer_height.value)||
       std::abs(height-material.initial_layer_height.value)>1e-7)
        throw ExportError("SVGX requires the same initial and normal layer height");
    if(material.sla_transition_layers.value!=0)
        throw ExportError("SVGX transition exposure is not supported by the reference decoder");
    if(material.sla_bottom_layers.value<0||material.sla_bottom_layers.value>UINT16_MAX)
        throw ExportError("SVGX bottom layer count exceeds uint16");
    if(std::abs(std::round(height*1000.)/1000.-height)>1e-7)
        throw ExportError("SVGX layer height requires at most three decimal places");
    for(double value:{material.exposure_time.value,material.initial_exposure_time.value}) {
        if(!std::isfinite(float(value))||value<.01||std::abs(std::round(value*100.)/100.-value)>1e-7)
            throw ExportError("SVGX exposure requires positive values with at most two decimal places");
    }
    sla::require_single_stage_motion(print,"SVGX",false);
    for(bool bottom:{false,true}) {
        if(sla::motion_parameter(print,"wait_before_cure",bottom)!=0)
            throw ExportError("SVGX cannot store a wait before cure");
        // The container has no motor fields. These are UVtools decoder
        // defaults, not a claim that every printer firmware uses those moves.
        for(const char *key:{"lift_height","lift_speed","retract_speed"}) {
            const float expected=std::string(key)=="lift_height"?6.f:100.f;
            if(sla::motion_parameter(print,key,bottom)!=expected)
                throw ExportError(std::string("SVGX cannot store custom ")+(bottom?"bottom ":"normal ")+key);
        }
    }
    svgx::xml_attribute(cfg.printer_model.value);
}

void FlashforgeSVGXArchive::export_print(const std::string fname, const SLAPrint &print,
                                          const ThumbnailsList &thumbnails,
                                          const std::string    &projectname)
{
    validate_configuration(print);
    if(m_layers.empty()||m_layers.size()!=print.print_layers().size())
        throw ExportError("SVGX requires a complete nonempty layer stack");
    const double height=print.default_object_config().layer_height.value;
    for(size_t i=0;i<m_layers.size();++i)
        if(std::abs(double(print.print_layers()[i].level())*SCALING_FACTOR-(i+1)*height)>1e-5)
            throw ExportError("SVGX cannot store variable layer Z positions");
    const bool portrait=m_cfg.display_orientation.value==sladoPortrait;
    svgx::FileParameters p;
    p.width=portrait?m_cfg.display_pixels_y.value:m_cfg.display_pixels_x.value;
    p.height=portrait?m_cfg.display_pixels_x.value:m_cfg.display_pixels_y.value;
    p.display_width=float(portrait?m_cfg.display_height.value:m_cfg.display_width.value);
    p.display_height=float(portrait?m_cfg.display_width.value:m_cfg.display_height.value);
    p.machine_z=float(m_cfg.max_print_height.value);p.layer_height=float(height);
    p.bottom_layers=uint16_t(sla::bottom_layers(print,m_layers.size()));
    p.bottom_exposure=float(print.material_config().initial_exposure_time.value);
    p.exposure=float(print.material_config().exposure_time.value);
    if(!m_cfg.printer_model.value.empty())p.machine=m_cfg.printer_model.value;
    for(const char *key:{"sla_material_settings_id","material_type"})
        if(const auto *name=print.full_print_config().option<ConfigOptionString>(key))
            if(!name->value.empty()){p.material=name->value;break;}
    svgx::write_file(fname,p,m_layers,thumbnails);
}

}
