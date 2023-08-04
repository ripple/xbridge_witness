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

#include <ripple/basics/hardened_hash.h>
#include <ripple/beast/net/IPEndpoint.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/json/json_value.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/STTx.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/SecretKey.h>
#include <ripple/protocol/XChainAttestations.h>

#include <boost/asio.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
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
    std::uint32_t lastLedgerSeq_;
    std::uint32_t accountSqn_;

    std::uint32_t networkID_;

    std::string logName_;

    virtual ~Submission()
    {
    }

    std::string const&
    getLogName() const;

    // return claimID and createCount lists separated by ':' made from all the
    // AttestationClaim and AttestationCreateAccount in current submission.
    // For ex. {":1:3:4", ":5:9"}
    virtual std::pair<std::string, std::string>
    forAttestIDs(
        std::function<void(std::uint64_t id)> commitFunc = [](std::uint64_t) {},
        std::function<void(std::uint64_t id)> createFunc =
            [](std::uint64_t) {}) const = 0;

    virtual Json::Value
    getJson(
        ripple::JsonOptions const opt = ripple::JsonOptions::none) const = 0;

    virtual std::size_t
    numAttestations() const = 0;

    virtual ripple::STTx
    getSignedTxn(
        config::TxnSubmit const& txn,
        ripple::XRPAmount const& fee,
        beast::Journal j) const = 0;

protected:
    Submission(
        std::uint32_t lastLedgerSeq,
        std::uint32_t accountSqn,
        std::uint32_t networkID,
        std::string_view const logName);
};

typedef std::unique_ptr<Submission> SubmissionPtr;

#ifdef USE_BATCH_ATTESTATION

struct SubmissionBatch : public Submission
{
    ripple::STXChainAttestationBatch batch_;

    SubmissionBatch(
        uint32_t lastLedgerSeq,
        uint32_t accountSqn,
        ripple::STXChainAttestationBatch const& batch);

    virtual std::pair<std::string, std::string>
    forAttestIDs(
        std::function<void(std::uint64_t id)> commitFunc,
        std::function<void(std::uint64_t id)> createFunc) const override;

    Json::Value
    getJson(ripple::JsonOptions const opt) const override;

    virtual std::size_t
    numAttestations() const override;

    virtual ripple::STTx
    getSignedTxn(
        config::TxnSubmit const& txn,
        ripple::XRPAmount const& fee,
        beast::Journal j) const override;
};

#endif

struct SubmissionClaim : public Submission
{
    ripple::STXChainBridge bridge_;
    ripple::Attestations::AttestationClaim claim_;

    SubmissionClaim(
        std::uint32_t lastLedgerSeq,
        std::uint32_t accountSqn,
        std::uint32_t networkID,
        ripple::STXChainBridge const& bridge,
        ripple::Attestations::AttestationClaim const& claim);

    virtual std::pair<std::string, std::string>
    forAttestIDs(
        std::function<void(std::uint64_t id)> commitFunc,
        std::function<void(std::uint64_t id)> createFunc) const override;

    virtual Json::Value
    getJson(ripple::JsonOptions const opt) const override;

    virtual std::size_t
    numAttestations() const override;

    virtual ripple::STTx
    getSignedTxn(
        config::TxnSubmit const& txn,
        ripple::XRPAmount const& fee,
        beast::Journal j) const override;
};

struct SubmissionCreateAccount : public Submission
{
    ripple::STXChainBridge bridge_;
    ripple::Attestations::AttestationCreateAccount create_;

    SubmissionCreateAccount(
        std::uint32_t lastLedgerSeq,
        std::uint32_t accountSqn,
        std::uint32_t networkID,
        ripple::STXChainBridge const& bridge,
        ripple::Attestations::AttestationCreateAccount const& create);

    virtual std::pair<std::string, std::string>
    forAttestIDs(
        std::function<void(std::uint64_t id)> commitFunc,
        std::function<void(std::uint64_t id)> createFunc) const override;

    virtual Json::Value
    getJson(ripple::JsonOptions const opt) const override;

    virtual std::size_t
    numAttestations() const override;

    virtual ripple::STTx
    getSignedTxn(
        config::TxnSubmit const& txn,
        ripple::XRPAmount const& fee,
        beast::Journal j) const override;
};

struct SignerListInfo
{
    enum KeySignerListStatus : int { unknown = -1, absent = 0, present };

    KeySignerListStatus status_ = KeySignerListStatus::unknown;
    bool presentInSignerList_ = false;
    bool ignoreSignerList_ = false;
    bool disableMaster_ = false;
    ripple::AccountID regularDoorID_;

    Json::Value
    toJson() const;
};

struct AttestedHistoryTx
{
    xbwd::XChainTxnType type_;
    ripple::AccountID src_, dst_;
    std::optional<std::uint64_t> createCount_, claimID_;

    inline bool
    operator==(AttestedHistoryTx const& o) const noexcept
    {
        return type_ == o.type_ && src_ == o.src_ && dst_ == o.dst_ &&
            createCount_ == o.createCount_ && claimID_ == o.claimID_;
    }

