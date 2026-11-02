#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace caspar { namespace test {

// Describes a sequence of frame indices or sample numbers as runs, to compare against an expected
// string:
//   0 1 2 3 4         "0..4"
//   0 1 2 2 2         "0..2 2*2"     (2 is held for two more)
//   8 9 0 1           "8..9 0..1"
//   3 ? 4             "3 ? 4"        (no index could be read)
std::string runs(const std::vector<std::optional<int>>& values);
std::string runs(const std::vector<std::int64_t>& values);

}} // namespace caspar::test
