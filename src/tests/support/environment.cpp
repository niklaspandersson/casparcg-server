#include "environment.h"

#include <common/env.h>
#include <common/utf.h>

#include <cstdlib>
#include <fstream>
#include <random>
#include <string>

namespace caspar { namespace test {

namespace {

std::filesystem::path root()
{
    auto output = std::getenv("CASPAR_TEST_OUTPUT");
    return std::filesystem::absolute(std::filesystem::path(output && *output ? output : "test-output") / "environment");
}

} // namespace

void configure_environment()
{
    auto dir = root();
    std::filesystem::create_directories(dir);

    // Test processes run in parallel: write a private copy, then rename it over the shared name.
    auto config = dir / "casparcg.config";
    auto temp   = dir / ("casparcg.config." + std::to_string(std::random_device{}()));
    std::ofstream(temp) << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                           "<configuration>\n"
                           "  <paths>\n"
                           "    <media-path>"
                        << (dir / "media").string()
                        << "/</media-path>\n"
                           "    <log-path disabled=\"true\" />\n"
                           "    <data-path>"
                        << (dir / "data").string()
                        << "/</data-path>\n"
                           "    <template-path>"
                        << (dir / "template").string()
                        << "/</template-path>\n"
                           "  </paths>\n"
                           "</configuration>\n";

    std::filesystem::rename(temp, config);

    env::configure(u16(config.string()));
}

std::filesystem::path media_folder() { return root() / "media"; }

}} // namespace caspar::test
