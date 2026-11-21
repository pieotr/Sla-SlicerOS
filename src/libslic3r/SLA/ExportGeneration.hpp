// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <utility>
#include <memory>
#include <mutex>
#include <string>
#include "libslic3r/Exception.hpp"

namespace Slic3r::sla {
// Invalidation and publication share only this short-lived commit lock.
// Encoding and worker stop/join must happen outside the lock.
struct ExportGenerationState {
    std::atomic<uint64_t> generation{0};
    std::atomic<uint64_t> input_generation{0};
    mutable std::mutex commit_mutex;
};
class ExportGenerationGuard;
// Deferred export binds to operator input, not to the later production of
// masks for that same input. It still expires on replacement or destruction.
class ExportInputGuard {
    std::shared_ptr<const ExportGenerationState> m_state;
    uint64_t m_generation;
public:
    explicit ExportInputGuard(std::shared_ptr<const ExportGenerationState> state)
        : m_state(std::move(state)), m_generation(0) {
        if (!m_state) throw RuntimeError("Export input source is unavailable");
        m_generation = m_state->input_generation.load(std::memory_order_acquire);
    }
    uint64_t generation() const { return m_generation; }
    bool current() const {
        return m_state && m_state->input_generation.load(std::memory_order_acquire) == m_generation;
    }
    void check() const;
    bool same_source(const ExportGenerationGuard &output) const;
};
// The state outlives its source when a queued request retains this guard.
// Source destruction advances the state, so checks never dereference a freed job.
class ExportGenerationGuard {
    friend class ExportInputGuard;
    std::shared_ptr<const ExportGenerationState> m_state;
    uint64_t m_generation;
    std::shared_ptr<std::atomic<bool>> m_cancelled;
public:
    explicit ExportGenerationGuard(std::shared_ptr<const ExportGenerationState> state)
        : m_state(std::move(state)), m_generation(0)
    {
        if (!m_state) throw RuntimeError("Export generation source is unavailable");
        m_generation=m_state->generation.load(std::memory_order_acquire);
    }
    uint64_t generation() const { return m_generation; }
    // A request gets its own cancellation flag; copies share that flag.
    // Cancellation and publication serialize through the source commit lock.
    ExportGenerationGuard with_cancellation() const;
    void cancel() const;
    bool cancelled() const { return m_cancelled && m_cancelled->load(std::memory_order_acquire); }
    bool current() const;
    // Implemented with the engine cancellation type, keeping this header small.
    void check() const;
    // Check source identity and rename under the invalidation lock. Does not
    // call back into the source, whose lifetime may already have ended.
    void publish(const std::string &staged_path, const std::string &target_path) const;
};
// A value document has a distinct source identity on copy. Moving transfers
// that identity; assignment and destruction retire the previous source.
class ExportGenerationOwner {
    friend class LayerDocument;
    std::shared_ptr<ExportGenerationState> m_state = std::make_shared<ExportGenerationState>();
public:
    ExportGenerationOwner() = default;
    ExportGenerationOwner(const ExportGenerationOwner &) : ExportGenerationOwner() {}
    ExportGenerationOwner(ExportGenerationOwner &&other) noexcept : m_state(std::move(other.m_state)) {}
    ExportGenerationOwner &operator=(const ExportGenerationOwner &other) {
        if (this != &other) {
            auto replacement = std::make_shared<ExportGenerationState>();
            invalidate(); m_state = std::move(replacement);
        }
        return *this;
    }
    ExportGenerationOwner &operator=(ExportGenerationOwner &&other) noexcept {
        invalidate();
        if (this != &other) m_state = std::move(other.m_state);
        return *this;
    }
    ~ExportGenerationOwner() { invalidate(); }
    void invalidate() const {
        if (!m_state) return;
        std::lock_guard<std::mutex> lock(m_state->commit_mutex);
        m_state->generation.fetch_add(1, std::memory_order_release);
        m_state->input_generation.fetch_add(1, std::memory_order_release);
    }
    ExportGenerationGuard guard() const { return ExportGenerationGuard(m_state); }
    ExportInputGuard input_guard() const { return ExportInputGuard(m_state); }
};
} // namespace Slic3r::sla
