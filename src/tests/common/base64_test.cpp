#include <common/base64.h>
#include <common/except.h>

#include <gtest/gtest.h>

#include <string>

namespace caspar { namespace {

std::string decode(const std::string& base64)
{
    auto bytes = from_base64(base64);
    return {bytes.begin(), bytes.end()};
}

TEST(base64, encodes_with_padding)
{
    EXPECT_EQ(to_base64("Man", 3), "TWFu");
    EXPECT_EQ(to_base64("Ma", 2), "TWE=");
    EXPECT_EQ(to_base64("M", 1), "TQ==");
    EXPECT_EQ(to_base64("", 0), "");
}

TEST(base64, decodes_with_padding)
{
    EXPECT_EQ(decode("TWFu"), "Man");
    EXPECT_EQ(decode("TWE="), "Ma");
    EXPECT_EQ(decode("TQ=="), "M");
}

TEST(base64, round_trips_binary_data)
{
    std::string data;
    for (int i = 0; i < 256; ++i)
        data += static_cast<char>(i);

    EXPECT_EQ(decode(to_base64(data.data(), data.size())), data);
}

TEST(base64, decode_ignores_line_breaks)
{
    std::string data(100, 'x');
    auto        encoded = to_base64(data.data(), data.size()); // breaks lines every 76 characters

    ASSERT_NE(encoded.find('\n'), std::string::npos);
    EXPECT_EQ(decode(encoded), data);
}

TEST(base64, decode_rejects_truncated_input) { EXPECT_THROW(from_base64("TWF"), user_error); }

}} // namespace caspar
