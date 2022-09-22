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

#include <xbwd/federator/Federator.h>

#include <xbwd/app/App.h>
#include <xbwd/app/DBInit.h>
#include <xbwd/basics/ChainTypes.h>
#include <xbwd/federator/TxnSupport.h>

#include <ripple/basics/strHex.h>
#include <ripple/beast/core/CurrentThreadName.h>
#include <ripple/json/Output.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STParsedJSON.h>
#include <ripple/protocol/STTx.h>
#include <ripple/protocol/STXChainAttestationBatch.h>
#include <ripple/protocol/Seed.h>
#include <ripple/protocol/jss.h>

#include <fmt/core.h>

#include <chrono>
#include <cmath>
#include <exception>
#include <future>
#include <sstream>
#include <stdexcept>

namespace xbwd {

std::shared_ptr<Federator>
make_Federator(
    App& app,
    boost::asio::io_service& ios,
    config::Config const& config,
    beast::Journal j)
{
    auto r =
        std::make_shared<Federator>(Federator::PrivateTag{}, app, config, j);

    std::shared_ptr<ChainListener> mainchainListener =
        std::make_shared<ChainListener>(
            ChainListener::IsMainchain::yes, config.bridge, r, j);
    std::shared_ptr<ChainListener> sidechainListener =
        std::make_shared<ChainListener>(
            ChainListener::IsMainchain::no, config.bridge, r, j);
    r->init(
        ios,
        config.lockingChainConfig.chainIp,
        std::move(mainchainListener),
        config.issuingChainConfig.chainIp,
        std::move(sidechainListener));

    return r;
}

Federator::Chain::Chain(config::ChainConfig const& config)
    : rewardAccount_{config.rewardAccount}, txnSubmit_(config.txnSubmit)
{
}

Federator::Federator(
    PrivateTag,
    App& app,
    config::Config const& config,
    beast::Journal j)
    : app_{app}
    , bridge_{config.bridge}
    , chains_{Chain{config.lockingChainConfig}, Chain{config.issuingChainConfig}}
    , keyType_{config.keyType}
    , signingPK_{derivePublicKey(config.keyType, config.signingKey)}
    , signingSK_{config.signingKey}
    , j_(j)
{
    std::fill(loopLocked_.begin(), loopLocked_.end(), true);
    events_.reserve(16);
}

void
Federator::init(
    boost::asio::io_service& ios,
    beast::IP::Endpoint const& mainchainIp,
    std::shared_ptr<ChainListener>&& mainchainListener,
    beast::IP::Endpoint const& sidechainIp,
    std::shared_ptr<ChainListener>&& sidechainListener)
{
    chains_[ChainType::locking].listener_ = std::move(mainchainListener);
    chains_[ChainType::locking].listener_->init(ios, mainchainIp);
    chains_[ChainType::issuing].listener_ = std::move(sidechainListener);
    chains_[ChainType::issuing].listener_->init(ios, sidechainIp);
}

Federator::~Federator()
{
    assert(!running_);
}

void
Federator::start()
{
    if (running_)
        return;
    requestStop_ = false;
    running_ = true;

    threads_[lt_event] = std::thread([this]() {
        beast::setCurrentThreadName("FederatorEvents");
        this->mainLoop();
    });

    threads_[lt_txnSubmit] = std::thread([this]() {
        beast::setCurrentThreadName("FederatorTxns");
        this->txnSubmitLoop();
    });
}

void
Federator::stop()
{
    if (running_)
    {
        requestStop_ = true;
        for (int i = 0; i < lt_last; ++i)
        {
            std::lock_guard l(cvMutexes_[i]);
            cvs_[i].notify_one();
        }

        for (int i = 0; i < lt_last; ++i)
            threads_[i].join();
        running_ = false;
    }
    chains_[ChainType::locking].listener_->shutdown();
    chains_[ChainType::issuing].listener_->shutdown();
}

void
Federator::push(FederatorEvent&& e)
{
    bool notify = false;
    {
        std::lock_guard l{eventsMutex_};
        notify = events_.empty();
        events_.push_back(std::move(e));
    }
    if (notify)
    {
        std::lock_guard l(cvMutexes_[lt_event]);
        cvs_[lt_event].notify_one();
    }
}

void
Federator::onEvent(event::XChainCommitDetected const& e)
{
    JLOGV(
        j_.trace(),
        "onEvent XChainTransferDetected",
        ripple::jv("event", e.toJson()));

    auto const& tblName = db_init::xChainTableName(e.dir_);
    ChainType const dstChain = e.dir_ == ChainDir::lockingToIssuing
        ? ChainType::issuing
        : ChainType::locking;

    auto const txnIdHex = ripple::strHex(e.txnHash_.begin(), e.txnHash_.end());

    {
        auto session = app_.getXChainTxnDB().checkoutDb();
        auto sql = fmt::format(
            R"sql(SELECT count(*) FROM {table_name} WHERE TransID = "{tx_hex}";)sql",
            fmt::arg("table_name", tblName),
            fmt::arg("tx_hex", txnIdHex));

        int count = 0;
        *session << sql, soci::into(count);
        if (session->got_data() && count > 0)
        {
            // Already have this transaction
            // TODO: Sanity check the claim id and deliveredAmt match
            // TODO: Stop historical transaction collection
            JLOGV(
                j_.fatal(),
                "onEvent XChainTransferDetected already present",
                ripple::jv("event", e.toJson()));
            return;  // Don't store it again
        }
    }

    int const success =
        ripple::isTesSuccess(e.status_) ? 1 : 0;  // soci complains about a bool
    auto const& rewardAccount = chains_[dstChain].rewardAccount_;
    auto const& optDst = e.otherChainDst_;

    // non-const so it may be moved from
    auto claimOpt =
        [&]() -> std::optional<ripple::AttestationBatch::AttestationClaim> {
        if (!success)
            return std::nullopt;
        if (!e.deliveredAmt_)
        {
            JLOGV(
                j_.error(),
                "missing delivered amount in successful xchain transfer",
                ripple::jv("event", e.toJson()));
            return std::nullopt;
        }

        return ripple::AttestationBatch::AttestationClaim{
            e.bridge_,
            signingPK_,
            signingSK_,
            e.src_,
            *e.deliveredAmt_,
            rewardAccount,
            e.dir_ == ChainDir::lockingToIssuing,
            e.claimID_,
            optDst};
    }();

    assert(!claimOpt || claimOpt->verify(e.bridge_));

    auto const encodedAmtOpt =
        [&]() -> std::optional<std::vector<std::uint8_t>> {
        if (!e.deliveredAmt_)
            return std::nullopt;
        ripple::Serializer s;
        e.deliveredAmt_->add(s);
        return std::move(s.modData());
    }();

    std::vector<std::uint8_t> const encodedBridge = [&] {
        ripple::Serializer s;
        bridge_.add(s);
        return std::move(s.modData());
    }();

    {
        auto session = app_.getXChainTxnDB().checkoutDb();

        // Soci blob does not play well with optional. Store an empty blob
        // when missing delivered amount
        soci::blob amtBlob{*session};
        if (encodedAmtOpt)
        {
            convert(*encodedAmtOpt, amtBlob);
        }

        soci::blob bridgeBlob(*session);
        convert(encodedBridge, bridgeBlob);

        soci::blob sendingAccountBlob(*session);
        // Convert to an AccountID first, because if the type changes we
        // want to catch it.
        ripple::AccountID const& sendingAccount{e.src_};
        convert(sendingAccount, sendingAccountBlob);

        soci::blob rewardAccountBlob(*session);
        convert(rewardAccount, rewardAccountBlob);

        soci::blob publicKeyBlob(*session);
        convert(signingPK_, publicKeyBlob);

        soci::blob signatureBlob(*session);
        if (claimOpt)
        {
            convert(claimOpt->signature, signatureBlob);
        }

        soci::blob otherChainDstBlob(*session);
        if (optDst)
        {
            convert(*optDst, otherChainDstBlob);
        }

        auto sql = fmt::format(
            R"sql(INSERT INTO {table_name}
                  (TransID, LedgerSeq, ClaimID, Success, DeliveredAmt, Bridge,
                   SendingAccount, RewardAccount, OtherChainDst, PublicKey, Signature)
                  VALUES
                  (:txnId, :lgrSeq, :claimID, :success, :amt, :bridge,
                   :sendingAccount, :rewardAccount, :otherChainDst, :pk, :sig);
            )sql",
            fmt::arg("table_name", tblName));

        *session << sql, soci::use(txnIdHex), soci::use(e.ledgerSeq_),
            soci::use(e.claimID_), soci::use(success), soci::use(amtBlob),
            soci::use(bridgeBlob), soci::use(sendingAccountBlob),
            soci::use(rewardAccountBlob), soci::use(otherChainDstBlob),
            soci::use(publicKeyBlob), soci::use(signatureBlob);
    }

