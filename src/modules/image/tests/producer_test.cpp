#include <modules/image/image.h>

#include <output_dir.h>
#include <producer_harness.h>
#include <runs.h>
#include <test_media.h>

#include <common/except.h>
#include <common/utf.h>

#include <gtest/gtest.h>

#include <limits>

namespace caspar { namespace image { namespace {

using namespace caspar::test;

std::wstring path(const std::filesystem::path& p) { return u16(p.string()); }

// A stamped still: frame 0 of a clip, written as a PNG.
std::filesystem::path make_still(const std::wstring& format = L"720p5000")
{
    return make_clip(
        {.format = format, .frames = 1, .codec = "png", .pixel_format = "rgb24", .extension = "png", .audio = false});
}

class image_producer : public ::testing::Test
{
  protected:
    producer_harness harness{L"720p5000", {image::init}};
};

TEST_F(image_producer, shows_the_image_on_every_tick_without_sound)
{
    auto producer = harness.create({path(make_still())});
    auto frames   = harness.receive(*producer, 20);

    EXPECT_EQ(runs(indices(frames)), "0 0*19");
    EXPECT_TRUE(samples(frames).empty());
    EXPECT_EQ(harness.underflows(), 0);
}

TEST_F(image_producer, reports_its_length)
{
    auto still = path(make_still());
    EXPECT_EQ(harness.create({still})->nb_frames(), std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(harness.create({still, L"LENGTH", L"10"})->nb_frames(), 10u);
}

TEST_F(image_producer, unknown_file_is_not_found)
{
    EXPECT_THROW(harness.create({path(output_dir() / "missing.png")}), file_not_found);
}

TEST(image_producer_formats, interlaced_channel_gets_the_image_on_both_fields)
{
    producer_harness harness(L"1080i5000", {image::init});
    auto             producer = harness.create({path(make_still(L"1080i5000"))});
    auto             frames   = harness.receive(*producer, 10);

    EXPECT_EQ(runs(indices(frames)), "0 0*9");
}

}}} // namespace caspar::image
