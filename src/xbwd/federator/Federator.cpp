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

#include "ripple/protocol/AccountID.h"
#include <xbwd/federator/Federator.h>

#include <xbwd/app/App.h>
#include <xbwd/app/DBInit.h>

#include <ripple/basics/strHex.h>
#include <ripple/beast/core/CurrentThreadName.h>
#include <ripple/json/Output.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STParsedJSON.h>
#include <ripple/protocol/STXChainAttestationBatch.h>
#include <ripple/protocol/Seed.h>
#include <ripple/protocol/jss.h>

#include <fmt/core.h>

#include <chrono>
#include <cmath>
#include <exception>
#include <sstream>

namespace xbwd {

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
    beast::Journal j)
{
    auto r = std::make_shared<Federator>(
        Federator::PrivateTag{},
        app,
        sidechain,
        keyType,
        signingKey,
        lockingChainRewardAccount,
        issuingChainRewardAccount,
        j);

    std::shared_ptr<ChainListener> mainchainListener =
        std::make_shared<ChainListener>(
            ChainListener::IsMainchain::yes, sidechain, r, j);
    std::shared_ptr<ChainListener> sidechainListener =
        std::make_shared<ChainListener>(
            ChainListener::IsMainchain::no, sidechain, r, j);
    r->init(
        ios,
        mainchainIp,
        std::move(mainchainListener),
        sidechainIp,
        std::move(sidechainListener));

    return r;
}

Federator::Federator(
    PrivateTag,
    App& app,
    ripple::STXChainBridge const& sidechain,
    ripple::KeyType keyType,
    ripple::SecretKey const& signingKey,
    ripple::AccountID lockingChainRewardAccount,
    ripple::AccountID issuingChainRewardAccount,
    beast::Journal j)
    : app_{app}
    , sidechain_{sidechain}
    , keyType_{keyType}
    , signingPK_{derivePublicKey(keyType, signingKey)}
    , signingSK_{signingKey}
    , lockingChainRewardAccount_{lockingChainRewardAccount}
    , issuingChainRewardAccount_{issuingChainRewardAccount}
    , j_(j)
{
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
    mainchainListener_ = std::move(mainchainListener);
    mainchainListener_->init(ios, mainchainIp);
    sidechainListener_ = std::move(sidechainListener);
    sidechainListener_->init(ios, sidechainIp);
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

    thread_ = std::thread([this]() {
        beast::setCurrentThreadName("Federator");
        this->mainLoop();
    });
}

void
Federator::stop()
{
    if (running_)
    {
        requestStop_ = true;
        {
            std::lock_guard<std::mutex> l(m_);
            cv_.notify_one();
        }

        thread_.join();
        running_ = false;
    }
    mainchainListener_->shutdown();
    sidechainListener_->shutdown();
}

void
Federator::push(FederatorEvent&& e)
{
    bool notify = false;
    {
        std::lock_guard<std::mutex> l{eventsMutex_};
        notify = events_.empty();
        events_.push_back(std::move(e));
    }
    if (notify)
    {
        std::lock_guard<std::mutex> l(m_);
        cv_.notify_one();
    }
}

void
Federator::onEvent(event::XChainCommitDetected const& e)
{
    JLOGV(
        j_.error(),
        "onEvent XChainTransferDetected",
        ripple::jv("event", e.toJson()));

    bool const wasLockingChainSend = (e.dir_ == event::Dir::lockingToIssuing);

    auto const& tblName = wasLockingChainSend
        ? db_init::xChainLockingToIssuingTableName()
        : db_init::xChainIssuingToLockingTableName();

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
    auto const& rewardAccount = wasLockingChainSend
        ? issuingChainRewardAccount_
        : lockingChainRewardAccount_;
    auto const& optDst = e.otherChainAccount_;

    auto const sigOpt = [&]() -> std::optional<ripple::Buffer> {
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

        auto const& bridge = e.bridge_;
        auto const& sendingAccount = e.src_;
        auto const& sendingAmount = *e.deliveredAmt_;
        auto const& claimID = e.claimID_;

        auto const toSign = ripple::AttestationBatch::AttestationClaim::message(
            bridge,
            sendingAccount,
            sendingAmount,
            rewardAccount,
            wasLockingChainSend,
            claimID,
            optDst);

        auto const sig =
            sign(signingPK_, signingSK_, ripple::makeSlice(toSign));
        {
            // TODO: Remove this test code
            //
            ripple::AttestationBatch::AttestationClaim claim{
                signingPK_,
                sig,
                sendingAccount,
                sendingAmount,
                rewardAccount,
                wasLockingChainSend,
                claimID,
                optDst};
            assert(claim.verify(bridge));
        }
        return sig;
    }();

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
        sidechain_.add(s);
        return std::move(s.modData());
    }();

    {
        auto session = app_.getXChainTxnDB().checkoutDb();

        // Soci blob does not play well with optional. Store an empty blob when
        // missing delivered amount
        soci::blob amtBlob{*session};
        if (encodedAmtOpt)
        {
            convert(*encodedAmtOpt, amtBlob);
        }

        soci::blob bridgeBlob(*session);
        convert(encodedBridge, bridgeBlob);

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
        if (sigOpt)
        {
            convert(*sigOpt, signatureBlob);
        }

        soci::blob otherChainAccountBlob(*session);
        if (optDst)
        {
            convert(*optDst, otherChainAccountBlob);
        }

        auto sql = fmt::format(
            R"sql(INSERT INTO {table_name}
                  (TransID, LedgerSeq, ClaimID, Success, DeliveredAmt, Bridge,
                   SendingAccount, RewardAccount, OtherChainAccount, PublicKey, Signature)
                  VALUES
                  (:txnId, :lgrSeq, :claimID, :success, :amt, :bridge,
                   :sendingAccount, :rewardAccount, :otherChainAccount, :pk, :sig);
            )sql",
            fmt::arg("table_name", tblName));

        *session << sql, soci::use(txnIdHex), soci::use(e.ledgerSeq_),
            soci::use(e.claimID_), soci::use(success), soci::use(amtBlob),
            soci::use(bridgeBlob), soci::use(sendingAccountBlob),
            soci::use(rewardAccountBlob), soci::use(otherChainAccountBlob),
            soci::use(publicKeyBlob), soci::use(signatureBlob);
    }
}

