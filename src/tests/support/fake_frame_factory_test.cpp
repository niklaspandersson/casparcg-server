#include "fake_frame_factory.h"

#include <core/frame/pixel_format.h>

#include <gtest/gtest.h>

#include <optional>

namespace caspar { namespace test { namespace {

core::pixel_format_desc yuv420(int width, int height)
{
    core::pixel_format_desc desc(core::pixel_format::ycbcr);
    desc.planes.emplace_back(width, height, 1);
    desc.planes.emplace_back(width / 2, height / 2, 1);
    desc.planes.emplace_back(width / 2, height / 2, 1);
    return desc;
}

TEST(fake_frame_factory, sizes_planes_like_the_accelerators)
{
    fake_frame_factory factory;

    auto frame = factory.create_frame(this, yuv420(64, 32));
    EXPECT_EQ(frame.image_data(0).size(), 64u * 32);
    EXPECT_EQ(frame.image_data(1).size(), 32u * 16);

    auto deep = factory.create_frame(this, yuv420(64, 32), common::bit_depth::bit16);
    EXPECT_EQ(deep.image_data(0).size(), 64u * 32 * 2);
}

TEST(fake_frame_factory, counts_frames_until_their_last_plane_is_released)
{
    fake_frame_factory factory;

    std::optional<core::const_frame> frame = core::const_frame(factory.create_frame(this, yuv420(64, 32)));
    auto                             copy  = *frame;
    EXPECT_EQ(factory.frames_created(), 1);
    EXPECT_EQ(factory.frames_alive(), 1);

    frame.reset();
    EXPECT_EQ(factory.frames_alive(), 1) << "a copy still references the planes";

    copy = core::const_frame{};
    EXPECT_EQ(factory.frames_alive(), 0);
    EXPECT_EQ(factory.frames_created(), 1);
}

TEST(fake_frame_factory, frames_may_outlive_the_factory)
{
    std::optional<fake_frame_factory> factory(std::in_place);
    auto                              frame = core::const_frame(factory->create_frame(this, yuv420(64, 32)));

    factory.reset();
    EXPECT_EQ(frame.image_data(0).size(), 64u * 32);
}

}}} // namespace caspar::test
