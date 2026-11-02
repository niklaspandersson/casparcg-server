#include "fake_frame_factory.h"
#include "test_media.h"

#include <core/frame/pixel_format.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <numeric>
#include <vector>

namespace caspar { namespace test { namespace {

using ::testing::Each;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::Optional;

std::vector<std::optional<int>> indices_up_to(int count)
{
    std::vector<std::optional<int>> result;
    for (int n = 0; n < count; ++n)
        result.push_back(n);
    return result;
}

std::vector<std::int64_t> samples_up_to(std::int64_t count)
{
    std::vector<std::int64_t> result(count);
    std::iota(result.begin(), result.end(), 0);
    return result;
}

TEST(test_media, stamp_round_trips_through_a_luma_plane)
{
    const int                 width = 320, height = 180;
    std::vector<std::uint8_t> luma(width * height, 128);

    for (int index : {0, 1, 2, 255, 12345, max_stamped_index}) {
        stamp_index(luma.data(), width, height, width, index);
        EXPECT_THAT(read_index(luma.data(), width, height, width), Optional(index));
    }
}

TEST(test_media, unstamped_picture_has_no_index)
{
    std::vector<std::uint8_t> luma(320 * 180, 128);
    EXPECT_EQ(read_index(luma.data(), 320, 180, 320), std::nullopt);
}

TEST(test_media, lossless_clip_round_trips_frames_and_audio)
{
    auto clip = decode_clip(make_clip({.frames = 50}));

    EXPECT_THAT(clip.indices, ElementsAreArray(indices_up_to(50)));
    EXPECT_EQ(clip.audio_channels, 2);
    EXPECT_THAT(clip.audio_samples, ElementsAreArray(samples_up_to(48000))) << "50 frames at 50 fps, 48 kHz";
}

TEST(test_media, stamp_survives_a_lossy_codec)
{
    auto clip = decode_clip(make_clip({.frames = 25, .codec = "mpeg4", .extension = "mov"}));
    EXPECT_THAT(clip.indices, ElementsAreArray(indices_up_to(25)));
}

TEST(test_media, interlaced_ten_bit_clip)
{
    auto clip = decode_clip(make_clip({.format       = L"1080i5000",
                                       .frames       = 10,
                                       .codec        = "prores_ks",
                                       .pixel_format = "yuv422p10le",
                                       .extension    = "mov"}));

    EXPECT_THAT(clip.indices, ElementsAreArray(indices_up_to(10)));
    EXPECT_EQ(clip.audio_samples.size(), 10u * 48000 / 25) << "1080i5000 is 25 frames per second";
}

TEST(test_media, reads_the_stamp_from_mixer_pixel_formats)
{
    const int          width = 320, height = 180, index = 4242;
    fake_frame_factory factory;

    std::vector<std::uint8_t> luma(width * height, 128);
    stamp_index(luma.data(), width, height, width, index);

    {
        core::pixel_format_desc desc(core::pixel_format::ycbcr);
        desc.planes.emplace_back(width, height, 1);
        auto frame = factory.create_frame(this, desc);
        std::copy(luma.begin(), luma.end(), frame.image_data(0).begin());
        EXPECT_THAT(read_index(core::const_frame(std::move(frame))), Optional(index)) << "8-bit YCbCr";
    }
    {
        core::pixel_format_desc desc(core::pixel_format::ycbcr);
        desc.planes.emplace_back(width, height, 1, common::bit_depth::bit10);
        auto frame   = factory.create_frame(this, desc);
        auto samples = reinterpret_cast<std::uint16_t*>(frame.image_data(0).data());
        for (int n = 0; n < width * height; ++n)
            samples[n] = static_cast<std::uint16_t>(luma[n] << 2);
        EXPECT_THAT(read_index(core::const_frame(std::move(frame))), Optional(index)) << "10-bit YCbCr";
    }
    {
        core::pixel_format_desc desc(core::pixel_format::bgra);
        desc.planes.emplace_back(width, height, 4);
        auto frame  = factory.create_frame(this, desc);
        auto pixels = frame.image_data(0).data();
        for (int n = 0; n < width * height; ++n) {
            std::fill_n(pixels + n * 4, 3, luma[n]);
            pixels[n * 4 + 3] = 255;
        }
        EXPECT_THAT(read_index(core::const_frame(std::move(frame))), Optional(index)) << "BGRA";
    }
}

TEST(test_media, reads_the_audio_ramp_from_interleaved_samples)
{
    std::vector<std::int32_t> samples;
    for (int n = 100; n < 104; ++n) {
        for (int c = 0; c < 8; ++c)
            samples.push_back((n << 8) | c);
    }
    EXPECT_THAT(ramp_samples(samples.data(), samples.size(), 8), ElementsAreArray({100, 101, 102, 103}));
}

}}} // namespace caspar::test
