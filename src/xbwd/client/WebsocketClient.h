#pragma once

//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2021 Ripple Labs Inc.

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

#include <xbwd/basics/ThreadSaftyAnalysis.h>

#include <ripple/core/Config.h>

#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/multi_buffer.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/optional.hpp>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>

namespace xbwd {

// TODO: Replace this class with `ServerHandler`
class WebsocketClient : public std::enable_shared_from_this<WebsocketClient>
{
    using error_code = boost::system::error_code;

    template <class ConstBuffers>
    static std::string
    buffer_string(ConstBuffers const& b);

    // mutex for ws_
    std::mutex m_;

    // mutex for shutdown
    std::mutex shutdownM_;
    bool isShutdown_ = false;
    std::condition_variable shutdownCv_;

    boost::asio::io_service& ios_;
    boost::asio::io_service::strand strand_;
    boost::asio::ip::tcp::socket stream_;
    boost::beast::websocket::stream<boost::asio::ip::tcp::socket&> GUARDED_BY(
        m_) ws_;
    boost::beast::multi_buffer rb_;

    std::atomic<bool> peerClosed_{true};

    std::function<void(Json::Value const&)> onMessageCallback_;
    std::atomic<std::uint32_t> nextId_{0};

    boost::asio::basic_waitable_timer<std::chrono::steady_clock> timer_;
    boost::asio::ip::tcp::endpoint const ep_;
    std::unordered_map<std::string, std::string> const headers_;
    std::function<void()> onConnectCallback_;
    beast::Journal j_;

    void
    cleanup();

public:
    // callback will be called from a io_service thread
    WebsocketClient(
        std::function<void(Json::Value const&)> onMessage,
        std::function<void()> onConnect,
        boost::asio::io_service& ios,
        beast::IP::Endpoint const& ip,
        std::unordered_map<std::string, std::string> const& headers,
        beast::Journal j);

    ~WebsocketClient();

    void
    connect();

    // Returns command id that will be returned in the response
    std::uint32_t
    send(std::string const& cmd, Json::Value params) EXCLUDES(m_);

    void
    shutdown() EXCLUDES(shutdownM_);

private:
    void
    onReadMsg(error_code const& ec) EXCLUDES(m_);

    // Called when the read op terminates
    void
    onReadDone();
};

}  // namespace xbwd
