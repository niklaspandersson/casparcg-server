#include "environment.h"
#include "log_capture.h"

#include <gmock/gmock.h>

int main(int argc, char** argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    caspar::test::install_log_capture();
    caspar::test::configure_environment();
    return RUN_ALL_TESTS();
}
