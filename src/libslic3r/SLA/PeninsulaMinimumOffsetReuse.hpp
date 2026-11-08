#ifndef SLIC3R_SLA_PENINSULA_MINIMUM_OFFSET_REUSE_HPP
#define SLIC3R_SLA_PENINSULA_MINIMUM_OFFSET_REUSE_HPP

#include "libslic3r/ExPolygon.hpp"
#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace Slic3r::sla::detail {

// Phase-local: linked input geometry/config must remain immutable until all
// users have joined. No geometry normalization, pointer ordering or deduping
// of the key itself: both original order and multiplicity are significant.
using PeninsulaPreviousShapes = std::vector<const ExPolygon *>;
struct PeninsulaPreviousShapesLess {
    bool operator()(const PeninsulaPreviousShapes &a, const PeninsulaPreviousShapes &b) const {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                            std::less<const ExPolygon *>{});
    }
};

class PeninsulaMinimumOffsetSlot {
    std::once_flag m_once;
    ExPolygons m_result;
    std::atomic<size_t> m_reads{0};
public:
    template<class Generate, class Cancel>
    const ExPolygons &get(Generate &&generate, Cancel &&cancel) {
        cancel();
        std::call_once(m_once, [&] {
            // Build locally; exceptions leave once_flag retryable and publish
            // neither partial geometry nor a completed read.
            ExPolygons complete = generate();
            cancel();
            m_result = std::move(complete);
        });
        // Waiting in call_once is not interruptible and includes active
        // generation plus possible retries after exceptions; no hard latency
        // bound is promised. Downstream work never holds the once gate.
        cancel();
        m_reads.fetch_add(1, std::memory_order_relaxed);
        return m_result;
    }
    // Read metadata only after every job has joined, including cancellation.
    size_t reads_after_join() const { return m_reads.load(std::memory_order_relaxed); }
};

class PeninsulaMinimumOffsetReuse {
    std::vector<std::unique_ptr<PeninsulaMinimumOffsetSlot>> m_owned;
    std::vector<PeninsulaMinimumOffsetSlot *> m_slots;
public:
    template<class Cancel>
    explicit PeninsulaMinimumOffsetReuse(const std::vector<PeninsulaPreviousShapes> &inputs, Cancel &&cancel)
        : m_slots(inputs.size(), nullptr) {
        std::map<PeninsulaPreviousShapes, std::vector<size_t>, PeninsulaPreviousShapesLess> groups;
        for (size_t i = 0; i < inputs.size(); ++i) {
            cancel();
            groups[inputs[i]].push_back(i);
        }
        for (const auto &group : groups) {
            cancel();
            if (group.second.size() < 2)
                continue;
            m_owned.push_back(std::make_unique<PeninsulaMinimumOffsetSlot>());
            for (size_t index : group.second)
                m_slots[index] = m_owned.back().get();
        }
    }
    PeninsulaMinimumOffsetSlot *slot(size_t index) const { return m_slots.at(index); }
    size_t repeated_groups() const { return m_owned.size(); }
    size_t computed_after_join() const {
        size_t count = 0;
        for (const auto &slot : m_owned)
            count += slot->reads_after_join() != 0;
        return count;
    }
    size_t reused_after_join() const {
        size_t count = 0;
        for (const auto &slot : m_owned) {
            const size_t reads = slot->reads_after_join();
            count += reads == 0 ? 0 : reads - 1;
        }
        return count;
    }
};
} // namespace Slic3r::sla::detail
#endif
