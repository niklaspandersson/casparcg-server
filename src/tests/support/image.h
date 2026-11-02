#pragma once

#include <core/frame/frame.h>

#include <filesystem>
#include <string>
#include <vector>

namespace caspar { namespace test {

// An RGBA picture with components from 0 to 1, for comparing what the mixer produced.
struct image
{
    int                width  = 0;
    int                height = 0;
    std::vector<float> rgba; // 4 per pixel, row by row

    image() = default;
    image(int width, int height);

    float*       at(int x, int y) { return &rgba[(static_cast<std::size_t>(y) * width + x) * 4]; }
    const float* at(int x, int y) const { return &rgba[(static_cast<std::size_t>(y) * width + x) * 4]; }

    // A mixer output frame: BGRA, 8 or 16 bits per component.
    static image from_frame(const core::const_frame& frame);

    // Writes a PNG with 8 or 16 bits per component.
    void write_png(const std::filesystem::path& path, bool sixteen_bit = false) const;
};

// How two images of the same size differ. Differences are per component, in the range 0..1;
// multiply by 255 for 8-bit steps.
struct image_difference
{
    double max         = 0; // the largest component difference
    double mean        = 0; // over every component
    double psnr        = 0; // dB, infinite when identical
    int    pixels_over = 0; // pixels with any component off by more than the tolerance given
    int    worst_x = 0, worst_y = 0;

    std::string describe() const;
};

image_difference compare(const image& a, const image& b, double tolerance = 1.0 / 255);

// |a - b| per component, multiplied by `gain` so small differences show; alpha is opaque.
image difference_image(const image& a, const image& b, float gain = 16);

}} // namespace caspar::test
