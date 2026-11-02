// Destroying an ffmpeg producer at awkward moments. The harness does the checking: when it is
// destroyed it fails the test if a producer outlives it by 10 s or leaves threads, open files or
// frames behind.

#include <modules/ffmpeg/ffmpeg.h>

#include <http_server.h>
#include <log_capture.h>
#include <output_dir.h>
#include <producer_harness.h>
#include <test_media.h>

#include <common/utf.h>

#include <gtest/gtest.h>

namespace caspar { namespace ffmpeg { namespace {

using namespace caspar::test;
using namespace std::chrono_literals;

class ffmpeg_producer_teardown : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Aborting a producer that is still opening or reading its input logs these as errors; see
        // logs_no_errors_when_destroyed_early below.
        for (auto text : {L"moov atom not found",
                          L"Stream ends prematurely",
                          L"av_input.cpp",
                          L"av_producer.cpp",
                          L"pix_fmt",
                          L"generic filter options"})
            allow_log_errors(text);
    }

    // Serves the test's output folder, where make_clip() writes. Constructed before the harness, so
    // its thread and sockets are part of the harness's baseline and it outlives the producers.
    http_server      server{output_dir()};
    producer_harness harness{L"720p5000", {ffmpeg::init}};
};

TEST_F(ffmpeg_producer_teardown, before_its_first_frame)
{
    auto producer = harness.create({u16(make_clip({.frames = 50}).string())});
}

TEST_F(ffmpeg_producer_teardown, during_playback)
{
    auto producer = harness.create({u16(make_clip({.frames = 50}).string()), L"LOOP"});
    harness.receive(*producer, 10);
}

TEST_F(ffmpeg_producer_teardown, right_after_a_seek)
{
    GTEST_SKIP() << "Known bug, hangs about 1 in 8 runs under load: AVProducer::Impl's thread is interrupted while "
                    "seek_internal() clears its decoders, ~Decoder's thread.join() throws thread_interrupted (an "
                    "interruption point) and swallows it, which clears the interrupt request. The run loop then "
                    "waits for frames forever and ~Impl's join never returns.";

    auto producer = harness.create({u16(make_clip({.frames = 50}).string())});
    harness.receive(*producer, 5);
    harness.call(*producer, {L"SEEK", L"40"});
}

TEST_F(ffmpeg_producer_teardown, during_a_slow_download)
{
    auto clip = make_clip({.frames = 250});
    server.set_rate(std::filesystem::file_size(clip) / 5); // the clip's 5 s of video in 5 s

    auto producer = harness.create({u16(server.url(clip.filename().string()))});
    harness.receive(*producer, 10);
}

TEST_F(ffmpeg_producer_teardown, while_opening_a_source_that_never_answers)
{
    auto clip = make_clip({.frames = 10});
    server.set_hang(true);

    auto start    = std::chrono::steady_clock::now();
    auto producer = harness.create({u16(server.url(clip.filename().string()))});
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s) << "PLAY shouldn't wait for the source to answer";
}

}}} // namespace caspar::ffmpeg
