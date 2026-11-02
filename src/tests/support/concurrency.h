#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace caspar { namespace test {

// Runs `action` over and over on a thread of its own, with a random pause between `min_pause` and
// `max_pause` between runs, until destroyed (which waits for the current run to finish). Use it to have
// something happen concurrently with what the test does on its own thread, e.g. AMCP calls
// arriving while the channel receives frames:
//
//   background_loop seeks([&] { harness.call(*producer, {L"SEEK", L"0"}); }, 30ms, 150ms);
//   auto frames = harness.receive(*producer, 200);
class background_loop
{
  public:
    background_loop(std::function<void()>     action,
                    std::chrono::microseconds min_pause,
                    std::chrono::microseconds max_pause);
    ~background_loop();

    background_loop(const background_loop&)            = delete;
    background_loop& operator=(const background_loop&) = delete;

    int iterations() const { return iterations_; }

  private:
    std::atomic<bool> stop_{false};
    std::atomic<int>  iterations_{0};
    std::thread       thread_;
};

// How many times a stress test should repeat itself: `base`, multiplied by $CASPAR_TEST_STRESS when
// that is set (e.g. CASPAR_TEST_STRESS=50 for a long hunt for a rare race).
int stress_iterations(int base);

}} // namespace caspar::test
