/**
 * Copyright @ 2021 VW Group. All rights reserved.
 *
 * This Source Code Form is subject to the terms of the Mozilla
 * Public License, v. 2.0. If a copy of the MPL was not distributed
 * with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <httplib/httplib.h>
#include <gtest/gtest.h>

#include <stdexcept>
#include <thread>
#include <vector>

TEST(HttpContentLength, ChecksLengthsAndHeaderNames)
{
    size_t length = 123;
    httplib::MultiMap headers;
    EXPECT_TRUE(httplib::detail::get_content_length(headers, length));
    EXPECT_EQ(length, 0u);

    for (const auto& value : {"0", "\t00042 \t", "8388608"}) {
        headers = {{"cOnTeNt-LeNgTh", value}};
        ASSERT_TRUE(httplib::detail::get_content_length(headers, length));
        EXPECT_EQ(length, std::string(value) == "0" ? 0u :
                          std::string(value) == "8388608" ? 8388608u : 42u);
    }
    for (const auto& value : {"", "  ", "-1", "+1", "1x", "1 2", "1, 1",
                              "8388609", "2147483647", "999999999999999999999999999999"}) {
        headers = {{"Content-Length", value}};
        EXPECT_FALSE(httplib::detail::get_content_length(headers, length)) << value;
    }
    headers = {{"Content-Length", "1"}, {"content-length", "1"}};
    EXPECT_FALSE(httplib::detail::get_content_length(headers, length));
}

#ifndef _WIN32
namespace {
class TestServer : public httplib::Server {
public:
    bool called = false;
    bool fail = false;
    std::string body;
    std::vector<std::string> bodies;
    std::vector<std::string> urls;

    bool handle_request(const httplib::Request& request, httplib::Response& response) override
    {
        called = true;
        if (fail) throw std::runtime_error("handler failure");
        body = request.body;
        bodies.push_back(request.body);
        urls.push_back(request.url);
        response.body = "ok";
        return true;
    }
};

std::string process(TestServer& server, const std::string& request)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        ADD_FAILURE() << "socketpair failed";
        return {};
    }
    std::thread worker([&] { server.process_request(sockets[1]); });
    httplib::detail::socket_write(sockets[0], request.data(), request.size());
    shutdown(sockets[0], SHUT_WR);
    std::string result;
    char buffer[1024];
    int count;
    while ((count = recv(sockets[0], buffer, sizeof(buffer), 0)) > 0) {
        result.append(buffer, count);
    }
    close(sockets[0]);
    worker.join();
    return result;
}
} // namespace

TEST(HttpContentLength, RejectsInvalidRequestsAndContinuesServing)
{
    TestServer server;
    for (const auto& header : {"Content-Length: -1", "Content-Length: 2147483647",
                               "Content-Length: 1\r\ncontent-length: 2"}) {
        EXPECT_TRUE(process(server, std::string("POST / HTTP/1.0\r\n") + header + "\r\n\r\n").empty());
        EXPECT_FALSE(server.called);
    }
    EXPECT_NE(process(server, "POST / HTTP/1.0\r\ncontent-length: 5 \t\r\n\r\nhello")
                  .find("200 OK"), std::string::npos);
    EXPECT_TRUE(server.called);
    EXPECT_EQ(server.body, "hello");
}

TEST(HttpContentLength, ValidatesBodiesForAllMethods)
{
    TestServer server;
    for (const auto& method : {"GET", "HEAD", "PUT"}) {
        for (const auto& header : {"Content-Length: -1", "content-length: 8388609",
                                   "Content-Length: 1\r\ncontent-length: 2",
                                   "Content-Length: 5"}) {
            EXPECT_TRUE(process(server, std::string(method) + " / HTTP/1.0\r\n" + header + "\r\n\r\n").empty());
            EXPECT_FALSE(server.called);
        }
    }

    const std::string response = process(server,
        "GET /first HTTP/1.0\r\ncontent-length: 5\r\n\r\nhello"
        "POST /second HTTP/1.0\r\nContent-Length: 3\r\n\r\nbye");
    EXPECT_NE(response.find("200 OK"), std::string::npos);
    ASSERT_EQ(server.urls.size(), 2u);
    EXPECT_EQ(server.urls[0], "/first");
    EXPECT_EQ(server.urls[1], "/second");
    ASSERT_EQ(server.bodies.size(), 2u);
    EXPECT_EQ(server.bodies[0], "hello");
    EXPECT_EQ(server.bodies[1], "bye");
}

TEST(HttpContentLength, WritesOnlyComputedLengthForRequestsAndResponses)
{
    for (bool response : {false, true}) {
        int sockets[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
        httplib::Request request;
        httplib::Response reply;
        httplib::MultiMap headers = {{"Content-Length", "10"}, {"content-length", "11"},
                                     {"CoNtEnT-LeNgTh", "12"},
                                     {"X-Other", "first"}, {"X-Other", "second"}};
        if (response) {
            reply.status = 200;
            reply.body = "hello";
            reply.headers = headers;
            httplib::detail::write_response(sockets[0], request, reply);
        } else {
            request.method = "POST";
            request.url = "/";
            request.body = "hello";
            request.headers = headers;
            httplib::detail::write_request(sockets[0], request);
        }
        shutdown(sockets[0], SHUT_WR);
        std::string output;
        char buffer[1024];
        int count;
        while ((count = recv(sockets[1], buffer, sizeof(buffer), 0)) > 0) {
            output.append(buffer, count);
        }
        close(sockets[0]);
        close(sockets[1]);
        EXPECT_NE(output.find("Content-Length: 5\r\n"), std::string::npos);
        EXPECT_EQ(output.find("Content-Length: 10\r\n"), std::string::npos);
        EXPECT_EQ(output.find("content-length: 11\r\n"), std::string::npos);
        EXPECT_EQ(output.find("CoNtEnT-LeNgTh: 12\r\n"), std::string::npos);
        EXPECT_NE(output.find("X-Other: first\r\n"), std::string::npos);
        EXPECT_NE(output.find("X-Other: second\r\n"), std::string::npos);
        EXPECT_NE(output.find("\r\n\r\nhello"), std::string::npos);
    }
}

TEST(HttpContentLength, ContainsWorkerExceptionsAndClosesConnection)
{
    TestServer server;
    server.fail = true;
    EXPECT_TRUE(process(server, "GET / HTTP/1.0\r\n\r\n").empty());
    EXPECT_TRUE(server.called);
    server.fail = false;
    EXPECT_NE(process(server, "GET / HTTP/1.0\r\n\r\n").find("200 OK"), std::string::npos);
}

TEST(HttpContentLength, ReadsResponseOnlyAfterReceivingBody)
{
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    httplib::Response response;
    response.headers.insert({"Content-Length", "8388608"});
    shutdown(sockets[0], SHUT_WR);
    EXPECT_FALSE(httplib::detail::read_content(sockets[1], response));
    EXPECT_TRUE(response.body.empty());
    close(sockets[0]);
    close(sockets[1]);
}

TEST(HttpContentLength, ReadsLargeResponseIntoReusedResponse)
{
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    const std::string body(8388608, 'x');
    std::thread writer([&] {
        httplib::detail::socket_write(sockets[0], body.data(), body.size());
        shutdown(sockets[0], SHUT_WR);
    });
    httplib::Response response;
    response.body = "old";
    response.headers.insert({"Content-Length", "8388608"});
    EXPECT_TRUE(httplib::detail::read_content(sockets[1], response));
    EXPECT_EQ(response.body, body);
    writer.join();
    close(sockets[0]);
    close(sockets[1]);
}
#endif
