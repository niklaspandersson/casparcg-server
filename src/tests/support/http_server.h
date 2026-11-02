#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace caspar { namespace test {

// A local HTTP server for network sources: serves the files in a folder on 127.0.0.1, and can
// throttle, delay or hang its responses and go away mid-download. It answers GET and HEAD with
// Range support and closes the connection after each response.
class http_server
{
  public:
    struct request
    {
        std::string                method;
        std::string                path;
        std::optional<std::string> range;          // the Range header, as sent
        std::uint64_t              bytes_sent = 0; // body bytes, so far
        bool                       completed  = false;
    };

    explicit http_server(std::filesystem::path root);
    ~http_server();

    http_server(const http_server&)            = delete;
    http_server& operator=(const http_server&) = delete;

    // http://127.0.0.1:<port>/<path>
    std::string    url(const std::string& path) const;
    unsigned short port() const;

    // Body bytes per second for each response; 0 is unlimited.
    void set_rate(std::uint64_t bytes_per_second);
    // Wait before answering each request.
    void set_delay(std::chrono::milliseconds delay);
    // Read requests but never answer them.
    void set_hang(bool hang);

    // Closes the listener and every open connection, as if the server died.
    void stop();
    // Listens again on the same port.
    void start();

    // Every request received, in order.
    std::vector<request> requests() const;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}} // namespace caspar::test
