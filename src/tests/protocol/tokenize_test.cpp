#include <protocol/util/tokenize.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <list>
#include <string>

namespace caspar { namespace IO { namespace {

using ::testing::ElementsAre;

std::list<std::wstring> tokens(const std::wstring& message)
{
    std::list<std::wstring> result;
    tokenize(message, result);
    return result;
}

TEST(tokenize, splits_on_spaces)
{
    EXPECT_THAT(tokens(L"PLAY 1-10 AMB"), ElementsAre(L"PLAY", L"1-10", L"AMB"));
    EXPECT_THAT(tokens(L"  PLAY   1-10  "), ElementsAre(L"PLAY", L"1-10"));
}

TEST(tokenize, keeps_quoted_strings_together)
{
    EXPECT_THAT(tokens(L"PLAY 1-10 \"my clip\" LOOP"), ElementsAre(L"PLAY", L"1-10", L"my clip", L"LOOP"));
}

TEST(tokenize, keeps_empty_quoted_strings)
{
    EXPECT_THAT(tokens(L"CG 1-10 ADD 1 \"\""), ElementsAre(L"CG", L"1-10", L"ADD", L"1", L""));
}

TEST(tokenize, unescapes_backslash_sequences)
{
    EXPECT_THAT(tokens(L"\"say \\\"hi\\\"\""), ElementsAre(L"say \"hi\""));
    EXPECT_THAT(tokens(L"\"a\\\\b\""), ElementsAre(L"a\\b"));
    EXPECT_THAT(tokens(L"\"line1\\nline2\""), ElementsAre(L"line1\nline2"));
}

TEST(tokenize, drops_unknown_escape_sequences) { EXPECT_THAT(tokens(L"a\\xb"), ElementsAre(L"ab")); }

TEST(tokenize, keeps_parameter_lists_together)
{
    EXPECT_THAT(tokens(L"MIXER 1-10 FILL (0 0 1 1) 25"), ElementsAre(L"MIXER", L"1-10", L"FILL", L"(0 0 1 1)", L"25"));
    EXPECT_THAT(tokens(L"f((a b) c) d"), ElementsAre(L"f((a b) c)", L"d"));
}

TEST(tokenize, appends_to_the_list_and_returns_its_size)
{
    std::list<std::wstring> result{L"existing"};

    EXPECT_EQ(tokenize(L"a b", result), 3u);
    EXPECT_THAT(result, ElementsAre(L"existing", L"a", L"b"));
}

}}} // namespace caspar::IO
