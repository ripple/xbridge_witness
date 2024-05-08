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
#include <xbwd/client/WebsocketClient.h>

#include <ripple/beast/net/IPEndpoint.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/STXChainBridge.h>

#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/address.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace xbwd {

class Federator;

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
    std::uint32_t accoutTxProcessed_ = 0;

    // Ledger that divide transactions on historical and new
    std::uint32_t startupLedger_ = 0;

    // Requesting ledgers in batch and check transactions after every batch
    // request. Used only when history is not finished in 'normal' way. This
    // means there are some gaps in the history.
    std::uint32_t const requestLedgerBatch_ = 100;
    std::uint32_t toRequestLedger_ = 0;

    // Minimal ledger validated by rippled. Retrieved from server_info
    std::uint32_t minValidatedLedger_ = 0;

    // History processed ledger
    std::atomic_uint ledgerProcessed_ = 0;

    // last processed ledger from previous session
    std::uint32_t lastLedgerProcessed_ = 0;

    void
    clear();
};

class ChainListener
{
private:
    ChainType const chainType_;

    ripple::STXChainBridge const bridge_;
    std::string const submitAccountStr_;
    Federator& federator_;
    std::optional<ripple::AccountID> const signAccount_;
    beast::Journal j_;

    std::unique_ptr<WebsocketClient> wsClient_;
    mutable std::mutex callbacksMtx_;

    using RpcCallback = std::function<void(Json::Value const&)>;
    std::unordered_map<std::uint32_t, RpcCallback> GUARDED_BY(callbacksMtx_)
        callbacks_;

    std::uint32_t const minUserLedger_ = 3;
    // Maximum transactions per one request for given account.
    std::uint32_t const txLimit_ = 10;
    // accout_tx request can be divided into chunks (txLimit_ size) with
    // severeal requests. This flag do not allow other transactions request to
    // be started in the middle of current request.
    bool inRequest_ = false;
    // last ledger requested for new tx
    std::uint32_t ledgerReqMax_ = 0;
    // last ledger that was processed for Door account (in case of errors /
    // disconnects). Will be requested from the other thread.
    std::atomic_uint32_t ledgerProcessedDoor_ = 0;
    // last ledger that was processed for Signing account (in case of errors /
    // disconnects)
    std::atomic_uint32_t ledgerProcessedSubmit_ = 0;
    // To determine ledger boundary acros consecutive requests for given
    // account.
    std::int32_t prevLedgerIndex_ = 0;
    // Artifical counter to emulate account_history_tx_index from
    // account_history_tx_stream subscription.
    std::int32_t txnHistoryIndex_ = 0;

    // current ledger info
    std::atomic_uint32_t ledgerIndex_ = 0;
    std::atomic_uint32_t ledgerFee_ = 0;

    HistoryProcessor hp_;

public:
    ChainListener(
        ChainType chainType,
        ripple::STXChainBridge const sidechain,
        std::optional<ripple::AccountID> submitAccount,
        Federator& federator,
        std::optional<ripple::AccountID> signAccount,
        std::uint32_t txLimit,
        std::uint32_t lastLedgerProcessed,
        beast::Journal j);

    ~ChainListener() = default;

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
    processTx(Json::Value const& v) const;

    std::uint32_t
    getDoorProcessedLedger() const;

    std::uint32_t
    getSubmitProcessedLedger() const;

    std::uint32_t
    getHistoryProcessedLedger() const;

    std::uint32_t
    getCurrentLedger() const;

    std::uint32_t
    getCurrentFee() const;

private:
    void
    onMessage(Json::Value const& msg) EXCLUDES(callbacksMtx_);

    void
    onConnect();

    void
    processMessage(Json::Value const& msg);

    void
    processAccountTx(Json::Value const& msg);

    // return true if request is continue
    bool
    processAccountTxHlp(Json::Value const& msg);

    // return true if no errors in response OR account doesn't exist
    bool
    processAccountInfo(Json::Value const& msg) const;

    void
    processServerInfo(Json::Value const& msg);

    // return true if no errors in response OR account doesn't exist
    bool
    processSigningAccountInfo(Json::Value const& msg) const;

    void
    processSignerListSet(Json::Value const& msg) const;

    void
    processAccountSet(Json::Value const& msg) const;

    void
    processSetRegularKey(Json::Value const& msg) const;

    // return true if bridge exists and no errors in the response
    bool
    processBridgeReq(Json::Value const& msg) const;

    void
    processNewLedger(std::uint32_t ledger);

    void
    initStartupLedger(std::uint32_t ledger);

    template <class E>
    void
    pushEvent(E&& e) const;

    void
    accountTx(
        std::string const& account,
        std::uint32_t ledger_min = 0,
        std::uint32_t ledger_max = 0,
        Json::Value const& marker = Json::Value());

    void
    accountInfo();

    void
    requestLedgers();

    void
    sendLedgerReq(std::uint32_t cnt);
};

}  // namespace xbwd
