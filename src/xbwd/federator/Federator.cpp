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
#include <xbwd/client/RpcResultParse.h>
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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
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

    auto getSubmitAccount =
        [&](ChainType chainType) -> std::optional<ripple::AccountID> {
        auto const& chainConfig = config.chainConfig[chainType];
        if (chainConfig.txnSubmit && chainConfig.txnSubmit->shouldSubmit)
        {
            return chainConfig.txnSubmit->submittingAccount;
        }
        return {};
    };

    std::shared_ptr<ChainListener> mainchainListener =
        std::make_shared<ChainListener>(
            ChainType::locking,
            config.bridges,
            getSubmitAccount(ChainType::locking),
            r,
            j);
    std::shared_ptr<ChainListener> sidechainListener =
        std::make_shared<ChainListener>(
            ChainType::issuing,
            config.bridges,
            getSubmitAccount(ChainType::issuing),
            r,
            j);
    r->init(
        ios,
        config.chainConfig[ChainType::locking].chainIp,
        std::move(mainchainListener),
        config.chainConfig[ChainType::issuing].chainIp,
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
    , chains_{Chain{config.chainConfig[ChainType::locking]}, Chain{config.chainConfig[ChainType::issuing]}}
    , autoSubmit_{chains_[ChainType::locking].txnSubmit_ &&
                  chains_[ChainType::locking].txnSubmit_->shouldSubmit,
                  chains_[ChainType::issuing].txnSubmit_ &&
                  chains_[ChainType::issuing].txnSubmit_->shouldSubmit}
    , keyType_{config.keyType}
    , signingPK_{derivePublicKey(config.keyType, config.signingKey)}
    , signingSK_{config.signingKey}
    , j_(j)
{
    for (auto const& b : config.bridges)
    {
        bridges_.try_emplace(b);

        for (auto const ct : {ChainType::locking, ChainType::issuing})
        {
            auto it = accounts_[ct].try_emplace(b.door(ct));
            it.first->second.signerListInfo_.ignoreSignerList_ =
                config.chainConfig[ct].ignoreSignerList;
        }
    }

    for (auto const ct : {ChainType::locking, ChainType::issuing})
    {
        for (auto const& [accID, lastAtt] :
             config.chainConfig[ct].lastAttestedCommitTxMap)
        {
            auto it = accounts_[ct].find(accID);
            if (it == accounts_[ct].end())
            {
                JLOGV(
                    j_.fatal(),
                    "Invalid config",
                    ripple::jv("account", accID),
                    ripple::jv("chain", to_string(ct)));
                throw std::runtime_error("Invalid config");
            }
            it->second.lastAttestedCommitTx_ = lastAtt;
        }
    }

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
    auto fillLastTxHash = [&]() -> bool {
        try
        {
            auto session = app_.getXChainTxnDB().checkoutDb();
            auto const sql = fmt::format(
                R"sql(SELECT DoorAccountID, ChainType, TransID, LedgerSeq FROM {table_name};
            )sql",
                fmt::arg("table_name", db_init::xChainSyncTable));

            std::uint32_t chainType = 0;
            std::string accountID, transID;
            std::uint32_t ledgerSeq = 0;
            int rows = 0;
            soci::statement st =
                ((*session).prepare << sql,
                 soci::into(accountID),
                 soci::into(chainType),
                 soci::into(transID),
                 soci::into(ledgerSeq));
            st.execute();
            while (st.fetch())
            {
                if (chainType !=
                        static_cast<std::uint32_t>(ChainType::issuing) &&
                    chainType != static_cast<std::uint32_t>(ChainType::locking))
                {
                    JLOG(j_.error())
                        << "error reading database: unknown chain type "
                        << chainType << ". Recreating init sync table.";
                    return false;
                }
                auto const ct = static_cast<ChainType>(chainType);

                ripple::uint256 dbTxnHash;
                if (dbTxnHash.parseHex(transID))
                {
                    JLOG(j_.error())
                        << "error reading database: cannot parse transation "
                           "hash "
                        << transID << ". Recreating init sync table.";
                    return false;
                }

                auto parsedAccID =
                    ripple::parseBase58<ripple::AccountID>(accountID);
                if (!parsedAccID)
                {
                    JLOG(j_.error())
                        << "error reading database: cannot parse accountID "
                           "hash "
                        << accountID << ". Recreating init sync table.";
                    return false;
                }

                if (accounts_[ct].find(*parsedAccID) == accounts_[ct].end())
                {
                    JLOGV(
                        j_.trace(),
                        "Skipping sync record from db",
                        ripple::jv("sync's account", accountID));
                    continue;
                }

                auto& is = accounts_[ct].at(*parsedAccID).initSync_;
                is.dbTxnHash_ = std::move(dbTxnHash);
                is.dbLedgerSqn_ = ledgerSeq;
                ++rows;
            }

            JLOG(j_.info())
                << "Database read finished." << rows << "rows readed.";
            return true;
        }
        catch (std::exception& e)
        {
            JLOGV(
                j_.error(),
                "error reading init sync table.",
                ripple::jv("what", e.what()));
            return false;
        }
    };

    auto initializeInitSyncTable = [&]() {
        try
        {
            {
                auto session = app_.getXChainTxnDB().checkoutDb();
                auto const sql = fmt::format(
                    R"sql(DELETE FROM {table_name};
                    )sql",
                    fmt::arg("table_name", db_init::xChainSyncTable));
                *session << sql;
            }

            for (auto const ct : {ChainType::locking, ChainType::issuing})
            {
                for (auto& [accID, aci] : accounts_[ct])
                {
                    auto const accStr = ripple::toBase58(accID);
                    auto& is = aci.initSync_;
                    is.dbLedgerSqn_ = 0u;
                    is.dbTxnHash_ = {};
                    auto const txnIdHex = ripple::strHex(
                        is.dbTxnHash_.begin(), is.dbTxnHash_.end());

                    auto session = app_.getXChainTxnDB().checkoutDb();
                    auto const sql = fmt::format(
                        R"sql(  INSERT INTO {table_name}
                                (DoorAccountID, ChainType, TransID, LedgerSeq)
                                VALUES
                                (:acc, :ct, :txnId, :lgrSeq);
                        )sql",
                        fmt::arg("table_name", db_init::xChainSyncTable));

                    *session << sql, soci::use(accStr),
                        soci::use(static_cast<std::uint32_t>(ct)),
                        soci::use(txnIdHex), soci::use(is.dbLedgerSqn_);
                }
            }
            JLOG(j_.info()) << "created DB table for initial sync, "
                            << db_init::xChainSyncTable;
        }
        catch (std::exception& e)
        {
            JLOGV(
                j_.fatal(),
                "error creating init sync table.",
                ripple::jv("what", e.what()));
            throw;
        }
    };

    if (!fillLastTxHash())
        initializeInitSyncTable();

    for (auto const ct : {ChainType::locking, ChainType::issuing})
    {
        for (auto& [accID, aci] : accounts_[ct])
        {
            auto const accStr = ripple::toBase58(accID);
            auto& is = aci.initSync_;

            JLOG(j_.trace()) << "Prepare init sync accID: " << accStr
                             << ", side: " << to_string(ct)
                             << ", DB ledgerSqn: " << is.dbLedgerSqn_
                             << ", DB txHash: " << is.dbTxnHash_
                             << (aci.lastAttestedCommitTx_.isNonZero()
                                     ? (", config txHash: " +
                                        to_string(aci.lastAttestedCommitTx_))
                                     : ", no config txHash");
        }
    }

    chains_[ChainType::locking].listener_ = std::move(mainchainListener);
    chains_[ChainType::locking].listener_->init(ios, mainchainIp);
    chains_[ChainType::issuing].listener_ = std::move(sidechainListener);
    chains_[ChainType::issuing].listener_->init(ios, sidechainIp);
}

