// What the harness reports when a test declares things in the wrong order. Uses core's colour
// producer, so it needs no module.

#include "producer_harness.h"

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <optional>

namespace caspar { namespace test { namespace {

TEST(producer_harness, says_so_when_a_producer_outlives_it)
{
    std::shared_ptr<core::frame_producer> producer; // declared before the harness: wrong
    std::optional<producer_harness>       harness(std::in_place);
    producer = harness->create({L"#FF000000"});

    EXPECT_NONFATAL_FAILURE(harness.reset(), "declare it after the harness");
}

TEST(producer_harness, says_so_when_received_frames_outlive_it)
{
    std::vector<produced_frame>     frames; // declared before the harness: wrong
    std::optional<producer_harness> harness(std::in_place);
    {
        auto producer = harness->create({L"#FF000000"});
        harness->set_pacing(producer_harness::pacing::fast);
        frames = harness->receive(*producer, 1);
    }

    EXPECT_NONFATAL_FAILURE(harness.reset(), "declare received frames after the harness");
}

}}} // namespace caspar::test
