#ifndef SLIC3R_SLA_MACHINE_SETTINGS_HPP
#define SLIC3R_SLA_MACHINE_SETTINGS_HPP

// Shared descriptors for material presets, validation and the generic LCD UI.
// Speeds are stored in mm/min and times in seconds.
namespace Slic3r::sla {
struct MachineSetting { const char *key; const char *label; const char *unit; double initial; bool integer; bool bottom; };
inline constexpr MachineSetting machine_settings[] = {
    {"sla_bottom_layers", "Bottom layers", "", 6., true, false},
    {"sla_transition_layers", "Transition layers", "", 0., true, false},
    {"sla_lift_height", "Lift height", "mm", 6., false, false},
    {"sla_lift_speed", "Lift speed", "mm/min", 80., false, false},
    {"sla_lift_height2", "Second lift height", "mm", 0., false, false},
    {"sla_lift_speed2", "Second lift speed", "mm/min", 80., false, false},
    {"sla_retract_speed", "Retract speed", "mm/min", 150., false, false},
    {"sla_retract_height2", "Second retract height", "mm", 0., false, false},
    {"sla_retract_speed2", "Second retract speed", "mm/min", 150., false, false},
    {"sla_wait_before_cure", "Wait before exposure", "s", 0., false, false},
    {"sla_wait_after_cure", "Wait after exposure", "s", 0., false, false},
    {"sla_wait_after_lift", "Wait after lift", "s", 0., false, false},
    {"sla_light_pwm", "UV power", "", 255., true, false},
    {"sla_bottom_lift_height", "Lift height", "mm", 6., false, true},
    {"sla_bottom_lift_speed", "Lift speed", "mm/min", 60., false, true},
    {"sla_bottom_lift_height2", "Second lift height", "mm", 0., false, true},
    {"sla_bottom_lift_speed2", "Second lift speed", "mm/min", 80., false, true},
    {"sla_bottom_retract_speed", "Retract speed", "mm/min", 150., false, true},
    {"sla_bottom_retract_height2", "Second retract height", "mm", 0., false, true},
    {"sla_bottom_retract_speed2", "Second retract speed", "mm/min", 150., false, true},
    {"sla_bottom_wait_before_cure", "Wait before exposure", "s", 0., false, true},
    {"sla_bottom_wait_after_cure", "Wait after exposure", "s", 0., false, true},
    {"sla_bottom_wait_after_lift", "Wait after lift", "s", 0., false, true},
    {"sla_bottom_light_pwm", "UV power", "", 255., true, true},
};
}

#define SLA_MACHINE_OPTIONS \
    ((ConfigOptionInt, sla_bottom_layers)) \
    ((ConfigOptionInt, sla_transition_layers)) \
    ((ConfigOptionFloat, sla_lift_height)) \
    ((ConfigOptionFloat, sla_lift_speed)) \
    ((ConfigOptionFloat, sla_lift_height2)) \
    ((ConfigOptionFloat, sla_lift_speed2)) \
    ((ConfigOptionFloat, sla_retract_speed)) \
    ((ConfigOptionFloat, sla_retract_height2)) \
    ((ConfigOptionFloat, sla_retract_speed2)) \
    ((ConfigOptionFloat, sla_wait_before_cure)) \
    ((ConfigOptionFloat, sla_wait_after_cure)) \
    ((ConfigOptionFloat, sla_wait_after_lift)) \
    ((ConfigOptionInt, sla_light_pwm)) \
    ((ConfigOptionFloat, sla_bottom_lift_height)) \
    ((ConfigOptionFloat, sla_bottom_lift_speed)) \
    ((ConfigOptionFloat, sla_bottom_lift_height2)) \
    ((ConfigOptionFloat, sla_bottom_lift_speed2)) \
    ((ConfigOptionFloat, sla_bottom_retract_speed)) \
    ((ConfigOptionFloat, sla_bottom_retract_height2)) \
    ((ConfigOptionFloat, sla_bottom_retract_speed2)) \
    ((ConfigOptionFloat, sla_bottom_wait_before_cure)) \
    ((ConfigOptionFloat, sla_bottom_wait_after_cure)) \
    ((ConfigOptionFloat, sla_bottom_wait_after_lift)) \
    ((ConfigOptionInt, sla_bottom_light_pwm))

#endif

