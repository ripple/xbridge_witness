//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright 2023 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xbwd/basics/StructuredLog.h>
#include <xbwd/client/WebsocketClient.h>
#include <xbwd/rpc/fromJSON.h>

#include <ripple/beast/unit_test.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/to_string.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/jss.h>

#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/websocket.hpp>
#include <fmt/format.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>

namespace xbwd {
namespace tests {

namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;

using namespace std::chrono_literals;

std::condition_variable gCv;
std::mutex gMcv;

static unsigned const NUM_THREADS = 2;
static std::string const LHOST = "127.0.0.1";
static std::uint16_t const LPORT = 55555;

void
fail(boost::beast::error_code ec, char const* what)
{
    auto s = fmt::format("{}: {}", what, ec.message());
    throw std::runtime_error(s);
}

// Echoes back all received WebSocket messages
class session : public std::enable_shared_from_this<session>
{
    boost::asio::io_context& ios_;
    websocket::stream<boost::beast::tcp_stream> ws_;
    boost::beast::flat_buffer buffer_;

public:
    explicit session(boost::asio::io_context& ios, tcp::socket&& socket)
        : ios_(ios), ws_(std::move(socket))
    {
    }

    void
    run()
    {
        ws_.set_option(websocket::stream_base::timeout::suggested(
            boost::beast::role_type::server));
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) {
                res.set(
                    boost::beast::http::field::server,
                    std::string(BOOST_BEAST_VERSION_STRING) +
                        " websocket-server-async");
            }));

        ws_.async_accept(boost::beast::bind_front_handler(
            &session::onAccept, shared_from_this()));
    }

    void
    onAccept(boost::beast::error_code ec)
    {
        if (ec == websocket::error::closed)
            return;
        if (ec)
            return fail(ec, "accept");
        doRead();
    }

    void
    doRead()
    {
        ws_.async_read(
            buffer_,
            boost::beast::bind_front_handler(
                &session::onRead, shared_from_this()));
    }

    void
    onRead(boost::beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);
        if (ec == websocket::error::closed)
            return;

        if (ec)
            fail(ec, "read");

        ws_.text(ws_.got_text());
        ws_.async_write(
            buffer_.data(),
            boost::beast::bind_front_handler(
                &session::onWrite, shared_from_this()));
    }

    void
    onWrite(boost::beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);
        if (ec == websocket::error::closed)
            return;
        if (ec)
            return fail(ec, "write");
        buffer_.consume(buffer_.size());
        doRead();
    }

    void
    shutdown()
    {
        ios_.post([this] {
            ws_.async_close(
                {}, [](boost::beast::error_code const&) { gCv.notify_all(); });
        });
    }
};

//------------------------------------------------------------------------------

class listener : public std::enable_shared_from_this<listener>
{
    boost::asio::io_context& ios_;
    tcp::acceptor acceptor_;
    std::shared_ptr<session> session_;

public:
    listener(boost::asio::io_context& ioc, tcp::endpoint endpoint)
        : ios_(ioc), acceptor_(ioc)
    {
        boost::beast::error_code ec;

        acceptor_.open(endpoint.protocol(), ec);
        if (ec)
            fail(ec, "open");

        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec)
            fail(ec, "set_option");

        acceptor_.bind(endpoint, ec);
        if (ec)
            fail(ec, "bind");

        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec)
            fail(ec, "listen");
    }

    void
    run()
    {
        doAccept();
    }

    void
    shutdown()
    {
        acceptor_.cancel();
        if (session_)
            session_->shutdown();
    }

private:
    void
    doAccept()
    {
        acceptor_.async_accept(
            boost::asio::make_strand(ios_),
            boost::beast::bind_front_handler(
                &listener::onAccept, shared_from_this()));
    }

    void
    onAccept(boost::beast::error_code ec, tcp::socket socket)
    {
        if (ec == boost::system::errc::operation_canceled)
            return;
        if (ec)
            fail(ec, "accept");

        session_ = std::make_shared<session>(ios_, std::move(socket));
        session_->run();

        doAccept();
    }
};

