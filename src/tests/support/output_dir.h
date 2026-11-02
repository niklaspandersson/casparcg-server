#pragma once

#include <filesystem>

namespace caspar { namespace test {

// The running test's own folder for files it writes and files written about it (the captured log,
// images on a mismatch). It is emptied when the test starts and kept afterwards, so it can be
// inspected after a failure:
//   $CASPAR_TEST_OUTPUT/<suite>/<test>/, by default ./test-output/<suite>/<test>/
std::filesystem::path output_dir();

// The same path, without creating the folder.
std::filesystem::path output_path();

}} // namespace caspar::test
