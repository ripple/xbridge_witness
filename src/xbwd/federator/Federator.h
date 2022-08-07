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
#include <xbwd/client/ChainListener.h>
#include <xbwd/federator/FederatorEvents.h>

#include <ripple/beast/net/IPEndpoint.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/json/json_value.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SecretKey.h>

#include <boost/asio.hpp>

#include <vector>

namespace xbwd {

class App;

class Federator : public std::enable_shared_from_this<Federator>
{
    std::thread thread_;
    bool running_ = false;
    std::atomic<bool> requestStop_ = false;

    App& app_;
    ripple::STXChainBridge const sidechain_;
    std::shared_ptr<ChainListener> mainchainListener_;
    std::shared_ptr<ChainListener> sidechainListener_;

    mutable std::mutex eventsMutex_;
    std::vector<FederatorEvent> GUARDED_BY(eventsMutex_) events_;

    ripple::KeyType const keyType_;
    ripple::PublicKey const signingPK_;
    ripple::SecretKey const signingSK_;
    ripple::AccountID lockingChainRewardAccount_;
    ripple::AccountID issuingChainRewardAccount_;

    // Use a condition variable to prevent busy waiting when the queue is
    // empty
    mutable std::mutex m_;
    std::condition_variable cv_;

    // prevent the main loop from starting until explictly told to run.
    // This is used to allow bootstrap code to run before any events are
    // processed
    mutable std::mutex mainLoopMutex_;
    bool mainLoopLocked_{true};
    std::condition_variable mainLoopCv_;
    beast::Journal j_;

public:
    // Tag so make_Federator can call `std::make_shared`
    class PrivateTag
    {
    };

    // Constructor should be private, but needs to be public so
    // `make_shared` can use it
    Federator(
        PrivateTag,
        App& app,
        ripple::STXChainBridge const& sidechain,
        ripple::KeyType keyType,
        ripple::SecretKey const& signingKey,
        ripple::AccountID lockingChainRewardAccount,
        ripple::AccountID issuingChainRewardAccount,
        beast::Journal j);

    ~Federator();

    void
    start();

    void
    stop() EXCLUDES(m_);

    void
    push(FederatorEvent&& e) EXCLUDES(m_, eventsMutex_);

    // Don't process any events until the bootstrap has a chance to run
    void
    unlockMainLoop() EXCLUDES(m_);

    Json::Value
    getInfo() const;

private:
    // Two phase init needed for shared_from this.
    // Only called from `make_Federator`
    void
    init(
        boost::asio::io_service& ios,
        beast::IP::Endpoint const& mainchainIp,
        std::shared_ptr<ChainListener>&& mainchainListener,
        beast::IP::Endpoint const& sidechainIp,
        std::shared_ptr<ChainListener>&& sidechainListener);

    void
    mainLoop() EXCLUDES(mainLoopMutex_);

    void
    onEvent(event::XChainCommitDetected const& e);

    void
    onEvent(event::XChainAccountCreateCommitDetected const& e);

    void
    onEvent(event::XChainTransferResult const& e);

    void
    onEvent(event::HeartbeatTimer const& e);

    friend std::shared_ptr<Federator>
    make_Federator(
        App& app,
        boost::asio::io_service& ios,
        ripple::STXChainBridge const& sidechain,
        ripple::KeyType keyType,
        ripple::SecretKey const& signingKey,
        beast::IP::Endpoint const& mainchainIp,
        beast::IP::Endpoint const& sidechainIp,
        ripple::AccountID lockingChainRewardAccount,
        ripple::AccountID issuingChainRewardAccount,
        beast::Journal j);
};

std::shared_ptr<Federator>
make_Federator(
    App& app,
    boost::asio::io_service& ios,
    ripple::STXChainBridge const& sidechain,
    ripple::KeyType keyType,
    ripple::SecretKey const& signingKey,
    beast::IP::Endpoint const& mainchainIp,
    beast::IP::Endpoint const& sidechainIp,
    ripple::AccountID lockingChainRewardAccount,
    ripple::AccountID issuingChainRewardAccount,
    beast::Journal j);

}  // namespace xbwd
