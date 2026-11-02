#include "producer_harness.h"
#include "leak_check.h"
#include "output_dir.h"
#include "test_media.h"
#include "watchdog.h"

#include <core/consumer/frame_consumer_registry.h>
#include <core/frame/frame_transform.h>
#include <core/frame/frame_visitor.h>
#include <core/producer/cg_proxy.h>
#include <core/producer/frame_producer_registry.h>

#include <gtest/gtest.h>

#include <common/utf.h>

#include <core/frame/pixel_format.h>

#include <algorithm>
#include <mutex>
#include <thread>

namespace caspar { namespace test {

namespace {

// The channel's audio has 16 channels, whatever the source had.
constexpr int channel_audio_channels = 16;

// Collects the images in a draw_frame and whether each one is heard: its audio volume, multiplied
// down the transform stack, is above zero (draw_frame::still() mutes a held frame).
struct image_collector : core::frame_visitor
{
    std::vector<core::const_frame> images;
    std::vector<bool>              audible;
    std::vector<double>            volumes{1.0};

    void push(const core::frame_transform& transform) override
    {
        volumes.push_back(volumes.back() * transform.audio_transform.volume);
    }
    void visit(const core::const_frame& frame) override
    {
        images.push_back(frame);
        audible.push_back(volumes.back() > 0.0);
    }
    void pop() override { volumes.pop_back(); }
};

constexpr auto hang_timeout = std::chrono::seconds(30);

// The registry destroys producers on a thread it starts on first use and keeps; start it before the
// harness takes its baseline.
void warm_up_producer_destroyer(const core::frame_producer_dependencies& dependencies,
                                const core::frame_producer_registry&     registry)
{
    static std::once_flag once;
    std::call_once(once, [&] {
        registry.create_producer(dependencies, std::vector<std::wstring>{L"#FF000000"}); // destroyed right away
    });
}

} // namespace

std::vector<std::optional<int>> indices(const std::vector<produced_frame>& frames)
{
    std::vector<std::optional<int>> result;
    for (auto& frame : frames)
        result.push_back(frame.index);
    return result;
}

std::vector<std::int64_t> samples(const std::vector<produced_frame>& frames)
{
    std::vector<std::int64_t> result;
    for (auto& frame : frames)
        result.insert(result.end(), frame.samples.begin(), frame.samples.end());
    return result;
}

producer_harness::producer_harness(const std::wstring& format, std::vector<module_init> modules)
    : format_(format_repository_.find(format))
    , frame_factory_(spl::make_shared<fake_frame_factory>())
    , cg_registry_(spl::make_shared<core::cg_producer_registry>())
    , producer_registry_(spl::make_shared<core::frame_producer_registry>())
    , consumer_registry_(spl::make_shared<core::frame_consumer_registry>())
{
    if (format_.format == core::video_format::invalid)
        throw std::invalid_argument("producer_harness: unknown format");

    warm_up_process();
    warm_up_producer_destroyer(core::frame_producer_dependencies(
                                   frame_factory_, {}, format_repository_, format_, producer_registry_, cg_registry_),
                               *producer_registry_);

    core::module_dependencies dependencies(cg_registry_, producer_registry_, consumer_registry_, nullptr);
    for (auto init : modules)
        init(dependencies);

    leak_check_.emplace();
}

producer_harness::~producer_harness()
{
    using namespace std::chrono_literals;

    // Still referenced by the test, so it can't be destroyed yet: a declaration order mistake, not a hang.
    for (auto& producer : producers_) {
        if (!producer.expired()) {
            ADD_FAILURE() << "A producer is still referenced when the harness is destroyed; declare it after the "
                             "harness";
            return;
        }
    }

    // Every producer holds the frame factory until its teardown is complete, so the harness holding
    // the only reference means they are all gone.
    if (!wait_until([&] { return frame_factory_.use_count() == 1; }, 10s)) {
        ADD_FAILURE() << "A producer was not destroyed within 10 s of the test releasing it"
                      << dump_thread_stacks(output_dir() / "stacks.txt");
        mark_process_unclean();
        return;
    }

    leak_check_->expect_none_left("the producers were destroyed");

    EXPECT_EQ(frame_factory_->frames_alive(), 0)
        << "frames still referenced after the producers were destroyed; declare received frames after the harness";
}

spl::shared_ptr<core::frame_producer> producer_harness::create(const std::vector<std::wstring>& params)
{
    watchdog_guard                    guard("producer_harness::create", hang_timeout);
    core::frame_producer_dependencies dependencies(
        frame_factory_, {}, format_repository_, format_, producer_registry_, cg_registry_);
    auto producer = producer_registry_->create_producer(dependencies, params);
    producers_.push_back(producer);
    return producer;
}

std::vector<produced_frame>
producer_harness::receive(core::frame_producer& producer, int count, std::chrono::milliseconds timeout)
{
    std::vector<produced_frame> result;
    auto                        deadline = std::chrono::steady_clock::now() + timeout;

    // A tick lasts one field in interlaced formats.
    const auto tick_duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / format_.fps));
    if (next_tick_ == std::chrono::steady_clock::time_point{})
        next_tick_ = std::chrono::steady_clock::now();

    while (static_cast<int>(result.size()) < count) {
        if (pacing_ == pacing::realtime) {
            std::this_thread::sleep_until(next_tick_);
            // After a stall (a slow call()), carry on from now rather than catching up in a burst.
            next_tick_ = std::max(next_tick_ + tick_duration, std::chrono::steady_clock::now());
        }

        auto tick       = tick_++;
        auto field      = format_.field_count == 2 ? (tick % 2 == 0 ? core::video_field::a : core::video_field::b)
                                                   : core::video_field::progressive;
        auto nb_samples = format_.audio_cadence[tick % format_.audio_cadence.size()];

        core::draw_frame frame;
        {
            watchdog_guard guard("producer.receive()", hang_timeout);
            frame = producer.receive(field, nb_samples);
        }
        if (!frame) {
            ++underflows_;
            if (std::chrono::steady_clock::now() > deadline) {
                ADD_FAILURE() << "No frame from " << u8(producer.print()) << " within " << timeout.count()
                              << " ms after " << result.size() << " of " << count << " frames"
                              << dump_thread_stacks(output_dir() / "stacks.txt");
                break;
            }
            if (pacing_ == pacing::fast)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        image_collector collector;
        frame.accept(collector);

        produced_frame produced{frame, field, std::move(collector.images), {}, {}, tick};
        for (std::size_t n = 0; n < produced.images.size(); ++n) {
            auto& image = produced.images[n];
            if (!produced.index && image.pixel_format_desc().format != core::pixel_format::invalid)
                produced.index = read_index(image);
            if (collector.audible[n]) {
                auto ramp = ramp_samples(image, channel_audio_channels);
                produced.samples.insert(produced.samples.end(), ramp.begin(), ramp.end());
            }
        }
        result.push_back(std::move(produced));
        deadline = std::chrono::steady_clock::now() + timeout;
    }
    return result;
}

std::wstring producer_harness::call(core::frame_producer&            producer,
                                    const std::vector<std::wstring>& params,
                                    std::chrono::milliseconds        timeout)
{
    std::future<std::wstring> reply;
    {
        watchdog_guard guard("producer.call()", hang_timeout);
        reply = producer.call(params);
    }
    if (reply.wait_for(timeout) != std::future_status::ready) {
        ADD_FAILURE() << "call() did not reply within " << timeout.count() << " ms"
                      << dump_thread_stacks(output_dir() / "stacks.txt");
        return {};
    }
    return reply.get();
}

}} // namespace caspar::test