    if (claimOpt)
    {
        pushAtt(e.bridge_, std::move(*claimOpt), e.ledgerBoundary_);
    }
}

void
Federator::onEvent(event::XChainAccountCreateCommitDetected const& e)
{
    JLOGV(
        j_.error(),
        "onEvent XChainAccountCreateDetected",
        ripple::jv("event", e.toJson()));

    auto const& tblName = db_init::xChainCreateAccountTableName(e.dir_);
    ChainType const dstChain = e.dir_ == ChainDir::lockingToIssuing
        ? ChainType::issuing
        : ChainType::locking;

    auto const txnIdHex = ripple::strHex(e.txnHash_.begin(), e.txnHash_.end());

    {
        auto session = app_.getXChainTxnDB().checkoutDb();
        auto sql = fmt::format(
            R"sql(SELECT count(*) FROM {table_name} WHERE TransID = "{tx_hex}";)sql",
            fmt::arg("table_name", tblName),
            fmt::arg("tx_hex", txnIdHex));

        int count = 0;
        *session << sql, soci::into(count);
        if (session->got_data() && count > 0)
        {
            // Already have this transaction
            // TODO: Sanity check the claim id and deliveredAmt match
            // TODO: Stop historical transaction collection
            return;  // Don't store it again
        }
    }

    int const success =
        ripple::isTesSuccess(e.status_) ? 1 : 0;  // soci complains about a bool
    auto const& rewardAccount = chains_[dstChain].rewardAccount_;
    auto const& dst = e.otherChainDst_;

    // non-const so it may be moved from
    auto createOpt = [&]()
        -> std::optional<ripple::AttestationBatch::AttestationCreateAccount> {
        if (!success)
            return std::nullopt;
        if (!e.deliveredAmt_)
        {
            JLOGV(
                j_.error(),
                "missing delivered amount in successful xchain create transfer",
                ripple::jv("event", e.toJson()));
            return std::nullopt;
        }

        return ripple::AttestationBatch::AttestationCreateAccount{
            e.bridge_,
            signingPK_,
            signingSK_,
            e.src_,
            *e.deliveredAmt_,
            e.rewardAmt_,
            rewardAccount,
            e.dir_ == ChainDir::lockingToIssuing,
            e.createCount_,
            dst};
    }();

    assert(!createOpt || createOpt->verify(e.bridge_));

    {
        auto session = app_.getXChainTxnDB().checkoutDb();

        // Soci blob does not play well with optional. Store an empty blob when
        // missing delivered amount
        soci::blob amtBlob{*session};
        if (e.deliveredAmt_)
        {
            convert(*e.deliveredAmt_, amtBlob);
        }

        soci::blob rewardAmtBlob{*session};
        convert(e.rewardAmt_, rewardAmtBlob);

        soci::blob bridgeBlob(*session);
        convert(bridge_, bridgeBlob);

        soci::blob sendingAccountBlob(*session);
        // Convert to an AccountID first, because if the type changes we want to
        // catch it.
        ripple::AccountID const& sendingAccount{e.src_};
        convert(sendingAccount, sendingAccountBlob);

        soci::blob rewardAccountBlob(*session);
        convert(rewardAccount, rewardAccountBlob);

        soci::blob publicKeyBlob(*session);
        convert(signingPK_, publicKeyBlob);

        soci::blob signatureBlob(*session);
        if (createOpt)
        {
            convert(createOpt->signature, signatureBlob);
        }

        soci::blob otherChainDstBlob(*session);
        convert(dst, otherChainDstBlob);

        if (e.deliveredAmt_)
            JLOGV(
                j_.trace(),
                "Insert into create table",
                ripple::jv("table_name", tblName),
                ripple::jv("success", success),
                ripple::jv("create_count", e.createCount_),
                ripple::jv("amt", *e.deliveredAmt_),
                ripple::jv("reward_amt", e.rewardAmt_),
                ripple::jv("sending_account", sendingAccount),
                ripple::jv("reward_account", rewardAccount),
                ripple::jv("other_chain_dst", dst));
        else
            JLOGV(
                j_.trace(),
                "Insert into create table",
                ripple::jv("table_name", tblName),
                ripple::jv("success", success),
                ripple::jv("create_count", e.createCount_),
                ripple::jv("amt", "no delivered amt"),
                ripple::jv("reward_amt", e.rewardAmt_),
                ripple::jv("sending_account", sendingAccount),
                ripple::jv("reward_account", rewardAccount),
                ripple::jv("other_chain_dst", dst));

        auto sql = fmt::format(
            R"sql(INSERT INTO {table_name}
                  (TransID, LedgerSeq, CreateCount, Success, DeliveredAmt, RewardAmt, Bridge,
                   SendingAccount, RewardAccount, otherChainDst, PublicKey, Signature)
                  VALUES
                  (:txnId, :lgrSeq, :createCount, :success, :amt, :rewardAmt, :bridge,
                   :sendingAccount, :rewardAccount, :otherChainDst, :pk, :sig);
            )sql",
            fmt::arg("table_name", tblName));

        *session << sql, soci::use(txnIdHex), soci::use(e.ledgerSeq_),
            soci::use(e.createCount_), soci::use(success), soci::use(amtBlob),
            soci::use(rewardAmtBlob), soci::use(bridgeBlob),
            soci::use(sendingAccountBlob), soci::use(rewardAccountBlob),
            soci::use(otherChainDstBlob), soci::use(publicKeyBlob),
            soci::use(signatureBlob);
    }
    if (createOpt)
    {
        pushAtt(e.bridge_, std::move(*createOpt), e.ledgerBoundary_);
    }
}

