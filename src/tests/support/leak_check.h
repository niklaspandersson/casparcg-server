#pragma once

#include "process.h"

#include <chrono>
#include <functional>
#include <string>

namespace caspar { namespace test {

// Starts the threads that live as long as the process once something first needs them (TBB's
// workers, the watchdog), so they are part of every baseline. Idempotent.
void warm_up_process();

// Polls `done` every few milliseconds until it returns true or `timeout` passes.
bool wait_until(const std::function<bool()>& done, std::chrono::milliseconds timeout);

// Takes a baseline of the process's threads and open files when constructed, and checks later that
// whatever was started since has gone away. The harnesses use it to check teardown.
class leak_check
{
  public:
    leak_check();

    // Waits up to `timeout` for the threads and files that weren't there at construction to go away.
    // If they don't, fails the test, listing them (`what` says whose they are) and dumping every
    // thread's stack, marks the process unclean and returns false.
    bool expect_none_left(const std::string& what, std::chrono::milliseconds timeout = std::chrono::seconds(5));

    // What is there now that wasn't at construction, leaving out the test's own log file.
    process_snapshot left() const;

  private:
    process_snapshot baseline_;
};

}} // namespace caspar::test
