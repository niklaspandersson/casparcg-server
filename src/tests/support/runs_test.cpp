#include "runs.h"

#include <gtest/gtest.h>

namespace caspar { namespace test { namespace {

using indices = std::vector<std::optional<int>>;

TEST(runs, describes_sequences)
{
    EXPECT_EQ(runs(indices{}), "");
    EXPECT_EQ(runs(indices{0, 1, 2, 3, 4}), "0..4");
    EXPECT_EQ(runs(indices{7}), "7");
    EXPECT_EQ(runs(indices{0, 1, 2, 2, 2}), "0..2 2*2");
    EXPECT_EQ(runs(indices{8, 9, 0, 1}), "8..9 0..1");
    EXPECT_EQ(runs(indices{3, std::nullopt, 4}), "3 ? 4");
    EXPECT_EQ(runs(indices{5, 5, 5}), "5 5*2");
    EXPECT_EQ(runs(indices{0, 1, 30, 31}), "0..1 30..31");
}

TEST(runs, describes_sample_numbers)
{
    EXPECT_EQ(runs(std::vector<std::int64_t>{0, 1, 2, 3, 100, 101}), "0..3 100..101");
}

}}} // namespace caspar::test
