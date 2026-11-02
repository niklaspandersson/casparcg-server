#include "http_server.h"

#include <boost/algorithm/string.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <fstream>
#include <future>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace caspar { namespace test {

namespace asio = boost::asio;
using asio::awaitable;
using asio::use_awaitable;
using asio::ip::tcp;

// Coroutines capture `this`: the destructor stops the io thread first, and pending coroutines are
// destroyed, never resumed, when io_ is.
struct http_server::impl
{
    const std::filesystem::path root_;
    asio::io_context            io_;
    tcp::acceptor               acceptor_{io_};
    unsigned short              port_ = 0;
    std::thread                 thread_;

    // Only touched on the io thread.
    std::set<std::shared_ptr<tcp::socket>> sockets_;

    mutable std::mutex        mutex_;
    std::uint64_t             rate_ = 0;
    std::chrono::milliseconds delay_{0};
    bool                      hang_ = false;
    std::vector<request>      requests_;

    explicit impl(std::filesystem::path root)
        : root_(std::move(root))
    {
    }

    void run()
    {
        listen(0);
        thread_ = std::thread([this] {
            auto guard = asio::make_work_guard(io_);
            io_.run();
        });
    }

    void shutdown()
    {
        on_io_thread([this] { close_all(); });
        io_.stop();
        thread_.join();
    }

    template <typename F>
    void on_io_thread(F&& f)
    {
        std::promise<void> done;
        asio::post(io_, [&] {
            f();
            done.set_value();
        });
        done.get_future().wait();
    }

    void listen(unsigned short port)
    {
        tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();
        port_ = acceptor_.local_endpoint().port();
        asio::co_spawn(io_, accept_loop(), asio::detached);
    }

    void close_all()
    {
        boost::system::error_code ec;
        acceptor_.close(ec);
        for (auto& socket : sockets_)
            socket->close(ec);
        sockets_.clear();
    }

    awaitable<void> accept_loop()
    {
        while (acceptor_.is_open()) {
            auto socket = std::make_shared<tcp::socket>(co_await acceptor_.async_accept(use_awaitable));
            sockets_.insert(socket);
            asio::co_spawn(io_, session(socket), [this, socket](std::exception_ptr) { sockets_.erase(socket); });
        }
    }

    awaitable<void> wait(std::chrono::steady_clock::duration duration)
    {
        asio::steady_timer timer(io_, duration);
        co_await timer.async_wait(use_awaitable);
    }

