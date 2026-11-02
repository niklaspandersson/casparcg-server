#include "process.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef __linux__
#include <sys/prctl.h>
#include <unistd.h>
#endif

namespace caspar { namespace test {

namespace {

std::atomic<bool> unclean{false};

#ifdef __linux__
std::map<int, std::string> list(const std::filesystem::path& dir, bool threads)
{
    std::map<int, std::string> result;
    std::error_code            ec;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        int id = 0;
        try {
            id = std::stoi(entry.path().filename().string());
        } catch (...) {
            continue;
        }
        std::string what;
        if (threads) {
            std::ifstream(entry.path() / "comm") >> what;
        } else {
            what = std::filesystem::read_symlink(entry.path(), ec).string();
            // Gone by now, or the descriptor this listing reads the directory through.
            if (ec || what == "/proc/" + std::to_string(getpid()) + "/fd")
                continue;
        }
        result[id] = what;
    }
    return result;
}
#endif

} // namespace

process_snapshot process_snapshot::take()
{
    process_snapshot snapshot;
#ifdef __linux__
    snapshot.threads = list("/proc/self/task", true);
    snapshot.files   = list("/proc/self/fd", false);
#endif
    return snapshot;
}

process_snapshot added(const process_snapshot& before, const process_snapshot& after)
{
    process_snapshot result;
    for (auto& [tid, name] : after.threads) {
        if (!before.threads.count(tid))
            result.threads[tid] = name;
    }
    for (auto& [fd, target] : after.files) {
        auto it = before.files.find(fd);
        if (it == before.files.end() || it->second != target)
            result.files[fd] = target;
    }
    return result;
}

std::string describe(const process_snapshot& snapshot)
{
    std::ostringstream text;
    for (auto& [tid, name] : snapshot.threads)
        text << "\n  thread " << tid << " \"" << name << "\"";
    for (auto& [fd, target] : snapshot.files)
        text << "\n  fd " << fd << " -> " << target;
    return text.str();
}

std::string dump_thread_stacks(const std::filesystem::path& path)
{
#ifdef __linux__
    auto command = "gdb -q -batch -p " + std::to_string(getpid()) +
                   " -ex 'set pagination off' -ex 'thread apply all bt' > '" + path.string() + "' 2>&1";
    if (std::system("command -v gdb > /dev/null 2>&1") == 0 && std::system(command.c_str()) == 0)
        return "\n  thread stacks: " + path.string();
    return "\n  (install gdb to get thread stacks here)";
#else
    return "";
#endif
}

void allow_debugger()
{
#ifdef __linux__
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif
}

void mark_process_unclean() { unclean = true; }
bool process_is_unclean() { return unclean; }

}} // namespace caspar::test
