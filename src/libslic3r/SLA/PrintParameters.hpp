#ifndef SLIC3R_SLA_PRINT_PARAMETERS_HPP
#define SLIC3R_SLA_PRINT_PARAMETERS_HPP
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Exception.hpp"
#include <algorithm>
#include <cmath>

namespace Slic3r::sla {
inline size_t bottom_layers(const SLAPrint &print, size_t count)
{
    return std::min(count, size_t(std::max(0, print.material_config().sla_bottom_layers.value)));
}
inline float layer_z(const SLAPrint &print, size_t index)
{
    if (index < print.print_layers().size())
        return float(double(print.print_layers()[index].level()) * SCALING_FACTOR);
    return float(print.material_config().initial_layer_height.value + index * print.default_object_config().layer_height.value);
}
inline float layer_exposure(const SLAPrint &print, size_t index)
{
    const auto &material = print.material_config();
    const auto bottom = size_t(std::max(0, material.sla_bottom_layers.value));
    const auto transition = size_t(std::max(0, material.sla_transition_layers.value));
    if (index < bottom) return float(material.initial_exposure_time.value);
    if (index - bottom < transition)
        return float(material.initial_exposure_time.value +
            (material.exposure_time.value - material.initial_exposure_time.value) *
            double(index - bottom + 1) / double(transition + 1));
    return float(material.exposure_time.value);
}
inline float motion_parameter(const SLAPrint &print, const char *suffix, bool bottom)
{
    const auto *option = print.material_config().option(std::string(bottom ? "sla_bottom_" : "sla_") + suffix);
    if (!option) throw RuntimeError(std::string("Missing LCD parameter: ") + suffix);
    return float(std::string(suffix) == "light_pwm" ? option->getInt() : option->getFloat());
}
inline double lcd_layer_time(const SLAPrint &print, size_t index)
{
    const bool bottom = index < bottom_layers(print, print.print_layers().size());
    auto param = [&](const char *key) { return double(motion_parameter(print, key, bottom)); };
    auto move_time = [](double height, double speed) {
        if (height <= 0) return 0.;
        if (speed <= 0) throw RuntimeError("An enabled LCD move requires a positive speed");
        return height * 60. / speed;
    };
    const double height = param("lift_height"), height2 = param("lift_height2"), retract2 = param("retract_height2");
    return layer_exposure(print, index) + param("wait_before_cure") + param("wait_after_cure") + param("wait_after_lift") +
        move_time(height, param("lift_speed")) + move_time(height2, param("lift_speed2")) +
        move_time(height + height2 - retract2, param("retract_speed")) + move_time(retract2, param("retract_speed2"));
}
inline uint32_t lcd_print_time(const SLAPrint &print)
{
    double seconds = 0.;
    for (size_t i = 0; i < print.print_layers().size(); ++i) seconds += lcd_layer_time(print, i);
    return uint32_t(std::min(std::ceil(seconds), double(UINT32_MAX)));
}
// Older containers have fewer fields. Reject unsupported values explicitly
// rather than exposing GUI settings that silently disappear from the file.
inline void require_single_stage_motion(const SLAPrint &print, const char *format, bool supports_pwm)
{
    for (const bool bottom : {false, true}) {
        for (const char *key : {"lift_height2", "retract_height2", "wait_after_cure", "wait_after_lift"})
            if (motion_parameter(print, key, bottom) != 0)
                throw ExportError(std::string(format) + " cannot store " + (bottom ? "bottom " : "normal ") + key + " in this encoder version");
        if (!supports_pwm && motion_parameter(print, "light_pwm", bottom) != 255)
            throw ExportError(std::string(format) + " cannot store a custom UV PWM in this encoder version");
    }
    if (motion_parameter(print, "retract_speed", true) != motion_parameter(print, "retract_speed", false))
        throw ExportError(std::string(format) + " requires the same normal and bottom retract speed");
}
}
#endif
