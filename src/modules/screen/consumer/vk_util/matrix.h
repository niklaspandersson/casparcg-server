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
 *
 * Matrix utilities for Vulkan geometric transforms.
 * Phase 5: Geometric Transforms
 */

#pragma once

#include <array>

namespace caspar { namespace core {
struct image_transform;
struct corners;
}} // namespace caspar::core

namespace caspar { namespace accelerator { namespace vk {

/**
 * 3x3 transformation matrix stored in column-major order for GLSL compatibility.
 * Layout: [m00, m10, m20, m01, m11, m21, m02, m12, m22]
 *
 * Matrix layout (math notation):
 * | m00 m01 m02 |
 * | m10 m11 m12 |
 * | m20 m21 m22 |
 *
 * For 2D transforms, the matrix represents:
 * | scale_x  rotate   translate_x |
 * | rotate   scale_y  translate_y |
 * | 0        0        1           |
 */
struct mat3
{
    std::array<float, 9> m;

    mat3();
    mat3(std::array<float, 9> values);

    // Element access (row, col)
    float  operator()(int row, int col) const;
    float& operator()(int row, int col);

    // Matrix operations
    mat3 operator*(const mat3& other) const;

    // Create identity matrix
    static mat3 identity();

    // Create translation matrix
    static mat3 translate(float tx, float ty);

    // Create scale matrix
    static mat3 scale(float sx, float sy);

    // Create rotation matrix (angle in radians)
    static mat3 rotate(float angle);
};

/**
 * 2D point/vector
 */
struct vec2
{
    float x, y;

    vec2() : x(0), y(0) {}
    vec2(float x_, float y_) : x(x_), y(y_) {}

    vec2 operator+(const vec2& other) const { return vec2(x + other.x, y + other.y); }
    vec2 operator-(const vec2& other) const { return vec2(x - other.x, y - other.y); }
    vec2 operator*(float s) const { return vec2(x * s, y * s); }
};

/**
 * Transform a 2D point by a 3x3 matrix.
 * Assumes the point has homogeneous coordinate w=1.
 */
vec2 transform_point(const mat3& m, const vec2& p);

/**
 * Compute the composite transformation matrix for an image_transform.
 * Follows the same order as the OGL implementation:
 * anchor × aspect × scale × rotation × aspect_inv × translation
 *
 * @param transform The image transform parameters
 * @param aspect_ratio The aspect ratio (width/height) of the frame
 * @return The composite 3x3 transformation matrix
 */
mat3 get_vertex_matrix(const core::image_transform& transform, double aspect_ratio);

/**
 * Apply perspective distortion to a point.
 * Uses bilinear interpolation of corner offsets.
 *
 * @param point The normalized point (0-1 range)
 * @param perspective The corner offsets
 * @return The distorted point
 */
vec2 apply_perspective(const vec2& point, const core::corners& perspective);

/**
 * Check if perspective is default (no distortion).
 */
bool is_default_perspective(const core::corners& perspective);

}}} // namespace caspar::accelerator::vk
