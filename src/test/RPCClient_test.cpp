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
#include <xbwd/rpc/RPCClient.h>
#include <xbwd/rpc/fromJSON.h>

#include <ripple/beast/unit_test.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/to_string.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/jss.h>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <fmt/format.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>

namespace xbwd {
namespace tests {
namespace rpcc {

#if defined(_DEBUG) && defined(TESTS_DEBUG)
#define DBG(...) __VA_ARGS__
#define DBG_ARGS(...) __VA_OPT__(, ) __VA_ARGS__
#else
#define DBG_ARGS(...)
#define DBG(...)
#endif

namespace websocket = boost::beast::websocket;
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

using namespace std::chrono_literals;

static std::condition_variable gCv;
static std::mutex gMcv;

static unsigned const NUM_THREADS = 2;
static std::string const LHOST = "127.0.0.1";
static std::uint16_t const LPORT = 55556;
static std::string MIME_TYPE = "application/json";

static void
fail(boost::beast::error_code ec, char const* what)
{
    auto s = fmt::format("{}: {}", what, ec.message());
    throw std::runtime_error(s);
}

template <class Rep, class Period>
static bool
wait_for(
    std::chrono::duration<Rep, Period> const& to,
    std::function<bool()> stop DBG_ARGS(std::string const& msg))
{
    std::unique_lock l{gMcv};
    if (stop())
        return true;

    auto const b = gCv.wait_for(l, to, stop);
    DBG(std::cout << msg << ", wait finished: " << (b ? "condition" : "timeout")
                  << std::endl;)
    return b;
}

template <class Body, class Allocator>
http::message_generator
handleRequest(http::request<Body, http::basic_fields<Allocator>>&& req)
{
    auto const badRequest = [&req](boost::beast::string_view why) {
        http::response<http::string_body> res{
            http::status::bad_request, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.keep_alive(false);
        res.body() = std::string(why);
        res.prepare_payload();
        return res;
    };

    if (req.method() != http::verb::post)
        return badRequest("Unknown HTTP-method");

    http::response<http::string_body> res{
        std::piecewise_construct,
        std::make_tuple(req.body()),
        std::make_tuple(http::status::ok, req.version())};
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, MIME_TYPE);
    res.keep_alive(false);
    return res;
}

// Handles an HTTP server connection
class session : public std::enable_shared_from_this<session>
{
    boost::asio::io_context& ioc_;
    boost::beast::tcp_stream stream_;
    boost::beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    std::atomic_bool finished_ = false;

public:
    session(boost::asio::io_context& ioc, tcp::socket&& socket)
        : ioc_(ioc), stream_(std::move(socket))
    {
    }

    void
    run()
    {
        // We need to be executing within a strand to perform async operations
        // on the I/O objects in this session. Although not strictly necessary
        // for single-threaded contexts, this example code is written to be
        // thread-safe by default.
        boost::asio::dispatch(
            stream_.get_executor(),
            boost::beast::bind_front_handler(
                &session::doRead, shared_from_this()));
    }

    void
    doRead()
    {
        req_ = {};
        stream_.expires_after(std::chrono::seconds(30));
        http::async_read(
            stream_,
            buffer_,
            req_,
            boost::beast::bind_front_handler(
                &session::onRead, shared_from_this()));
    }

    void
    onRead(boost::beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if (ec == http::error::end_of_stream)
            return doClose();
        else if (ec)
            return fail(ec, "read");
        else
            sendResponse(handleRequest(std::move(req_)));
    }

    void
    sendResponse(http::message_generator&& msg)
    {
        bool const keepAlive = msg.keep_alive();
        boost::beast::async_write(
            stream_,
            std::move(msg),
            boost::beast::bind_front_handler(
                &session::onWrite, shared_from_this(), keepAlive));
    }

