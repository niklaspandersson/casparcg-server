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

#include "../StdAfx.h"

#include "matrix.h"

#include <core/frame/frame_transform.h>

#include <cmath>

namespace caspar { namespace accelerator { namespace vk {

// mat3 implementation

mat3::mat3()
    : m{1, 0, 0, 0, 1, 0, 0, 0, 1} // identity
{
}

mat3::mat3(std::array<float, 9> values)
    : m(values)
{
}

float mat3::operator()(int row, int col) const
{
    // Column-major order: index = col * 3 + row
    return m[col * 3 + row];
}

float& mat3::operator()(int row, int col)
{
    return m[col * 3 + row];
}

mat3 mat3::operator*(const mat3& other) const
{
    mat3 result;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            result(row, col) = (*this)(row, 0) * other(0, col) + (*this)(row, 1) * other(1, col) +
                               (*this)(row, 2) * other(2, col);
        }
    }
    return result;
}

mat3 mat3::identity()
{
    return mat3({1, 0, 0, 0, 1, 0, 0, 0, 1});
}

mat3 mat3::translate(float tx, float ty)
{
    // Column-major:
    // | 1  0  tx |
    // | 0  1  ty |
    // | 0  0  1  |
    return mat3({1, 0, 0, 0, 1, 0, tx, ty, 1});
}

mat3 mat3::scale(float sx, float sy)
{
    // Column-major:
    // | sx 0  0 |
    // | 0  sy 0 |
    // | 0  0  1 |
    return mat3({sx, 0, 0, 0, sy, 0, 0, 0, 1});
}

mat3 mat3::rotate(float angle)
{
    // Column-major:
    // | cos  -sin  0 |
    // | sin   cos  0 |
    // | 0     0    1 |
    float c = std::cos(angle);
    float s = std::sin(angle);
    return mat3({c, s, 0, -s, c, 0, 0, 0, 1});
}

vec2 transform_point(const mat3& m, const vec2& p)
{
    // Multiply [x, y, 1] by matrix
    float x = m(0, 0) * p.x + m(0, 1) * p.y + m(0, 2);
    float y = m(1, 0) * p.x + m(1, 1) * p.y + m(1, 2);
    return vec2(x, y);
}

mat3 get_vertex_matrix(const core::image_transform& transform, double aspect_ratio)
{
    // Build composite transform matrix following OGL order:
    // anchor × aspect × scale × rotation × aspect_inv × translation

    float anchor_x = static_cast<float>(transform.anchor[0]);
    float anchor_y = static_cast<float>(transform.anchor[1]);
    float scale_x  = static_cast<float>(transform.fill_scale[0]);
    float scale_y  = static_cast<float>(transform.fill_scale[1]);
    float trans_x  = static_cast<float>(transform.fill_translation[0]);
    float trans_y  = static_cast<float>(transform.fill_translation[1]);
    float angle    = static_cast<float>(transform.angle);
    float aspect   = static_cast<float>(aspect_ratio);

    // Create individual matrices
    mat3 anchor_matrix     = mat3::translate(-anchor_x, -anchor_y);
    mat3 scale_matrix      = mat3::scale(scale_x, scale_y);
    mat3 aspect_matrix     = mat3::scale(1.0f, 1.0f / aspect);
    mat3 aspect_inv_matrix = mat3::scale(1.0f, aspect);
    mat3 rotation_matrix   = mat3::rotate(angle);
    mat3 translation_matrix = mat3::translate(trans_x, trans_y);

    // Compose: anchor × aspect × scale × rotation × aspect_inv × translation
    // Note: Matrix multiplication is right-to-left for transformation order
    // But we compose left-to-right because that's how the OGL code works
    return anchor_matrix * aspect_matrix * scale_matrix * rotation_matrix * aspect_inv_matrix * translation_matrix;
}

vec2 apply_perspective(const vec2& point, const core::corners& perspective)
{
    // Bilinear interpolation of corner offsets, following OGL implementation
    // This applies the perspective distortion based on the point's position

    float x = point.x;
    float y = point.y;

    float result_x = x;
    float result_y = y;

    // ul: upper-left contribution
    // x' = (1-y) * a + (1 - a * (1-y)) * x
    float ul_x = static_cast<float>(perspective.ul[0]);
    float ul_y = static_cast<float>(perspective.ul[1]);
    result_x += (1 - y) * ul_x + (1 - ul_x + ul_x * y) * x - x;
    result_y += (1 - x) * ul_y + (1 - ul_y + ul_y * x) * y - y;

    // ur/ll contributions
    float ur_x = static_cast<float>(perspective.ur[0]);
    float ur_y = static_cast<float>(perspective.ur[1]);
    float ll_x = static_cast<float>(perspective.ll[0]);
    float ll_y = static_cast<float>(perspective.ll[1]);

    // ur: x' = x * (a * (1-y) + y)
    result_x += x * (ur_x * (1 - y) + y) - x;
    // ll: y' = y * (a * (1-x) + x)
    result_y += y * (ll_y * (1 - x) + x) - y;

    // Continued ur/ll: x' = y * a + x * (1 - a * y)
    result_x += y * ll_x + x * (1 - ll_x * y) - x;
    result_y += x * ur_y + y * (1 - ur_y * x) - y;

    // lr: lower-right contribution
    // x' = x * (y * a + (1-y))
    float lr_x = static_cast<float>(perspective.lr[0]);
    float lr_y = static_cast<float>(perspective.lr[1]);
    result_x += x * (y * lr_x + (1 - y)) - x;
    result_y += y * (x * lr_y + (1 - x)) - y;

    return vec2(result_x, result_y);
}

bool is_default_perspective(const core::corners& perspective)
{
    const double epsilon = 0.0001;
    return std::abs(perspective.ul[0] - 0.0) < epsilon && std::abs(perspective.ul[1] - 0.0) < epsilon &&
           std::abs(perspective.ur[0] - 1.0) < epsilon && std::abs(perspective.ur[1] - 0.0) < epsilon &&
           std::abs(perspective.ll[0] - 0.0) < epsilon && std::abs(perspective.ll[1] - 1.0) < epsilon &&
           std::abs(perspective.lr[0] - 1.0) < epsilon && std::abs(perspective.lr[1] - 1.0) < epsilon;
}

}}} // namespace caspar::accelerator::vk