void
Federator::onEvent(event::XChainTransferResult const& e)
{
    // TODO: Update the database with result info
}

void
Federator::onEvent(event::HeartbeatTimer const& e)
{
    JLOG(j_.trace()) << "HeartbeatTimer";
}

void
Federator::pushAttOnSubmitTxn(ripple::STXChainBridge const& bridge)
{
    // batch mutex must already be held
    bool notify = false;
    {
        std::lock_guard tl{txnsMutex_};
        notify = txns_.empty();
        txns_.push_back(ripple::STXChainAttestationBatch{
            bridge,
            curClaimAtts_.begin(),
            curClaimAtts_.end(),
            curCreateAtts_.begin(),
            curCreateAtts_.end()});
        curClaimAtts_.clear();
        curCreateAtts_.clear();
    }
    if (notify)
    {
        std::lock_guard l(cvMutexes_[lt_event]);
        cvs_[lt_event].notify_one();
    }
}

void
Federator::pushAtt(
    ripple::STXChainBridge const& bridge,
    ripple::AttestationBatch::AttestationClaim&& att,
    bool ledgerBoundary)
{
    std::lock_guard bl{batchMutex_};
    curClaimAtts_.emplace_back(std::move(att));
    assert(
        curClaimAtts_.size() + curCreateAtts_.size() <=
        ripple::AttestationBatch::maxAttestations);
    if (ledgerBoundary ||
        curClaimAtts_.size() + curCreateAtts_.size() >=
            ripple::AttestationBatch::maxAttestations)
        pushAttOnSubmitTxn(bridge);
}

