// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LayerListStream.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>

#include "LayerMemoryBudget.hpp"
#include "libslic3r/Exception.hpp"

namespace Slic3r::sla::liststream {

namespace lo = layerops;
using Ctx    = LayerDocument::ListStreamContext;
using Result = LayerDocument::ListStreamResult;
using Ref    = LayerDocument::LayerRef;

namespace {

struct Range { size_t first, last; };
Range resolve_range(size_t first, size_t last_requested, size_t count)
{
    if (count == 0) throw std::invalid_argument("There are no layers.");
    const size_t last = std::min(last_requested, count - 1);
    if (first > last) throw std::invalid_argument("The layer range is empty or outside of the layers.");
    return {first, last};
}

// The validators of layerops report with std::invalid_argument; the document API reports with RuntimeError.
template<class F> Result guarded(F &&fn)
{
    try { return fn(); }
    catch (const std::invalid_argument &error) { throw RuntimeError(error.what()); }
}
std::vector<Ref> copies_of(const std::vector<size_t> &source)
{
    std::vector<Ref> refs(source.size());
    for (size_t i = 0; i < source.size(); ++i) refs[i] = {source[i], false};
    return refs;
}
// The input only as placeholders for validators that look at the number of layers.
std::vector<lo::Mask> placeholders(size_t count) { return std::vector<lo::Mask>(count); }
// Union of the model bounds of every layer, decoded in parallel.
lo::Rect model_bounds(Ctx &ctx)
{
    std::vector<lo::Rect> bounds(ctx.layer_count);
    ctx.parallel(0, ctx.layer_count - 1, 2, [&](size_t i) { bounds[i] = lo::bounding_rect(ctx.load(i)); });
    lo::Rect all;
    for (const lo::Rect &b : bounds) all = lo::union_bounds(all, b);
    return all;
}

} // namespace

Operation clone(const lo::CloneParams &p)
{
    return [p](Ctx &ctx) {
        return guarded([&] {
            Result r;
            r.z_mm = ctx.z_mm;
            r.change.list = lo::clone_plan(ctx.layer_count, r.z_mm, p, ctx.progress);
            r.refs = copies_of(r.change.list.source_index);
            return r;
        });
    };
}

Operation remove(const lo::RemoveParams &p)
{
    return [p](Ctx &ctx) {
        return guarded([&] {
            std::vector<size_t> counts(ctx.layer_count, 0);
            if (p.use_threshold && p.first_layer < ctx.layer_count)
                ctx.parallel(p.first_layer, std::min(p.last_layer, ctx.layer_count - 1), 1,
                             [&](size_t i) { counts[i] = ctx.load(i).nonzero_count(); });
            Result r;
            r.z_mm = ctx.z_mm;
            r.change.list = lo::remove_plan(ctx.layer_count, r.z_mm, p, [&](size_t i) { return counts[i]; }, ctx.progress);
            r.refs = copies_of(r.change.list.source_index);
            return r;
        });
    };
}

Operation reheight(const lo::ReHeightParams &p)
{
    return [p](Ctx &ctx) {
        return guarded([&] {
            if (const std::string err = lo::validate_reheight(p, placeholders(ctx.layer_count), ctx.z_mm); !err.empty())
                throw std::invalid_argument(err);
            Result r;
            r.z_mm = ctx.z_mm;
            r.change.list = lo::reheight_plan(ctx.layer_count, r.z_mm, p, ctx.progress);
            r.refs = copies_of(r.change.list.source_index);
            if (p.method == lo::ReHeightMethod::ReHeight && p.preset.is_multiply) {
                const unsigned modifier = p.preset.modifier;
                ctx.parallel(0, p.preset.layer_count - 1, modifier + 2, [&](size_t k) {
                    std::vector<lo::Mask> group;
                    group.reserve(modifier);
                    for (unsigned i = 0; i < modifier; ++i) group.push_back(ctx.load(k * modifier + i));
                    std::vector<const lo::Mask *> pointers;
                    for (const auto &m : group) pointers.push_back(&m);
                    r.refs[k] = ctx.emit(lo::reheight_merge(pointers, p), k * modifier);
                });
            }
            return r;
        });
    };
}

Operation double_exposure(const lo::DoubleExposureParams &p)
{
    return [p](Ctx &ctx) {
        return guarded([&] {
            Result r;
            std::vector<lo::DoubleExposurePiece> pieces;
            lo::DoubleExposureResult plan = lo::double_exposure_plan(ctx.z_mm, p, r.z_mm, pieces);
            r.change.list = std::move(plan.list);
            r.change.exposure_s = std::move(plan.exposure_s);
            r.change.lift_mm = std::move(plan.lift_mm);
            r.change.wait_before_cure_s = std::move(plan.wait_before_cure_s);
            r.change.bottom_layer_count = plan.bottom_layer_count;
            r.refs.resize(pieces.size());
            std::vector<size_t> first_at(ctx.layer_count, size_t(-1)), second_at(ctx.layer_count, size_t(-1));
            for (size_t k = 0; k < pieces.size(); ++k) {
                if (pieces[k].role == lo::DoubleExposurePiece::Role::Copy) r.refs[k] = {pieces[k].source, false};
                else (pieces[k].role == lo::DoubleExposurePiece::Role::First ? first_at : second_at)[pieces[k].source] = k;
            }
            const Range range = resolve_range(p.first_layer, p.last_layer, ctx.layer_count);
            ctx.parallel(range.first, range.last, 6, [&](size_t i) {
                const lo::Mask layer = ctx.load(i);
                lo::Mask first, second;
                lo::double_exposure_pair(layer, i, p, first, second);
                r.refs[first_at[i]] = ctx.emit(std::move(first), i);
                r.refs[second_at[i]] = ctx.emit(std::move(second), i);
            });
            return r;
        });
    };
}

Operation change_resolution(const lo::ChangeResolutionParams &p)
{
    return [p](Ctx &ctx) {
        return guarded([&] {
            if (p.new_width == 0 || p.new_height == 0) throw std::invalid_argument("The new resolution must be greater than 0.\n");
            const lo::Rect region = model_bounds(ctx);
            if (const std::string err = lo::validate_change_resolution(p, ctx.layer_count, ctx.width, ctx.height, region); !err.empty())
                throw std::invalid_argument(err);
            Result r;
            r.z_mm = ctx.z_mm;
            r.change.list.source_index.resize(ctx.layer_count);
            for (size_t i = 0; i < ctx.layer_count; ++i) r.change.list.source_index[i] = i;
            r.refs.resize(ctx.layer_count);
            // The output is larger or smaller than the input: one input plus up to two canvases of the new size.
            const size_t buffers = 3 + (size_t(p.new_width) * p.new_height) / std::max<size_t>(1, ctx.width * ctx.height) * 2;
            ctx.parallel(0, ctx.layer_count - 1, buffers, [&](size_t i) {
                r.refs[i] = ctx.emit(lo::change_resolution_layer(ctx.load(i), region, p), std::nullopt);
            });
            return r;
        });
    };
}

Operation phased_exposure(const lo::PhasedExposureParams &p, const lo::PrinterContext &context)
{
    return [p, context](Ctx &ctx) {
        return guarded([&] {
            if (const std::string err = lo::validate_phased_exposure(p, ctx.z_mm); !err.empty()) throw std::invalid_argument(err);
            const size_t count = ctx.layer_count;
            const Range range = resolve_range(p.first_layer, p.last_layer, count);
            struct Out { Ref ref; double z; lo::LayerParams params; size_t source; };
            std::vector<std::vector<Out>> pieces(range.last - range.first + 1);
            ctx.parallel(range.first, range.last, 6, [&](size_t li) {
                lo::Mask layer = ctx.load(li);
                auto made = lo::phased_exposure_layer(layer, li, ctx.z_mm[li], ctx.table[li], p, context);
                auto &out = pieces[li - range.first];
                for (lo::PhasedExposurePiece &pc : made)
                    out.push_back({pc.original ? Ref{li, false} : ctx.emit(std::move(pc.mask), li), pc.z, pc.params, pc.source});
            });
            Result r;
            auto push = [&](Ref ref, double z, const lo::LayerParams &lp, size_t src) {
                r.refs.push_back(ref); r.z_mm.push_back(z); r.table.push_back(lp); r.change.list.source_index.push_back(src);
            };
            for (size_t i = 0; i < range.first; ++i) push({i, false}, ctx.z_mm[i], ctx.table[i], i);
            for (auto &vec : pieces) for (Out &o : vec) push(o.ref, o.z, o.params, o.source);
            for (size_t i = range.last + 1; i < count; ++i) push({i, false}, ctx.z_mm[i], ctx.table[i], i);
            r.change.bottom_layer_count = size_t(std::count_if(r.table.begin(), r.table.end(), [](const auto &lp) { return lp.is_bottom; }));
            return r;
        });
    };
}

Operation timelapse(const lo::TimelapseParams &p, const lo::PrinterContext &context)
{
    return [p, context](Ctx &ctx) {
        return guarded([&] {
            Result r;
            r.z_mm = ctx.z_mm;
            r.table = ctx.table;
            lo::TimelapseVirtualLayer virtual_layer;
            const lo::TimelapseResult plan = lo::timelapse_plan(ctx.layer_count, ctx.width, ctx.height, r.z_mm, r.table, p, context,
                [&](int &cx, int &cy) {
                    const lo::Rect b = model_bounds(ctx);
                    cx = b.x + b.w / 2; cy = b.y + b.h / 2; // Rect{} gives 0, 0 as model_center does
                }, virtual_layer, ctx.progress);
            r.change.list = plan.list;
            r.refs.resize(plan.inserted.size());
            std::optional<Ref> blank;
            for (size_t k = 0; k < plan.inserted.size(); ++k) {
                if (!plan.inserted[k]) { r.refs[k] = {plan.list.source_index[k], false}; continue; }
                if (!blank) blank = ctx.emit(virtual_layer.make(), plan.list.source_index[k]);
                r.refs[k] = *blank;
            }
            // The row of a layer that follows an inserted one was changed by the plan: nothing else to patch here.
            return r;
        });
    };
}

Operation raise_on_print_finish(const lo::RaiseOnPrintFinishParams &p, const lo::PrinterContext &context)
{
    return [p, context](Ctx &ctx) {
        return guarded([&] {
            Result r;
            r.z_mm = ctx.z_mm;
            r.table = ctx.table;
            const size_t count = ctx.layer_count;
            const lo::RaiseDummyLayer dummy = lo::raise_on_print_finish_plan(count, ctx.width, ctx.height, r.z_mm, r.table, p, context,
                [&](size_t &x, size_t &y, bool &found) {
                    const lo::Mask last = ctx.load(count - 1);
                    for (size_t i = 0; i < last.px.size(); ++i)
                        if (last.px[i]) { x = i % ctx.width; y = i / ctx.width; found = true; break; }
                },
                [&](int &cx, int &cy) {
                    const lo::Rect b = model_bounds(ctx);
                    cx = b.x + b.w / 2; cy = b.y + b.h / 2;
                });
            r.change.list.source_index.resize(count + 1);
            for (size_t i = 0; i < count; ++i) r.change.list.source_index[i] = i;
            r.change.list.source_index[count] = count - 1; // the new last layer is derived from the old last layer
            r.refs = copies_of(r.change.list.source_index);
            r.refs[count] = ctx.emit(dummy.make(), count - 1);
            return r;
        });
    };
}

Operation dynamic_layer_height(const lo::DynamicLayerHeightParams &p, const lo::PrinterContext &context)
{
    return [p, context](Ctx &ctx) {
        return guarded([&] {
            // The stack walk holds ~10 decoded layers (cursor layers, sum, XOR, temporaries): check it against the budget.
            ctx.parallel(0, 0, 10, [](size_t) {});
            Result r;
            r.z_mm = ctx.z_mm;
            r.table = ctx.table;
            std::vector<Ref> refs;
            const lo::DynamicLayerHeightResult res = lo::dynamic_layer_height_plan(
                ctx.layer_count, r.z_mm, r.table, p, context, [&](size_t i) { return ctx.load(i); },
                [&](lo::Mask &&mask, size_t head, bool pristine) {
                    // An unchanged layer keeps its payload (nothing is stored); a stack is stored once, right away.
                    refs.push_back(pristine ? Ref{head, false} : ctx.emit(std::move(mask), head));
                },
                ctx.progress);
            r.change.list = res.list;
            r.refs = std::move(refs);
            return r;
        });
    };
}

Operation repair_layers(const lo::RepairLayersParams &p)
{
    return [p](Ctx &ctx) {
        return guarded([&] {
            Result r;
            r.z_mm = ctx.z_mm;
            lo::RepairIO io;
            io.layer_count = ctx.layer_count; io.width = ctx.width; io.height = ctx.height;
            io.load = [&](size_t i) { return ctx.load(i); };
            io.store = [&](lo::Mask &&mask, std::optional<size_t> source, bool scratch) {
                const Ref ref = scratch ? ctx.emit_scratch(std::move(mask), source) : ctx.emit(std::move(mask), source);
                return lo::RepairRef{ref.index, ref.emitted};
            };
            io.load_ref = [&](lo::RepairRef ref) { return ctx.load_ref(Ref{ref.index, ref.emitted}); };
            io.parallel = ctx.parallel;
            std::vector<lo::RepairRef> refs;
            const lo::RepairLayersResult res = lo::repair_layers_plan(io, r.z_mm, p, refs, ctx.progress);
            r.change.list = res.list;
            r.refs.resize(refs.size());
            for (size_t i = 0; i < refs.size(); ++i) r.refs[i] = {refs[i].index, refs[i].emitted};
            return r;
        });
    };
}

Operation lithophane(const lo::Mask &image, size_t width, size_t height, const lo::LithophaneParams &p, const lo::PrinterContext &context,
                     const lo::LayerParams &bottom_row, const lo::LayerParams &normal_row)
{
    return [=](Ctx &ctx) {
        return guarded([&] {
            std::vector<lo::Mask> distinct;
            std::vector<size_t> sequence;
            std::vector<lo::LayerParams> params;
            Result r;
            lo::lithophane_stack(image, width, height, p, context, distinct, sequence, r.z_mm, params, ctx.progress);
            // The lithophane only decides exposure and which layers are bottom layers. Lifts, speeds and waits stay those
            // of the document's first bottom / normal layer instead of being reset to zero, which would print badly.
            for (lo::LayerParams &row : params) {
                lo::LayerParams merged = row.is_bottom ? bottom_row : normal_row;
                merged.is_bottom = row.is_bottom;
                merged.exposure_s = row.exposure_s;
                row = merged;
            }
            r.table = std::move(params);
            // New layers also inherit the unmodelled fields (light off, ...) of the nearest old layer.
            const size_t old_count = ctx.layer_count;
            std::vector<std::optional<Ref>> done(distinct.size());
            r.refs.resize(sequence.size());
            for (size_t i = 0; i < sequence.size(); ++i) {
                r.change.list.source_index.push_back(std::min(i, old_count - 1));
                auto &slot = done[sequence[i]];
                if (!slot) slot = ctx.emit(lo::Mask(distinct[sequence[i]]), r.change.list.source_index.back());
                r.refs[i] = *slot;
            }
            return r;
        });
    };
}

bool apply_dynamic_lifts(LayerDocument &doc, const lo::DynamicLiftsParams &p, double fallback_layer_height_mm, const LayerProgress &progress)
{
    // Only the number of lit pixels of every layer matters: one layer decoded at a time (scan), never the document.
    std::vector<size_t> counts(doc.layer_count(), 0);
    if (!doc.scan_masks(LayerMaskPlane::Main, [&](size_t i, const lo::Mask &m) { counts[i] = m.nonzero_count(); }, progress)) return false;
    return doc.apply_parameter_operation(
        [&](std::vector<lo::Mask> &, std::vector<double> &, std::vector<lo::LayerParams> &table, const LayerProgress &pr) {
            try { lo::dynamic_lifts_from_counts(counts, table, lo::dynamic_lifts_defaults(table, p), pr); }
            catch (const std::invalid_argument &error) { throw RuntimeError(error.what()); }
            return LayerDocument::LayerListChange{};
        },
        false, fallback_layer_height_mm, progress);
}

} // namespace Slic3r::sla::liststream
