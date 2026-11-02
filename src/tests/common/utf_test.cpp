#include <common/utf.h>

#include <gtest/gtest.h>

namespace caspar { namespace {

TEST(utf, round_trips_non_ascii_text)
{
    const std::string utf8 = "Malmö – 日本語 – 🎬";

    EXPECT_EQ(u8(u16(utf8)), utf8);
}

TEST(utf, converts_to_wide_characters)
{
    EXPECT_EQ(u16(std::string("Malmö")), L"Malmö");
    EXPECT_EQ(u8(std::wstring(L"Malmö")), "Malmö");
}

}} // namespace caspar