    void
    onWrite(
        bool keepAlive,
        boost::beast::error_code ec,
        std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if (ec)
            return fail(ec, "write");

        if (!keepAlive)
        {
            // This means we should close the connection, usually because
            // the response indicated the "Connection: close" semantic.
            return doClose();
        }

        doRead();
    }

    void
    doClose()
    {
        boost::beast::error_code ec;
        stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
    }

    void
    shutdown()
    {
        ioc_.post([this] {
            stream_.cancel();
            doClose();
            std::unique_lock l(gMcv);
            finished_ = true;
            gCv.notify_all();
        });
    }

    bool
    finished() const
    {
        return finished_;
    }
};

//------------------------------------------------------------------------------

// Accepts incoming connections and launches the sessions
class listener : public std::enable_shared_from_this<listener>
{
    boost::asio::io_context& ioc_;
    tcp::acceptor acceptor_;
    std::shared_ptr<session> session_;

public:
    listener(boost::asio::io_context& ioc, tcp::endpoint endpoint)
        : ioc_(ioc), acceptor_(boost::asio::make_strand(ioc))
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

    bool
    finished() const
    {
        return session_ ? session_->finished() : true;
    }

private:
    void
    doAccept()
    {
        // The new connection gets its own strand
        acceptor_.async_accept(
            boost::asio::make_strand(ioc_),
            boost::beast::bind_front_handler(
                &listener::onAccept, shared_from_this()));
    }

    void
    onAccept(boost::beast::error_code ec, tcp::socket socket)
    {
        if (ec == boost::system::errc::operation_canceled)
            return;

        if (ec)
        {
            fail(ec, "accept");
            return;
        }
        else
        {
            session_ = std::make_shared<session>(ioc_, std::move(socket));
            session_->run();
        }

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
        if (server_)
        {
            server_->shutdown();
            wait_for(1s, [this]() {
                return server_->finished();
            } DBG_ARGS("Connection::shutdownServer()"));
            server_.reset();
        }
    }
};

class RPCClient_test : public beast::unit_test::suite
{
    ripple::Logs logs_;
    beast::Journal j_;

public:
    RPCClient_test()
        : logs_(beast::severities::kInfo), j_([&, this]() {
            logs_.silent(true);
            return logs_.journal("Tests");
        }())
    {
    }

private:
    void
    testRpc()
    {
        testcase("Test http rpc client");

        Connection c;
        c.startServer(LHOST, LPORT);
        c.startIOThreads();

        std::shared_ptr<rpc_call::RPCClient> rpcClient =
            std::make_shared<rpc_call::RPCClient>(
                c.ios_, rpc::AddrEndpoint{LHOST, LPORT}, j_);

        Json::Value params;
        params[ripple::jss::method] = "test_cmd";
        params[ripple::jss::api_version] =
            xbwd::rpc_call::apiMaximumSupportedVersion;

        auto resp = rpcClient->post(params);

        BEAST_EXPECT(resp.second == params);
    }

    void
    testRpcNoConnection()
    {
        testcase("Test http rpc client - no connection");

        Connection c;
        c.startIOThreads();

        bool exc = false;

        try
        {
            std::shared_ptr<rpc_call::RPCClient> rpcClient =
                std::make_shared<rpc_call::RPCClient>(
                    c.ios_, rpc::AddrEndpoint{LHOST, LPORT}, j_);

            Json::Value params;
            params[ripple::jss::method] = "test_cmd";
            params[ripple::jss::api_version] =
                xbwd::rpc_call::apiMaximumSupportedVersion;

            auto resp = rpcClient->post(params);
        }
        catch (std::exception const&)
        {
            exc = true;
        }

        BEAST_EXPECT(exc);
    }

public:
    void
    run() override
    {
        testRpc();
        testRpcNoConnection();
    }
};

BEAST_DEFINE_TESTSUITE(RPCClient, rpc, xbwd);

}  // namespace rpcc
}  // namespace tests
}  // namespace xbwd
