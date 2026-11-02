#include <modules/ffmpeg/ffmpeg.h>

#include <http_server.h>
#include <output_dir.h>
#include <producer_harness.h>
#include <runs.h>
#include <test_media.h>

#include <common/except.h>
#include <common/utf.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <limits>

namespace caspar { namespace ffmpeg { namespace {

using namespace caspar::test;
using ::testing::Each;

std::wstring path(const std::filesystem::path& p) { return u16(p.string()); }

std::vector<std::size_t> sample_counts(const std::vector<produced_frame>& frames)
{
    std::vector<std::size_t> result;
    for (auto& frame : frames)
        result.push_back(frame.samples.size());
    return result;
}

// Each index in [0, count) `times` times in a row.
std::vector<std::optional<int>> each_repeated(int count, int times)
{
    std::vector<std::optional<int>> result;
    for (int n = 0; n < count; ++n)
        result.insert(result.end(), times, n);
    return result;
}

std::vector<std::optional<int>> indices_on(const std::vector<produced_frame>& frames, core::video_field field)
{
    std::vector<std::optional<int>> result;
    for (auto& frame : frames) {
        if (frame.field == field)
            result.push_back(frame.index);
    }
    return result;
}

class ffmpeg_producer : public ::testing::Test
{
  protected:
    producer_harness harness{L"720p5000", {ffmpeg::init}};
};

TEST_F(ffmpeg_producer, plays_every_frame_and_its_audio_in_order)
{
    auto producer = harness.create({path(make_clip({.frames = 50}))});

    auto frames = harness.receive(*producer, 50);
    EXPECT_EQ(runs(indices(frames)), "0..49");
    EXPECT_EQ(runs(samples(frames)), "0..47999");
    EXPECT_THAT(sample_counts(frames), Each(960)) << "48 kHz at 50 frames per second";
}

TEST_F(ffmpeg_producer, reports_its_length)
{
    auto producer = harness.create({path(make_clip({.frames = 50}))});
    harness.receive(*producer, 1);
    EXPECT_EQ(producer->nb_frames(), 50u);

    auto looping = harness.create({path(make_clip({.frames = 50, .name = "looping"})), L"LOOP"});
    harness.receive(*looping, 1);
    EXPECT_EQ(looping->nb_frames(), std::numeric_limits<std::uint32_t>::max());
}

TEST_F(ffmpeg_producer, holds_the_last_frame_silently_at_the_end)
{
    auto producer = harness.create({path(make_clip({.frames = 50}))});

    auto frames = harness.receive(*producer, 60);
    EXPECT_EQ(runs(indices(frames)), "0..49 49*10");
    EXPECT_EQ(runs(samples(frames)), "0..47999");
}

TEST_F(ffmpeg_producer, loops)
{
    auto producer = harness.create({path(make_clip({.frames = 50})), L"LOOP"});

    auto frames = harness.receive(*producer, 120);
    EXPECT_EQ(runs(indices(frames)), "0..49 0..49 0..19");
    EXPECT_EQ(runs(samples(frames)), "0..47999 0..47999 0..19199");
}

TEST_F(ffmpeg_producer, starts_at_seek)
{
    auto producer = harness.create({path(make_clip({.frames = 50})), L"SEEK", L"10"});

    auto frames = harness.receive(*producer, 5);
    EXPECT_EQ(runs(indices(frames)), "10..14");
    EXPECT_EQ(runs(samples(frames)), "9600..14399");
}

TEST_F(ffmpeg_producer, plays_from_in_to_out)
{
    auto producer = harness.create({path(make_clip({.frames = 50})), L"IN", L"10", L"OUT", L"20"});

    EXPECT_EQ(runs(indices(harness.receive(*producer, 15))), "10..19 19*5");
    EXPECT_EQ(producer->nb_frames(), 10u);
}

TEST_F(ffmpeg_producer, plays_length_frames)
{
    auto producer = harness.create({path(make_clip({.frames = 50})), L"LENGTH", L"5"});

    EXPECT_EQ(runs(indices(harness.receive(*producer, 8))), "0..4 4*3");
}

TEST_F(ffmpeg_producer, loops_between_in_and_out)
{
    auto producer = harness.create({path(make_clip({.frames = 50})), L"IN", L"10", L"OUT", L"20", L"LOOP"});

    EXPECT_EQ(runs(indices(harness.receive(*producer, 25))), "10..19 10..19 10..14");
}

TEST_F(ffmpeg_producer, seek_command_jumps_to_the_frame)
{
    auto producer = harness.create({path(make_clip({.frames = 50}))});
    auto before   = harness.receive(*producer, 5);

    EXPECT_EQ(harness.call(*producer, {L"SEEK", L"30"}), L"30");

    auto after = harness.receive(*producer, 5);
    EXPECT_EQ(runs(indices(before)) + " " + runs(indices(after)), "0..4 30..34");
    EXPECT_EQ(runs(samples(after)), "28800..33599");
}

TEST_F(ffmpeg_producer, loop_command_turns_looping_on)
{
    auto producer = harness.create({path(make_clip({.frames = 20}))});

    EXPECT_EQ(harness.call(*producer, {L"LOOP", L"1"}), L"1");
    EXPECT_EQ(runs(indices(harness.receive(*producer, 30))), "0..19 0..9");
}

TEST(ffmpeg_producer_formats, interlaced_clip_sends_each_frame_for_both_fields)
{
    producer_harness harness(L"1080i5000", {ffmpeg::init});
    auto             producer = harness.create({path(make_clip({.format = L"1080i5000", .frames = 10}))});

    auto frames = harness.receive(*producer, 20);
    EXPECT_EQ(runs(indices_on(frames, core::video_field::a)), "0..9");
    EXPECT_EQ(runs(indices_on(frames, core::video_field::b)), "0..9");
    EXPECT_EQ(runs(samples(frames)), "0..19199") << "960 samples per field";
    EXPECT_EQ(producer->nb_frames(), 20u) << "counted in fields";
}

TEST(ffmpeg_producer_formats, progressive_clip_fills_alternate_fields)
{
    producer_harness harness(L"1080i5000", {ffmpeg::init});
    auto             producer = harness.create({path(make_clip({.format = L"1080p5000", .frames = 20}))});

    auto frames = harness.receive(*producer, 20);
    EXPECT_EQ(runs(indices(frames)), "0..19");
    EXPECT_EQ(runs(indices_on(frames, core::video_field::a)), "0 2 4 6 8 10 12 14 16 18");
}

TEST(ffmpeg_producer_formats, half_rate_clip_shows_each_frame_twice)
{
    producer_harness harness(L"1080p5000", {ffmpeg::init});
    auto             producer = harness.create({path(make_clip({.format = L"1080p2500", .frames = 10}))});

    auto frames = harness.receive(*producer, 20);
    EXPECT_EQ(indices(frames), each_repeated(10, 2));
    EXPECT_EQ(runs(samples(frames)), "0..19199");
}

TEST_F(ffmpeg_producer, plays_over_http)
{
    auto        clip = make_clip({.frames = 50});
    http_server server(clip.parent_path());

    auto producer = harness.create({u16(server.url(clip.filename().string()))});
    EXPECT_EQ(runs(indices(harness.receive(*producer, 50))), "0..49");
}

TEST_F(ffmpeg_producer, unknown_file_is_not_found)
{
    EXPECT_THROW(harness.create({path(output_dir() / "missing.mkv")}), file_not_found);
}

}}} // namespace caspar::ffmpeg
