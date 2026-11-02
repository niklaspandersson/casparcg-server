// The image mixer on OpenGL and on Vulkan: each backend against known answers, then the two against
// each other on the same scene. Each parity test writes <scene>.opengl.png, <scene>.vulkan.png and
// <scene>.diff.png (differences x16) to its output folder. Add scenes to scenes() as needed.

#include "compositor_harness.h"

#include <output_dir.h>

#include <core/frame/frame_transform.h>

#include <gtest/gtest.h>

#include <functional>
#include <memory>

namespace caspar { namespace test { namespace {

// 75% colour bars over the top two thirds, a grey ramp from black to white below.
image bars(int width, int height)
{
    static const float colours[8][3] = {{.75f, .75f, .75f},
                                        {.75f, .75f, 0},
                                        {0, .75f, .75f},
                                        {0, .75f, 0},
                                        {.75f, 0, .75f},
                                        {.75f, 0, 0},
                                        {0, 0, .75f},
                                        {0, 0, 0}};
    image              picture(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto p = picture.at(x, y);
            if (y < height * 2 / 3) {
                auto c = colours[x * 8 / width];
                p[0] = c[0], p[1] = c[1], p[2] = c[2];
            } else {
                p[0] = p[1] = p[2] = static_cast<float>(x) / (width - 1);
            }
            p[3] = 1;
        }
    }
    return picture;
}

// A vertical grey ramp, to put layers on.
image vertical_ramp(int width, int height)
{
    image picture(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto p = picture.at(x, y);
            p[0] = p[1] = p[2] = static_cast<float>(y) / (height - 1);
            p[3]               = 1;
        }
    }
    return picture;
}

std::unique_ptr<compositor_harness> try_backend(backend b)
{
    try {
        return std::make_unique<compositor_harness>(b);
    } catch (compositor_unavailable&) {
        return nullptr;
    }
}

// Each backend against known answers.

class compositor : public ::testing::TestWithParam<backend>
{
  protected:
    void SetUp() override
    {
        harness = try_backend(GetParam());
        if (!harness)
            GTEST_SKIP() << to_string(GetParam()) << " isn't available here";
    }

    std::unique_ptr<compositor_harness> harness;
};

TEST_P(compositor, bgra_passes_through_unchanged)
{
    auto picture = bars(harness->format().width, harness->format().height);

    auto diff = compare(harness->render({harness->bgra_frame(picture)}), picture);
    EXPECT_EQ(diff.pixels_over, 0) << diff.describe();
}

TEST_P(compositor, ycbcr_matches_the_conversion_formula)
{
    auto picture = bars(harness->format().width, harness->format().height);

    for (auto depth : {common::bit_depth::bit8, common::bit_depth::bit10}) {
        // 4:4:4, so chroma subsampling doesn't blur the bar edges.
        auto out  = harness->render({harness->ycbcr_frame(picture, {1, 1, depth, core::color_space::bt709})});
        auto diff = compare(out, picture, 2.0 / 255); // the 8-bit codes round
        EXPECT_EQ(diff.pixels_over, 0) << (depth == common::bit_depth::bit8 ? "8" : "10")
                                       << "-bit: " << diff.describe();
    }
}

INSTANTIATE_TEST_SUITE_P(backends, compositor, ::testing::Values(backend::opengl, backend::vulkan), [](auto& info) {
    return to_string(info.param);
});

// The two backends against each other.

struct scene
{
    std::string                                                       name;
    std::function<std::vector<core::draw_frame>(compositor_harness&)> layers;
    double                                                            tolerance = 1.0 / 255;
};

std::vector<scene> scenes()
{
    auto full = [](compositor_harness& h, image (*pattern)(int, int)) {
        return pattern(h.format().width, h.format().height);
    };

    return {
        {"ycbcr_422_8bit", [=](auto& h) { return std::vector{h.ycbcr_frame(full(h, bars))}; }},
        {"ycbcr_422_10bit",
         [=](auto& h) { return std::vector{h.ycbcr_frame(full(h, bars), {2, 1, common::bit_depth::bit10})}; }},
        {"half_opacity_over_ramp",
         [=](auto& h) {
             auto top                                = h.bgra_frame(full(h, bars));
             top.transform().image_transform.opacity = 0.5;
             return std::vector{h.bgra_frame(full(h, vertical_ramp)), top};
         }},
        {"scaled_and_moved",
         [=](auto& h) {
             auto frame                                         = h.bgra_frame(full(h, bars));
             frame.transform().image_transform.fill_scale       = {0.5, 0.5};
             frame.transform().image_transform.fill_translation = {0.3, 0.2};
             return std::vector{frame};
         }},
    };
}

class compositor_parity : public ::testing::TestWithParam<scene>
{
};

TEST_P(compositor_parity, opengl_and_vulkan_agree)
{
    auto opengl = try_backend(backend::opengl);
    auto vulkan = try_backend(backend::vulkan);
    if (!opengl || !vulkan)
        GTEST_SKIP() << "needs both OpenGL and Vulkan";

    auto& s = GetParam();
    auto  a = opengl->render(s.layers(*opengl));
    auto  b = vulkan->render(s.layers(*vulkan));

    a.write_png(output_dir() / (s.name + ".opengl.png"));
    b.write_png(output_dir() / (s.name + ".vulkan.png"));
    difference_image(a, b).write_png(output_dir() / (s.name + ".diff.png"));

    auto diff = compare(a, b, s.tolerance);
    EXPECT_EQ(diff.pixels_over, 0) << diff.describe() << "\n  images: " << output_dir().string();
}

INSTANTIATE_TEST_SUITE_P(scenes, compositor_parity, ::testing::ValuesIn(scenes()), [](auto& info) {
    return info.param.name;
});

}}} // namespace caspar::test
