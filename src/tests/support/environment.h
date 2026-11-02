#pragma once

#include <filesystem>

namespace caspar { namespace test {

// Configures the server environment (common/env.h) once per test process, from a minimal
// casparcg.config whose media, data and template folders are under test-output/environment/.
// Producers and consumers read their options and paths from it. Called from the test main().
void configure_environment();

// The media folder of that configuration.
std::filesystem::path media_folder();

}} // namespace caspar::test
