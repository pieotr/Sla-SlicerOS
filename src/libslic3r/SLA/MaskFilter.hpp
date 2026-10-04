// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

namespace Slic3r::sla {
// Normalized odd square box filter, with OpenCV/UVtools Reflect101 boundaries.
// Keep full sums until the final division: rounding an intermediate horizontal
// pass would change grayscale. Working storage is only one column sum per X.
inline std::vector<uint8_t> box_blur(const std::vector<uint8_t> &input,
                                    std::size_t width, std::size_t height, unsigned radius,
                                    const std::function<void()> &check_cancel = {}) {
    if (!width || !height || width > 32768 || height > 32768 ||
        width * height > 128ULL * 1024 * 1024 ||
        width * height != input.size() || radius > 4096)
        throw std::invalid_argument("Invalid box blur dimensions or radius");
    if (!radius) return input;
    auto reflect = [](int index, int length) {
        if (length == 1) return 0;
        const int period = 2 * (length - 1);
        index %= period;
        if (index < 0) index += period;
        return index < length ? index : period-index;
    };
    const uint64_t kernel = 2ULL * radius + 1, area = kernel * kernel;
    std::vector<uint64_t> columns(width,0);
    std::vector<uint8_t> output(input.size());
    // Repeated reflected periods have an exact sum. This keeps initialization
    // linear even for a huge radius on an image only one or two pixels high.
    const uint64_t vertical_period = height == 1 ? 1 : 2 * (height-1);
    const uint64_t cycles = kernel / vertical_period, remainder = kernel % vertical_period;
    for (std::size_t x = 0; x < width; ++x) {
        if (check_cancel && x % 256 == 0) check_cancel();
        if (cycles) {
            uint64_t sum = input[x];
            if (height > 1) {
                sum += input[(height-1)*width+x];
                for (std::size_t y = 1; y+1 < height; ++y) sum += 2 * input[y*width+x];
            }
            columns[x] = sum * cycles;
        }
        for (uint64_t k = 0; k < remainder; ++k)
            columns[x] += input[std::size_t(reflect(-int(radius)+int(k),int(height)))*width+x];
    }
    const uint64_t horizontal_period = width == 1 ? 1 : 2 * (width-1);
    const uint64_t horizontal_cycles = kernel / horizontal_period;
    const uint64_t horizontal_remainder = kernel % horizontal_period;
    for (std::size_t y = 0; y < height; ++y) {
        if (check_cancel) check_cancel();
        if (y) {
            const auto leaving = std::size_t(reflect(int(y)-int(radius)-1,int(height)))*width;
            const auto entering = std::size_t(reflect(int(y)+int(radius),int(height)))*width;
            for (std::size_t x = 0; x < width; ++x)
                columns[x] = columns[x] - input[leaving+x] + input[entering+x];
        }
        uint64_t sum = 0;
        if (horizontal_cycles) {
            sum = columns[0];
            if (width > 1) {
                sum += columns[width-1];
                for (std::size_t x = 1; x+1 < width; ++x) sum += 2 * columns[x];
            }
            sum *= horizontal_cycles;
        }
        for (uint64_t k = 0; k < horizontal_remainder; ++k)
            sum += columns[std::size_t(reflect(-int(radius)+int(k),int(width)))];
        for (std::size_t x = 0; x < width; ++x) {
            if (x) {
                sum -= columns[std::size_t(reflect(int(x)-int(radius)-1,int(width)))];
                sum += columns[std::size_t(reflect(int(x)+int(radius),int(width)))];
            }
            output[y*width+x] = uint8_t((sum+area/2)/area);
        }
    }
    return output;
}
} // namespace Slic3r::sla