void
Federator::sendDBAttests(ChainType const ct, auto const& doorID)
{
    auto const chainDir = ct == ChainType::locking ? ChainDir::issuingToLocking
                                                   : ChainDir::lockingToIssuing;
    int commits = 0;
    int creates = 0;

    try
    {
        auto const& tblName = db_init::xChainTableName(chainDir);
        auto session = app_.getXChainTxnDB().checkoutDb();
        soci::blob amtBlob(*session);
        soci::blob bridgeBlob(*session);
        soci::blob sendingAccountBlob(*session);
        soci::blob rewardAccountBlob(*session);
        soci::blob otherChainDstBlob(*session);
        soci::blob publicKeyBlob(*session);
        soci::blob signatureBlob(*session);

        std::string transID;
        int ledgerSeq;
        int claimID;
        int success;

        auto const sql = fmt::format(
            R"sql(SELECT TransID, LedgerSeq, ClaimID, Success, DeliveredAmt,
                     Bridge, SendingAccount, RewardAccount, OtherChainDst,
                     PublicKey, Signature FROM {table_name} ORDER BY ClaimID;
        )sql",
            fmt::arg("table_name", tblName));

        soci::indicator otherChainDstInd;
        soci::statement st =
            ((*session).prepare << sql,
             soci::into(transID),
             soci::into(ledgerSeq),
             soci::into(claimID),
             soci::into(success),
             soci::into(amtBlob),
             soci::into(bridgeBlob),
             soci::into(sendingAccountBlob),
             soci::into(rewardAccountBlob),
             soci::into(otherChainDstBlob, otherChainDstInd),
             soci::into(publicKeyBlob),
             soci::into(signatureBlob));
        st.execute();

        ripple::STXChainBridge bridge;
        while (st.fetch())
        {
            ripple::PublicKey signingPK;
            convert(publicKeyBlob, signingPK);

            ripple::Buffer sigBuf;
            convert(signatureBlob, sigBuf);

            ripple::STAmount sendingAmount;
            convert(amtBlob, sendingAmount, ripple::sfAmount);

            ripple::AccountID sendingAccount;
            convert(sendingAccountBlob, sendingAccount);

            ripple::AccountID rewardAccount;
            convert(rewardAccountBlob, rewardAccount);

            std::optional<ripple::AccountID> optDst;
            if (otherChainDstInd == soci::i_ok)
            {
                optDst.emplace();
                convert(otherChainDstBlob, *optDst);
            }

            convert(bridgeBlob, bridge, ripple::sfXChainBridge);
            if (!bridges_.contains(bridge))
            {
                JLOGV(
                    j_.trace(),
                    "Skipping attestation claim from db",
                    ripple::jv("attestation's bridge", bridge));
                continue;
            }

            if (doorID != bridge.door(ct))
            {
                JLOGV(
                    j_.trace(),
                    "Skipping attestation claim from db",
                    ripple::jv(
                        "attestation's door",
                        ripple::toBase58(bridge.door(ct))),
                    ripple::jv("parameter door", ripple::toBase58(doorID)),
                    ripple::jv("chain", to_string(ct)));
                continue;
            }

            pushAtt(
                bridge,
                ripple::AttestationBatch::AttestationClaim{
                    signingPK,
                    sigBuf,
                    sendingAccount,
                    sendingAmount,
                    rewardAccount,
                    chainDir == ChainDir::lockingToIssuing,
                    static_cast<std::uint64_t>(claimID),
                    optDst},
                ct,
                true);
            ++commits;
        }
    }
    catch (std::exception& e)
    {
        JLOGV(
            j_.fatal(),
            "sendDBAttests error reading commit table.",
            ripple::jv("what", e.what()));
        throw;
    }

    try
    {
        auto const& tblName = db_init::xChainCreateAccountTableName(chainDir);
        auto session = app_.getXChainTxnDB().checkoutDb();
        soci::blob amtBlob(*session);
        soci::blob rewardAmtBlob(*session);
        soci::blob bridgeBlob(*session);
        soci::blob sendingAccountBlob(*session);
        soci::blob rewardAccountBlob(*session);
        soci::blob otherChainDstBlob(*session);
        soci::blob publicKeyBlob(*session);
        soci::blob signatureBlob(*session);

        std::string transID;
        int ledgerSeq;
        int createCount;
        int success;

        auto const sql = fmt::format(
            R"sql(SELECT TransID, LedgerSeq, CreateCount, Success, DeliveredAmt, RewardAmt,
                     Bridge, SendingAccount, RewardAccount, OtherChainDst,
                     PublicKey, Signature FROM {table_name} ORDER BY CreateCount;
        )sql",
            fmt::arg("table_name", tblName));

        soci::indicator otherChainDstInd;
        soci::statement st =
            ((*session).prepare << sql,
             soci::into(transID),
             soci::into(ledgerSeq),
             soci::into(createCount),
             soci::into(success),
             soci::into(amtBlob),
             soci::into(rewardAmtBlob),
             soci::into(bridgeBlob),
             soci::into(sendingAccountBlob),
             soci::into(rewardAccountBlob),
             soci::into(otherChainDstBlob, otherChainDstInd),
             soci::into(publicKeyBlob),
             soci::into(signatureBlob));
        st.execute();

        ripple::STXChainBridge bridge;
        while (st.fetch())
        {
            ripple::PublicKey signingPK;
            convert(publicKeyBlob, signingPK);

            ripple::Buffer sigBuf;
            convert(signatureBlob, sigBuf);

            ripple::STAmount sendingAmount;
            convert(amtBlob, sendingAmount, ripple::sfAmount);

            ripple::STAmount rewardAmount;
            convert(rewardAmtBlob, rewardAmount, ripple::sfAmount);

            ripple::AccountID sendingAccount;
            convert(sendingAccountBlob, sendingAccount);

            ripple::AccountID rewardAccount;
            convert(rewardAccountBlob, rewardAccount);

            ripple::AccountID dstAccount;
            convert(otherChainDstBlob, dstAccount);

            convert(bridgeBlob, bridge, ripple::sfXChainBridge);
            if (!bridges_.contains(bridge))
            {
                JLOGV(
                    j_.trace(),
                    "Skipping attestation create account from db",
                    ripple::jv("attestation's bridge", bridge));
                continue;
            }

            if (doorID != bridge.door(ct))
            {
                JLOGV(
                    j_.trace(),
                    "Skipping attestation create account from db",
                    ripple::jv(
                        "attestation's door",
                        ripple::toBase58(bridge.door(ct))),
                    ripple::jv("parameter door", ripple::toBase58(doorID)),
                    ripple::jv("chain", to_string(ct)));
                continue;
            }

            pushAtt(
                bridge,
                ripple::AttestationBatch::AttestationCreateAccount{
                    signingPK,
                    sigBuf,
                    sendingAccount,
                    sendingAmount,
                    rewardAmount,
                    rewardAccount,
                    chainDir == ChainDir::lockingToIssuing,
                    static_cast<std::uint64_t>(createCount),
                    dstAccount},
                ct,
                true);
            ++creates;
        }
    }
    catch (std::exception& e)
    {
        JLOGV(
            j_.fatal(),
            "sendDBAttests error reading createAccount table.",
            ripple::jv("what", e.what()));
        throw;
    }

    JLOG(j_.trace()) << "sendDBAttests " << to_string(ct) << " commit "
                     << commits << " create account " << creates;
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
Federator::initSync(
    ChainType const ct,
    ripple::AccountID const& doorID,
    ripple::AccountID const& otherDoorID,
    ripple::uint256 const& eHash,
    std::int32_t const rpcOrder,
    FederatorEvent const& e)
{
    auto& aci(accounts_[ct].at(doorID));
    auto& is(aci.initSync_);

    if (!is.historyDone_)
    {
        auto const& aciOther(accounts_[otherChain(ct)].at(otherDoorID));
        if (eHash.isNonZero() &&
            ((is.dbTxnHash_ == eHash) ||
             (aciOther.lastAttestedCommitTx_ == eHash)))
        {
            is.historyDone_ = true;
            is.rpcOrder_ = rpcOrder;
            JLOG(j_.trace()) << "initSync found previous tx " << to_string(ct)
                             << " " << eHash;
        }
    }

    bool const historical = rpcOrder < 0;
    bool const skip = historical && is.historyDone_;
    if (!skip)
    {
        if (historical)
            aci.replays_.emplace_front(e);
        else
            aci.replays_.emplace_back(e);
    }

    tryFinishInitSync(ct, doorID);
}

