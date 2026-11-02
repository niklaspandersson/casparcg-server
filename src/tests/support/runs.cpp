#include "runs.h"

namespace caspar { namespace test {

namespace {

template <typename T>
std::string describe(const std::vector<std::optional<T>>& values)
{
    std::string result;
    auto        append = [&](const std::string& text) { result += (result.empty() ? "" : " ") + text; };

    std::size_t n = 0;
    while (n < values.size()) {
        if (!values[n]) {
            append("?");
            ++n;
            continue;
        }

        // A run of +1 steps...
        auto first = *values[n];
        auto last  = first;
        ++n;
        while (n < values.size() && values[n] && *values[n] == last + 1)
            last = *values[n++];
        append(first == last ? std::to_string(first) : std::to_string(first) + ".." + std::to_string(last));

        // ...then how long its last value is held.
        std::size_t held = 0;
        while (n < values.size() && values[n] && *values[n] == last) {
            ++held;
            ++n;
        }
        if (held > 0)
            append(std::to_string(last) + "*" + std::to_string(held));
    }
    return result;
}

} // namespace

std::string runs(const std::vector<std::optional<int>>& values) { return describe(values); }

std::string runs(const std::vector<std::int64_t>& values)
{
    return describe(std::vector<std::optional<std::int64_t>>(values.begin(), values.end()));
}

}} // namespace caspar::test
