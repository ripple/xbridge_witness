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

#include <ripple/basics/strHex.h>
#include <ripple/beast/core/CurrentThreadName.h>
#include <ripple/json/Output.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STParsedJSON.h>
#include <ripple/protocol/STXChainClaimProof.h>
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
    ripple::STSidechain const& sidechain,
    ripple::KeyType keyType,
    ripple::SecretKey const& signingKey,
    beast::IP::Endpoint const& mainchainIp,
    beast::IP::Endpoint const& sidechainIp,
    beast::Journal j)
{
    auto r = std::make_shared<Federator>(
        Federator::PrivateTag{}, app, sidechain, keyType, signingKey, j);

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
    ripple::STSidechain const& sidechain,
    ripple::KeyType keyType,
    ripple::SecretKey const& signingKey,
    beast::Journal j)
    : app_{app}
    , sidechain_{sidechain}
    , keyType_{keyType}
    , signingPK_{derivePublicKey(keyType, signingKey)}
    , signingSK_{signingKey}
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
Federator::onEvent(event::XChainTransferDetected const& e)
{
    JLOGV(
        j_.error(),
        "onEvent XChainTransferDetected",
        ripple::jv("event", e.toJson()));

    bool const wasSrcChainSend = (e.dir_ == event::Dir::mainToSide);

    auto const& tblName = wasSrcChainSend
        ? db_init::xChainMainToSideTableName()
        : db_init::xChainSideToMainTableName();

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
            // TODO: Sanity check the xChainSeq and deliveredAmt match
            // TODO: Stop historical transaction collection
            return;  // Don't store it again
        }
    }

    int const success =
        ripple::isTesSuccess(e.status_) ? 1 : 0;  // soci complains about a bool
    auto const hexSignature = [&]() -> boost::optional<std::string> {
        if (!success)
            return boost::none;
        if (!e.deliveredAmt_)
        {
            JLOGV(
                j_.error(),
                "missing delivered amount in successful xchain transfer",
                ripple::jv("event", e.toJson()));
            return boost::none;
        }
        auto const toSign = ripple::ChainClaimProofMessage(
            sidechain_, *e.deliveredAmt_, e.xChainSeq_, wasSrcChainSend);
        auto const sig =
            sign(signingPK_, signingSK_, ripple::makeSlice(toSign));

        {
            // TODO: Remove this test code
            //
            std::vector<std::pair<ripple::PublicKey, ripple::Buffer>> sigs;
            sigs.emplace_back(signingPK_, sig);
            ripple::STXChainClaimProof proof(
                sidechain_,
                *e.deliveredAmt_,
                e.xChainSeq_,
                wasSrcChainSend,
                std::move(sigs));
            assert(proof.verify());
        }
        return ripple::strHex(sig);
    }();

    auto const encodedAmtOpt =
        [&]() -> std::optional<std::vector<std::uint8_t>> {
        if (!e.deliveredAmt_)
            return std::nullopt;
        ripple::Serializer s;
        e.deliveredAmt_->add(s);
        return std::move(s.modData());
    }();

    // TODO: cache this string
    // TODO: decide on the correct token type
    std::string const publicKey =
        ripple::toBase58(ripple::TokenType::AccountPublic, signingPK_);

    std::vector<std::uint8_t> const encodedSidechain = [&] {
        ripple::Serializer s;
        sidechain_.add(s);
        return std::move(s.modData());
    }();

    {
        auto session = app_.getXChainTxnDB().checkoutDb();
        // Soci blob does not play well with optional. Store an empty blob when
        // missing delivered amount
        soci::blob amtBlob{*session};
        soci::blob sidechainBlob(*session);
        if (encodedAmtOpt)
        {
            convert(*encodedAmtOpt, amtBlob);
        }
        convert(encodedSidechain, sidechainBlob);

        auto sql = fmt::format(
            R"sql(INSERT INTO {table_name}
                  (TransID, LedgerSeq, XChainSeq, Success, DeliveredAmt, Sidechain, PublicKey, Signature)
                  VALUES
                  (:txnId, :lgrSeq, :xChainSeq, :success, :amt, :sidechain, :pk, :sig);
            )sql",
            fmt::arg("table_name", tblName));

        *session << sql, soci::use(txnIdHex), soci::use(e.ledgerSeq_),
            soci::use(e.xChainSeq_), soci::use(success), soci::use(amtBlob),
            soci::use(sidechainBlob), soci::use(publicKey),
            soci::use(hexSignature);
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