    std::size_t log(request r)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(std::move(r));
        return requests_.size() - 1;
    }

    void update(std::size_t index, std::uint64_t bytes_sent, bool completed)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_[index].bytes_sent = bytes_sent;
        requests_[index].completed  = completed;
    }

    awaitable<void> respond(tcp::socket& socket, const std::string& status, const std::string& headers)
    {
        auto text = "HTTP/1.1 " + status + "\r\n" + headers + "Connection: close\r\n\r\n";
        co_await asio::async_write(socket, asio::buffer(text), use_awaitable);
    }

    awaitable<void> session(std::shared_ptr<tcp::socket> socket)
    {
        std::string head;
        co_await asio::async_read_until(*socket, asio::dynamic_buffer(head), "\r\n\r\n", use_awaitable);

        std::istringstream lines(head);
        std::string        line;
        std::getline(lines, line);
        std::vector<std::string> parts;
        boost::split(parts, boost::trim_copy(line), boost::is_space(), boost::token_compress_on);

        request r;
        r.method = parts.size() > 0 ? parts[0] : "";
        r.path   = parts.size() > 1 ? parts[1] : "/";
        while (std::getline(lines, line)) {
            auto colon = line.find(':');
            if (colon != std::string::npos && boost::iequals(line.substr(0, colon), "range"))
                r.range = boost::trim_copy(line.substr(colon + 1));
        }
        auto index = log(r);

        std::uint64_t             rate;
        std::chrono::milliseconds delay;
        bool                      hang;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rate  = rate_;
            delay = delay_;
            hang  = hang_;
        }

        if (delay.count() > 0)
            co_await wait(delay);

        if (hang) {
            // Never answer; this returns when the client or stop() closes the connection.
            char byte;
            co_await asio::async_read(*socket, asio::buffer(&byte, 1), use_awaitable);
            co_return;
        }

        auto file = root_ / r.path.substr(1);
        if (r.path.find("..") != std::string::npos || !std::filesystem::is_regular_file(file)) {
            co_await respond(*socket, "404 Not Found", "Content-Length: 0\r\n");
            update(index, 0, true);
            co_return;
        }

        const std::uint64_t size   = std::filesystem::file_size(file);
        std::uint64_t       first  = 0;
        std::uint64_t       last   = size == 0 ? 0 : size - 1;
        std::string         status = "200 OK";
        std::string         headers;

        if (r.range) {
            // bytes=<first>-[<last>]
            std::uint64_t      a = 0, b = last;
            char               dash;
            std::istringstream spec(r.range->substr(r.range->find('=') + 1));
            spec >> a >> dash >> b; // b keeps its default for an open-ended range
            if (!boost::starts_with(*r.range, "bytes=") || a >= size || b < a) {
                co_await respond(*socket,
                                 "416 Range Not Satisfiable",
                                 "Content-Range: bytes */" + std::to_string(size) + "\r\nContent-Length: 0\r\n");
                update(index, 0, true);
                co_return;
            }
            first  = a;
            last   = std::min(b, last);
            status = "206 Partial Content";
            headers += "Content-Range: bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" +
                       std::to_string(size) + "\r\n";
        }

        const std::uint64_t length = size == 0 ? 0 : last - first + 1;
        headers += "Content-Length: " + std::to_string(length) +
                   "\r\nAccept-Ranges: bytes\r\nContent-Type: application/octet-stream\r\n";
        co_await respond(*socket, status, headers);

        if (r.method == "HEAD") {
            update(index, 0, true);
            co_return;
        }

        std::ifstream in(file, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(first));

        // Chunks of a tenth of a second at the throttled rate, so throttling stays smooth.
        const std::size_t chunk_size = rate > 0 ? std::max<std::size_t>(1, rate / 10) : 64 * 1024;
        std::vector<char> chunk(chunk_size);
        std::uint64_t     sent = 0;
        while (sent < length) {
            auto n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), length - sent));
            in.read(chunk.data(), static_cast<std::streamsize>(n));
            co_await asio::async_write(*socket, asio::buffer(chunk.data(), n), use_awaitable);
            sent += n;
            update(index, sent, sent == length);
            if (rate > 0 && sent < length)
                co_await wait(std::chrono::microseconds(n * 1000000 / rate));
        }
        update(index, sent, true);

        boost::system::error_code ec;
        socket->shutdown(tcp::socket::shutdown_send, ec);
    }
};

http_server::http_server(std::filesystem::path root)
    : impl_(std::make_unique<impl>(std::move(root)))
{
    impl_->run();
}

http_server::~http_server() { impl_->shutdown(); }

std::string http_server::url(const std::string& path) const
{
    return "http://127.0.0.1:" + std::to_string(impl_->port_) + "/" + path;
}

unsigned short http_server::port() const { return impl_->port_; }

void http_server::set_rate(std::uint64_t bytes_per_second)
{
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->rate_ = bytes_per_second;
}

void http_server::set_delay(std::chrono::milliseconds delay)
{
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->delay_ = delay;
}

void http_server::set_hang(bool hang)
{
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->hang_ = hang;
}

void http_server::stop()
{
    impl_->on_io_thread([this] { impl_->close_all(); });
}

void http_server::start()
{
    impl_->on_io_thread([this] { impl_->listen(impl_->port_); });
}

std::vector<http_server::request> http_server::requests() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->requests_;
}

}} // namespace caspar::test
