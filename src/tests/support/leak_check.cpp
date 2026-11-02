#include "leak_check.h"
#include "output_dir.h"
#include "watchdog.h"

#include <gtest/gtest.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <mutex>
#include <thread>

namespace caspar { namespace test {

void warm_up_process()
{
    static std::once_flag once;
    std::call_once(once, [] {
        start_watchdog();
        tbb::parallel_for(0, 2000, [](int) { std::this_thread::sleep_for(std::chrono::microseconds(500)); });
    });
}

bool wait_until(const std::function<bool()>& done, std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

leak_check::leak_check()
    : baseline_(process_snapshot::take())
{
}

process_snapshot leak_check::left() const
{
    // The log capture opens the test's log file during the test; it isn't the subject's.
    const auto log_file = (output_path() / "log.txt").string();
    auto       result   = added(baseline_, process_snapshot::take());
    std::erase_if(result.files, [&](auto& file) { return file.second == log_file; });
    return result;
}

bool leak_check::expect_none_left(const std::string& what, std::chrono::milliseconds timeout)
{
    if (wait_until([&] { return left().threads.empty() && left().files.empty(); }, timeout))
        return true;

    ADD_FAILURE() << "Left behind " << timeout.count() << " ms after " << what << ":" << describe(left())
                  << dump_thread_stacks(output_dir() / "stacks.txt");
    mark_process_unclean();
    return false;
}

}} // namespace caspar::test
