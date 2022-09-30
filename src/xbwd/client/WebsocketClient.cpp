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

#include <ripple/basics/Log.h>
#include <ripple/json/Output.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>
#include <ripple/json/to_string.h>
#include <ripple/protocol/jss.h>
#include <ripple/server/Port.h>

#include <boost/beast/websocket.hpp>

#include <condition_variable>
#include <iostream>
#include <string>
#include <unordered_map>

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
        if (!peerClosed_)
        {
            {
                std::lock_guard l{m_};
                ws_.async_close({}, strand_.wrap([&](error_code ec) {
                    stream_.cancel(ec);

                    std::lock_guard l(shutdownM_);
                    isShutdown_ = true;
                    shutdownCv_.notify_one();
                }));
            }
        }
        else
        {
            std::lock_guard<std::mutex> l(shutdownM_);
            isShutdown_ = true;
            shutdownCv_.notify_one();
        }
    }));
}

void
WebsocketClient::shutdown()
{
    cleanup();
    std::unique_lock l{shutdownM_};
    shutdownCv_.wait(l, [this] { return isShutdown_; });
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
{
}

WebsocketClient::~WebsocketClient()
{
    shutdown();
}

void
WebsocketClient::connect()
{
    std::lock_guard<std::mutex> l(shutdownM_);
    if (isShutdown_)
        return;

    try
    {
        // TODO: Change all the beast::IP:Endpoints to boost endpoints
        stream_.connect(ep_);
        peerClosed_ = false;
        ws_.set_option(boost::beast::websocket::stream_base::decorator(
            [&](boost::beast::websocket::request_type& req) {
                for (auto const& h : headers_)
                    req.set(h.first, h.second);
            }));
        ws_.handshake(
            ep_.address().to_string() + ":" + std::to_string(ep_.port()), "/");
        ws_.async_read(
            rb_,
            strand_.wrap(std::bind(
                &WebsocketClient::onReadMsg, this, std::placeholders::_1)));
        onConnectCallback_();
        JLOGV(
            j_.info(),
            "WebsocketClient connected to",
            ripple::jv("ip", ep_.address()),
            ripple::jv("port", ep_.port()));
    }
    catch (std::exception& e)
    {
        JLOGV(
            j_.debug(),
            "WebsocketClient::exception connecting to endpoint",
            ripple::jv("what", e.what()),
            ripple::jv("ip", ep_.address()),
            ripple::jv("port", ep_.port()));
        boost::system::error_code ecc;
        stream_.close(ecc);
        std::weak_ptr<WebsocketClient> wptr = shared_from_this();
        timer_.expires_after(CONNECT_TIMEOUT);
        timer_.async_wait([wptr](boost::system::error_code const& ec) {
            if (ec == boost::asio::error::operation_aborted)
                return;
            if (auto ptr = wptr.lock(); ptr)
                ptr->connect();
        });
    }
}

std::uint32_t
WebsocketClient::send(std::string const& cmd, Json::Value params)
{
    params[ripple::jss::method] = cmd;
    params[ripple::jss::jsonrpc] = "2.0";
    params[ripple::jss::ripplerpc] = "2.0";

    auto const id = nextId_++;
    params[ripple::jss::id] = id;
    auto const s = to_string(params);

    std::lock_guard l{m_};
    ws_.write_some(true, boost::asio::buffer(s));
    return id;
}

void
WebsocketClient::onReadMsg(error_code const& ec)
{
    if (ec)
    {
        JLOGV(
            j_.trace(),
            "WebsocketClient::onReadMsg error",
            ripple::jv("ec", ec));
        if (ec == boost::beast::websocket::error::closed)
            peerClosed_ = true;
        connect();
        return;
    }

    Json::Value jval;
    Json::Reader jr;
    jr.parse(buffer_string(rb_.data()), jval);
    rb_.consume(rb_.size());
    JLOGV(j_.trace(), "WebsocketClient::onReadMsg", ripple::jv("msg", jval));
    onMessageCallback_(jval);

    std::lock_guard l{m_};
    ws_.async_read(
        rb_,
        strand_.wrap(std::bind(
            &WebsocketClient::onReadMsg, this, std::placeholders::_1)));
}

// Called when the read op terminates
void
WebsocketClient::onReadDone()
{
}

}  // namespace xbwd
