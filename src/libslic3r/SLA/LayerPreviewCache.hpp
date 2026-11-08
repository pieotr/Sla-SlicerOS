// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerDocument.hpp"
#include <optional>
#include <utility>

namespace Slic3r::sla {
// One decoded preview, owned by the editor rather than persisted in a project.
// A revision alone is insufficient: different documents can have equal revisions,
// and Undo can revisit one after a worker has already started on newer pixels.
class LayerPreviewCache {
public:
    class Prepared {
        friend class LayerPreviewCache;
        ExportInputGuard m_source;
        ExportGenerationGuard m_generation;
        size_t m_first, m_last;
        LayerMaskPlane m_plane;
        png::ImageGreyscale m_image;

        Prepared(ExportInputGuard source, ExportGenerationGuard generation,
                 size_t first, size_t last, LayerMaskPlane plane, png::ImageGreyscale image)
            : m_source(std::move(source)), m_generation(std::move(generation)),
              m_first(first), m_last(last), m_plane(plane), m_image(std::move(image)) {}
        bool matches(const LayerDocument &document, size_t first, size_t last, LayerMaskPlane plane) const {
            return m_first == first && m_last == last && m_plane == plane &&
                m_generation.current() && m_source.same_source(document.export_generation_guard());
        }
    public:
        Prepared(const Prepared &) = delete;
        Prepared &operator=(const Prepared &) = delete;
        Prepared(Prepared &&) noexcept = default;
        Prepared &operator=(Prepared &&) noexcept = default;
    };

private:
    std::optional<Prepared> m_frame;

public:
    // Run on a worker while the owner excludes competing document edits.
    // No wxWidgets access and no mutation of the cache. The completed result
    // is transferred to the owner only after the worker has been joined.
    static Prepared prepare(const LayerDocument &document, size_t first, size_t last,
                            LayerMaskPlane plane, const LayerProgress &progress = {}) {
        auto source = document.export_input_guard();
        auto generation = document.export_generation_guard();
        const auto guarded = [&](size_t done, size_t total) {
            generation.check();
            check_layer_progress(progress, done, total);
            generation.check();
            return true;
        };
        // overlay also handles one layer and checks cancellation before/after
        // its full PNG decode. Its scratch storage is bounded by two masks.
        auto image = document.overlay(first, last, plane, guarded);
        return Prepared(std::move(source), std::move(generation), first, last, plane, std::move(image));
    }

    // Owner-thread publication refuses a stale document or changed selection.
    // Failure leaves the preceding frame untouched; find() never exposes it for
    // an incompatible selection/generation. Only one frame is retained.
    void publish(Prepared &&prepared, const LayerDocument &document,
                 size_t first, size_t last, LayerMaskPlane plane) {
        if (!prepared.matches(document, first, last, plane))
            throw RuntimeError("Layer preview no longer matches the selected document and range");
        m_frame.emplace(std::move(prepared));
    }

    const png::ImageGreyscale *find(const LayerDocument &document,
                                  size_t first, size_t last, LayerMaskPlane plane) const {
        return m_frame && m_frame->matches(document, first, last, plane) ? &m_frame->m_image : nullptr;
    }
    void clear() { m_frame.reset(); }
};
} // namespace Slic3r::sla
