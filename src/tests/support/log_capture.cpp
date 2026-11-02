#include "log_capture.h"
#include "output_dir.h"

#include <common/log.h>
#include <common/utf.h>

#include <boost/core/null_deleter.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>

namespace caspar { namespace test {

namespace {

namespace logging = boost::log;
using boost::log::trivial::severity_level;

struct captured_log
{
    std::mutex                mutex;
    std::vector<log_record>   records;
    std::vector<std::wstring> allowed;
    std::size_t               checked = 0; // records before this index were already reported
    std::filesystem::path     file_path;
    std::ofstream             file;
    bool                      in_test = false;
};

captured_log& state()
{
    static captured_log log;
    return log;
}

bool is_error(severity_level level) { return level >= severity_level::error; }

class capture_backend : public logging::sinks::basic_sink_backend<logging::sinks::synchronized_feeding>
{
  public:
    void consume(const logging::record_view& rec)
    {
        auto level   = logging::extract_or_default<severity_level>("Severity", rec, severity_level::info);
        auto message = logging::extract_or_default<std::wstring>("Message", rec, std::wstring{});

        auto&                       log = state();
        std::lock_guard<std::mutex> lock(log.mutex);

        // Records between tests (static init, a previous test's late threads) have no test to fail.
        if (!log.in_test)
            return;

        if (!log.file.is_open()) {
            std::filesystem::create_directories(log.file_path.parent_path());
            log.file.open(log.file_path);
        }
        log.file << "[" << logging::trivial::to_string(level) << "] " << u8(message) << "\n";
        log.file.flush();

        log.records.push_back({level, std::move(message)});
    }
};

class capture_listener : public ::testing::EmptyTestEventListener
{
    void OnTestStart(const ::testing::TestInfo&) override
    {
        std::filesystem::remove_all(output_path());

        auto&                       log = state();
        std::lock_guard<std::mutex> lock(log.mutex);
        log.records.clear();
        log.allowed.clear();
        log.checked   = 0;
        log.file_path = output_path() / "log.txt"; // created on the first record
        log.in_test   = true;
    }

    // GoogleTest calls OnTestEnd listeners in reverse order, so this runs before the result printer
    // and a failure added here still counts for the test.
    void OnTestEnd(const ::testing::TestInfo& info) override
    {
        check_log_errors();

        auto&                       log = state();
        std::lock_guard<std::mutex> lock(log.mutex);
        log.in_test = false;
        log.file.close();

        if (info.result()->Failed() && !log.records.empty())
            std::cout << "  log: " << log.file_path.string() << std::endl;
    }
};

} // namespace

std::vector<log_record> log_records()
{
    auto&                       log = state();
    std::lock_guard<std::mutex> lock(log.mutex);
    return log.records;
}

void allow_log_errors(const std::wstring& text)
{
    auto&                       log = state();
    std::lock_guard<std::mutex> lock(log.mutex);
    log.allowed.push_back(text);
}

void check_log_errors()
{
    std::vector<log_record> unexpected;
    {
        auto&                       log = state();
        std::lock_guard<std::mutex> lock(log.mutex);
        for (; log.checked < log.records.size(); ++log.checked) {
            auto& record = log.records[log.checked];
            if (!is_error(record.level))
                continue;
            bool allowed = false;
            for (auto& text : log.allowed)
                allowed = allowed || record.message.find(text) != std::wstring::npos;
            if (!allowed)
                unexpected.push_back(record);
        }
    }

    for (auto& record : unexpected)
        ADD_FAILURE() << "Unexpected " << logging::trivial::to_string(record.level)
                      << " logged: " << u8(record.message);
}

void install_log_capture()
{
    auto level = std::getenv("CASPAR_TEST_LOG_LEVEL");
    log::set_log_level(level && *level ? u16(std::string(level)) : L"debug");

    using sink_t = logging::sinks::synchronous_sink<capture_backend>;
    logging::core::get()->add_sink(boost::make_shared<sink_t>(boost::make_shared<capture_backend>()));

    ::testing::UnitTest::GetInstance()->listeners().Append(new capture_listener);
}

}} // namespace caspar::test
