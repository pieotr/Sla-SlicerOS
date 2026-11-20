///|/ Copyright (c) Prusa Research 2023 Tomáš Mészáros @tamasmeszaros
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef ARRANGESETTINGSDB_APPCFG_HPP
#define ARRANGESETTINGSDB_APPCFG_HPP

#include <string>

#include "ArrangeSettingsView.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
class AppConfig;

class ArrangeSettingsDb_AppCfg: public arr2::ArrangeSettingsDb
{
public:
    enum Slots { slotSLA };

private:
    AppConfig *m_appcfg;
    Slots m_current_slot = slotSLA;

    struct FloatRange { float minval = 0.f, maxval = 100.f; };
    struct Slot
    {
        Values      vals;
        Values      defaults;
        FloatRange  dobj_range, dbed_range;
        std::string postfix;
    };

    // Settings and their defaults of the (only) SLA slot
    Slot m_settings_sla;

    template<class Self>
    static auto & get_slot(Self *self, Slots slot) {
        (void)slot;
        return self->m_settings_sla;
    }

    template<class Self> static auto &get_slot(Self *self)
    {
        return get_slot(self, self->m_current_slot);
    }

    template<class Self>
    static auto& get_ref(Self *self) { return get_slot(self).vals; }

public:
    explicit ArrangeSettingsDb_AppCfg(AppConfig *appcfg);

    void sync();

    float get_distance_from_objects() const override { return get_ref(this).d_obj; }
    float get_distance_from_bed() const  override { return get_ref(this).d_bed; }
    bool  is_rotation_enabled() const override { return get_ref(this).rotations; }

    XLPivots get_xl_alignment() const override { return m_settings_sla.vals.xl_align; }
    GeometryHandling get_geometry_handling() const override { return m_settings_sla.vals.geom_handling; }
    ArrangeStrategy get_arrange_strategy() const override { return m_settings_sla.vals.arr_strategy; }

    void distance_from_obj_range(float &min, float &max) const override;
    void distance_from_bed_range(float &min, float &max) const override;

    ArrangeSettingsDb& set_distance_from_objects(float v) override;
    ArrangeSettingsDb& set_distance_from_bed(float v) override;
    ArrangeSettingsDb& set_rotation_enabled(bool v) override;

    ArrangeSettingsDb& set_xl_alignment(XLPivots v) override;
    ArrangeSettingsDb& set_geometry_handling(GeometryHandling v) override;
    ArrangeSettingsDb& set_arrange_strategy(ArrangeStrategy v) override;

    Values get_defaults() const override { return get_slot(this).defaults; }

    void set_active_slot(Slots slot) noexcept { m_current_slot = slot; }
    void set_distance_from_obj_range(Slots slot, float min, float max)
    {
        get_slot(this, slot).dobj_range = FloatRange{min, max};
    }

    void set_distance_from_bed_range(Slots slot, float min, float max)
    {
        get_slot(this, slot).dbed_range = FloatRange{min, max};
    }

    Values &get_defaults(Slots slot) { return get_slot(this, slot).defaults; }
};

} // namespace Slic3r

#endif // ARRANGESETTINGSDB_APPCFG_HPP
