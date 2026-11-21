// SPDX-License-Identifier: AGPL-3.0-or-later
// Native port of UVtools OperationBlur (U/Operations/OperationBlur.cs). The filters are OpenCV's own
// (stackBlur, blur, pyrDown/pyrUp, medianBlur, GaussianBlur, filter2D), so the results are byte identical
// to UVtools (Emgu/OpenCV 4.12) for the variants UVtools really runs; see doc/AUTOMATION.md.
// Pyramid: UVtools 7.0.0 computes the pyramid into a detached buffer and never writes it back (verified with
// UVtoolsCmd: the layer is unchanged). We implement the DESCRIBED behaviour (down + up, original size).
// OpenCV is private to this translation unit: the interface is Mask in / Mask out.
#pragma once

#include "LayerOperations.hpp"

namespace Slic3r::sla::layerops {

enum class BlurAlgorithm { StackBlur, BoxBlur, Pyramid, MedianBlur, GaussianBlur, Filter2D };

struct BlurParams {
    LayerSelection selection;                       // roi = region that is replaced by the blurred image
    BlurAlgorithm  algorithm = BlurAlgorithm::StackBlur;
    unsigned       size = 1;                        // kernel width = height (StackBlur/Gaussian/Median: odd)
    std::string    kernel_text = "1 1 1\n1 1 1\n1 1 1"; // Filter2D: raw (not normalised) 8-bit kernel, space separated
    int            anchor_x = -1, anchor_y = -1;    // -1 = centre (Box blur and Filter2D)
};

// Empty string = valid. Texts follow UVtools ("Size must be a odd number.", "Invalid kernel: ...").
std::string validate_blur(const BlurParams &);
// Blurs one layer (the whole image; the ROI is applied by the kernel).
Mask        blur_mask(const Mask &, const BlurParams &);
StreamKernel blur_kernel(const BlurParams &, const StreamLayout &);
void         blur_layers(std::vector<Mask> &layers, const BlurParams &, const LayerProgress &progress = {});

} // namespace Slic3r::sla::layerops
