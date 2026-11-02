#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace caspar { namespace test {

// The threads and open file descriptors of this process, for checking that a test's subject leaves
// nothing behind. Linux only; elsewhere both are empty, so the checks pass trivially.
struct process_snapshot
{
    std::map<int, std::string> threads; // tid -> name
    std::map<int, std::string> files;   // fd -> what it refers to

    static process_snapshot take();
};

// What `after` has that `before` doesn't.
process_snapshot added(const process_snapshot& before, const process_snapshot& after);

std::string describe(const process_snapshot& snapshot);

// Writes every thread's stack to `path` (with gdb, when installed) and returns a line saying where
// they are, to append to a failure message.
std::string dump_thread_stacks(const std::filesystem::path& path);

// Lets a debugger attach to this process (Linux's ptrace scope), for dump_thread_stacks().
void allow_debugger();

// For a failure that may leave threads running, such as a teardown that never finished: main()
// then ends the process without running static destructors, which those threads could still use.
void mark_process_unclean();
bool process_is_unclean();

}} // namespace caspar::test