void
Federator::tryFinishInitSync(
    ChainType const ct,
    ripple::AccountID const& doorID)
{
    auto& aci(accounts_[ct].at(doorID));
    auto& is(aci.initSync_);

    if (!is.historyDone_ || !is.oldTxExpired_)
        return;

    JLOGV(
        j_.debug(),
        "initSyncDone.",
        ripple::jv("chain", to_string(ct)),
        ripple::jv("account", toBase58(doorID)),
        ripple::jv("events to replay", aci.replays_.size()));

    is.syncing_ = false;
    chains_[otherChain(ct)].listener_->stopHistoricalTxns(doorID);
    if (autoSubmit_[ct])
        sendDBAttests(ct, doorID);
    for (auto const& event : aci.replays_)
    {
        std::visit([this](auto const& e) { this->onEvent(e); }, event);
    }
    aci.replays_.clear();
}

void
Federator::onEvent(event::XChainCommitDetected const& e)
{
    ChainType const dstChain = e.dir_ == ChainDir::lockingToIssuing
        ? ChainType::issuing
        : ChainType::locking;

    JLOGV(
        j_.trace(),
        "onEvent XChainTransferDetected",
        ripple::jv("chain", to_string(dstChain)),
        ripple::jv("event", e.toJson()));

    auto const& doorID(e.bridge_.door(dstChain));
    auto const doorAccStr(ripple::toBase58(doorID));
    auto const& is(accounts_[dstChain].at(doorID).initSync_);

    if (is.syncing_)
    {
        if (!e.rpcOrder_)
            return;
        auto const& otherDoorID(e.bridge_.door(otherChain(dstChain)));
        initSync(dstChain, doorID, otherDoorID, e.txnHash_, *e.rpcOrder_, e);
        return;
    }

    if (e.rpcOrder_ && *e.rpcOrder_ < is.rpcOrder_)
    {
        // don't need older ones
        return;
    }

    auto const& tblName = db_init::xChainTableName(e.dir_);
    auto const txnIdHex = ripple::strHex(e.txnHash_.begin(), e.txnHash_.end());
    {
        auto session = app_.getXChainTxnDB().checkoutDb();
        auto const sql = fmt::format(
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
        e.bridge_.add(s);
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

        auto const sql = fmt::format(
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
    {
        auto session = app_.getXChainTxnDB().checkoutDb();
        auto const sql = fmt::format(
            R"sql(UPDATE {table_name} SET TransID = :tx_hash WHERE DoorAccountID = :acc_id AND ChainType = :chain_type;
            )sql",
            fmt::arg("table_name", db_init::xChainSyncTable));
        auto const chainType = static_cast<std::uint32_t>(dstChain);
        *session << sql, soci::use(txnIdHex), soci::use(doorAccStr),
            soci::use(chainType);
    }

    if (autoSubmit_[dstChain] && claimOpt)
    {
        bool processNow = e.ledgerBoundary_ || !e.rpcOrder_;
        pushAtt(e.bridge_, std::move(*claimOpt), dstChain, processNow);
    }
}

void
Federator::onEvent(event::XChainAccountCreateCommitDetected const& e)
{
    ChainType const dstChain = e.dir_ == ChainDir::lockingToIssuing
        ? ChainType::issuing
        : ChainType::locking;

    auto const& doorID(e.bridge_.door(dstChain));
    auto const doorAccStr(ripple::toBase58(doorID));
    auto const& is(accounts_[dstChain].at(doorID).initSync_);

    JLOGV(
        j_.trace(),
        "onEvent XChainAccountCreateDetected",
        ripple::jv("chain", to_string(dstChain)),
        ripple::jv("event", e.toJson()));

    if (is.syncing_)
    {
        if (!e.rpcOrder_)
            return;
        auto const& otherDoorID(e.bridge_.door(otherChain(dstChain)));
        initSync(dstChain, doorID, otherDoorID, e.txnHash_, *e.rpcOrder_, e);
        return;
    }

    if (e.rpcOrder_ && *e.rpcOrder_ < is.rpcOrder_)
    {
        // don't need older ones
        return;
    }

    auto const& tblName = db_init::xChainCreateAccountTableName(e.dir_);
    auto const txnIdHex = ripple::strHex(e.txnHash_.begin(), e.txnHash_.end());
    {
        auto session = app_.getXChainTxnDB().checkoutDb();
        auto const sql = fmt::format(
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
        convert(e.bridge_, bridgeBlob);

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

        auto const sql = fmt::format(
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
    {
        auto session = app_.getXChainTxnDB().checkoutDb();
        auto const sql = fmt::format(
            R"sql(UPDATE {table_name} SET TransID = :tx_hash WHERE DoorAccountID = :acc_id AND ChainType = :chain_type;
            )sql",
            fmt::arg("table_name", db_init::xChainSyncTable));
        auto const chainType = static_cast<std::uint32_t>(dstChain);
        *session << sql, soci::use(txnIdHex), soci::use(doorAccStr),
            soci::use(chainType);
    }
    if (autoSubmit_[dstChain] && createOpt)
    {
        bool processNow = e.ledgerBoundary_ || !e.rpcOrder_;
        pushAtt(e.bridge_, std::move(*createOpt), dstChain, processNow);
    }
}

void
Federator::onEvent(event::XChainTransferResult const& e)
{
    // TODO: Update the database with result info
    // really need this?
}

void
Federator::onEvent(event::HeartbeatTimer const& e)
{
    JLOG(j_.trace()) << "HeartbeatTimer";
}

void
Federator::onEvent(event::EndOfHistory const& e)
{
    JLOGV(j_.trace(), "init EndOfHistory", ripple::jv("event", e.toJson()));

    auto const ct = otherChain(e.chainType_);
    bool bFound = false;

    for (auto const& [b, ai] : bridges_)
    {
        auto const& bridgeDoor(b.door(e.chainType_));
        if (e.accounts_.find(bridgeDoor) != e.accounts_.end())
        {
            bFound = true;
            auto const& bridgeOppositeDoor(b.door(ct));
            auto& is(accounts_[ct].at(bridgeOppositeDoor).initSync_);

            if (is.syncing_)
            {
                is.historyDone_ = true;
                tryFinishInitSync(ct, bridgeOppositeDoor);
            }
        }
    }

    if (!bFound)
    {
        JLOGV(
            j_.warn(),
            "EndOfHistory - accounts not found",
            ripple::jv("event", e.toJson()));
    }
}

std::pair<std::string, std::string>
forAttestIDs(
    ripple::STXChainAttestationBatch const& batch,
    std::function<void(std::uint64_t id)> commitFunc = [](std::uint64_t) {},
    std::function<void(std::uint64_t id)> createFunc = [](std::uint64_t) {})
{
    std::stringstream commitAttests;
    std::stringstream createAttests;
    auto temp = ripple::STXChainAttestationBatch::for_each_claim_batch<int>(
        batch.claims().begin(),
        batch.claims().end(),
        [&](auto batchStart, auto batchEnd) -> int {
            for (auto i = batchStart; i != batchEnd; ++i)
            {
                commitAttests << ":" << i->claimID;
                commitFunc(i->claimID);
            }
            return 0;
        });

    temp = ripple::STXChainAttestationBatch::for_each_create_batch<int>(
        batch.creates().begin(),
        batch.creates().end(),
        [&](auto batchStart, auto batchEnd) -> int {
            for (auto i = batchStart; i != batchEnd; ++i)
            {
                createAttests << ":" << i->createCount;
                createFunc(i->createCount);
            }
            return 0;
        });

    return {commitAttests.str(), createAttests.str()};
}

static std::unordered_set<ripple::TERUnderlyingType> SkippableTxnResult(
    {ripple::tesSUCCESS,
     ripple::tecXCHAIN_NO_CLAIM_ID,
     ripple::tecXCHAIN_SENDING_ACCOUNT_MISMATCH,
     ripple::tecXCHAIN_ACCOUNT_CREATE_PAST,
     ripple::tecXCHAIN_WRONG_CHAIN,
     ripple::tecXCHAIN_PROOF_UNKNOWN_KEY,
     ripple::tecXCHAIN_NO_SIGNERS_LIST,
     ripple::tecBAD_XCHAIN_TRANSFER_ISSUE,
     ripple::tecINSUFFICIENT_RESERVE,
     ripple::tecNO_DST_INSUF_XRP});

void
Federator::onEvent(event::XChainAttestsResult const& e)
{
    JLOGV(
        j_.debug(),
        "XChainAttestsResult",
        ripple::jv("chain", to_string(e.chainType_)),
        ripple::jv("accountSqn", e.accountSqn_),
        ripple::jv("result", transHuman(e.ter_)));

    if (!autoSubmit_[e.chainType_])
        return;

    if (SkippableTxnResult.find(TERtoInt(e.ter_)) != SkippableTxnResult.end())
    {
        std::lock_guard l{txnsMutex_};
        auto& subs = submitted_[e.chainType_];
        if (auto i = std::find_if(
                subs.begin(),
                subs.end(),
                [&](auto const& i) { return i.accountSqn_ == e.accountSqn_; });
            i != subs.end())
        {
            auto const attestedIDs = forAttestIDs(
                i->batch_,
                [&](std::uint64_t id) {
                    deleteFromDB(e.chainType_, id, false);
                },
                [&](std::uint64_t id) {
                    deleteFromDB(e.chainType_, id, true);
                });
            JLOGV(
                j_.trace(),
                "XChainAttestsResult ",
                ripple::jv("chain", to_string(e.chainType_)),
                ripple::jv("accountSqn", e.accountSqn_),
                ripple::jv("result", e.ter_),
                ripple::jv("commitAttests", attestedIDs.first),
                ripple::jv("createAttests", attestedIDs.second));

            subs.erase(i);
        }
    }
    // else, will resubmit after txn ttl (i.e. TxnTTLLedgers = 4) ledgers
    // may also get here during init sync.
}

void
Federator::onEvent(event::NewLedger const& e)
{
    for (auto& [doorID, aci] : accounts_[e.chainType_])
    {
        JLOGV(
            j_.trace(),
            "NewLedger",
            ripple::jv("chain", to_string(e.chainType_)),
            ripple::jv("ledgerIndex", e.ledgerIndex_),
            ripple::jv("fee", e.fee_));
        ledgerIndexes_[e.chainType_].store(e.ledgerIndex_);
        ledgerFees_[e.chainType_].store(e.fee_);

        auto& is(aci.initSync_);
        if (is.syncing_)
        {
            is.oldTxExpired_ = e.ledgerIndex_ > is.dbLedgerSqn_;
            tryFinishInitSync(e.chainType_, doorID);
            continue;
        }

        if (!autoSubmit_[e.chainType_])
            return;

        bool notify = false;
        {
            std::lock_guard l{txnsMutex_};
            auto& subs = submitted_[e.chainType_];
            // add expired txn to errored_ for resubmit
            auto notInclude =
                std::find_if(subs.begin(), subs.end(), [&](auto const& s) {
                    return s.lastLedgerSeq_ > e.ledgerIndex_;
                });
            while (subs.begin() != notInclude)
            {
                assert(!is.syncing_);
                auto& front = subs.front();
                if (front.retriesAllowed_ > 0)
                {
                    front.retriesAllowed_--;
                    front.accountSqn_ = 0;
                    front.lastLedgerSeq_ = 0;
                    errored_[e.chainType_].emplace_back(front);
                }
                else
                {
                    auto const attestedIDs = forAttestIDs(front.batch_);
                    JLOGV(
                        j_.warn(),
                        "Giving up after repeated retries",
                        ripple::jv("chain", to_string(e.chainType_)),
                        ripple::jv("commitAttests", attestedIDs.first),
                        ripple::jv("createAttests", attestedIDs.second),
                        ripple::jv(
                            "batch",
                            front.batch_.getJson(ripple::JsonOptions::none)));
                }
                submitted_[e.chainType_].pop_front();
            }
            notify = !errored_[e.chainType_].empty();
        }
        if (notify)
        {
            std::lock_guard l(cvMutexes_[lt_txnSubmit]);
            cvs_[lt_txnSubmit].notify_one();
        }
        else
        {
            std::lock_guard bl{batchMutex_};
            for (auto const& [b, ai] : bridges_)
            {
                if ((b.door(e.chainType_) == doorID) &&
                    (ai.curClaimAtts_[e.chainType_].size() +
                         ai.curCreateAtts_[e.chainType_].size() >
                     0))
                    pushAttOnSubmitTxn(b, e.chainType_);
            }
        }
    }
}

void
Federator::updateSignerListStatus(
    ripple::AccountID const& doorID,
    ChainType const chainType)
{
    auto& aci(accounts_[chainType].at(doorID));
    auto& signerListInfo(aci.signerListInfo_);

    auto const signingAcc = calcAccountID(signingPK_);

    // check signer list
    signerListInfo.status_ = signerListInfo.presentInSignerList_
        ? SignerListInfo::present
        : SignerListInfo::absent;

    // check master key
    if ((signerListInfo.status_ != SignerListInfo::present) &&
        !signerListInfo.disableMaster_)
    {
        if (doorID == signingAcc)
            signerListInfo.status_ = SignerListInfo::present;
    }

    // check regular key
    if ((signerListInfo.status_ != SignerListInfo::present) &&
        signerListInfo.regularDoorID_.isNonZero() &&
        (signerListInfo.regularDoorID_ == signingAcc))
    {
        signerListInfo.status_ = SignerListInfo::present;
    }
}

void
Federator::onEvent(event::XChainSignerListSet const& e)
{
    auto const signingAcc = calcAccountID(signingPK_);
    auto& aci(accounts_[e.chainType_].at(e.masterDoorID_));
    auto& signerListInfo(aci.signerListInfo_);

    signerListInfo.presentInSignerList_ =
        e.signerList_.find(signingAcc) != e.signerList_.end();
    updateSignerListStatus(e.masterDoorID_, e.chainType_);

    JLOGV(
        j_.info(),
        "event::XChainSignerListSet",
        ripple::jv("SigningAcc", ripple::toBase58(signingAcc)),
        ripple::jv("DoorID", ripple::toBase58(e.masterDoorID_)),
        ripple::jv("ChainType", to_string(e.chainType_)),
        ripple::jv("SignerListInfo", signerListInfo.toJson()));
}

void
Federator::onEvent(event::XChainSetRegularKey const& e)
{
    auto const signingAcc = calcAccountID(signingPK_);
    auto& aci(accounts_[e.chainType_].at(e.masterDoorID_));
    auto& signerListInfo(aci.signerListInfo_);

    signerListInfo.regularDoorID_ = e.regularDoorID_;
    updateSignerListStatus(e.masterDoorID_, e.chainType_);

    JLOGV(
        j_.info(),
        "event::XChainSetRegularKey",
        ripple::jv("SigningAcc", ripple::toBase58(signingAcc)),
        ripple::jv("DoorID", ripple::toBase58(e.masterDoorID_)),
        ripple::jv("ChainType", to_string(e.chainType_)),
        ripple::jv("SignerListInfo", signerListInfo.toJson()));
}

void
Federator::onEvent(event::XChainAccountSet const& e)
{
    auto const signingAcc = calcAccountID(signingPK_);
    auto& aci(accounts_[e.chainType_].at(e.masterDoorID_));
    auto& signerListInfo(aci.signerListInfo_);

    signerListInfo.disableMaster_ = e.disableMaster_;
    updateSignerListStatus(e.masterDoorID_, e.chainType_);

    JLOGV(
        j_.info(),
        "event::XChainAccountSet",
        ripple::jv("SigningAcc", ripple::toBase58(signingAcc)),
        ripple::jv("DoorID", ripple::toBase58(e.masterDoorID_)),
        ripple::jv("ChainType", to_string(e.chainType_)),
        ripple::jv("SignerListInfo", signerListInfo.toJson()));
}

void
Federator::pushAttOnSubmitTxn(
    ripple::STXChainBridge const& bridge,
    ChainType chainType)
{
    // batch mutex must already be held
    bool notify = false;

    auto& ai(bridges_.at(bridge));
    auto const doorID(bridge.door(chainType));
    auto const& aci(accounts_[chainType].at(doorID));
    auto const& signerListInfo(aci.signerListInfo_);

    if (signerListInfo.ignoreSignerList_ ||
        (signerListInfo.status_ != SignerListInfo::absent))
    {
        JLOGV(
            j_.debug(),
            "in signer list, atestations proceed",
            ripple::jv("ChainType", to_string(chainType)));

        std::lock_guard tl{txnsMutex_};
        notify = txns_[ChainType::locking].empty() &&
            txns_[ChainType::issuing].empty();

        txns_[chainType].emplace_back(
            0,
            0,
            ripple::STXChainAttestationBatch{
                bridge,
                ai.curClaimAtts_[chainType].begin(),
                ai.curClaimAtts_[chainType].end(),
                ai.curCreateAtts_[chainType].begin(),
                ai.curCreateAtts_[chainType].end()});
        ai.curClaimAtts_[chainType].clear();
        ai.curCreateAtts_[chainType].clear();
    }
    else
    {
        ai.curClaimAtts_[chainType].clear();
        ai.curCreateAtts_[chainType].clear();

        JLOGV(
            j_.info(),
            "not in signer list, atestations dropped",
            ripple::jv("ChainType", to_string(chainType)));
    }

    if (notify)
    {
        std::lock_guard l(cvMutexes_[lt_txnSubmit]);
        cvs_[lt_txnSubmit].notify_one();
    }
}

void
Federator::pushAtt(
    ripple::STXChainBridge const& bridge,
    ripple::AttestationBatch::AttestationClaim&& att,
    ChainType chainType,
    bool ledgerBoundary)
{
    std::lock_guard bl{batchMutex_};

    auto& ai(bridges_.at(bridge));

    ai.curClaimAtts_[chainType].emplace_back(std::move(att));
    assert(
        ai.curClaimAtts_[chainType].size() +
            ai.curCreateAtts_[chainType].size() <=
        ripple::AttestationBatch::maxAttestations);
    if (ledgerBoundary ||
        ai.curClaimAtts_[chainType].size() +
                ai.curCreateAtts_[chainType].size() >=
            ripple::AttestationBatch::maxAttestations)
        pushAttOnSubmitTxn(bridge, chainType);
}

void
Federator::pushAtt(
    ripple::STXChainBridge const& bridge,
    ripple::AttestationBatch::AttestationCreateAccount&& att,
    ChainType chainType,
    bool ledgerBoundary)
{
    std::lock_guard bl{batchMutex_};

    auto& ai(bridges_.at(bridge));

    ai.curCreateAtts_[chainType].emplace_back(std::move(att));
    assert(
        ai.curClaimAtts_[chainType].size() +
            ai.curCreateAtts_[chainType].size() <=
        ripple::AttestationBatch::maxAttestations);
    if (ledgerBoundary ||
        ai.curClaimAtts_[chainType].size() +
                ai.curCreateAtts_[chainType].size() >=
            ripple::AttestationBatch::maxAttestations)
        pushAttOnSubmitTxn(bridge, chainType);
}

void
Federator::submitTxn(Submission const& submission, ChainType dstChain)
{
    auto const attestedIDs = forAttestIDs(submission.batch_);
    JLOGV(
        j_.trace(),
        "Submitting transaction",
        ripple::jv("chain", to_string(dstChain)),
        ripple::jv("commitAttests", attestedIDs.first),
        ripple::jv("createAttests", attestedIDs.second),
        ripple::jv(
            "batch", submission.batch_.getJson(ripple::JsonOptions::none)));

    if (submission.batch_.numAttestations() == 0)
        return;

    // already verified txnSubmit before call submitTxn()
    config::TxnSubmit const& txnSubmit = *chains_[dstChain].txnSubmit_;
    ripple::XRPAmount fee{ledgerFees_[dstChain].load() + FeeExtraDrops};
    ripple::STTx const toSubmit = txn::getSignedTxn(
        txnSubmit.submittingAccount,
        submission.batch_,
        submission.accountSqn_,
        submission.lastLedgerSeq_,
        fee,
        txnSubmit.keypair,
        j_);

    Json::Value const request = [&] {
        Json::Value r;
        r[ripple::jss::tx_blob] =
            ripple::strHex(toSubmit.getSerializer().peekData());
        return r;
    }();

    auto callback = [&](Json::Value const& v) {
        // drop tem submissions. Other errors will be processed after txn TTL.
        if (v.isMember(ripple::jss::result))
        {
            auto const& result = v[ripple::jss::result];
            if (result.isMember(ripple::jss::engine_result_code) &&
                result[ripple::jss::engine_result_code].isIntegral())
            {
                auto txnTER = ripple::TER::fromInt(
                    result[ripple::jss::engine_result_code].asInt());
                if (ripple::isTemMalformed(txnTER))
                {
                    if (result.isMember(ripple::jss::tx_json))
                    {
                        auto const& txJson = result[ripple::jss::tx_json];
                        if (txJson.isMember(ripple::jss::Sequence) &&
                            txJson[ripple::jss::Sequence].isIntegral())
                        {
                            std::uint32_t sqn =
                                txJson[ripple::jss::Sequence].asUInt();

                            std::lock_guard l{txnsMutex_};
                            auto& subs = submitted_[dstChain];
                            if (auto i = std::find_if(
                                    subs.begin(),
                                    subs.end(),
                                    [&](auto const& i) {
                                        return i.accountSqn_ == sqn;
                                    });
                                i != subs.end())
                            {
                                auto const attestedIDs =
                                    forAttestIDs(submission.batch_);
                                JLOGV(
                                    j_.warn(),
                                    "Tem txn submit result, removing "
                                    "submission",
                                    ripple::jv("account sequence", sqn),
                                    ripple::jv("chain", to_string(dstChain)),
                                    ripple::jv(
                                        "commitAttests", attestedIDs.first),
                                    ripple::jv(
                                        "createAttests", attestedIDs.second));
                                subs.erase(i);
                            }
                        }
                    }
                }
            }
        }
    };

    chains_[dstChain].listener_->send("submit", request, callback);
    JLOG(j_.trace()) << "txn submitted";  // the listener logs as well
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
    ChainArray<std::string> accountStrs;
    for (ChainType ct : {ChainType::locking, ChainType::issuing})
    {
        config::TxnSubmit const& txnSubmit = *chains_[ct].txnSubmit_;
        if (chains_[ct].txnSubmit_ && chains_[ct].txnSubmit_->shouldSubmit)
            accountStrs[ct] =
                ripple::toBase58(chains_[ct].txnSubmit_->submittingAccount);
        else
            JLOG(j_.warn())
                << "Will not submit transaction for chain " << to_string(ct);
    }
    if (accountStrs[ChainType::locking].empty() &&
        accountStrs[ChainType::issuing].empty())
    {
        return;
    }

    auto const lt = lt_txnSubmit;
    {
        std::unique_lock l{loopMutexes_[lt]};
        loopCvs_[lt].wait(l, [this, lt] { return !loopLocked_[lt]; });
    }

    ChainArray<bool> waitingAccountInfo{false, false};
    ChainArray<std::uint32_t> accountInfoSqns{0u, 0u};
    std::mutex accountInfoMutex;
    // return if ready to submit txn
    auto getReady = [&](ChainType chain) -> bool {
        if (ledgerIndexes_[chain] == 0 || ledgerFees_[chain] == 0)
        {
            JLOG(j_.trace())
                << "Not ready, waiting for validated ledgers from stream";
            return false;
        }

        // TODO add other readiness check such as verify if witness is in
        // signerList as needed

        if (accountSqns_[chain] != 0)
            return true;

        {
            std::lock_guard aiLock{accountInfoMutex};
            if (waitingAccountInfo[chain])
                return false;

            if (accountInfoSqns[chain] != 0)
            {
                accountSqns_[chain] = accountInfoSqns[chain];
                accountInfoSqns[chain] = 0;
                return true;
            }
            waitingAccountInfo[chain] = true;
        }

        auto callback = [&, ct = chain](Json::Value const& accountInfo) {
            JLOGV(
                j_.trace(),
                "txn submit account info",
                ripple::jv("accountInfo", accountInfo));
            if (accountInfo.isMember(ripple::jss::result) &&
                accountInfo[ripple::jss::result].isMember("account_data"))
            {
                auto const ad =
                    accountInfo[ripple::jss::result]["account_data"];
                if (ad.isMember(ripple::jss::Sequence) &&
                    ad[ripple::jss::Sequence].isIntegral())
                {
                    std::lock_guard aiLock{accountInfoMutex};
                    assert(waitingAccountInfo[ct] && accountInfoSqns[ct] == 0);
                    accountInfoSqns[ct] = ad[ripple::jss::Sequence].asUInt();
                    waitingAccountInfo[ct] = false;
                    JLOG(j_.trace())
                        << "got account sqn " << accountInfoSqns[ct];
                }
            }
        };
        Json::Value request;
        request[ripple::jss::account] = accountStrs[chain];
        request[ripple::jss::ledger_index] = "validated";
        chains_[chain].listener_->send("account_info", request, callback);
        JLOG(j_.trace()) << "Not ready, waiting account sqn";
        return false;
    };

    std::vector<Submission> localTxns;
    ChainType submitChain = ChainType::locking;
    while (!requestStop_)
    {
        {
            std::lock_guard l{txnsMutex_};
            assert(localTxns.empty());
            for (auto i = 0; i < 2; ++i)
            {
                submitChain = otherChain(submitChain);
                if (accountStrs[submitChain].empty())
                    continue;
                if (errored_[submitChain].empty())
                {
                    if (!txns_[submitChain].empty())
                    {
                        if (!getReady(submitChain))
                            continue;
                        localTxns.swap(txns_[submitChain]);
                        break;
                    }
                }
                else
                {
                    if (submitted_[submitChain].empty())
                    {
                        accountSqns_[submitChain] = 0;
                        if (!getReady(submitChain))
                            continue;
                        localTxns.swap(errored_[submitChain]);
                        break;
                    }
                }
            }
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

        for (auto& txn : localTxns)
        {
            txn.lastLedgerSeq_ =
                ledgerIndexes_[submitChain].load() + TxnTTLLedgers;
            txn.accountSqn_ = accountSqns_[submitChain]++;
            {
                std::lock_guard tl{txnsMutex_};
                submitted_[submitChain].emplace_back(txn);
            }
            {
                // TODO move out of submit loop
                auto session = app_.getXChainTxnDB().checkoutDb();
                auto const sql = fmt::format(
                    R"sql(UPDATE {table_name} SET LedgerSeq = :ledger_sqn WHERE DoorAccountID = :acc_id AND ChainType = :chain_type;
                    )sql",
                    fmt::arg("table_name", db_init::xChainSyncTable));
                auto const chainType = static_cast<std::uint32_t>(submitChain);
                auto const accountIDStr =
                    ripple::toBase58(txn.batch_.bridge().door(submitChain));
                *session << sql, soci::use(txn.lastLedgerSeq_),
                    soci::use(accountIDStr), soci::use(chainType);
                JLOGV(
                    j_.trace(),
                    "syncDB update ledgerSqn txnSubmitLoop",
                    ripple::jv("chain", to_string(submitChain)),
                    ripple::jv("ledgerSqn", txn.lastLedgerSeq_));
            }
            submitTxn(txn, submitChain);
        }
        localTxns.clear();
    }
}

namespace {

struct AttestationsID
{
    std::deque<uint32_t> commitAttests;
    std::deque<uint32_t> createAttests;
};

struct BridgeAtt
{
    unsigned commitCount = 0;
    unsigned createCount = 0;
    std::unordered_map<ripple::STXChainBridge, AttestationsID> att;
    bool initiating = false;

    void
    clear()
    {
        att.clear();
        commitCount = createCount = 0;
    }

    Json::Value
    toJson() const
    {
        Json::Value ret{Json::objectValue};

        ret["initiating"] = initiating ? "True" : "False";
        ret["commit_attests_size"] = commitCount;
        if (commitCount)
        {
            Json::Value attArr{Json::arrayValue};
            for (auto const& [b, ai] : att)
            {
                Json::Value obj{Json::objectValue};
                obj["bridge"] = b.getJson(ripple::JsonOptions::none);

                Json::Value idArr{Json::arrayValue};
                for (auto const& id : ai.commitAttests)
                    idArr.append(id);
                obj["attests"] = std::move(idArr);
                attArr.append(std::move(obj));
            }

            ret["commit_attests"] = attArr;
        }

        ret["create_account_attests_size"] = createCount;
        if (createCount)
        {
            Json::Value attArr{Json::arrayValue};
            for (auto const& [b, ai] : att)
            {
                Json::Value obj{Json::objectValue};
                obj["bridge"] = b.getJson(ripple::JsonOptions::none);

                Json::Value idArr{Json::arrayValue};
                for (auto const id : ai.createAttests)
                    idArr.append(id);
                obj["attests"] = std::move(idArr);
                attArr.append(std::move(obj));
            }

            ret["create_account_attests"] = attArr;
        }

        return ret;
    }
};

template <template <typename, typename...> typename Cont>
void
getAttests(BridgeAtt& bridgeAtt, Cont<Submission> const& submissions)
{
    bridgeAtt.clear();
    for (auto const& a : submissions)
    {
        forAttestIDs(
            a.batch_,
            [&](std::uint64_t id) {
                assert(
                    id <= static_cast<std::uint64_t>(
                              std::numeric_limits<std::uint32_t>::max()));
                bridgeAtt.att[a.batch_.bridge()].commitAttests.push_back(
                    static_cast<uint32_t>(id));
                ++bridgeAtt.commitCount;
            },
            [&](std::uint64_t id) {
                assert(
                    id <= static_cast<std::uint64_t>(
                              std::numeric_limits<std::uint32_t>::max()));
                bridgeAtt.att[a.batch_.bridge()].createAttests.push_back(
                    static_cast<uint32_t>(id));
                ++bridgeAtt.createCount;
            });
    }
}
}  // namespace

Json::Value
Federator::getInfo() const
{
    // TODO
    // Track transactons per secons
    // Track when last transaction or event was submitted
    Json::Value ret{Json::objectValue};
    {
        // Pending events
        // In most cases, events have been moved by event loop thread
        std::lock_guard l{eventsMutex_};
        ret["pending_events_size"] = (int)events_.size();
        if (events_.size() > 0)
        {
            Json::Value pendingEvents{Json::arrayValue};
            for (auto const& event : events_)
            {
                std::visit(
                    [&](auto const& e) { pendingEvents.append(e.toJson()); },
                    event);
            }
            ret["pending_events"] = pendingEvents;
        }
    }

    for (ChainType ct : {ChainType::locking, ChainType::issuing})
    {
        Json::Value side{Json::objectValue};
        side["ledger_index"] = ledgerIndexes_[ct].load();
        side["fee"] = ledgerFees_[ct].load();

        BridgeAtt bridgeAtt;

        {
            std::lock_guard l{txnsMutex_};

            getAttests(bridgeAtt, submitted_[ct]);
            side["submitted"] = bridgeAtt.toJson();

            getAttests(bridgeAtt, errored_[ct]);
            side["errored"] = bridgeAtt.toJson();

            getAttests(bridgeAtt, txns_[ct]);
        }

        {
            std::lock_guard l{batchMutex_};
            for (auto const& [b, ai] : bridges_)
            {
                bridgeAtt.initiating =
                    accounts_[ct].at(b.door(ct)).initSync_.syncing_;

                for (auto const& a : ai.curClaimAtts_[ct])
                {
                    bridgeAtt.att[b].commitAttests.push_back(
                        static_cast<uint32_t>(a.claimID));
                    ++bridgeAtt.commitCount;
                }
                for (auto const& a : ai.curCreateAtts_[ct])
                {
                    bridgeAtt.att[b].createAttests.push_back(
                        static_cast<uint32_t>(a.createCount));
                    ++bridgeAtt.createCount;
                }
            }
        }
        side["pending"] = bridgeAtt.toJson();

        ret[to_string(ct)] = side;
    }

    return ret;
}

void
Federator::deleteFromDB(ChainType ct, std::uint64_t id, bool isCreateAccount)
{
    auto session = app_.getXChainTxnDB().checkoutDb();
    auto const& tblName = [&]() {
        if (isCreateAccount)
            return db_init::xChainCreateAccountTableName(
                ct == ChainType::locking ? ChainDir::issuingToLocking
                                         : ChainDir::lockingToIssuing);
        else
            return db_init::xChainTableName(
                ct == ChainType::locking ? ChainDir::issuingToLocking
                                         : ChainDir::lockingToIssuing);
    }();

    auto const sql = [&]() {
        if (isCreateAccount)
            return fmt::format(
                R"sql(DELETE FROM {table_name} WHERE CreateCount = :cid;
                )sql",
                fmt::arg("table_name", tblName));
        else
            return fmt::format(
                R"sql(DELETE FROM {table_name} WHERE ClaimID = :cid;
                )sql",
                fmt::arg("table_name", tblName));
    }();
    *session << sql, soci::use(id);
};

void
Federator::pullAndAttestTx(
    ripple::STXChainBridge const& bridge,
    ChainType ct,
    ripple::uint256 const& txHash,
    Json::Value& result)
{
    if (accounts_[otherChain(ct)]
            .at(bridge.door(otherChain(ct)))
            .initSync_.syncing_)
    {
        result["error"] = "syncing";
        return;
    }

    auto callback = [this, srcChain = ct](Json::Value const& v) {
        chains_[srcChain].listener_->processTx(v);
    };

    Json::Value request;
    request[ripple::jss::transaction] = to_string(txHash);
    chains_[ct].listener_->send("tx", request, callback);
}

Submission::Submission(
    uint32_t lastLedgerSeq,
    uint32_t accountSqn,
    ripple::STXChainAttestationBatch const& batch)
    : lastLedgerSeq_(lastLedgerSeq), accountSqn_(accountSqn), batch_(batch)
{
}

Json::Value
SignerListInfo::toJson() const
{
    Json::Value result{Json::objectValue};
    result["status"] = static_cast<int>(status_);
    result["disableMaster"] = disableMaster_;
    result["regularDoorID"] = regularDoorID_.isNonZero()
        ? ripple::toBase58(regularDoorID_)
        : std::string();
    result["presentInSignerList"] = presentInSignerList_;
    result["ignoreSignerList"] = ignoreSignerList_;

    return result;
}

}  // namespace xbwd
