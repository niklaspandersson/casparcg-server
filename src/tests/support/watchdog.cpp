#include "watchdog.h"
#include "output_dir.h"
#include "process.h"

#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

namespace caspar { namespace test {

namespace {

struct entry
{
    std::chrono::steady_clock::time_point deadline;
    std::string                           what;
    std::filesystem::path                 stacks;
};

struct watchdog
{
    std::mutex                          mutex;
    std::condition_variable             changed;
    std::map<unsigned long long, entry> entries;
    unsigned long long                  next_id = 0;
    std::thread                         thread;

    watchdog()
    {
        thread = std::thread([this] { run(); });
        thread.detach(); // lives as long as the process
    }

    void run()
    {
        std::unique_lock<std::mutex> lock(mutex);
        while (true) {
            if (entries.empty()) {
                changed.wait(lock);
                continue;
            }
            // Copies: the entry may be erased while this waits.
            auto first = entries.begin();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->second.deadline < first->second.deadline)
                    first = it;
            }
            const auto id       = first->first;
            const auto deadline = first->second.deadline;
            if (changed.wait_until(lock, deadline) == std::cv_status::timeout && entries.count(id) &&
                std::chrono::steady_clock::now() >= deadline) {
                auto hung = entries[id];
                lock.unlock();
                std::cerr << "\n[watchdog] " << hung.what << " did not return; ending the test process."
                          << dump_thread_stacks(hung.stacks) << std::endl;
                std::_Exit(3);
            }
        }
    }
};

// Never destroyed: its thread waits on it until the process ends, and destroying a condition
// variable with a waiter blocks the exit.
watchdog& instance()
{
    static auto dog = new watchdog;
    return *dog;
}

} // namespace

watchdog_guard::watchdog_guard(std::string what, std::chrono::milliseconds timeout)
{
    auto&                       dog = instance();
    std::lock_guard<std::mutex> lock(dog.mutex);
    id_              = dog.next_id++;
    dog.entries[id_] = {std::chrono::steady_clock::now() + timeout,
                        std::move(what) + " (after " + std::to_string(timeout.count()) + " ms)",
                        output_dir() / "stacks.txt"};
    dog.changed.notify_all();
}

watchdog_guard::~watchdog_guard()
{
    auto&                       dog = instance();
    std::lock_guard<std::mutex> lock(dog.mutex);
    dog.entries.erase(id_);
    dog.changed.notify_all();
}

void start_watchdog() { instance(); }

}} // namespace caspar::test
