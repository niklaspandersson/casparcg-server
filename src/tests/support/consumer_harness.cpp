#include "consumer_harness.h"
#include "output_dir.h"
#include "test_media.h"
#include "watchdog.h"

#include <common/except.h>
#include <common/utf.h>

#include <core/consumer/channel_info.h>
#include <core/consumer/frame_consumer_registry.h>
#include <core/frame/pixel_format.h>
#include <core/producer/cg_proxy.h>
#include <core/producer/frame_producer_registry.h>

#include <gtest/gtest.h>

#include <boost/exception/diagnostic_information.hpp>

#include <thread>

namespace caspar { namespace test {

namespace {

constexpr auto hang_timeout = std::chrono::seconds(30);
constexpr int  grey         = 128;

} // namespace

consumer_harness::consumer_harness(const std::wstring&      format,
                                   std::vector<module_init> modules,
                                   common::bit_depth        depth)
    : format_(format_repository_.find(format))
    , depth_(depth)
    , frame_factory_(spl::make_shared<fake_frame_factory>())
    , cg_registry_(spl::make_shared<core::cg_producer_registry>())
    , producer_registry_(spl::make_shared<core::frame_producer_registry>())
    , consumer_registry_(spl::make_shared<core::frame_consumer_registry>())
{
    if (format_.format == core::video_format::invalid)
        throw std::invalid_argument("consumer_harness: unknown format");

    warm_up_process();

    core::module_dependencies dependencies(cg_registry_, producer_registry_, consumer_registry_, nullptr);
    for (auto init : modules)
        init(dependencies);

    leak_check_.emplace();
}

consumer_harness::~consumer_harness()
{
    for (auto& consumer : consumers_) {
        if (!consumer.expired()) {
            ADD_FAILURE() << "A consumer is still referenced when the harness is destroyed; declare it after the "
                             "harness, or close() it";
            mark_process_unclean();
            return;
        }
    }

    // Released consumers are destroyed on threads of their own (the registry's destroy proxy).
    if (leak_check_->expect_none_left("the consumers were released", std::chrono::seconds(10)))
        EXPECT_EQ(frame_factory_->frames_alive(), 0) << "frames still referenced after the consumers were destroyed";
}

spl::shared_ptr<core::frame_consumer> consumer_harness::create(const std::vector<std::wstring>& params)
{
    watchdog_guard     guard("consumer_harness::create", hang_timeout);
    core::channel_info channel_info(1, depth_, core::color_space::bt709);

    auto consumer = consumer_registry_->create_consumer(params, format_repository_, {}, channel_info);
    consumer->initialize(format_, channel_info, 0);
    consumers_.push_back(consumer);
    return consumer;
}

core::const_frame consumer_harness::make_frame(int index, std::int64_t first_sample, int samples)
{
    const bool              deep = depth_ != common::bit_depth::bit8;
    core::pixel_format_desc desc(core::pixel_format::bgra);
    desc.planes.emplace_back(
        format_.width, format_.height, 4, deep ? common::bit_depth::bit16 : common::bit_depth::bit8);

    auto frame = frame_factory_->create_frame(this, desc, deep ? common::bit_depth::bit16 : common::bit_depth::bit8);

    // The stamp drawn in luma, then copied into B, G and R.
    std::vector<std::uint8_t> luma(static_cast<std::size_t>(format_.width) * format_.height, grey);
    stamp_index(luma.data(), format_.width, format_.height, format_.width, index);

    auto pixels = frame.image_data(0).data();
    for (std::size_t n = 0; n < luma.size(); ++n) {
        for (int c = 0; c < 4; ++c) {
            int value = c == 3 ? 255 : luma[n];
            if (deep)
                reinterpret_cast<std::uint16_t*>(pixels)[n * 4 + c] = static_cast<std::uint16_t>(value << 8 | value);
            else
                pixels[n * 4 + c] = static_cast<std::uint8_t>(value);
        }
    }

    std::vector<std::int32_t> audio(static_cast<std::size_t>(samples) * format_.audio_channels);
    for (int s = 0; s < samples; ++s) {
        for (int c = 0; c < format_.audio_channels; ++c)
            audio[static_cast<std::size_t>(s) * format_.audio_channels + c] =
                static_cast<std::int32_t>(((first_sample + s) << 8) | c);
    }
    frame.audio_data() = array<std::int32_t>(std::move(audio));

    return core::const_frame(std::move(frame));
}

bool consumer_harness::send(core::frame_consumer& consumer, int count)
{
    const bool interlaced    = format_.field_count == 2;
    const auto tick_duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / format_.hz));
    if (next_tick_ == std::chrono::steady_clock::time_point{})
        next_tick_ = std::chrono::steady_clock::now();

    for (int n = 0; n < count; ++n) {
        const bool first_of_tick = !interlaced || frames_sent_ % 2 == 0;
        if (first_of_tick && pacing_ == pacing::realtime) {
            std::this_thread::sleep_until(next_tick_);
            next_tick_ = std::max(next_tick_ + tick_duration, std::chrono::steady_clock::now());
        }

        const auto field   = !interlaced             ? core::video_field::progressive
                             : frames_sent_ % 2 == 0 ? core::video_field::a
                                                     : core::video_field::b;
        const int  samples = format_.audio_cadence[frames_sent_ % format_.audio_cadence.size()];
        auto       frame   = make_frame(frames_sent_, samples_sent_, samples);
        ++frames_sent_;
        samples_sent_ += samples;

        try {
            watchdog_guard guard("consumer.send()", hang_timeout);
            if (!consumer.send(field, frame).get()) {
                error_ = "send() returned false";
                return false;
            }
        } catch (...) {
            error_ = boost::current_exception_diagnostic_information();
            return false;
        }
    }
    return true;
}

void consumer_harness::close(spl::shared_ptr<core::frame_consumer>& consumer, std::chrono::milliseconds timeout)
{
    std::weak_ptr<core::frame_consumer> weak = consumer;
    consumer                                 = core::frame_consumer::empty();
    if (!weak.expired())
        ADD_FAILURE() << "close(): the consumer is still referenced elsewhere";

    for (auto& other : consumers_) {
        if (!other.expired()) {
            ADD_FAILURE() << "close() waits for everything consumers started to go away, so it needs the other "
                             "consumers closed first";
            return;
        }
    }

    // The registry's destroy proxy destroys the consumer on a thread of its own, which flushes and
    // closes its output; the consumer is gone when everything it started is.
    leak_check_->expect_none_left("the consumer was closed", timeout);
}

}} // namespace caspar::test
