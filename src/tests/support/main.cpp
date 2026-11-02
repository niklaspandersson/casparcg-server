#include "environment.h"
#include "log_capture.h"
#include "process.h"

#include <gmock/gmock.h>

#include <cstdlib>

int main(int argc, char** argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    caspar::test::install_log_capture();
    caspar::test::configure_environment();
    caspar::test::allow_debugger();

    int result = RUN_ALL_TESTS();

    // Threads a failed teardown left running could crash in static destructors; don't run them.
    if (caspar::test::process_is_unclean())
        std::_Exit(result == 0 ? 1 : result);
    return result;
}

// Default options for sanitizer builds of the tests (CASPARCG_SANITIZE), so a test executable run
// directly behaves as it does under CTest. The sanitizer runtimes call these hooks at startup;
// variables like LSAN_OPTIONS still override them. They live here because main() is always linked.

#ifdef CASPAR_TEST_SANITIZER_DIR

#define CASPAR_TEST_STRINGIFY_(x) #x
#define CASPAR_TEST_STRINGIFY(x) CASPAR_TEST_STRINGIFY_(x)
#define CASPAR_TEST_DIR CASPAR_TEST_STRINGIFY(CASPAR_TEST_SANITIZER_DIR)

extern "C" {

// Known leaks are listed in lsan.supp. Slow unwinding gives full allocation stacks through
// libraries built without frame pointers (FFmpeg).
const char* __lsan_default_options() { return "suppressions=" CASPAR_TEST_DIR "/lsan.supp:fast_unwind_on_malloc=0"; }

// UBSan only prints by default; ending the test makes a finding fail it.
const char* __ubsan_default_options()
{
    return "halt_on_error=1:print_stacktrace=1:suppressions=" CASPAR_TEST_DIR "/ubsan.supp";
}
}

#endif