struct Connection
{
    boost::asio::io_service ios_;
    std::vector<std::thread> ioThreads_;
    std::shared_ptr<listener> server_;
    Json::Value repl_;
    std::atomic_bool connected_ = false;

    Connection() : ios_(NUM_THREADS)
    {
    }

    ~Connection()
    {
        shutdownServer();
        waitIOThreads();
    }

    void
    onConnect()
    {
        connected_ = true;
        gCv.notify_all();
    }

    void
    onMessage(Json::Value const& msg)
    {
        repl_ = msg;
        gCv.notify_all();
    }

    void
    startIOThreads()
    {
        ioThreads_.reserve(NUM_THREADS);
        for (auto i = 0; i < NUM_THREADS; ++i)
            ioThreads_.emplace_back([this] { ios_.run(); });
    }

    void
    waitIOThreads()
    {
        for (auto& t : ioThreads_)
            if (t.joinable())
                t.join();
        ioThreads_.clear();
    }

    void
    startServer(std::string const& host, std::uint16_t port)
    {
        auto const address = boost::asio::ip::make_address(host);
        server_ =
            std::make_shared<listener>(ios_, tcp::endpoint{address, port});
        server_->run();
    }

    void
    shutdownServer()
    {
        server_->shutdown();
        std::unique_lock l{gMcv};
        gCv.wait_for(l, 1s);
        server_.reset();
    }
};

class WS_test : public beast::unit_test::suite
{
    ripple::Logs logs_;
    beast::Journal j_;

public:
    WS_test()
        : logs_(beast::severities::kInfo), j_([&, this]() {
            logs_.silent(true);
            return logs_.journal("Tests");
        }())
    {
    }

private:
    void
    testWS()
    {
        testcase("Test Websocket");

        Connection c;
        c.startServer(LHOST, LPORT);
        c.startIOThreads();

        std::shared_ptr<WebsocketClient> wsClient =
            std::make_shared<WebsocketClient>(
                [self = &c](Json::Value const& msg) { self->onMessage(msg); },
                [self = &c]() { self->onConnect(); },
                c.ios_,
                beast::IP::Endpoint{
                    boost::asio::ip::make_address(LHOST), LPORT},
                std::unordered_map<std::string, std::string>{},
                j_);

        wsClient->connect();

        Json::Value params;
        params[ripple::jss::account] = "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg";
        auto id = wsClient->send("account_info", params, "locking");
        {
            std::unique_lock l{gMcv};
            gCv.wait_for(l, 1s);
        }

        params[ripple::jss::id] = id;
        params[ripple::jss::method] = "account_info";
        params[ripple::jss::jsonrpc] = "2.0";
        params[ripple::jss::ripplerpc] = "2.0";

        BEAST_EXPECT(c.repl_ == params);
    }

    void
    testReconnect()
    {
        testcase("Test Websocket reconnect");
        Connection c;

        std::shared_ptr<WebsocketClient> wsClient =
            std::make_shared<WebsocketClient>(
                [self = &c](Json::Value const& msg) { self->onMessage(msg); },
                [self = &c]() { self->onConnect(); },
                c.ios_,
                beast::IP::Endpoint{
                    boost::asio::ip::make_address(LHOST), LPORT},
                std::unordered_map<std::string, std::string>{},
                j_);
        wsClient->connect();

        c.startServer(LHOST, LPORT);
        c.startIOThreads();

        if (!c.connected_)
        {
            std::unique_lock l{gMcv};
            gCv.wait_for(l, 10s);
        }
        BEAST_EXPECT(c.connected_ == true);

        Json::Value params;
        params[ripple::jss::account] = "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg";
        auto id = wsClient->send("account_info", params, "locking");
        {
            std::unique_lock l{gMcv};
            gCv.wait_for(l, 1s);
        }

        params[ripple::jss::id] = id;
        params[ripple::jss::method] = "account_info";
        params[ripple::jss::jsonrpc] = "2.0";
        params[ripple::jss::ripplerpc] = "2.0";

        BEAST_EXPECT(c.repl_ == params);
    }

public:
    void
    run() override
    {
        testWS();
        testReconnect();
    }
};

BEAST_DEFINE_TESTSUITE(WS, client, xbwd);

}  // namespace tests

}  // namespace xbwd
