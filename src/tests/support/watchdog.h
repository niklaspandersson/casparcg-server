#pragma once

#include <chrono>
#include <string>

namespace caspar { namespace test {

// Guards a call into the code under test that might never return. If the guard still exists after
// `timeout`, the watchdog writes every thread's stack to the test's output folder, prints what hung
// and ends the process: a hung call can't be abandoned safely. CTest runs each test in its own
// process, so only that test fails.
class watchdog_guard
{
  public:
    watchdog_guard(std::string what, std::chrono::milliseconds timeout);
    ~watchdog_guard();

    watchdog_guard(const watchdog_guard&)            = delete;
    watchdog_guard& operator=(const watchdog_guard&) = delete;

  private:
    unsigned long long id_;
};

// Starts the watchdog's thread, so it exists before a test takes its thread baseline.
void start_watchdog();

}} // namespace caspar::test
