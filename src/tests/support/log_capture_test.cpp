#include "log_capture.h"
#include "output_dir.h"

#include <common/log.h>

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <thread>

namespace caspar { namespace test { namespace {

TEST(log_capture, records_the_running_tests_log)
{
    CASPAR_LOG(info) << L"hello from the test";

    auto records = log_records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].level, boost::log::trivial::info);
    EXPECT_EQ(records[0].message, L"hello from the test");
}

TEST(log_capture, writes_the_log_to_the_output_dir)
{
    CASPAR_LOG(warning) << L"written to disk";

    std::ifstream     file(output_dir() / "log.txt");
    std::stringstream contents;
    contents << file.rdbuf();
    EXPECT_EQ(contents.str(), "[warning] written to disk\n");
}

TEST(log_capture, fails_on_an_unexpected_error)
{
    EXPECT_NONFATAL_FAILURE(
        {
            CASPAR_LOG(error) << L"something broke";
            check_log_errors();
        },
        "Unexpected error logged: something broke");
}

TEST(log_capture, fails_on_an_error_from_another_thread)
{
    EXPECT_NONFATAL_FAILURE(
        {
            std::thread([] { CASPAR_LOG(error) << L"background failure"; }).join();
            check_log_errors();
        },
        "background failure");
}

TEST(log_capture, allowed_errors_pass)
{
    allow_log_errors(L"expected");
    CASPAR_LOG(error) << L"an expected failure";
}

TEST(log_capture, reports_each_error_once)
{
    EXPECT_NONFATAL_FAILURE(
        {
            CASPAR_LOG(error) << L"first";
            check_log_errors();
            check_log_errors();
        },
        "first");
}

}}} // namespace caspar::test
