#include "watchdog.h"

#include <gtest/gtest.h>

#include <thread>

namespace caspar { namespace test { namespace {

using namespace std::chrono_literals;

TEST(watchdog, ends_the_process_when_a_guarded_call_hangs)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            watchdog_guard guard("a call that hangs", 100ms);
            std::this_thread::sleep_for(10s);
        },
        ::testing::ExitedWithCode(3),
        "a call that hangs \\(after 100 ms\\) did not return");
}

TEST(watchdog, stays_quiet_when_the_call_returns_in_time)
{
    {
        watchdog_guard guard("a quick call", 500ms);
    }
    std::this_thread::sleep_for(700ms);
}

}}} // namespace caspar::test
