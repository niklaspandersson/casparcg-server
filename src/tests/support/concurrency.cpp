#include "concurrency.h"

#include <algorithm>
#include <cstdlib>
#include <random>
#include <string>

namespace caspar { namespace test {

background_loop::background_loop(std::function<void()>     action,
                                 std::chrono::microseconds min_pause,
                                 std::chrono::microseconds max_pause)
    : thread_([this, action = std::move(action), min_pause, max_pause] {
        std::mt19937                       random(std::random_device{}());
        std::uniform_int_distribution<int> pause(static_cast<int>(min_pause.count()),
                                                 static_cast<int>(max_pause.count()));
        while (!stop_) {
            action();
            ++iterations_;
            std::this_thread::sleep_for(std::chrono::microseconds(pause(random)));
        }
    })
{
}

background_loop::~background_loop()
{
    stop_ = true;
    thread_.join();
}

int stress_iterations(int base)
{
    auto factor = std::getenv("CASPAR_TEST_STRESS");
    return factor && *factor ? base * std::max(1, std::stoi(factor)) : base;
}

}} // namespace caspar::test
