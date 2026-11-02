#pragma once

#include "fake_frame_factory.h"
#include "process.h"

#include <common/memory.h>

#include <core/frame/draw_frame.h>
#include <core/frame/frame.h>
#include <core/module_dependencies.h>
#include <core/producer/frame_producer.h>
#include <core/video_format.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace caspar { namespace test {

// One frame as the channel received it.
struct produced_frame
{
    core::draw_frame               frame;
    core::video_field              field;
    std::vector<core::const_frame> images;  // the frames inside `frame`, in draw order
    std::optional<int>             index;   // read_index() of the first image
    std::vector<std::int64_t>      samples; // ramp sample numbers in channel 0 of the audible images' audio
    int                            tick;    // the channel tick it came on
};

std::vector<std::optional<int>> indices(const std::vector<produced_frame>& frames);
std::vector<std::int64_t>       samples(const std::vector<produced_frame>& frames);

// Runs one producer the way a channel does, without a server, AMCP or accelerator.
//
// The harness sets up the registries a module's init() registers into, and creates producers
// through frame_producer_registry with the same parameters as AMCP's PLAY, so parameter parsing and
// the registry's wrapping are part of the test. Frames come from a fake_frame_factory.
class producer_harness
{
  public:
    using module_init = void (*)(const core::module_dependencies&);

    // `format` is the channel's video format, e.g. L"1080i5000".
    explicit producer_harness(const std::wstring& format = L"720p5000", std::vector<module_init> modules = {});

    // Checks that the producers left nothing behind, and fails the test if they did:
    // - every producer created here is destroyed within 10 s, including teardown a producer does on
    //   threads of its own;
    // - then, within 5 s, the process has no threads or open files it didn't have when the harness
    //   was constructed (Linux), and no frames from the harness's frame factory are referenced.
    // Release the producers and frames first: declare them after the harness.
    ~producer_harness();

    producer_harness(const producer_harness&)            = delete;
    producer_harness& operator=(const producer_harness&) = delete;

    // Creates a producer from AMCP-style parameters, e.g. {path, L"LOOP", L"SEEK", L"10"}.
    // Throws like PLAY does when nothing matches.
    //
    // Every call into a producer (create, receive, call) is guarded by a watchdog: one that hangs
    // for 30 s ends the test process with every thread's stack in the output folder.
    spl::shared_ptr<core::frame_producer> create(const std::vector<std::wstring>& params);

    // Ticks the channel until `count` frames have come out, alternating fields in interlaced
    // formats and asking for the format's audio cadence. A tick that gets an empty frame (the
    // producer isn't ready, or underflowed) counts as an underflow and the next tick follows.
    // Fails the test and returns what came out if the next frame takes longer than `timeout`.
    //
    // Ticks are paced at the channel's rate, as on air: a producer may behave differently when it
    // can't stay ahead (e.g. hold its last frame at a loop), so a faster clock tests something else.
    std::vector<produced_frame>
    receive(core::frame_producer& producer, int count, std::chrono::milliseconds timeout = std::chrono::seconds(5));

    // producer.call(params), waiting at most `timeout` for the reply. Fails the test on a timeout.
    std::wstring call(core::frame_producer&            producer,
                      const std::vector<std::wstring>& params,
                      std::chrono::milliseconds        timeout = std::chrono::seconds(5));

    enum class pacing
    {
        realtime, // one tick per frame duration (default)
        fast,     // the next tick as soon as the previous one returned
    };
    void set_pacing(pacing p) { pacing_ = p; }

    // Ticks that got an empty frame so far.
    int underflows() const { return underflows_; }

    const core::video_format_desc& format() const { return format_; }
    fake_frame_factory&            frame_factory() { return *frame_factory_; }

  private:
    core::video_format_repository                  format_repository_;
    core::video_format_desc                        format_;
    spl::shared_ptr<fake_frame_factory>            frame_factory_;
    spl::shared_ptr<core::cg_producer_registry>    cg_registry_;
    spl::shared_ptr<core::frame_producer_registry> producer_registry_;
    spl::shared_ptr<core::frame_consumer_registry> consumer_registry_;
    process_snapshot                               baseline_;
    pacing                                         pacing_ = pacing::realtime;
    std::chrono::steady_clock::time_point          next_tick_;
    int                                            tick_       = 0;
    int                                            underflows_ = 0;
};

}} // namespace caspar::test
