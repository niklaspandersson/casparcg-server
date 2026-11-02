#include "output_dir.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace caspar { namespace test {

namespace {

// Parameterized test names contain '/', which would nest folders.
std::string folder_name(std::string name)
{
    for (auto& c : name) {
        if (c == '/' || c == '\\' || c == ':')
            c = '_';
    }
    return name;
}

} // namespace

std::filesystem::path output_path()
{
    auto info = ::testing::UnitTest::GetInstance()->current_test_info();
    if (!info)
        throw std::logic_error("caspar::test::output_dir() called outside a test");

    auto root = std::getenv("CASPAR_TEST_OUTPUT");
    auto dir  = std::filesystem::path(root && *root ? root : "test-output") / folder_name(info->test_suite_name()) /
               folder_name(info->name());
    return std::filesystem::absolute(dir);
}

std::filesystem::path output_dir()
{
    auto dir = output_path();
    std::filesystem::create_directories(dir);
    return dir;
}

}} // namespace caspar::test
