#include <modules/ffmpeg/ffmpeg.h>

#include <consumer_harness.h>
#include <log_capture.h>
#include <output_dir.h>
#include <runs.h>
#include <test_media.h>

#include <common/utf.h>

#include <gtest/gtest.h>

#include <fstream>

namespace caspar { namespace ffmpeg { namespace {

using namespace caspar::test;

std::wstring path(const std::filesystem::path& p) { return u16(p.string()); }

// Lossless video and audio, so the stamps and the ramp come back exactly.
std::vector<std::wstring> lossless_file(const std::filesystem::path& file)
{
    return {L"FILE", path(file), L"-codec:v", L"ffv1", L"-codec:a", L"pcm_s32le"};
}

TEST(ffmpeg_consumer, writes_every_frame_and_its_audio)
{
    consumer_harness harness(L"720p5000", {ffmpeg::init});
    auto             file     = output_dir() / "out.mov";
    auto             consumer = harness.create(lossless_file(file));

    ASSERT_TRUE(harness.send(*consumer, 50)) << harness.error();
    harness.close(consumer);

    auto written = decode_clip(file);
    EXPECT_EQ(runs(written.indices), "0..49");
    EXPECT_EQ(runs(written.audio_samples), "0..47999");
    EXPECT_EQ(written.audio_channels, 16);
}

TEST(ffmpeg_consumer, writes_mp4_with_the_default_codecs)
{
    consumer_harness harness(L"720p5000", {ffmpeg::init});
    auto             file     = output_dir() / "out.mp4";
    auto             consumer = harness.create({L"FILE", path(file)});

    ASSERT_TRUE(harness.send(*consumer, 50)) << harness.error();
    harness.close(consumer);

    auto written = decode_clip(file); // H.264 and AAC: the stamps survive, the ramp doesn't
    EXPECT_EQ(runs(written.indices), "0..49");
    EXPECT_EQ(written.audio_channels, 16);
}

TEST(ffmpeg_consumer, interlaced_channel_writes_every_field)
{
    // Each field is its own send() and is written as a frame of its own; see known-bugs.md on
    // whether an interlaced channel should be recorded as interlaced frames instead.
    consumer_harness harness(L"1080i5000", {ffmpeg::init});
    auto             file     = output_dir() / "out.mov";
    auto             consumer = harness.create(lossless_file(file));

    ASSERT_TRUE(harness.send(*consumer, 20)) << harness.error();
    harness.close(consumer);

    auto written = decode_clip(file);
    EXPECT_EQ(runs(written.indices), "0..19");
    EXPECT_EQ(runs(written.audio_samples), "0..19199") << "960 samples per field";
}

TEST(ffmpeg_consumer, sixteen_bit_channel)
{
    consumer_harness harness(L"720p5000", {ffmpeg::init}, common::bit_depth::bit16);
    auto             file     = output_dir() / "out.mov";
    auto             consumer = harness.create(lossless_file(file));

    ASSERT_TRUE(harness.send(*consumer, 10)) << harness.error();
    harness.close(consumer);

    EXPECT_EQ(runs(decode_clip(file).indices), "0..9");
}

TEST(ffmpeg_consumer, close_writes_everything_already_sent)
{
    // Sent faster than real time, so frames are still queued when the consumer is closed.
    consumer_harness harness(L"720p5000", {ffmpeg::init});
    harness.set_pacing(consumer_harness::pacing::fast);
    auto file     = output_dir() / "out.mov";
    auto consumer = harness.create(lossless_file(file));

    ASSERT_TRUE(harness.send(*consumer, 30)) << harness.error();
    harness.close(consumer);

    EXPECT_EQ(runs(decode_clip(file).indices), "0..29");
}

TEST(ffmpeg_consumer, closed_before_any_frame)
{
    consumer_harness harness(L"720p5000", {ffmpeg::init});
    auto             consumer = harness.create(lossless_file(output_dir() / "out.mov"));
    harness.close(consumer);
}

TEST(ffmpeg_consumer, unwritable_path_fails_send)
{
    consumer_harness harness(L"720p5000", {ffmpeg::init});
    std::ofstream(output_dir() / "not-a-folder") << "a file";
    auto consumer = harness.create(lossless_file(output_dir() / "not-a-folder" / "out.mov"));

    // The output is opened on the consumer's own thread; a failure surfaces on a later send().
    EXPECT_FALSE(harness.send(*consumer, 50)) << "the consumer kept accepting frames";
    EXPECT_NE(harness.error(), "");
}

TEST(ffmpeg_consumer_known_issues, reports_frames_it_drops)
{
    GTEST_SKIP() << "Known issue: when the encoder can't keep up, send() drops frames once the queue (64) is "
                    "full and only tags the diagnostics graph; the recording has gaps and nothing is logged. "
                    "See known-bugs.md.";

    consumer_harness harness(L"1080p5000", {ffmpeg::init});
    harness.set_pacing(consumer_harness::pacing::fast); // faster than ProRes encodes 1080p
    auto file     = output_dir() / "out.mov";
    auto consumer = harness.create({L"FILE", path(file), L"-codec:v", L"prores_ks", L"-codec:a", L"pcm_s32le"});

    ASSERT_TRUE(harness.send(*consumer, 300)) << harness.error();
    harness.close(consumer);

    if (decode_clip(file).indices.size() < 300) {
        bool warned = false;
        for (auto& record : log_records())
            warned = warned || (record.level >= boost::log::trivial::warning &&
                                record.message.find(L"drop") != std::wstring::npos);
        EXPECT_TRUE(warned) << "frames were dropped without a warning";
    }
}

TEST(ffmpeg_consumer_known_issues, no_warning_about_its_own_default_options)
{
    GTEST_SKIP() << "Known issue: the consumer adds preset:v=veryfast when the container's default video codec is "
                    "H.264, even when another codec is chosen, then warns that the option was unused. See "
                    "known-bugs.md.";

    consumer_harness harness(L"720p5000", {ffmpeg::init});
    auto             consumer = harness.create(lossless_file(output_dir() / "out.mov"));
    ASSERT_TRUE(harness.send(*consumer, 1)) << harness.error();
    harness.close(consumer);

    for (auto& record : log_records())
        EXPECT_EQ(record.message.find(L"Unused option preset"), std::wstring::npos) << u8(record.message);
}

}}} // namespace caspar::ffmpeg
