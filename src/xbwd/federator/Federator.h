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

#include <xbwd/app/Config.h>
#include <xbwd/basics/ChainTypes.h>
#include <xbwd/basics/ThreadSaftyAnalysis.h>
#include <xbwd/client/ChainListener.h>
#include <xbwd/federator/FederatorEvents.h>

#include <ripple/beast/net/IPEndpoint.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/json/json_value.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/STXChainAttestationBatch.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/SecretKey.h>

#include <boost/asio.hpp>

#include <atomic>
#include <condition_variable>
#include <list>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace xbwd {

class App;

// resubmit at most 5 times.
static constexpr std::uint8_t MaxResubmits = 5;
// attestation txns not in validated ledgers will be dropped after 4 ledgers
static constexpr std::uint8_t TxnTTLLedgers = 4;
// txn fee in addition to the reference txn fee in the last observed ledger
static constexpr std::uint32_t FeeExtraDrops = 10;

struct Submission
{
    std::uint8_t retriesAllowed_ = MaxResubmits;
    // Is it enough?
    // Probably not enough for tecDIR_FULL and tecXCHAIN_ACCOUNT_CREATE_TOO_MANY
    // But there is not a good number for those. They probably need a flow
    // control mechanism and probably does not worth it?
    uint32_t lastLedgerSeq_;
    uint32_t accountSqn_;
    ripple::STXChainAttestationBatch batch_;

    Submission(
        uint32_t lastLedgerSeq,
        uint32_t accountSqn,
        ripple::STXChainAttestationBatch const& batch);
};

class Federator : public std::enable_shared_from_this<Federator>
{
    enum class IsInSignerList: int {iis_not_init = -1, iis_false = 0, iis_true};

    enum LoopTypes { lt_event, lt_txnSubmit, lt_last };
    std::array<std::thread, lt_last> threads_;
    bool running_ = false;
    std::atomic<bool> requestStop_ = false;

    App& app_;
    ripple::STXChainBridge const bridge_;

    struct Chain
    {
        std::shared_ptr<ChainListener> listener_;
        ripple::AccountID rewardAccount_;
        std::optional<config::TxnSubmit> txnSubmit_;
        bool ignoreSignerList_ = false;

        explicit Chain(config::ChainConfig const& config);
    };

    ChainArray<Chain> chains_;

    mutable std::mutex eventsMutex_;
    std::vector<FederatorEvent> GUARDED_BY(eventsMutex_) events_;

    mutable std::mutex txnsMutex_;
    ChainArray<std::vector<Submission>> GUARDED_BY(txnsMutex_) txns_;
    ChainArray<std::list<Submission>> GUARDED_BY(txnsMutex_) submitted_;
    ChainArray<std::vector<Submission>> GUARDED_BY(txnsMutex_) errored_;

    ripple::KeyType const keyType_;
    ripple::PublicKey const signingPK_;
    ripple::SecretKey const signingSK_;

    ChainArray<IsInSignerList>  inSignerList_ =
        ChainArray<IsInSignerList>(IsInSignerList::iis_not_init, IsInSignerList::iis_not_init);

    // Use a condition variable to prevent busy waiting when the queue is
    // empty
    mutable std::array<std::mutex, lt_last> cvMutexes_;
    mutable std::array<std::condition_variable, lt_last> cvs_;

    // prevent the main loop from starting until explictly told to run.
    // This is used to allow bootstrap code to run before any events are
    // processed
    mutable std::array<std::mutex, lt_last> loopMutexes_;
    std::array<bool, lt_last> loopLocked_;
    std::array<std::condition_variable, lt_last> loopCvs_;

    mutable std::mutex batchMutex_;
    // in-progress batches (one collection for each attestation type). Will be
    // submitted when either all the transactions from that ledger are
    // collected, or the batch limit is reached
    // Both collections are guarded by the same mutex because both collections
    // need to be locked to check the total size, and 2) given the events likely
    // come from the same thread there should never be lock contention when
    // adding to the collections
    ChainArray<std::vector<ripple::AttestationBatch::AttestationClaim>>
        GUARDED_BY(batchMutex_) curClaimAtts_;
    ChainArray<std::vector<ripple::AttestationBatch::AttestationCreateAccount>>
        GUARDED_BY(batchMutex_) curCreateAtts_;
    ChainArray<std::atomic<uint32_t>> ledgerIndexes_{0u, 0u};
    ChainArray<std::atomic<uint32_t>> ledgerFees_{0u, 0u};
    ChainArray<uint32_t> accountSqns_{0u, 0u};  // submission thread only

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
        config::Config const& config,
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
    txnSubmitLoop() EXCLUDES(txnSubmitLoopMutex_);

    void
    onEvent(event::XChainCommitDetected const& e);

    void
    onEvent(event::XChainAccountCreateCommitDetected const& e);

    void
    onEvent(event::XChainTransferResult const& e);

    void
    onEvent(event::HeartbeatTimer const& e);

    void
    onEvent(event::NewLedger const& e);

    void
    onEvent(event::XChainAttestsResult const& e);

    void
    onEvent(event::XChainSignerListSet const& e);

    void
    pushAtt(
        ripple::STXChainBridge const& bridge,
        ripple::AttestationBatch::AttestationClaim&& att,
        ChainType chainType,
        bool ledgerBoundary);

    void
    pushAtt(
        ripple::STXChainBridge const& bridge,
        ripple::AttestationBatch::AttestationCreateAccount&& att,
        ChainType chainType,
        bool ledgerBoundary);

    // Code to run from `pushAtt` when submitting a transaction
    void
    pushAttOnSubmitTxn(
        ripple::STXChainBridge const& bridge,
        ChainType chainType) REQUIRES(batchMutex_)
        EXCLUDES(txnsMutex_, cvMutexes_);

    void
    submitTxn(Submission const& submission, ChainType dstChain);

    friend std::shared_ptr<Federator>
    make_Federator(
        App& app,
        boost::asio::io_service& ios,
        config::Config const& config,
        beast::Journal j);
};

std::shared_ptr<Federator>
make_Federator(
    App& app,
    boost::asio::io_service& ios,
    config::Config const& config,
    beast::Journal j);

}  // namespace xbwd
