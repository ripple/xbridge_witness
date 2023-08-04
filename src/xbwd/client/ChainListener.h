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

#include <xbwd/basics/ChainTypes.h>
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

struct HistoryProcessor
{
    enum state : int {
        CHECK_BRIDGE,
        WAIT_CB,
        RETR_HISTORY,
        RETR_LEDGERS,
        CHECK_LEDGERS,
        FINISHED
    };
    state state_ = CHECK_BRIDGE;

    // Flag that ask to stop processing historical trasactions.
    // Atomic cause can be set from other thread (federator).
    std::atomic_bool stopHistory_ = false;

    // Save last history request before requesting for ledgers
    Json::Value marker_;
    unsigned accoutTxProcessed_ = 0;

    // Ledger that divide transactions on historical and new
    unsigned startupLedger_ = 0;

    // requesting ledgers in batch and check transactions after every batch
    // request
    unsigned const requestLedgerBatch_ = 100;
    unsigned toRequestLedger_ = 0;

    unsigned minValidatedLedger_ = 0;

    void
    clear();
};

class ChainListener : public std::enable_shared_from_this<ChainListener>
{
private:
    ChainType const chainType_;

    ripple::STXChainBridge const bridge_;
    std::string const witnessAccountStr_;
    std::weak_ptr<Federator> const federator_;
    std::optional<ripple::AccountID> const signingAccount_;
    beast::Journal j_;

    std::shared_ptr<WebsocketClient> wsClient_;
    mutable std::mutex callbacksMtx_;

    using RpcCallback = std::function<void(Json::Value const&)>;
    std::unordered_map<std::uint32_t, RpcCallback> GUARDED_BY(callbacksMtx_)
        callbacks_;

    unsigned const minUserLedger_ = 3;
    // Maximum transactions per one request for given account.
    unsigned const txLimit_ = 10;
    // accout_tx request can be divided into chunks (txLimit_ size) with
    // severeal requests. This flag do not allow other transactions request to
    // be started in the middle of current request.
    bool inRequest_ = false;
    // last ledger requested for new tx
    unsigned ledgerReqMax_ = 0;
    // To determine ledger boundary acros consecutive requests for given
    // account.
    std::int32_t prevLedgerIndex_ = 0;
    // Artifical counter to emulate account_history_tx_index from
    // account_history_tx_stream subscription.
    std::int32_t txnHistoryIndex_ = 0;

    HistoryProcessor hp_;

public:
    ChainListener(
        ChainType chainType,
        ripple::STXChainBridge const sidechain,
        std::optional<ripple::AccountID> submitAccountOpt,
        std::weak_ptr<Federator>&& federator,
        std::optional<ripple::AccountID> signingAccount,
        beast::Journal j);

    virtual ~ChainListener();

    void
    init(boost::asio::io_service& ios, beast::IP::Endpoint const& ip);

    void
    shutdown();

    void
    stopHistoricalTxns();

    Json::Value
    getInfo() const;

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
    send(std::string const& cmd, Json::Value const& params) const
        EXCLUDES(callbacksMtx_);

    /**
     * process tx RPC response
     * @param v the response
     */
    void
    processTx(Json::Value const& v) const noexcept;

private:
    void
    onMessage(Json::Value const& msg) EXCLUDES(callbacksMtx_);

    void
    onConnect();

    void
    processMessage(Json::Value const& msg);

    void
    processAccountTx(Json::Value const& msg);

    bool
    processAccountTxHlp(Json::Value const& msg);

    void
    processAccountInfo(Json::Value const& msg) const noexcept;

    void
    processServerInfo(Json::Value const& msg) noexcept;

    void
    processSigningAccountInfo(Json::Value const& msg) const noexcept;

    void
    processSignerListSet(Json::Value const& msg) const noexcept;

    void
    processAccountSet(Json::Value const& msg) const noexcept;

    void
    processSetRegularKey(Json::Value const& msg) const noexcept;

    bool
    processBridgeReq(Json::Value const& msg);

    void
    processNewLedger(unsigned ledger);

    template <class E>
    void
    pushEvent(E&& e) const;

    void
    accountTx(
        std::string const& account,
        unsigned ledger_min = 0,
        unsigned ledger_max = 0,
        Json::Value const& marker = Json::Value());

    void
    requestLedgers();

    void
    sendLedgerReq(unsigned cnt);
};

}  // namespace xbwd