    template <class Hasher>
    friend void
    hash_append(Hasher& h, AttestedHistoryTx const& ah) noexcept
    {
        using beast::hash_append;
        hash_append(h, static_cast<int>(ah.type_));
        hash_append(h, ah.src_);
        hash_append(h, ah.dst_);
        hash_append(h, ah.createCount_ ? *ah.createCount_ : 0);
        hash_append(h, ah.claimID_ ? *ah.claimID_ : 0);
    }

    static std::optional<AttestedHistoryTx>
    fromEvent(FederatorEvent const& e);
};

class Federator : public std::enable_shared_from_this<Federator>
{
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
        std::optional<ripple::uint256> lastAttestedCommitTx_;

        explicit Chain(config::ChainConfig const& config);
    };

    ChainArray<Chain> chains_;
    ChainArray<bool const> const autoSubmit_;  // event thread only

    mutable std::mutex eventsMutex_;
    std::vector<FederatorEvent> GUARDED_BY(eventsMutex_) events_;

    mutable std::mutex txnsMutex_;
    ChainArray<std::vector<SubmissionPtr>> GUARDED_BY(txnsMutex_) txns_;
    ChainArray<std::list<SubmissionPtr>> GUARDED_BY(txnsMutex_) submitted_;
    ChainArray<std::vector<SubmissionPtr>> GUARDED_BY(txnsMutex_) errored_;

    std::optional<ripple::AccountID> signingAccount_;
    ripple::KeyType const keyType_;
    ripple::PublicKey const signingPK_;
    ripple::SecretKey const signingSK_;

    ChainArray<SignerListInfo> signerListsInfo_;

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
    ChainArray<std::vector<ripple::Attestations::AttestationClaim>> GUARDED_BY(
        batchMutex_) curClaimAtts_;
    ChainArray<std::vector<ripple::Attestations::AttestationCreateAccount>>
        GUARDED_BY(batchMutex_) curCreateAtts_;
    ChainArray<std::atomic_uint32_t> ledgerIndexes_{0u, 0u};
    ChainArray<std::atomic_uint32_t> ledgerFees_{0u, 0u};
    ChainArray<std::uint32_t> accountSqns_{0u, 0u};  // tx submit thread only

    struct InitSync
    {
        std::atomic<bool> syncing_{true};
        ripple::uint256 dbTxnHash_;
        std::uint32_t dbLedgerSqn_{0u};
        bool historyDone_{false};
        bool oldTxExpired_{false};
        std::int32_t rpcOrder_{std::numeric_limits<std::int32_t>::min()};
        std::unordered_set<AttestedHistoryTx, ripple::hardened_hash<>>
            attestedTx_;
    };

    ChainArray<InitSync> initSync_;
    ChainArray<std::deque<FederatorEvent>> replays_;
    beast::Journal j_;

    bool const useBatch_;

    ChainArray<std::atomic_uint32_t> networkID_;

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

    /**
     * Answering a RPC request for attesting an out of order transaction.
     * The local witness node sends a tx RPC request to the connected
     * rippled node to pull the details of a transaction. If the response
     * has the right details, attest the transaction.
     *
     * @param bridge the bridge spec
     * @param ct the chain type
     * @param txHash the transaction hash
     * @param result the response to the RPC request.
     */
    void
    pullAndAttestTx(
        ripple::STXChainBridge const& bridge,
        ChainType ct,
        ripple::uint256 const& txHash,
        Json::Value& result);

    void
    checkSigningKey(
        ChainType const ct,
        bool const masterDisabled,
        std::optional<ripple::AccountID> const& regularAcc);

    void
    setNetwordID(std::uint32_t networkID, ChainType ct);

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
    onEvent(event::XChainSetRegularKey const& e);

    void
    onEvent(event::XChainAccountSet const& e);

    void
    updateSignerListStatus(ChainType const chainType);

    void
    onEvent(event::EndOfHistory const& e);

    void
    initSync(
        ChainType const ct,
        ripple::uint256 const& eHash,
        std::int32_t const rpcOrder,
        FederatorEvent const& e);

    void
    tryFinishInitSync(ChainType const ct);

    bool
    isSyncing() const;

    void
    pushAtt(
        ripple::STXChainBridge const& bridge,
        ripple::Attestations::AttestationClaim&& att,
        ChainType chainType,
        bool ledgerBoundary);

    void
    pushAtt(
        ripple::STXChainBridge const& bridge,
        ripple::Attestations::AttestationCreateAccount&& att,
        ChainType chainType,
        bool ledgerBoundary);

    // Code to run from `pushAtt` when submitting a transaction
    void
    pushAttOnSubmitTxn(
        ripple::STXChainBridge const& bridge,
        ChainType chainType) REQUIRES(batchMutex_)
        EXCLUDES(txnsMutex_, cvMutexes_);

    void
    submitTxn(SubmissionPtr&& submission, ChainType dstChain);

    void
    deleteFromDB(
        ChainType ct,
        std::uint64_t claimID,
        bool isCreateAccount);  // TODO add bridge

    void
    sendDBAttests(ChainType ct);

    friend std::shared_ptr<Federator>
    make_Federator(
        App& app,
        boost::asio::io_service& ios,
        config::Config const& config,
        beast::Journal j);

    std::size_t
    maxAttests() const;
};

std::shared_ptr<Federator>
make_Federator(
    App& app,
    boost::asio::io_service& ios,
    config::Config const& config,
    beast::Journal j);

}  // namespace xbwd
