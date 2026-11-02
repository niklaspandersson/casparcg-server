#pragma once

#include "fake_frame_factory.h"
#include "leak_check.h"

#include <common/bit_depth.h>
#include <common/memory.h>

#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>
#include <core/module_dependencies.h>
#include <core/video_format.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace caspar { namespace test {

// Feeds one consumer the way a channel's output does, without a server, AMCP or accelerator.
//
// The harness sets up the registries a module's init() registers into and creates consumers through
// frame_consumer_registry with the same parameters as AMCP's ADD, so parameter parsing and the
// registry's wrapping are part of the test. It sends what the mixer sends: full-size BGRA host frames
// (16 bits per component in a 16-bit channel) with the channel's 16 audio channels. Each frame
// carries a stamped index (read_index()) and the audio ramp (ramp_samples()), counting up from 0
// across everything sent, so a test checks exactly what the consumer wrote.
//
// Use it for any module's consumer: what it writes or sends, its parameters, how it handles
// failure, and that it tears down cleanly. It doesn't model the mixer's output (no transforms,
// keying or real rendering), several consumers sharing a channel's frames, or AMCP's ADD/REMOVE:
// test those end to end (src/tests/e2e).
class consumer_harness
{
  public:
    using module_init = void (*)(const core::module_dependencies&);

    // `format` is the channel's video format, e.g. L"1080i5000".
    explicit consumer_harness(const std::wstring&      format  = L"720p5000",
                              std::vector<module_init> modules = {},
                              common::bit_depth        depth   = common::bit_depth::bit8);

    // Checks that every consumer was closed (or released) and left no threads, open files or frames
    // behind, and fails the test if not. Declare consumers after the harness.
    ~consumer_harness();

    consumer_harness(const consumer_harness&)            = delete;
    consumer_harness& operator=(const consumer_harness&) = delete;

    // Creates a consumer from AMCP-style parameters, e.g. {L"FILE", path, L"-codec:v", L"ffv1"}, and
    // initializes it for the channel. Throws like ADD does when nothing matches.
    spl::shared_ptr<core::frame_consumer> create(const std::vector<std::wstring>& params);

    // Sends the next `count` frames as the channel would: one per tick, or two per tick (fields a
    // and b, each its own frame and index) in interlaced formats, with the format's audio cadence,
    // and waits for each send()'s future. Returns false as soon as send() throws or its future says
    // false, which is how a consumer asks the channel to remove it; error() then says why.
    bool send(core::frame_consumer& consumer, int count);

    // Releases the consumer and waits until it is destroyed, so whatever it writes is complete
    // (a consumer may flush a queue of frames first). Fails the test if that takes longer than
    // `timeout`, or if other consumers are still open (it waits for everything consumers started to
    // go away).
    void close(spl::shared_ptr<core::frame_consumer>& consumer,
               std::chrono::milliseconds              timeout = std::chrono::seconds(30));

    // Why the last failed send() failed.
    const std::string& error() const { return error_; }

    // Frames and samples (per channel) sent so far; the next frame's index and first sample.
    int          frames_sent() const { return frames_sent_; }
    std::int64_t samples_sent() const { return samples_sent_; }

    // A frame like send() sends, for tests that call the consumer directly.
    core::const_frame make_frame(int index, std::int64_t first_sample, int samples);

    enum class pacing
    {
        realtime, // one tick per frame duration (default)
        fast,     // the next tick as soon as the previous one returned
    };
    void set_pacing(pacing p) { pacing_ = p; }

    const core::video_format_desc& format() const { return format_; }

  private:
    core::video_format_repository                    format_repository_;
    core::video_format_desc                          format_;
    common::bit_depth                                depth_;
    spl::shared_ptr<fake_frame_factory>              frame_factory_;
    spl::shared_ptr<core::cg_producer_registry>      cg_registry_;
    spl::shared_ptr<core::frame_producer_registry>   producer_registry_;
    spl::shared_ptr<core::frame_consumer_registry>   consumer_registry_;
    std::vector<std::weak_ptr<core::frame_consumer>> consumers_;
    std::optional<leak_check>                        leak_check_;
    pacing                                           pacing_ = pacing::realtime;
    std::chrono::steady_clock::time_point            next_tick_;
    int                                              frames_sent_  = 0;
    std::int64_t                                     samples_sent_ = 0;
    std::string                                      error_;
};

}} // namespace caspar::test
