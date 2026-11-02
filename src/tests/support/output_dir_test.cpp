#include "output_dir.h"

#include <gtest/gtest.h>

namespace caspar { namespace test { namespace {

TEST(output_dir, is_named_after_the_test)
{
    auto dir = output_dir();
    EXPECT_TRUE(std::filesystem::is_directory(dir));
    EXPECT_EQ(dir.filename(), "is_named_after_the_test");
    EXPECT_EQ(dir.parent_path().filename(), "output_dir");
}

TEST(output_dir, output_path_does_not_create_the_folder)
{
    EXPECT_FALSE(std::filesystem::exists(output_path()));
    output_dir();
    EXPECT_TRUE(std::filesystem::exists(output_path()));
}

}}} // namespace caspar::test
