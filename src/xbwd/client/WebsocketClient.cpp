//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2016 Ripple Labs Inc.

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

#include <xbwd/client/WebsocketClient.h>

#include <xbwd/basics/StructuredLog.h>

#include <ripple/basics/Log.h>
#include <ripple/json/Output.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>
#include <ripple/json/to_string.h>
#include <ripple/protocol/jss.h>

#include <boost/beast/websocket.hpp>

#include <chrono>
#include <iostream>
#include <unordered_map>

using namespace std::chrono_literals;

namespace xbwd {

auto constexpr CONNECT_TIMEOUT = std::chrono::seconds{5};

template <class ConstBuffers>
std::string
WebsocketClient::buffer_string(ConstBuffers const& b)
{
    using boost::asio::buffer;
    using boost::asio::buffer_size;
    std::string s;
    s.resize(buffer_size(b));
    buffer_copy(buffer(&s[0], s.size()), b);
    return s;
}

void
WebsocketClient::cleanup()
{
    ios_.post(strand_.wrap([this] {
        timer_.cancel();
        if (state_ == ST_CONNECTED)
        {
            {
                std::lock_guard l{m_};
                ws_.async_close({}, strand_.wrap([&](error_code ec) {
                    stream_.cancel(ec);

                    std::lock_guard l(shutdownM_);
                    timer_.cancel();
                    state_ = ST_SHUTDOWN;
                    shutdownCv_.notify_one();
                }));
            }
        }
        else
        {
            std::lock_guard<std::mutex> l(shutdownM_);
            state_ = ST_SHUTDOWN;
            shutdownCv_.notify_one();
        }
    }));
}

void
WebsocketClient::shutdown()
{
    if (state_ == ST_SHUTDOWN)
        return;
    cleanup();
    std::unique_lock l{shutdownM_};
    if (state_ != ST_SHUTDOWN)
        shutdownCv_.wait(l, [this] { return state_ == ST_SHUTDOWN; });
}

WebsocketClient::WebsocketClient(
    std::function<void(Json::Value const&)> onMessage,
    std::function<void()> onConnect,
    boost::asio::io_service& ios,
    beast::IP::Endpoint const& ip,
    std::unordered_map<std::string, std::string> const& headers,
    beast::Journal j)
    : ios_(ios)
    , strand_(ios_)
    , stream_(ios_)
    , ws_(stream_)
    , onMessageCallback_(onMessage)
    , timer_(ios)
    , ep_(ip.address(), ip.port())
    , headers_(headers)
    , onConnectCallback_(onConnect)
    , j_{j}
    , callbackThread_(&WebsocketClient::runCallbacks, this)
{
}

WebsocketClient::~WebsocketClient()
{
    shutdown();
    if (callbackThread_.joinable())
        callbackThread_.join();
}

void
WebsocketClient::connect()
{
    try
    {
        {
            std::lock_guard<std::mutex> ls(shutdownM_);
            if (state_ == ST_SHUTDOWN)
                return;

            std::lock_guard lw{m_};

            rb_.clear();
            // TODO: Change all the beast::IP:Endpoints to boost endpoints
            stream_.connect(ep_);
            ws_.set_option(boost::beast::websocket::stream_base::decorator(
                [&](boost::beast::websocket::request_type& req) {
                    for (auto const& h : headers_)
                        req.set(h.first, h.second);
                }));
            ws_.handshake(
                ep_.address().to_string() + ":" + std::to_string(ep_.port()),
                "/");
            state_ = ST_CONNECTED;

            JLOGV(
                j_.info(),
                "WebsocketClient connected to",
                jv("ip", ep_.address()),
                jv("port", ep_.port()));

            ws_.async_read(
                rb_,
                std::bind(
                    &WebsocketClient::onReadMsg, this, std::placeholders::_1));
        }

        onConnectCallback_();
    }
    catch (std::exception& e)
    {
        JLOGV(
            j_.debug(),
            "WebsocketClient::exception connecting to endpoint",
            jv("what", e.what()),
            jv("ip", ep_.address()),
            jv("port", ep_.port()));
        reconnect("exception in connection");
    }
}

std::uint32_t
WebsocketClient::send(
    std::string const& cmd,
    Json::Value params,
    std::string const& chain,
    std::function<void(std::uint32_t)> onID)
{
    {
        std::lock_guard<std::mutex> l(shutdownM_);
        if (state_ != ST_CONNECTED)
        {
            // Attestations will be re-send after TTL, everything else - after
            // reconnect
            JLOGV(
                j_.trace(),
                "WebsocketClient::send",
                jv("chainType", chain),
                jv("error", "not connected"));
            return 0;
        }
    }

    params[ripple::jss::method] = cmd;
    params[ripple::jss::jsonrpc] = "2.0";
    params[ripple::jss::ripplerpc] = "2.0";

    auto const id = nextId_++;
    onID(id);
    params[ripple::jss::id] = id;
    auto const s = to_string(params);
    JLOGV(
        j_.trace(),
        "WebsocketClient::send",
        jv("chainType", chain),
        jv("msg", params));
    try
    {
        std::lock_guard l{m_};
        ws_.write_some(true, boost::asio::buffer(s));
    }
    catch (...)
    {
        reconnect("exception at sending data");
    }
    return id;
}

void
WebsocketClient::onReadMsg(error_code const& ec)
{
    if (ec)
    {
        boost::beast::websocket::close_reason reason;
        {
            std::lock_guard l{m_};
            reason = ws_.reason();
        }

        JLOGV(
            j_.error(),
            "WebsocketClient::onReadMsg error",
            jv("ec", ec),
            jv("code", reason.code),
            jv("msg", reason.reason));

        reconnect("error reading data");
        return;
    }

    {
        std::lock_guard l(messageMut_);
        receivingQueue_.push_back(std::move(rb_));
        messageCv_.notify_one();
    }

    {
        std::lock_guard l{m_};
        if (state_ != ST_CONNECTED)
            return;
        rb_.clear();
        ws_.async_read(
            rb_,
            std::bind(
                &WebsocketClient::onReadMsg, this, std::placeholders::_1));
    }
}

void
WebsocketClient::reconnect(std::string_view reason)
{
    std::lock_guard<std::mutex> l(shutdownM_);

    if (state_ == ST_SHUTDOWN)
        return;
    state_ = ST_INIT;

    JLOGV(j_.info(), "WebsocketClient::reconnect()", jv("reason", reason));

    boost::system::error_code ecc;
    stream_.close(ecc);
    timer_.expires_after(CONNECT_TIMEOUT);
    timer_.async_wait([this](boost::system::error_code const& ec) {
        if (ec == boost::asio::error::operation_aborted)
            return;
        this->connect();
    });
}

// Called when the read op terminates
void
WebsocketClient::onReadDone()
{
}

void
WebsocketClient::runCallbacks()
{
    std::uint64_t maxSize = 0;

    for (; state_ != ST_SHUTDOWN;)
    {
        processingQueue_.clear();
        {
            std::unique_lock l{messageMut_};
            if (receivingQueue_.empty())
                messageCv_.wait_for(l, 50ms);
            processingQueue_.swap(receivingQueue_);
        }

        auto const x = processingQueue_.size();

        if (x > maxSize)
        {
            maxSize = x;
            JLOGV(
                j_.info(),
                "WebsocketClient::runCallbacks",
                jv("Updated maxQueueSize", maxSize));
        }

        for (auto const& rb : processingQueue_)
        {
            if (state_ == ST_SHUTDOWN)
                break;

            auto const s = buffer_string(rb.data());
            // JLOGV(j_.trace(), "WebsocketClient::runCallbacks",
            // jv("queueSize", x), jv("msg", s));
            Json::Value jval;
            Json::Reader jr;
            jr.parse(s, jval);
            onMessageCallback_(jval);
        }
    }

    JLOGV(
        j_.info(),
        "WebsocketClient::runCallbacks finished",
        jv("maxQueueSize", maxSize));
}

}  // namespace xbwd
