// AGPL-3.0-or-later. Based on the local UVtools 6.0.1 AnycubicZipFile.cs
// manifest/scene definitions and AnycubicFile.EncodePW0. No external process.
#include "AnycubicZipArchive.hpp"
#include "libslic3r/SLA/PrintParameters.hpp"
#include "libslic3r/SLA/RasterRounding.hpp"
#include <cmath>
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/miniz_extension.hpp"
#include <nlohmann/json.hpp>
#include <cstring>
#include <limits>

namespace Slic3r {
namespace {
using Json = nlohmann::json;
void u32(std::vector<uint8_t> &out, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(uint8_t(value >> shift));
}
void f32(std::vector<uint8_t> &out, float value) {
    uint32_t bits; std::memcpy(&bits, &value, 4); u32(out, bits);
}
void text(std::vector<uint8_t> &out, const std::string &value, size_t length) {
    const size_t start = out.size(); out.resize(start + length);
    std::copy_n(value.begin(), std::min(length, value.size()), out.begin() + start);
}
// PW0 stores four-bit gray. Black/white runs use two bytes (max 4095),
// intermediate grays one byte (max 15); pixels are row-major with no header.
std::vector<uint8_t> pw0(const png::ImageGreyscale &image) {
    std::vector<uint8_t> out;
    for (size_t first = 0; first < image.buf.size();) {
        const uint8_t color = image.buf[first] >> 4;
        size_t end = first + 1;
        while (end < image.buf.size() && (image.buf[end] >> 4) == color) ++end;
        for (size_t left = end - first; left;) {
            const auto count = uint16_t(std::min(left, size_t(color == 0 || color == 15 ? 4095 : 15)));
            if (color == 0 || color == 15) {
                out.push_back(uint8_t((color << 4) | (count >> 8)));
                out.push_back(uint8_t(count));
            } else out.push_back(uint8_t((color << 4) | count));
            left -= count;
        }
        first = end;
    }
    return out;
}
// The file's public reader derives time from native float32 parameters, not
// from the unrounded process preset or a generic integer LCD estimate. Keep
// this projection local: setup policy and JSON aliases are format contracts.
float zip_time_number(const Json &value, const char *field) {
    const float native=value.get<float>();
    if(!std::isfinite(native) || native<0)
        throw ExportError(std::string("Anycubic ZIP time requires a finite nonnegative ")+field);
    return native;
}
float zip_round2(float value, bool away=false) {
    if(!std::isfinite(value))throw ExportError("Anycubic ZIP native print time exceeds the finite float range");
    // MathF.Round leaves large floats unchanged. Multiplication must remain
    // float32: promoting3.135f to double changes the two-decimal boundary.
    if(std::abs(value)>=1e8f)return value;
    const float scaled=value*100.f;
    return (away?std::round(scaled):std::nearbyint(scaled))/100.f;
}
float zip_core_speed(const Json &native_speed) {
    const float speed=zip_time_number(native_speed,"speed")*60.f;
    return zip_round2(speed,true); // native mm/s -> public mm/min, Away2
}
struct AnycubicZipParameterPlan {
    Json layers=Json::array();
    std::vector<double> positions;
    float print_time=0;
};
AnycubicZipParameterPlan plan_zip_parameters(const SLAPrint &print,size_t count,const Json &resin) {
    const sla::ScopedNearestRounding rounding;
    AnycubicZipParameterPlan plan;plan.positions.reserve(count);
    const size_t bottom_count=sla::bottom_layers(print,count);
    double previous_z=0;
    float complete_time=300.f; // FileFormat.ExtraPrintTime, local reference policy
    const float wait=zip_time_number(resin.at("slicepara").at("off_time"),"common pre-cure wait");
    for(size_t i=0;i<count;++i) {
        const bool bottom=i<bottom_count;
        const auto &first=resin.at("slice_extpara").at("multi_state_paras").at(bottom?"bott_0":"normal_0");
        const auto &second=resin.at("slice_extpara").at("multi_state_paras").at(bottom?"bott_1":"normal_1");
        const double z=sla::layer_z(print,i),thickness=z-previous_z;
        if(!std::isfinite(z) || !std::isfinite(thickness) || thickness<=0)
            throw ExportError("Anycubic ZIP layer Z positions must increase finitely");
        Json row={{"exposure_time",sla::layer_exposure(print,i)},{"layer_index",i},
            {"layer_minheight",previous_z},{"layer_thickness",thickness},
            {"zup_height",first.at("height")},{"zup_speed",first.at("up_speed")}};
        // Decode applies Away2 to these explicit row fields before assigning
        // Layer properties. Inherited second lift/wait values retain native
        // float precision; they do not pass through those Layer setters.
        const float exposure=zip_round2(zip_time_number(row.at("exposure_time"),"exposure"),true);
        if(exposure<=0)throw ExportError("Anycubic ZIP positive exposure collapses in native precision");
        const float lift=zip_round2(zip_time_number(row.at("zup_height"),"first lift"),true);
        const float lift2=zip_time_number(second.at("height"),"second lift");
        const float speed=zip_core_speed(row.at("zup_speed"));
        const float speed2=zip_core_speed(second.at("up_speed"));
        const float retract=zip_core_speed(first.at("down_speed"));
        // The independent secondary retract speed is stored but this schema
        // has no secondary retract height. Validate its native number without
        // inventing a return distance paired with the second lift.
        (void)zip_core_speed(second.at("down_speed"));
        float motor=0;
        const auto phase=[&](float distance,float velocity) {
            if(!std::isfinite(distance) || distance<0)
                throw ExportError("Anycubic ZIP native lift/retract distance exceeds the finite float range");
            if(distance==0)return; // zero speed is valid for an inactive phase
            if(velocity<=0)throw ExportError("Anycubic ZIP active motion speed rounds to zero in native precision");
            const float seconds=distance/(velocity/60.f);
            motor+=seconds;
            if(!std::isfinite(seconds) || !std::isfinite(motor))
                throw ExportError("Anycubic ZIP native motion time exceeds the finite float range");
        };
        phase(lift,speed);phase(lift2,speed2);phase(lift+lift2,retract);
        motor=zip_round2(motor);
        // Non-GCode Layer.CalculatePrintTime uses max(motor,LightOff).
        // Anycubic ZIP's LightOffDelay getter aliases the common pre-cure wait.
        const float rests=wait+exposure;
        const float seconds=rests+std::max(motor,wait);
        complete_time+=seconds;
        if(!std::isfinite(rests) || !std::isfinite(seconds) || !std::isfinite(complete_time))
            throw ExportError("Anycubic ZIP native print time exceeds the finite float range");
        plan.layers.push_back(std::move(row));plan.positions.push_back(z);previous_z=z;
    }
    plan.print_time=zip_round2(complete_time);
    return plan;
}
struct MaskStats {
    size_t x0, y0, x1 = 0, y1 = 0, pixels = 0, largest = 0;
    uint32_t objects = 0;
};
// Measure the exported, quantized mask rather than the pre-edit mesh.
// Area is occupied pixel area, not OpenCV's pixel-center contour approximation.
MaskStats measure(const png::ImageGreyscale &image) {
    MaskStats stats{image.cols, image.rows};
    // Connected components by row runs: only two rows and active component
    // sizes are retained, avoiding a screen-sized label image/flood queue.
    struct Run { size_t first, last, label; };
    struct Component { size_t parent, pixels; };
    std::vector<Run> previous;
    std::vector<Component> components;
    auto root = [&](size_t label) {
        while (components[label].parent != label) {
            components[label].parent = components[components[label].parent].parent;
            label = components[label].parent;
        }
        return label;
    };
    for (size_t y = 0; y < image.rows; ++y) {
        std::vector<Run> current;
        size_t prior = 0;
        for (size_t x = 0; x < image.cols;) {
            if (!(image.buf[y*image.cols+x] >> 4)) { ++x; continue; }
            const auto first = x++;
            while (x < image.cols && (image.buf[y*image.cols+x] >> 4)) ++x;
            const auto label = components.size();
            components.push_back({label, x-first});
            current.push_back({first, x, label});
            stats.x0 = std::min(stats.x0, first); stats.y0 = std::min(stats.y0, y);
            stats.x1 = std::max(stats.x1, x); stats.y1 = std::max(stats.y1, y+1);
            stats.pixels += x-first;
            // Inclusive one-pixel overlap includes diagonal (8-connected)
            // neighbors. Runs use an exclusive right endpoint.
            while (prior < previous.size() && previous[prior].last < first) ++prior;
            for (size_t p = prior; p < previous.size() && previous[p].first <= x; ++p) {
                const auto a = root(label), b = root(previous[p].label);
                if (a != b) {
                    components[b].parent = a;
                    components[a].pixels += components[b].pixels;
                }
            }
        }
        const auto none = std::numeric_limits<size_t>::max();
        std::vector<size_t> mapping(components.size(), none);
        std::vector<Component> active;
        for (auto &run : current) {
            const auto parent = root(run.label);
            if (mapping[parent] == none) {
                mapping[parent] = active.size();
                active.push_back({active.size(), components[parent].pixels});
            }
            run.label = mapping[parent];
        }
        for (size_t i = 0; i < components.size(); ++i)
            if (components[i].parent == i && mapping[i] == none) {
                ++stats.objects;
                stats.largest = std::max(stats.largest, components[i].pixels);
            }
        components = std::move(active); previous = std::move(current);
    }
    for (const auto &component : components) {
        ++stats.objects; stats.largest = std::max(stats.largest, component.pixels);
    }
    if (!stats.pixels) stats.x0 = stats.y0 = stats.x1 = stats.y1 = 0;
    return stats;
}
void add_preview(Zipper &zip, unsigned index, unsigned width, unsigned height,
                 const png::ImageGreyscale &mask, const ThumbnailData *rendered)
{
    const size_t source_width = rendered ? rendered->width : mask.cols;
    const size_t source_height = rendered ? rendered->height : mask.rows;
    const double scale = std::min(double(width)/source_width, double(height)/source_height);
    const auto draw_width = std::max(1u, unsigned(source_width*scale));
    const auto draw_height = std::max(1u, unsigned(source_height*scale));
    const unsigned left = (width-draw_width)/2, top = (height-draw_height)/2;
    std::vector<uint8_t> rgb(size_t(width)*height*3);
    for (unsigned y = 0; y < draw_height; ++y)
        for (unsigned x = 0; x < draw_width; ++x) {
            const auto sx = size_t(x)*source_width/draw_width, sy = size_t(y)*source_height/draw_height;
            const auto target = (size_t(y+top)*width+x+left)*3;
            for (unsigned channel = 0; channel < 3; ++channel)
                rgb[target+channel] = rendered ? rendered->pixels[((source_height-1-sy)*source_width+sx)*4+channel] : mask.get(sy, sx);
        }
    size_t size = 0;
    auto free_png = [](void *data) { mz_free(data); };
    std::unique_ptr<void, decltype(free_png)> png(tdefl_write_image_to_png_file_in_memory_ex(
        rgb.data(), int(width), int(height), 3, &size, MZ_DEFAULT_LEVEL, 0), free_png);
    if (!png) throw ExportError("Cannot encode Anycubic ZIP preview");
    zip.add_entry("preview_images/preview_" + std::to_string(index) + ".png", static_cast<const uint8_t *>(png.get()), size);
}
}

void AnycubicZipArchive::export_print(const std::string filename, const SLAPrint &print,
                                     const ThumbnailsList &thumbnails, const std::string &)
{
    if (m_layers.empty() || m_layers.size() > 65535)
        throw ExportError("Anycubic ZIP requires between 1 and 65535 layers");
    // UVtools exposes one common pre-cure wait and no PWM/after-cure wait
    // or independently configurable second retract height for this family.
    for (const bool bottom : {false, true}) {
        for (const char *key : {"wait_after_cure", "wait_after_lift", "retract_height2"})
            if (sla::motion_parameter(print, key, bottom) != 0)
                throw ExportError(std::string("Anycubic ZIP cannot store ") + key);
        if (sla::motion_parameter(print, "light_pwm", bottom) != 255)
            throw ExportError("Anycubic ZIP cannot store a custom UV PWM");
    }
    if (sla::motion_parameter(print, "wait_before_cure", true) != sla::motion_parameter(print, "wait_before_cure", false))
        throw ExportError("Anycubic ZIP requires the same bottom and normal pre-cure wait");
    const auto &material = print.material_config();
    const bool portrait = m_cfg.display_orientation.value == sladoPortrait;
    const size_t cols = portrait ? m_cfg.display_pixels_y.value : m_cfg.display_pixels_x.value;
    const size_t rows = portrait ? m_cfg.display_pixels_x.value : m_cfg.display_pixels_y.value;
    const double width = portrait ? m_cfg.display_height.value : m_cfg.display_width.value;
    const double height = portrait ? m_cfg.display_width.value : m_cfg.display_height.value;
    const double pitch_x = width / cols, pitch_y = height / rows, pixel_area = pitch_x * pitch_y;
    auto param = [&](const char *key, bool bottom = false) { return sla::motion_parameter(print, key, bottom); };
    const bool two_stage = param("lift_height2") > 0 || param("lift_height2", true) > 0;
    Json stages = Json::object();
    for (const bool bottom : {false, true})
        for (const bool second : {false, true}) {
            const std::string name = std::string(bottom ? "bott_" : "normal_") + (second ? "1" : "0");
            stages[name] = {{"height", param(second ? "lift_height2" : "lift_height", bottom)},
                {"up_speed", param(second ? "lift_speed2" : "lift_speed", bottom) / 60.f},
                {"down_speed", param(second ? "retract_speed2" : "retract_speed", bottom) / 60.f}};
        }
    const std::string machine = m_cfg.printer_model.value.empty() ? "Custom SLA" : m_cfg.printer_model.value;
    const auto bottom_count = sla::bottom_layers(print, m_layers.size());
    Json resin = {{"version", "2"}, {"property", {{"version", "3"}, {"name", "default_resin"},
        {"density", material.material_density.value}, {"price", material.bottle_cost.value},
        {"volume", material.bottle_volume.value}}},
        {"slice_extpara", {{"version", "3"}, {"multi_state_used", two_stage ? 1 : 0},
            {"transition_layercount", std::min(size_t(std::max(0, material.sla_transition_layers.value)), m_layers.size()-bottom_count)},
            {"transition_type", 0}, {"multi_state_paras", stages}, {"intelli_mode", 0}}},
        {"slicepara", {{"anti_count", 1}, {"bott_layers", bottom_count},
            {"bott_time", material.initial_exposure_time.value}, {"exposure_time", material.exposure_time.value},
            {"off_time", param("wait_before_cure")}, {"use_indivi_layerpara", 1},
            {"zthick", print.default_object_config().layer_height.value},
            {"zup_height", param("lift_height") + param("lift_height2")},
            {"zup_speed", param("lift_speed") / 60.f}, {"zdown_speed", param("retract_speed") / 60.f}}}};
    // Complete native row/time validation before creating the ZIP or previews.
    const auto parameter_plan=plan_zip_parameters(print,m_layers.size(),resin);
    Json settings = {{"version", "3"}, {"machine_type", {{"version", "3"}, {"name", machine},
        {"key_suffix", m_extension}, {"key_image_format", "pw0Img"}, {"res_x", cols}, {"res_y", rows},
        {"xy_pixel", pitch_x * 1000}, {"xy_pixel_y", pitch_y * 1000},
        {"print_xsize", width}, {"print_ysize", height}, {"print_zsize", m_cfg.max_print_height.value},
        {"max_file_version", 518}, {"raster_antialiasing", 16}, {"max_samples", 16}, {"property", 119},
        {"prev_back_color", {0., .28, .39}}, {"prev_model_color", {.8, .8, .8}},
        {"prev_supports_color", {.07, .93, .93}}, {"prev_image_size", {224, 168}},
        {"prev2_back_color", {.08, .11, .16}}, {"prev2_image_size", {336, 252}},
        {"cloudprev_back_color", {0., .28, .39}}, {"cloudprev_imag_size", {800, 600}},
        {"raster_segments_capacity", 0},
        {"child_screen", Json::array({{{"x", 0}, {"y", 0}, {"width", cols}, {"height", rows}}})}}},
        {"machine_extern", {{"version", "3"}, {"alias", machine}, {"cloud_property", 0},
            {"factory_resins", Json::array()}, {"user_resins", Json::array({resin})},
            {"active_resins", Json::array({"default_resin"})}, {"firmware_calc_print_time", 0}}}};
    Zipper zip(filename, Zipper::FAST_COMPRESSION);
    auto add_json = [&](const char *name, const Json &json) { zip.add_entry(name); zip << json.dump(2); };
    add_json("anycubic_photon_resins.pwsp", settings);
    add_json("software_info.conf", {{"mark", "SLA-Slicer"}, {"Version", "1.0"}, {"opengl", "3.3-CoreProfile"}, {"os", "native"}});
    std::vector<uint8_t> definitions;
    MaskStats total{cols, rows};
    double volume_mm3 = 0, previous_z = 0;
    for (size_t i = 0; i < m_layers.size(); ++i) {
        png::ImageGreyscale image;
        if (!png::decode_png(png::ReadBuf{m_layers[i].data(), m_layers[i].size()}, image) || image.cols != cols || image.rows != rows)
            throw ExportError("Anycubic ZIP layer dimensions do not match the printer screen");
        if (i == 0) {
            const auto rendered = std::find_if(thumbnails.begin(), thumbnails.end(), [](const auto &thumbnail) { return thumbnail.is_valid(); });
            const auto *preview = rendered == thumbnails.end() ? nullptr : &*rendered;
            add_preview(zip, 0, 224, 168, image, preview);
            add_preview(zip, 1, 336, 252, image, preview);
        }
        const double z=parameter_plan.positions[i];
        const double thickness=parameter_plan.layers.at(i).at("layer_thickness").get<double>();
        const auto encoded = pw0(image);
        zip.add_entry("layer_images/layer_" + std::to_string(i) + ".pw0Img", encoded.data(), encoded.size());
        const auto stats = measure(image);
        if (stats.pixels) {
            total.x0 = std::min(total.x0, stats.x0); total.y0 = std::min(total.y0, stats.y0);
            total.x1 = std::max(total.x1, stats.x1); total.y1 = std::max(total.y1, stats.y1);
            total.pixels += stats.pixels;
        }
        volume_mm3 += stats.pixels * pixel_area * thickness;
        f32(definitions, float(z)); f32(definitions, float(stats.pixels * pixel_area));
        f32(definitions, stats.pixels ? float(stats.x0 * pitch_x - width / 2) : 0.f);
        f32(definitions, stats.pixels ? float(stats.y0 * pitch_y - height / 2) : 0.f);
        f32(definitions, stats.pixels ? float(stats.x1 * pitch_x - width / 2) : 0.f);
        f32(definitions, stats.pixels ? float(stats.y1 * pitch_y - height / 2) : 0.f);
        u32(definitions, stats.objects); f32(definitions, float(stats.largest * pixel_area));
        definitions.resize(definitions.size() + 32); // Eight reserved uint32s.
        previous_z = z;
    }
    add_json("layers_controller.conf", {{"count", m_layers.size()}, {"paras", parameter_plan.layers}});
    const double milliliters = volume_mm3 / 1000.;
    add_json("print_info.json", {{"cost", material.bottle_volume.value > 0 ? milliliters * material.bottle_cost.value / material.bottle_volume.value : 0.},
        {"currency", "EUR"}, {"print_time", parameter_plan.print_time}, {"volume", milliliters}});
    std::vector<uint8_t> scene;
    text(scene, "ANYCUBIC-PWSZ", 16); text(scene, "SLA-Slicer 1.0", 64);
    u32(scene, 3); u32(scene, 1); u32(scene, 0); u32(scene, 0); f32(scene, 1.f); u32(scene, uint32_t(m_layers.size()));
    f32(scene, total.pixels ? float(total.x0 * pitch_x - width / 2) : 0.f);
    f32(scene, total.pixels ? float(total.y0 * pitch_y - height / 2) : 0.f); f32(scene, 0.f);
    f32(scene, total.pixels ? float(total.x1 * pitch_x - width / 2) : 0.f);
    f32(scene, total.pixels ? float(total.y1 * pitch_y - height / 2) : 0.f); f32(scene, float(previous_z));
    u32(scene, 0); scene.resize(scene.size() + 256);
    text(scene, "<---", 4); u32(scene, uint32_t(m_layers.size()));
    scene.insert(scene.end(), definitions.begin(), definitions.end()); text(scene, "--->", 4);
    zip.add_entry("scene.slice", scene.data(), scene.size());
    zip.finalize();
}
}