void
Federator::pushAtt(
    ripple::STXChainBridge const& bridge,
    ripple::AttestationBatch::AttestationCreateAccount&& att,
    bool ledgerBoundary)
{
    std::lock_guard bl{batchMutex_};
    curCreateAtts_.emplace_back(std::move(att));
    assert(
        curClaimAtts_.size() + curCreateAtts_.size() <=
        ripple::AttestationBatch::maxAttestations);
    if (ledgerBoundary ||
        curClaimAtts_.size() + curCreateAtts_.size() >=
            ripple::AttestationBatch::maxAttestations)
        pushAttOnSubmitTxn(bridge);
}

void
Federator::submitTxn(ripple::STXChainAttestationBatch const& batch)
{
    JLOGV(
        j_.trace(),
        "Submitting transaction",
        ripple::jv("batch", batch.getJson(ripple::JsonOptions::none)));

    if (batch.numAttestations() == 0)
        return;

    ChainType const dstChain = [&] {
        if (auto const& claims = batch.claims(); !claims.empty())
        {
            return claims.begin()->wasLockingChainSend ? ChainType::issuing
                                                       : ChainType::locking;
        }
        if (auto const& creates = batch.creates(); !creates.empty())
        {
            return creates.begin()->wasLockingChainSend ? ChainType::issuing
                                                        : ChainType::locking;
        }
        assert(0);
        return ChainType::issuing;
    }();

    if (!chains_[dstChain].txnSubmit_)
    {
        JLOG(j_.trace())
            << "Cannot submit transaction without txnSubmit information";
        return;
    }
    config::TxnSubmit const& txnSubmit = *chains_[dstChain].txnSubmit_;
    if (!txnSubmit.shouldSubmit)
    {
        JLOG(j_.trace()) << "Not submitting txn because shouldSubmit is false";
        return;
    }
    std::uint32_t const seq = [&] {
        // TODO repalace this code. It's just a stand in that gets the seq
        // number every time.
        std::promise<Json::Value> promise;
        std::future<Json::Value> future = promise.get_future();
        auto callback = [&promise](Json::Value const& v) {
            promise.set_value(v);
        };
        Json::Value request;
        request[ripple::jss::account] =
            ripple::toBase58(txnSubmit.submittingAccount);
        request[ripple::jss::ledger_index] = "validated";
        chains_[dstChain].listener_->send("account_info", request, callback);
        Json::Value const accountInfo = future.get();
        JLOGV(
            j_.trace(),
            "txn submit account info",
            ripple::jv("accountInfo", accountInfo));
        if (accountInfo.isMember("account_data"))
        {
            auto const ad = accountInfo["account_data"];
            if (ad.isMember("Sequence"))
            {
                return ad["Sequence"].asUInt();
            }
        }
        throw std::runtime_error("Could not find sequence number");
    }();

    // TODO: decide on fee
    ripple::XRPAmount fee{100};
    ripple::STTx const toSubmit = txn::getSignedTxn(
        txnSubmit.submittingAccount,
        batch,
        seq,
        fee,
        txnSubmit.publicKey,
        txnSubmit.signingKey,
        j_);

    Json::Value const request = [&] {
        Json::Value r;
        // TODO add failHard?
        r[ripple::jss::tx_blob] =
            ripple::strHex(toSubmit.getSerializer().peekData());
        return r;
    }();

    // TODO: Save the id and listen for errors
    auto const id = chains_[dstChain].listener_->send("submit", request);
    JLOGV(j_.trace(), "txn submit message id", ripple::jv("id", id));
}

