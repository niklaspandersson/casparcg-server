/*
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: CasparCG Team
 */

#pragma once

#include <core/mixer/image/blend_modes.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace caspar { namespace accelerator { namespace vk {

class texture;

/**
 * Push constants structure for blend compute shader.
 * Must match the layout in blend.comp
 *
 * Phase 5: Extended with full transform matrix and geometry parameters.
 * Phase 6: Extended with color adjustments and chroma key parameters.
 * Phase 7: Extended with pixel format decoding and color space conversion.
 */
struct blend_push_constants
{
    // Blend parameters
    int32_t blend_mode;     // 0-28 blend mode index
    int32_t keyer;          // 0=linear, 1=additive
    float   opacity;        // 0.0-1.0
    int32_t _pad0;          // Padding for alignment

    // Transform matrix (3x3, column-major, stored as 3 vec3s for GLSL compatibility)
    // Row 0: m[0], m[1], m[2]
    // Row 1: m[3], m[4], m[5]
    // Row 2: m[6], m[7], m[8]
    float transform_matrix[9];
    float _pad1[3];         // Padding to align next field

    // Perspective corners (for bilinear interpolation)
    // Each corner is (x, y): ul, ur, ll, lr
    float perspective_ul[2];
    float perspective_ur[2];
    float perspective_ll[2];
    float perspective_lr[2];

    // Clipping rectangle (normalized 0-1)
    float clip_left;
    float clip_top;
    float clip_right;
    float clip_bottom;

    // Cropping rectangle (normalized 0-1)
    float crop_left;
    float crop_top;
    float crop_right;
    float crop_bottom;

    // Image dimensions
    int32_t src_width;
    int32_t src_height;
    int32_t dst_width;
    int32_t dst_height;

    // Feature flags
    int32_t use_perspective;    // 1 if perspective is non-default
    int32_t use_clipping;       // 1 if clipping should be applied
    int32_t use_cropping;       // 1 if cropping should be applied
    int32_t invert;             // 1 if colors should be inverted

    // Phase 6: Color adjustments (Contrast/Saturation/Brightness)
    int32_t use_csb;            // 1 if CSB should be applied
    float   brightness;         // 1.0 = normal
    float   saturation;         // 1.0 = normal
    float   contrast;           // 1.0 = normal

    // Phase 6: Levels control
    int32_t use_levels;         // 1 if levels should be applied
    float   levels_min_input;   // 0.0-1.0
    float   levels_max_input;   // 0.0-1.0
    float   levels_gamma;       // 0.1-10.0

    float   levels_min_output;  // 0.0-1.0
    float   levels_max_output;  // 0.0-1.0
    int32_t _pad2;              // Padding for alignment
    int32_t _pad3;              // Padding for alignment

    // Phase 6: Chroma key parameters
    int32_t use_chroma;                     // 1 if chroma keying enabled
    int32_t chroma_show_mask;               // 1 to visualize alpha mask
    float   chroma_target_hue;              // 0.0-1.0 (hue in 0-360 mapped to 0-1)
    float   chroma_hue_width;               // 0.0-1.0

    float   chroma_min_saturation;          // 0.0-1.0
    float   chroma_min_brightness;          // 0.0-1.0
    float   chroma_softness;                // 0.0-1.0
    float   chroma_spill_suppress;          // 0.0-1.0 (range in 0-360 mapped to 0-1)

    float   chroma_spill_suppress_saturation; // 0.0-1.0
    int32_t _pad4[3];                       // Padding for alignment

    // Phase 7: Pixel format and color space
    int32_t pixel_format;       // 0-12 (gray, bgra, rgba, argb, abgr, ycbcr, ycbcra, luma, bgr, rgb, uyvy, gbrp, gbrap)
    int32_t color_space;        // 0=bt601, 1=bt709, 2=bt2020
    int32_t num_planes;         // 1-4 number of texture planes
    int32_t is_straight_alpha;  // 1 if alpha is straight (non-premultiplied)

    // Precision factors for bit depth scaling (per plane)
    float   precision_factor[4];  // 1.0 for 8-bit, 64.0 for 10-bit, 16.0 for 12-bit, 1.0 for 16-bit

    // Color matrix for YCbCr to RGB conversion (row-major 3x3)
    // Row 0: [1.0, 0.0, Cr_R]  - Y contribution, Cb contribution (0), Cr->R contribution
    // Row 1: [1.0, Cb_G, Cr_G] - Y contribution, Cb->G contribution, Cr->G contribution
    // Row 2: [1.0, Cb_B, 0.0]  - Y contribution, Cb->B contribution, Cr contribution (0)
    float   color_matrix[12];     // 3x3 matrix + 3 padding for alignment

    // Luma coefficients for saturation/contrast calculations
    float   luma_coeff[4];        // R, G, B coefficients + padding

    // Plane dimensions for chroma subsampling (plane 1 = Cb/Cr for YCbCr)
    int32_t plane1_width;         // Chroma plane width (may be src_width/2 for YUV420/422)
    int32_t plane1_height;        // Chroma plane height (may be src_height/2 for YUV420)
    int32_t _pad5[2];             // Padding for alignment
};

/**
 * Vulkan compute pipeline for blend operations.
 * Phase 4: GPU-accelerated blend modes.
 * Phase 5: Geometric transforms (FILL, ROTATION, PERSPECTIVE, CLIP, CROP).
 *
 * Provides:
 * - Compute shader execution for image blending
 * - Support for all 29 Photoshop-compatible blend modes
 * - Full geometric transforms with matrix math
 * - Perspective distortion via bilinear corner interpolation
 * - Clipping and cropping support
 * - Push constant based parameter passing
 */
class blend_pipeline final
{
  public:
    // device, physical_device, command_pool, queue are VkHandle cast to void*
    blend_pipeline(void* device, void* physical_device, void* command_pool, void* queue);
    ~blend_pipeline();

    blend_pipeline(const blend_pipeline&)            = delete;
    blend_pipeline& operator=(const blend_pipeline&) = delete;

    /**
     * Execute blend operation using compute shader.
     *
     * @param src Source texture (read-only) - for single-plane formats (BGRA, etc.)
     * @param dst Destination texture (read-write)
     * @param params Blend parameters
     */
    void execute(texture& src, texture& dst, const blend_push_constants& params);

    /**
     * Execute blend operation using compute shader with multi-plane support.
     *
     * @param planes Source textures for each plane (1-4 planes)
     * @param dst Destination texture (read-write)
     * @param params Blend parameters including pixel format info
     */
    void execute(const std::vector<std::shared_ptr<texture>>& planes, texture& dst, const blend_push_constants& params);

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}}} // namespace caspar::accelerator::vk
