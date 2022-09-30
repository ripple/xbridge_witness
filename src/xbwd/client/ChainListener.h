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

#include <ripple/beast/net/IPEndpoint.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/STXChainBridge.h>

#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/address.hpp>

#include <memory>
#include <mutex>
#include <string>

namespace xbwd {

class Federator;
class WebsocketClient;

class ChainListener : public std::enable_shared_from_this<ChainListener>
{
public:
    enum class IsMainchain { no, yes };

private:
    bool const isMainchain_;  // TODO change to isLockingChain?
    ripple::STXChainBridge const bridge_;
    std::string witnessAccountStr_;
    std::weak_ptr<Federator> federator_;
    mutable std::mutex m_;
    beast::Journal j_;

    std::shared_ptr<WebsocketClient> wsClient_;
    mutable std::mutex callbacksMtx_;

    using RpcCallback = std::function<void(Json::Value const&)>;
    std::map<std::uint32_t, RpcCallback> GUARDED_BY(callbacksMtx_) callbacks_;

public:
    ChainListener(
        IsMainchain isMainchain,
        ripple::STXChainBridge const sidechain,
        std::optional<ripple::AccountID> submitAccountOpt,
        std::weak_ptr<Federator>&& federator,
        beast::Journal j);

    virtual ~ChainListener();

    void
    init(boost::asio::io_service& ios, beast::IP::Endpoint const& ip);

    void
    shutdown();

    void
    stopHistoricalTxns();

    Json::Value
    getInfo() const EXCLUDES(m_);

    /**
     * send a RPC and call the callback with the RPC result
     * @param cmd PRC command
     * @param params RPC command parameter
     * @param onResponse callback to process RPC result
     */
    void
    send(
        std::string const& cmd,
        Json::Value const& params,
        RpcCallback onResponse);

    // Returns command id that will be returned in the response
    std::uint32_t
    send(std::string const& cmd, Json::Value const& params)
        EXCLUDES(callbacksMtx_);

private:
    void
    onMessage(Json::Value const& msg) EXCLUDES(callbacksMtx_);

    void
    onConnect();

    std::string const&
    chainName() const;

    void
    processMessage(Json::Value const& msg) EXCLUDES(m_);

    template <class E>
    void
    pushEvent(E&& e) REQUIRES(m_);
};

}  // namespace xbwd
