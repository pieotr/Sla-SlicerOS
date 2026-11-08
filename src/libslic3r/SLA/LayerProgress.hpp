// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <exception>
#include <functional>
#include <cstddef>
namespace Slic3r::sla {
using LayerProgress = std::function<bool(size_t, size_t)>;
// A cancellation outcome is distinct from an invalid archive or I/O failure.
// Callers may suppress this exception only after the worker has unwound.
struct LayerOperationCancelled : std::exception {
    const char *what() const noexcept override { return "Layer operation cancelled"; }
};
inline void check_layer_progress(const LayerProgress &progress, size_t done = 0, size_t total = 0) {
    if (progress && !progress(done, total)) throw LayerOperationCancelled{};
}
}
