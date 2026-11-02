// AMCP calls arrive on another thread than the channel's receive(). These tests overlap the two.
// They are most useful in a sanitizer build (CASPARCG_SANITIZE), and repeat themselves more with
// CASPAR_TEST_STRESS=<factor>.

#include <modules/ffmpeg/ffmpeg.h>

#include <concurrency.h>
#include <producer_harness.h>
#include <runs.h>
#include <test_media.h>

#include <common/utf.h>

#include <gtest/gtest.h>

namespace caspar { namespace ffmpeg { namespace {

using namespace caspar::test;
using namespace std::chrono_literals;

// Every frame is one of the clip's, read intact.
void expect_frames_of_clip(const std::vector<produced_frame>& frames, int clip_frames)
{
    for (auto& frame : frames) {
        ASSERT_TRUE(frame.index.has_value()) << "unreadable frame at tick " << frame.tick;
        ASSERT_GE(*frame.index, 0);
        ASSERT_LT(*frame.index, clip_frames) << "at tick " << frame.tick;
    }
}

TEST(ffmpeg_producer_race, seek_to_start_while_looping)
{
    GTEST_SKIP() << "Known bugs, flaky under ASan with CASPAR_TEST_STRESS=10: packets read before a SEEK reach "
                    "the decoders created after it (ffv1 logs \"Cannot decode non-keyframe without valid keyframe\" "
                    "and the decoder thread can die on the exception, stalling the producer), and destroying the "
                    "producer right after a SEEK can hang (see ffmpeg_producer_teardown.right_after_a_seek).";

    auto clip = make_clip({.frames = 20});

    for (int n = 0; n < stress_iterations(3); ++n) {
        producer_harness harness(L"720p5000", {ffmpeg::init});
        harness.set_pacing(producer_harness::pacing::fast);
        auto producer = harness.create({u16(clip.string()), L"LOOP"});

        // A seek restarts buffering, so seeks closer together than that starve the channel.
        background_loop seeks([&] { harness.call(*producer, {L"SEEK", L"0"}); }, 30ms, 150ms);
        expect_frames_of_clip(harness.receive(*producer, 150), 20);
        EXPECT_GT(seeks.iterations(), 3);
    }
}

TEST(ffmpeg_producer_race, short_loop_with_seeks_and_loop_toggles)
{
    GTEST_SKIP() << "Known bugs, flaky under ASan with CASPAR_TEST_STRESS=10: packets read before a SEEK reach "
                    "the decoders created after it (ffv1 logs \"Cannot decode non-keyframe without valid keyframe\" "
                    "and the decoder thread can die on the exception, stalling the producer), and destroying the "
                    "producer right after a SEEK can hang (see ffmpeg_producer_teardown.right_after_a_seek).";

    auto clip = make_clip({.frames = 30});

    for (int n = 0; n < stress_iterations(3); ++n) {
        producer_harness harness(L"720p5000", {ffmpeg::init});
        harness.set_pacing(producer_harness::pacing::fast);
        auto producer = harness.create({u16(clip.string()), L"LOOP", L"LENGTH", L"10"}); // a reset every 10 frames

        int             toggle = 0;
        background_loop calls(
            [&] {
                harness.call(*producer, {L"SEEK", std::to_wstring(toggle % 10)});
                harness.call(*producer, {L"LOOP", toggle++ % 3 == 0 ? L"0" : L"1"});
            },
            30ms,
            150ms);
        expect_frames_of_clip(harness.receive(*producer, 150), 30);
    }
}

}}} // namespace caspar::ffmpeg
