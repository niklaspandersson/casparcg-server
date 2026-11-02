#pragma once

#include <boost/log/trivial.hpp>

#include <string>
#include <vector>

namespace caspar { namespace test {

// Every test's log is captured: written to <output_dir>/log.txt and kept in memory. An error or
// fatal record fails the test, unless the test allowed it with allow_log_errors() first. Records
// logged on background threads count too, which is how swallowed exceptions surface.

struct log_record
{
    boost::log::trivial::severity_level level;
    std::wstring                        message;
};

// The running test's records so far, in order.
std::vector<log_record> log_records();

// Error and fatal records whose message contains `text` don't fail the running test.
void allow_log_errors(const std::wstring& text);

// Fails the running test once for each error or fatal record that isn't allowed. Runs
// automatically when a test ends; call it earlier to check at a specific point.
void check_log_errors();

// Installs the log sink and the GoogleTest listener. Called from the test main().
void install_log_capture();

}} // namespace caspar::test