void
Federator::onEvent(event::XChainAccountCreateCommitDetected const& e)
{
    JLOGV(
        j_.error(),
        "onEvent XChainAccountCreateDetected",
        ripple::jv("event", e.toJson()));

    bool const wasLockingChainSend = (e.dir_ == event::Dir::lockingToIssuing);

    auto const& tblName = wasLockingChainSend
        ? db_init::xChainCreateAccountLockingTableName()
        : db_init::xChainCreateAccountIssuingTableName();

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
    auto const& rewardAccount = wasLockingChainSend
        ? issuingChainRewardAccount_
        : lockingChainRewardAccount_;
    auto const& dst = e.otherChainAccount_;

    auto const sigOpt = [&]() -> std::optional<ripple::Buffer> {
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

        auto const& bridge = e.bridge_;
        auto const& sendingAccount = e.src_;
        auto const& sendingAmount = *e.deliveredAmt_;
        auto const& rewardAmount = e.rewardAmt_;
        auto const& createCount = e.createCount_;

        auto const toSign =
            ripple::AttestationBatch::AttestationCreateAccount::message(
                bridge,
                sendingAccount,
                sendingAmount,
                rewardAmount,
                rewardAccount,
                wasLockingChainSend,
                createCount,
                dst);

        auto const sig =
            sign(signingPK_, signingSK_, ripple::makeSlice(toSign));
        {
            // TODO: Remove this test code
            //
            ripple::AttestationBatch::AttestationCreateAccount claim{
                signingPK_,
                sig,
                sendingAccount,
                sendingAmount,
                rewardAmount,
                rewardAccount,
                wasLockingChainSend,
                createCount,
                dst};
            assert(claim.verify(bridge));
        }
        return sig;
    }();

    auto const encodedAmtOpt =
        [&]() -> std::optional<std::vector<std::uint8_t>> {
        if (!e.deliveredAmt_)
            return std::nullopt;
        ripple::Serializer s;
        e.deliveredAmt_->add(s);
        return std::move(s.modData());
    }();

    auto const encodedRewardAmt = [&] {
        ripple::Serializer s;
        e.rewardAmt_.add(s);
        return std::move(s.modData());
    }();

    std::vector<std::uint8_t> const encodedBridge = [&] {
        ripple::Serializer s;
        sidechain_.add(s);
        return std::move(s.modData());
    }();

    {
        auto session = app_.getXChainTxnDB().checkoutDb();

        // Soci blob does not play well with optional. Store an empty blob when
        // missing delivered amount
        soci::blob amtBlob{*session};
        if (encodedAmtOpt)
        {
            convert(*encodedAmtOpt, amtBlob);
        }

        soci::blob rewardAmtBlob{*session};
        convert(encodedRewardAmt, rewardAmtBlob);

        soci::blob bridgeBlob(*session);
        convert(encodedBridge, bridgeBlob);

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
        if (sigOpt)
        {
            convert(*sigOpt, signatureBlob);
        }

        soci::blob otherChainAccountBlob(*session);
        convert(dst, otherChainAccountBlob);

        auto sql = fmt::format(
            R"sql(INSERT INTO {table_name}
                  (TransID, LedgerSeq, CreateCount, Success, DeliveredAmt, RewardAmt, Bridge,
                   SendingAccount, RewardAccount, otherChainAccountBlob, PublicKey, Signature)
                  VALUES
                  (:txnId, :lgrSeq, :createCount, :success, :amt, :rewardAmt, :bridge,
                   :sendingAccount, :rewardAccount, :otherChainAccount, :pk, :sig);
            )sql",
            fmt::arg("table_name", tblName));

        *session << sql, soci::use(txnIdHex), soci::use(e.ledgerSeq_),
            soci::use(e.createCount_), soci::use(success), soci::use(amtBlob),
            soci::use(rewardAmtBlob), soci::use(bridgeBlob),
            soci::use(sendingAccountBlob), soci::use(rewardAccountBlob),
            soci::use(otherChainAccountBlob), soci::use(publicKeyBlob),
            soci::use(signatureBlob);
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
Federator::unlockMainLoop()
{
    std::lock_guard<std::mutex> l(mainLoopMutex_);
    mainLoopLocked_ = false;
    mainLoopCv_.notify_one();
}

void
Federator::mainLoop()
{
    {
        std::unique_lock l{mainLoopMutex_};
        mainLoopCv_.wait(l, [this] { return !mainLoopLocked_; });
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
            std::unique_lock l{m_};
            // Allow for spurious wakeups. The alternative requires locking the
            // eventsMutex_
            cv_.wait_for(l, 1s);
            continue;
        }

        for (auto const& event : localEvents)
            std::visit([this](auto&& e) { this->onEvent(e); }, event);
        localEvents.clear();
    }
}

Json::Value
Federator::getInfo() const
{
    // TODO
    Json::Value ret{Json::objectValue};
    return ret;
}

}  // namespace xbwd
