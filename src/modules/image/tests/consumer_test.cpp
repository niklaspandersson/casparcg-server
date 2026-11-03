#include <modules/image/image.h>

#include <consumer_harness.h>
#include <environment.h>
#include <output_dir.h>
#include <runs.h>
#include <test_media.h>

#include <common/utf.h>

#include <gtest/gtest.h>

namespace caspar { namespace image { namespace {

using namespace caspar::test;

TEST(image_consumer, writes_the_first_frame_as_png_and_asks_to_be_removed)
{
    consumer_harness harness(L"720p5000", {image::init});

    // The consumer writes <media folder>/<name>.png; point it at the output folder instead.
    auto name     = std::filesystem::relative(output_dir() / "snapshot", media_folder());
    auto consumer = harness.create({L"IMAGE", u16(name.string())});

    EXPECT_FALSE(harness.send(*consumer, 3)) << "removed after the first frame";
    EXPECT_EQ(harness.frames_sent(), 1);
    harness.close(consumer); // waits for the thread that writes the file

    EXPECT_EQ(runs(decode_clip(output_dir() / "snapshot.png").indices), "0");
}

}}} // namespace caspar::image