void
Federator::unlockMainLoop()
{
    for (int i = 0; i < lt_last; ++i)
    {
        std::lock_guard l(loopMutexes_[i]);
        loopLocked_[i] = false;
        loopCvs_[i].notify_one();
    }
}

void
Federator::mainLoop()
{
    auto const lt = lt_event;
    {
        std::unique_lock l{loopMutexes_[lt]};
        loopCvs_[lt].wait(l, [this, lt] { return !loopLocked_[lt]; });
    }

    std::vector<FederatorEvent> localEvents;
    localEvents.reserve(16);
    while (!requestStop_)
    {
        {
            std::lock_guard l{eventsMutex_};
            assert(localEvents.empty());
            localEvents.swap(events_);
        }
        if (localEvents.empty())
        {
            using namespace std::chrono_literals;
            // In rare cases, an event may be pushed and the condition
            // variable signaled before the condition variable is waited on.
            // To handle this, set a timeout on the wait.
            std::unique_lock l{cvMutexes_[lt]};
            // Allow for spurious wakeups. The alternative requires locking the
            // eventsMutex_
            cvs_[lt].wait_for(l, 1s);
            continue;
        }

        for (auto const& event : localEvents)
            std::visit([this](auto&& e) { this->onEvent(e); }, event);
        localEvents.clear();
    }
}

void
Federator::txnSubmitLoop()
{
    auto const lt = lt_txnSubmit;
    {
        std::unique_lock l{loopMutexes_[lt]};
        loopCvs_[lt].wait(l, [this, lt] { return !loopLocked_[lt]; });
    }

    std::vector<ripple::STXChainAttestationBatch> localTxns;
    localTxns.reserve(16);
    while (!requestStop_)
    {
        {
            std::lock_guard l{txnsMutex_};
            assert(localTxns.empty());
            localTxns.swap(txns_);
        }
        if (localTxns.empty())
        {
            using namespace std::chrono_literals;
            // In rare cases, an event may be pushed and the condition
            // variable signaled before the condition variable is waited on.
            // To handle this, set a timeout on the wait.
            std::unique_lock l{cvMutexes_[lt]};
            // Allow for spurious wakeups. The alternative requires locking the
            // eventsMutex_
            cvs_[lt].wait_for(l, 1s);
            continue;
        }

        for (auto const& txn : localTxns)
            submitTxn(txn);
        localTxns.clear();
    }
}

Json::Value
Federator::getInfo() const
{
    // TODO
    // Get the events size
    // Get the transactions size
    // Track transactons per secons
    // Track when last transaction or event was submitted
    Json::Value ret{Json::objectValue};
    return ret;
}

}  // namespace xbwd
