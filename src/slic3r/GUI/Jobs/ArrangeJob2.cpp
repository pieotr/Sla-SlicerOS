///|/ Copyright (c) Prusa Research 2023 Tomáš Mészáros @tamasmeszaros
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ArrangeJob2.hpp"

#include <numeric>
#include <iterator>

#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMeshSlicer.hpp>
#include <libslic3r/Geometry/ConvexHull.hpp>
#include <libslic3r/MultipleBeds.hpp>

#include <libslic3r/SLAPrint.hpp>

#include <slic3r/GUI/Plater.hpp>
#include <slic3r/GUI/GLCanvas3D.hpp>
#include <slic3r/GUI/GUI_App.hpp>

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

class GUISelectionMask: public arr2::SelectionMask {
    const Selection *m_sel;

public:
    explicit GUISelectionMask(const Selection *sel) : m_sel{sel} {}

    std::vector<bool> selected_objects() const override
    {
        auto selmap = m_sel->get_object_idxs();

        std::vector<bool> ret(m_sel->get_model()->objects.size(), false);

        for (auto sel : selmap) {
            ret[sel] = true;
        }

        return ret;
    }

    std::vector<bool> selected_instances(int obj_id) const override
    {
        auto objcnt = static_cast<int>(m_sel->get_model()->objects.size());
        auto icnt   = obj_id < objcnt ?
                          m_sel->get_model()->objects[obj_id]->instances.size() :
                          0;

        std::vector<bool> ret(icnt, false);

        auto selmap = m_sel->get_content();
        auto objit = selmap.find(obj_id);

        if (objit != selmap.end() && obj_id < objcnt) {
            ret = std::vector<bool>(icnt, false);
            for (auto sel : objit->second) {
                ret[sel] = true;
            }
        }

        return ret;
    }
};

arr2::SceneBuilder build_scene(Plater &plater, ArrangeSelectionMode mode)
{
    arr2::SceneBuilder builder;

    const int current_bed{s_multiple_beds.get_active_bed()};
    const std::map<ObjectID, int> &beds_map{s_multiple_beds.get_inst_map()};
    if (mode == ArrangeSelectionMode::SelectionOnly) {
        auto gui_selection = std::make_unique<GUISelectionMask>(&plater.get_selection());

        std::set<ObjectID> considered_instances;
        for (std::size_t object_index{0}; object_index < plater.model().objects.size(); ++object_index) {
            const ModelObject *object{plater.model().objects[object_index]};
            for (std::size_t instance_index{0}; instance_index < object->instances.size(); ++instance_index) {
                const ModelInstance *instance{object->instances[instance_index]};

                const bool is_selected{gui_selection->selected_instances(object_index)[instance_index]};
                const auto instance_bed_index{beds_map.find(instance->id())};

                if (
                    is_selected
                    || instance_bed_index != beds_map.end()
                ) {
                    considered_instances.insert(instance->id());
                }
            }
        }
        builder.set_selection(std::move(gui_selection));
        builder.set_considered_instances(std::move(considered_instances));
    } else if (mode == ArrangeSelectionMode::CurrentBedSelectionOnly) {
        auto gui_selection{std::make_unique<GUISelectionMask>(&plater.get_selection())};

        std::set<ObjectID> considered_instances;
        arr2::BedConstraints constraints;
        for (std::size_t object_index{0}; object_index < plater.model().objects.size(); ++object_index) {
            const ModelObject *object{plater.model().objects[object_index]};
            for (std::size_t instance_index{0}; instance_index < object->instances.size(); ++instance_index) {
                const ModelInstance *instance{object->instances[instance_index]};

                const bool is_selected{gui_selection->selected_instances(object_index)[instance_index]};

                const auto instance_bed_index{beds_map.find(instance->id())};
                if (
                    is_selected
                    || (
                        instance_bed_index != beds_map.end()
                        && instance_bed_index->second == current_bed
                    )
                ) {
                    constraints.insert({instance->id(), current_bed});
                    considered_instances.insert(instance->id());
                }
            }
        }

        builder.set_selection(std::move(gui_selection));
        builder.set_bed_constraints(std::move(constraints));
        builder.set_considered_instances(std::move(considered_instances));
    } else if (mode == ArrangeSelectionMode::CurrentBedFull) {
        std::set<ObjectID> instances_on_bed;
        arr2::BedConstraints constraints;
        for (const auto &instance_bed : beds_map) {
            if (instance_bed.second == current_bed) {
                instances_on_bed.insert(instance_bed.first);
                constraints.insert(instance_bed);
            }
        }
        builder.set_bed_constraints(std::move(constraints));
        builder.set_considered_instances(std::move(instances_on_bed));
    }

    builder.set_arrange_settings(plater.canvas3D()->get_arrange_settings_view());

    if (plater.config()) {
        const Vec2crd gap{s_multiple_beds.get_bed_gap()};
        builder.set_bed(*plater.config(), gap);
    }

    builder.set_model(plater.model());

    if(plater.printer_technology() != ptSLA)
        throw std::logic_error("SLA arrangement requires an SLA session");
    builder.set_sla_print(&plater.active_sla_print());

    return builder;
}

FillBedJob2::FillBedJob2(arr2::Scene &&scene, const Callbacks &cbs) : Base(std::move(scene), _u8L("Filling bed"), cbs) {}

ArrangeJob2::ArrangeJob2(arr2::Scene &&scene, const Callbacks &cbs) : Base(std::move(scene), _u8L("Arranging"), cbs) {}

}} // namespace Slic3r
