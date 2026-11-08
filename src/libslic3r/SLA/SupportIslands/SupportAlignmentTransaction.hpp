#ifndef slic3r_SLA_SupportAlignmentTransaction_hpp_
#define slic3r_SLA_SupportAlignmentTransaction_hpp_

#include "SupportIslandPoint.hpp"
#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace Slic3r::sla {

// An alignment iteration may project distinct contacts onto the same integer
// coordinate. Keep the last valid iteration, including outline/graph position,
// if halving a move cannot separate them. Restoring point alone is insufficient:
// subsequent restricted moves also depend on each concrete point's state.
class SupportAlignmentTransaction
{
    SupportIslandPoints &m_points;
    SupportIslandPoints m_previous;
    bool m_finished = false;

    void rollback() noexcept
    {
        m_points.swap(m_previous);
        m_finished = true;
    }

public:
    explicit SupportAlignmentTransaction(SupportIslandPoints &points) : m_points(points)
    {
        m_previous.reserve(points.size());
        for (const auto &point : points) m_previous.push_back(point->clone());
    }

    SupportAlignmentTransaction(const SupportAlignmentTransaction &) = delete;
    SupportAlignmentTransaction &operator=(const SupportAlignmentTransaction &) = delete;

    ~SupportAlignmentTransaction()
    {
        if (!m_finished) rollback();
    }

    // True accepts the proposed iteration. False restores the last valid
    // positions and tells alignment to stop. Exceptions also restore on unwind.
    bool commit(const std::function<void()> &throw_on_cancel = {})
    {
        if (m_finished) throw std::logic_error("Support alignment transaction already completed");
        if (m_points.size() != m_previous.size())
            throw std::logic_error("Support alignment changed the number of contacts");
        std::vector<size_t> sorted(m_points.size());
        std::iota(sorted.begin(), sorted.end(), 0);
        const auto less = [this](size_t a, size_t b) {
            const Point &pa = m_points[a]->point, &pb = m_points[b]->point;
            return pa.x() < pb.x() || (pa.x() == pb.x() && pa.y() < pb.y());
        };
        if (throw_on_cancel) throw_on_cancel();
        std::sort(sorted.begin(), sorted.end(), less);
        // Allocate attempt counters only if an actual collision needs repair.
        std::vector<unsigned> attempts;
        for (;;) {
            if (throw_on_cancel) throw_on_cancel();
            size_t duplicate = sorted.size();
            for (size_t i = 1; i < sorted.size(); ++i) {
                if (m_points[sorted[i - 1]]->point == m_points[sorted[i]]->point) {
                    duplicate = sorted[i];
                    break;
                }
            }
            if (duplicate == sorted.size()) {
                m_finished = true;
                return true;
            }
            if (attempts.empty()) attempts.resize(sorted.size(), 0);
            // A bounded number of integer halvings per contact also prevents
            // cycles between different duplicate groups. Never drop contacts.
            if (++attempts[duplicate] > unsigned(std::numeric_limits<coord_t>::digits + 1)) {
                rollback();
                return false;
            }
            auto &point = m_points[duplicate];
            const Point before = point->point;
            // Preserve the established arithmetic and event order whenever it
            // makes progress, so previously valid geometry remains unchanged.
            const Point halfway = m_previous[duplicate]->point / 2 + before / 2;
            point->move(halfway);
            if (point->point == before) {
                rollback();
                return false;
            }
            std::sort(sorted.begin(), sorted.end(), less);
        }
    }
};

} // namespace Slic3r::sla
#endif
