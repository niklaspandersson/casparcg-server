#include "http_server.h"
#include "output_dir.h"
#include "test_media.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>
#include <thread>

namespace caspar { namespace test { namespace {

namespace asio = boost::asio;
using asio::ip::tcp;
using namespace std::chrono_literals;

std::string
send_request(tcp::socket& socket, const std::string& method, const std::string& path, const std::string& range)
{
    std::string text = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!range.empty())
        text += "Range: " + range + "\r\n";
    text += "\r\n";
    asio::write(socket, asio::buffer(text));
    return text;
}

// The raw response: status line, headers and body.
std::string fetch(const http_server& server,
                  const std::string& path,
                  const std::string& range  = "",
                  const std::string& method = "GET")
{
    asio::io_context io;
    tcp::socket      socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), server.port()});
    send_request(socket, method, path, range);

    std::string               response;
    boost::system::error_code ec;
    asio::read(socket, asio::dynamic_buffer(response), ec); // until the server closes
    return response;
}

std::string body(const std::string& response) { return response.substr(response.find("\r\n\r\n") + 4); }

std::string write_file(const std::string& name, std::size_t size)
{
    std::string data;
    for (std::size_t n = 0; n < size; ++n)
        data += static_cast<char>('a' + n % 26);
    std::ofstream(output_dir() / name, std::ios::binary) << data;
    return data;
}

TEST(http_server, serves_files_and_logs_requests)
{
    auto        data = write_file("file.bin", 1000);
    http_server server(output_dir());

    auto response = fetch(server, "/file.bin");
    EXPECT_THAT(response, ::testing::StartsWith("HTTP/1.1 200 OK\r\n"));
    EXPECT_THAT(response, ::testing::HasSubstr("Content-Length: 1000\r\n"));
    EXPECT_EQ(body(response), data);

    auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].method, "GET");
    EXPECT_EQ(requests[0].path, "/file.bin");
    EXPECT_EQ(requests[0].bytes_sent, 1000u);
    EXPECT_TRUE(requests[0].completed);
}

TEST(http_server, serves_byte_ranges)
{
    auto        data = write_file("file.bin", 1000);
    http_server server(output_dir());

    auto response = fetch(server, "/file.bin", "bytes=100-199");
    EXPECT_THAT(response, ::testing::StartsWith("HTTP/1.1 206 Partial Content\r\n"));
    EXPECT_THAT(response, ::testing::HasSubstr("Content-Range: bytes 100-199/1000\r\n"));
    EXPECT_EQ(body(response), data.substr(100, 100));

    EXPECT_EQ(body(fetch(server, "/file.bin", "bytes=900-")), data.substr(900));
    EXPECT_EQ(server.requests().at(1).range, "bytes=900-");
}

TEST(http_server, answers_missing_files_and_bad_ranges)
{
    write_file("file.bin", 1000);
    http_server server(output_dir());

    EXPECT_THAT(fetch(server, "/missing.bin"), ::testing::StartsWith("HTTP/1.1 404"));
    EXPECT_THAT(fetch(server, "/file.bin", "bytes=1000-"), ::testing::StartsWith("HTTP/1.1 416"));
    EXPECT_THAT(fetch(server, "/../file.bin"), ::testing::StartsWith("HTTP/1.1 404"));
}

TEST(http_server, head_sends_no_body)
{
    write_file("file.bin", 1000);
    http_server server(output_dir());

    auto response = fetch(server, "/file.bin", "", "HEAD");
    EXPECT_THAT(response, ::testing::HasSubstr("Content-Length: 1000\r\n"));
    EXPECT_EQ(body(response), "");
}

TEST(http_server, throttles_the_rate)
{
    write_file("file.bin", 100'000);
    http_server server(output_dir());
    server.set_rate(200'000);

    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(body(fetch(server, "/file.bin")).size(), 100'000u);
    EXPECT_GE(std::chrono::steady_clock::now() - start, 400ms) << "100 kB at 200 kB/s";
}

TEST(http_server, delays_responses)
{
    write_file("file.bin", 10);
    http_server server(output_dir());
    server.set_delay(300ms);

    auto start = std::chrono::steady_clock::now();
    fetch(server, "/file.bin");
    EXPECT_GE(std::chrono::steady_clock::now() - start, 300ms);
}

TEST(http_server, hangs_until_stopped_then_starts_again)
{
    auto        data = write_file("file.bin", 10);
    http_server server(output_dir());
    server.set_hang(true);

    asio::io_context io;
    tcp::socket      socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), server.port()});
    send_request(socket, "GET", "/file.bin", "");

    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(socket.available(), 0u) << "a hanging server answers nothing";
    EXPECT_EQ(server.requests().size(), 1u);

    server.stop();
    std::string               response;
    boost::system::error_code ec;
    asio::read(socket, asio::dynamic_buffer(response), ec);
    EXPECT_TRUE(ec) << "stop() closes open connections";
    EXPECT_EQ(response, "");

    server.set_hang(false);
    server.start();
    EXPECT_EQ(body(fetch(server, "/file.bin")), data) << "same port after start()";
}

TEST(http_server, ffmpeg_reads_a_clip_over_http)
{
    auto        clip = make_clip({.frames = 10});
    http_server server(output_dir());

    auto decoded = decode_clip(server.url(clip.filename().string()));
    EXPECT_EQ(decoded.indices.size(), 10u);
    EXPECT_EQ(decoded.indices.back(), 9);
}

}}} // namespace caspar::test
