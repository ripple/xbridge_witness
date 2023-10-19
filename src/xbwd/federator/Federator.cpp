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
#include <xbwd/basics/StructuredLog.h>
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
#include <ripple/protocol/Seed.h>
#include <ripple/protocol/jss.h>

#include <fmt/core.h>

#include <algorithm>
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
    ripple::Logs& l)
{
    auto r = std::make_shared<Federator>(
        Federator::PrivateTag{}, app, config, l.journal("Federator"));

    auto getSubmitAccount =
        [&](ChainType chainType) -> std::optional<ripple::AccountID> {
        auto const& chainConfig = chainType == ChainType::locking
            ? config.lockingChainConfig
            : config.issuingChainConfig;
        if (chainConfig.txnSubmit && chainConfig.txnSubmit->shouldSubmit)
        {
            return chainConfig.txnSubmit->submittingAccount;
        }
        return {};
    };

    std::shared_ptr<ChainListener> mainchainListener =
        std::make_shared<ChainListener>(
            ChainType::locking,
            config.bridge,
            getSubmitAccount(ChainType::locking),
            r,
            config.signingAccount,
            l.journal("LListener"));
    std::shared_ptr<ChainListener> sidechainListener =
        std::make_shared<ChainListener>(
            ChainType::issuing,
            config.bridge,
            getSubmitAccount(ChainType::issuing),
            r,
            config.signingAccount,
            l.journal("IListener"));
    r->init(
        ios,
        config.lockingChainConfig.chainIp,
        std::move(mainchainListener),
        config.issuingChainConfig.chainIp,
        std::move(sidechainListener));

    return r;
}

Federator::Chain::Chain(config::ChainConfig const& config)
    : rewardAccount_{config.rewardAccount}
    , txnSubmit_(config.txnSubmit)
    , lastAttestedCommitTx_(config.lastAttestedCommitTx)
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
    , autoSubmit_{chains_[ChainType::locking].txnSubmit_ &&
                  chains_[ChainType::locking].txnSubmit_->shouldSubmit,
                  chains_[ChainType::issuing].txnSubmit_ &&
                  chains_[ChainType::issuing].txnSubmit_->shouldSubmit}
    , keyType_{config.keyType}
    , signingAccount_(config.signingAccount)
    , signingPK_{derivePublicKey(config.keyType, config.signingKey)}
    , signingSK_{config.signingKey}
    , j_(j)
    , useBatch_(config.useBatch)
{
    signerListsInfo_[ChainType::locking].ignoreSignerList_ =
        config.lockingChainConfig.ignoreSignerList;
    signerListsInfo_[ChainType::issuing].ignoreSignerList_ =
        config.issuingChainConfig.ignoreSignerList;

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
                R"sql(SELECT ChainType, TransID, LedgerSeq FROM {table_name};
            )sql",
                fmt::arg("table_name", db_init::xChainSyncTable));

            std::uint32_t chainType = 0;
            std::string transID;
            std::uint32_t ledgerSeq = 0;
            int rows = 0;
            soci::statement st =
                ((*session).prepare << sql,
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

                if (!initSync_[ct].dbTxnHash_.parseHex(transID))
                {
                    JLOG(j_.error())
                        << "error reading database: cannot parse transation "
                           "hash "
                        << transID << ". Recreating init sync table.";
                    return false;
                }

                initSync_[ct].dbLedgerSqn_ = ledgerSeq;
                ++rows;
            }
            return rows == 2;  // both chainTypes
        }
        catch (std::exception& e)
        {
            JLOGV(
                j_.error(),
                "error reading init sync table.",
                jv("what", e.what()));
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
                initSync_[ct].dbLedgerSqn_ = 0u;
                initSync_[ct].dbTxnHash_ = {};
                auto const txnIdHex = ripple::strHex(
                    initSync_[ct].dbTxnHash_.begin(),
                    initSync_[ct].dbTxnHash_.end());
                auto session = app_.getXChainTxnDB().checkoutDb();
                auto const sql = fmt::format(
                    R"sql(INSERT INTO {table_name}
                      (ChainType, TransID, LedgerSeq)
                      VALUES
                      (:ct, :txnId, :lgrSeq);
                )sql",
                    fmt::arg("table_name", db_init::xChainSyncTable));
                *session << sql, soci::use(static_cast<std::uint32_t>(ct)),
                    soci::use(txnIdHex), soci::use(initSync_[ct].dbLedgerSqn_);
            }
            JLOG(j_.info()) << "created DB table for initial sync, "
                            << db_init::xChainSyncTable;
        }
        catch (std::exception& e)
        {
            JLOGV(
                j_.fatal(),
                "error creating init sync table.",
                jv("what", e.what()));
            throw;
        }
    };

    if (!fillLastTxHash())
        initializeInitSyncTable();

    for (auto const ct : {ChainType::locking, ChainType::issuing})
    {
        if (autoSubmit_[ct])
            readDBAttests(ct);
        JLOGV(
            j_.info(),
            "Prepare init sync",
            jv("chainType", to_string(ct)),
            jv("DB ledgerSqn", initSync_[ct].dbLedgerSqn_),
            jv("DB txHash ", initSync_[ct].dbTxnHash_),
            jv("config txHash",
               chains_[ct].lastAttestedCommitTx_
                   ? to_string(*chains_[ct].lastAttestedCommitTx_)
                   : std::string("not set")));
    }

    chains_[ChainType::locking].listener_ = std::move(mainchainListener);
    chains_[ChainType::locking].listener_->init(ios, mainchainIp);
    chains_[ChainType::issuing].listener_ = std::move(sidechainListener);
    chains_[ChainType::issuing].listener_->init(ios, sidechainIp);
}

void
Federator::readDBAttests(ChainType ct)
{
    auto const oct = otherChain(ct);
    int commits = 0;
    int creates = 0;

    try
    {
        auto const& tblName = db_init::xChainTableName(ct);
        auto session = app_.getXChainTxnDB().checkoutDb();
        soci::blob amtBlob(*session);
        soci::blob bridgeBlob(*session);
        soci::blob sendingAccountBlob(*session);
        soci::blob rewardAccountBlob(*session);
        soci::blob otherChainDstBlob(*session);
        soci::blob signingAccountBlob(*session);
        soci::blob publicKeyBlob(*session);
        soci::blob signatureBlob(*session);

        std::string transID;
        int ledgerSeq;
        int claimID;
        int success;

        auto const sql = fmt::format(
            R"sql(SELECT TransID, LedgerSeq, ClaimID, Success, DeliveredAmt,
                     Bridge, SendingAccount, RewardAccount, OtherChainDst,
                     SigningAccount, PublicKey, Signature
                  FROM {table_name} ORDER BY ClaimID;
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
             soci::into(signingAccountBlob),
             soci::into(publicKeyBlob),
             soci::into(signatureBlob));
        st.execute();

        ripple::STXChainBridge bridge;

        while (st.fetch())
        {
            ripple::AccountID signingAccount;
            convert(signingAccountBlob, signingAccount);

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
            if (bridge != bridge_)
            {
                JLOGV(
                    j_.warn(),
                    "readDBAttests bridge mismatch, skipping attestation",
                    jv("db bridge", bridge.getJson(ripple::JsonOptions::none)),
                    jv("current bridge",
                       bridge_.getJson(ripple::JsonOptions::none)));
                continue;
            }

            // The attestation will be created by the other chain
            auto p = SubmissionPtr(new SubmissionClaim(
                0,  // will be updated when new ledger arrive
                0,  // will be updated if resubmitted
                0,  // will be updated when NetworkID arrive
                bridge,
                ripple::Attestations::AttestationClaim{
                    signingAccount,
                    signingPK,
                    sigBuf,
                    sendingAccount,
                    sendingAmount,
                    rewardAccount,
                    ct == ChainType::locking,
                    static_cast<std::uint64_t>(claimID),
                    optDst}));
            {
                std::lock_guard tl{txnsMutex_};
                submitted_[oct].emplace_back(std::move(p));
            }

            ++commits;
        }
    }
    catch (std::exception& e)
    {
        JLOGV(
            j_.fatal(),
            "readDBAttests error reading commit table.",
            jv("what", e.what()));
        throw;
    }

    try
    {
        auto const& tblName = db_init::xChainCreateAccountTableName(ct);
        auto session = app_.getXChainTxnDB().checkoutDb();
        soci::blob amtBlob(*session);
        soci::blob rewardAmtBlob(*session);
        soci::blob bridgeBlob(*session);
        soci::blob sendingAccountBlob(*session);
        soci::blob rewardAccountBlob(*session);
        soci::blob otherChainDstBlob(*session);
        soci::blob signingAccountBlob(*session);
        soci::blob publicKeyBlob(*session);
        soci::blob signatureBlob(*session);

        std::string transID;
        int ledgerSeq;
        int createCount;
        int success;

        auto const sql = fmt::format(
            R"sql(SELECT TransID, LedgerSeq, CreateCount, Success, DeliveredAmt, RewardAmt,
                     Bridge, SendingAccount, RewardAccount, OtherChainDst,
                     SigningAccount, PublicKey, Signature
                  FROM {table_name} ORDER BY CreateCount;
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
             soci::into(signingAccountBlob),
             soci::into(publicKeyBlob),
             soci::into(signatureBlob));
        st.execute();

        ripple::STXChainBridge bridge;

        while (st.fetch())
        {
            ripple::AccountID signingAccount;
            convert(signingAccountBlob, signingAccount);

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
            if (bridge != bridge_)
            {
                JLOGV(
                    j_.warn(),
                    "readDBAttests bridge mismatch, skipping attestation",
                    jv("db bridge", bridge.getJson(ripple::JsonOptions::none)),
                    jv("current bridge",
                       bridge_.getJson(ripple::JsonOptions::none)));
                continue;
            }

            // The attestation will be created by the other chain
            auto p = SubmissionPtr(new SubmissionCreateAccount(
                0,  // will be updated when new ledger arrive
                0,  // will be updated if resubmitted
                0,  // will be updated when NetworkID arrive
                bridge,
                ripple::Attestations::AttestationCreateAccount{
                    signingAccount,
                    signingPK,
                    sigBuf,
                    sendingAccount,
                    sendingAmount,
                    rewardAmount,
                    rewardAccount,
                    ct == ChainType::locking,
                    static_cast<std::uint64_t>(createCount),
                    dstAccount}));
            {
                std::lock_guard tl{txnsMutex_};
                submitted_[oct].emplace_back(std::move(p));
            }

            ++creates;
        }
    }
    catch (std::exception& e)
    {
        JLOGV(
            j_.fatal(),
            "readDBAttests error reading createAccount table.",
            jv("what", e.what()));
        throw;
    }

    JLOG(j_.info()) << "readDBAttests " << to_string(ct) << " commit "
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

// Called from 2 events that require attestations
void
Federator::initSync(
    ChainType const ct,
    ripple::uint256 const& eHash,
    std::int32_t const rpcOrder,
    FederatorEvent const& e)
{
    JLOG(j_.trace()) << "initSync start";

    if (!initSync_[ct].historyDone_)
    {
        if (initSync_[ct].dbTxnHash_ == eHash ||
            (chains_[ct].lastAttestedCommitTx_ == eHash))
        {
            initSync_[ct].historyDone_ = true;
            initSync_[ct].rpcOrder_ = rpcOrder;
            JLOGV(
                j_.info(),
                "initSync found previous tx",
                jv("event", toJson(e)));
        }
    }

    bool const historical = rpcOrder < 0;
    bool const skip = historical && initSync_[ct].historyDone_;
    if (!skip)
    {
        {
            // TODO remove after tests or when adding multiple bridges
            // assert order of insertion, so that the replay later will be in
            // order
            static ChainArray<std::int32_t> rpcOrderNew{-1, -1};
            static ChainArray<std::int32_t> rpcOrderOld{0, 0};
            JLOG(j_.trace()) << "initSync " << to_string(ct)
                             << ", rpcOrderNew=" << rpcOrderNew[ct]
                             << " rpcOrderOld=" << rpcOrderOld[ct]
                             << " rpcOrder=" << rpcOrder;
            if (historical)
            {
                assert(rpcOrderOld[ct] > rpcOrder);
                rpcOrderOld[ct] = rpcOrder;
            }
            else
            {
                assert(rpcOrderNew[ct] < rpcOrder);
                rpcOrderNew[ct] = rpcOrder;
            }
        }
        if (historical)
            replays_[ct].emplace_front(e);
        else
            replays_[ct].emplace_back(e);
    }

    tryFinishInitSync(ct);
}

std::optional<AttestedHistoryTx>
AttestedHistoryTx::fromEvent(FederatorEvent const& r)
{
    if (event::XChainCommitDetected const* pcd =
            std::get_if<event::XChainCommitDetected>(&r))
        return AttestedHistoryTx{
            xbwd::XChainTxnType::xChainAddClaimAttestation,
            pcd->src_,
            pcd->otherChainDst_ ? *pcd->otherChainDst_ : ripple::AccountID(),
            std::nullopt,
            pcd->claimID_};
    else if (
        event::XChainAccountCreateCommitDetected const* pac =
            std::get_if<event::XChainAccountCreateCommitDetected>(&r))
        return AttestedHistoryTx{
            xbwd::XChainTxnType::xChainAddAccountCreateAttestation,
            pac->src_,
            pac->otherChainDst_,
            pac->createCount_,
            std::nullopt};
    else
        return {};
}

void
Federator::tryFinishInitSync(ChainType const ct)
{
    ChainType const oct = otherChain(ct);

    if (!initSync_[ct].historyDone_)
        return;

    if (initSync_[ct].syncing_)
    {
        JLOGV(
            j_.info(),
            "initSyncDone",
            jv("chainType", to_string(ct)),
            jv("account", toBase58(bridge_.door(ct))),
            jv("events to replay", replays_[ct].size()));

        initSync_[ct].syncing_ = false;
        chains_[ct].listener_->stopHistoricalTxns();

        if (initSync_[oct].syncing_)
        {
            JLOGV(
                j_.info(),
                "initSyncDone waiting for other chain",
                jv("other chain", to_string(oct)));
        }
    }

    if (initSync_[oct].syncing_)
        return;

    for (auto const cht : {ChainType::locking, ChainType::issuing})
    {
        auto const ocht = otherChain(cht);
        auto& repl(replays_[cht]);
        unsigned del_cnt = 0;
        std::size_t const rel_size = repl.size();
        for (auto it = repl.begin(); it != repl.end();)
        {
            auto const ah = AttestedHistoryTx::fromEvent(*it);
            // events from the one side checking against attestations from the
            // other side
            if (ah && initSync_[ocht].attestedTx_.contains(*ah))
            {
                ++del_cnt;
                it = repl.erase(it);
            }
            else
                ++it;
        }

        JLOGV(
            j_.info(),
            "initSyncDone start replay",
            jv("chainType", to_string(cht)),
            jv("account", ripple::toBase58(bridge_.door(cht))),
            jv("events to replay", rel_size),
            jv("attested events", initSync_[cht].attestedTx_.size()),
            jv("events to delete", del_cnt));

        for (auto const& event : repl)
        {
            std::visit([this](auto const& e) { this->onEvent(e); }, event);
        }
        repl.clear();
        initSync_[cht].attestedTx_.clear();
    }
}

bool
Federator::isSyncing() const
{
    return initSync_[ChainType::locking].syncing_ ||
        initSync_[ChainType::issuing].syncing_;
}

void
Federator::onEvent(event::XChainCommitDetected const& e)
{
    auto const ct = e.chainType_;
    auto const oct = otherChain(ct);
    JLOGV(j_.debug(), "onEvent XChainCommitDetected", jv("event", e.toJson()));

    if (isSyncing())
    {
        if (!e.rpcOrder_)
        {
            JLOGV(j_.error(), "syncing, no rpc order", jv("event", e.toJson()));
            return;
        }
        initSync(ct, e.txnHash_, *e.rpcOrder_, e);
        return;
    }

    if (e.rpcOrder_ && *e.rpcOrder_ < initSync_[ct].rpcOrder_)
    {
        JLOGV(
            j_.info(),
            "skip older",
            jv("event", e.toJson()),
            jv("initSync rpcOrder", initSync_[ct].rpcOrder_));
        return;
    }

    auto const& tblName = db_init::xChainTableName(ct);
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
                "XChainCommitDetected already present",
                jv("event", e.toJson()));
            return;  // Don't store it again
        }
    }

    // soci complains about a bool
    int const success = ripple::isTesSuccess(e.status_) ? 1 : 0;
    // The attestation will be send from the other chain, so the other chain
    // will get the reward
    auto const& rewardAccount = chains_[oct].rewardAccount_;
    auto const& optDst = e.otherChainDst_;

    // non-const so it may be moved from
    auto claimOpt =
        [&]() -> std::optional<ripple::Attestations::AttestationClaim> {
        if (!success)
            return std::nullopt;
        if (!e.deliveredAmt_)
        {
            JLOGV(
                j_.error(),
                "missing delivered amount in successful xchain transfer",
                jv("event", e.toJson()));
            return std::nullopt;
        }

        return ripple::Attestations::AttestationClaim{
            e.bridge_,
            signingAccount_ ? *signingAccount_
                            : ripple::calcAccountID(signingPK_),
            signingPK_,
            signingSK_,
            e.src_,
            *e.deliveredAmt_,
            rewardAccount,
            ct == ChainType::locking,
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

        soci::blob signingAccountBlob(*session);
        if (claimOpt)
        {
            convert(claimOpt->attestationSignerAccount, signingAccountBlob);
        }

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

        JLOGV(
            j_.trace(),
            "Insert into claim table",
            jv("chainType", to_string(ct)),
            jv("tableName", tblName),
            jv("success", success),
            jv("ledgerSeq", e.ledgerSeq_),
            jv("claimID", fmt::format("{:x}", e.claimID_)),
            jv("amt",
               e.deliveredAmt_ ? e.deliveredAmt_->getFullText()
                               : std::string("no delivered amt")),
            jv("sendingAccount", e.src_),
            jv("rewardAccount", rewardAccount),
            jv("otherChainDst",
               !optDst || !*optDst ? std::string() : ripple::toBase58(*optDst)),
            jv("signingAccount",
               !claimOpt
                   ? std::string()
                   : ripple::toBase58(claimOpt->attestationSignerAccount)));

        auto const sql = fmt::format(
            R"sql(INSERT INTO {table_name}
                  (TransID, LedgerSeq, ClaimID, Success, DeliveredAmt, Bridge,
                   SendingAccount, RewardAccount, OtherChainDst, SigningAccount, PublicKey, Signature)
                  VALUES
                  (:txnId, :lgrSeq, :claimID, :success, :amt, :bridge,
                   :sendingAccount, :rewardAccount, :otherChainDst, :signingAccount, :pk, :sig);
            )sql",
            fmt::arg("table_name", tblName));

        *session << sql, soci::use(txnIdHex), soci::use(e.ledgerSeq_),
            soci::use(e.claimID_), soci::use(success), soci::use(amtBlob),
            soci::use(bridgeBlob), soci::use(sendingAccountBlob),
            soci::use(rewardAccountBlob), soci::use(otherChainDstBlob),
            soci::use(signingAccountBlob), soci::use(publicKeyBlob),
            soci::use(signatureBlob);
    }
    {
        auto session = app_.getXChainTxnDB().checkoutDb();
        auto const sql = fmt::format(
            R"sql(UPDATE {table_name} SET TransID = :tx_hash WHERE ChainType = :chain_type;
            )sql",
            fmt::arg("table_name", db_init::xChainSyncTable));
        auto const chainType = static_cast<std::uint32_t>(ct);
        *session << sql, soci::use(txnIdHex), soci::use(chainType);
    }

    // The attestation will be created by the other chain
    if (autoSubmit_[oct] && claimOpt)
    {
        bool processNow = e.ledgerBoundary_ || !e.rpcOrder_;
        pushAtt(e.bridge_, std::move(*claimOpt), oct, processNow);
    }
}

void
Federator::onEvent(event::XChainAccountCreateCommitDetected const& e)
{
    auto const ct = e.chainType_;
    auto const oct = otherChain(ct);
    JLOGV(
        j_.debug(),
        "onEvent XChainAccountCreateCommitDetected",
        jv("event", e.toJson()));

    if (isSyncing())
    {
        if (!e.rpcOrder_)
        {
            JLOGV(j_.error(), "no rpc order", jv("event", e.toJson()));
            return;
        }
        initSync(ct, e.txnHash_, *e.rpcOrder_, e);
        return;
    }

    if (e.rpcOrder_ && *e.rpcOrder_ < initSync_[ct].rpcOrder_)
    {
        JLOGV(
            j_.info(),
            "skip older",
            jv("event", e.toJson()),
            jv("initSync rpcOrder", initSync_[ct].rpcOrder_));
        return;
    }

    auto const& tblName = db_init::xChainCreateAccountTableName(ct);
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

    // soci complains about a bool
    int const success = ripple::isTesSuccess(e.status_) ? 1 : 0;
    // Attestation will be send from other chain, so the other chain will get
    // the reward
    auto const& rewardAccount = chains_[oct].rewardAccount_;
    auto const& dst = e.otherChainDst_;

    // non-const so it may be moved from
    auto createOpt =
        [&]() -> std::optional<ripple::Attestations::AttestationCreateAccount> {
        if (!success)
            return std::nullopt;
        if (!e.deliveredAmt_)
        {
            JLOGV(
                j_.error(),
                "missing delivered amount in successful xchain create transfer",
                jv("event", e.toJson()));
            return std::nullopt;
        }

        return ripple::Attestations::AttestationCreateAccount{
            e.bridge_,
            signingAccount_ ? *signingAccount_
                            : ripple::calcAccountID(signingPK_),
            signingPK_,
            signingSK_,
            e.src_,
            *e.deliveredAmt_,
            e.rewardAmt_,
            rewardAccount,
            ct == ChainType::locking,
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

        soci::blob signingAccountBlob(*session);
        if (createOpt)
        {
            convert(createOpt->attestationSignerAccount, signingAccountBlob);
        }

        soci::blob publicKeyBlob(*session);
        convert(signingPK_, publicKeyBlob);

        soci::blob signatureBlob(*session);
        if (createOpt)
        {
            convert(createOpt->signature, signatureBlob);
        }

        soci::blob otherChainDstBlob(*session);
        convert(dst, otherChainDstBlob);

        JLOGV(
            j_.trace(),
            "Insert into create table",
            jv("chainType", to_string(ct)),
            jv("tableName", tblName),
            jv("success", success),
            jv("ledgerSeq", e.ledgerSeq_),
            jv("createCount", fmt::format("{:x}", e.createCount_)),
            jv("amt",
               e.deliveredAmt_ ? e.deliveredAmt_->getFullText()
                               : std::string("no delivered amt")),
            jv("rewardAmt", e.rewardAmt_),
            jv("sendingAccount", sendingAccount),
            jv("rewardAccount", rewardAccount),
            jv("otherChainDst", dst),
            jv("signingAccount",
               !createOpt
                   ? std::string()
                   : ripple::toBase58(createOpt->attestationSignerAccount)));

        auto const sql = fmt::format(
            R"sql(INSERT INTO {table_name}
                  (TransID, LedgerSeq, CreateCount, Success, DeliveredAmt, RewardAmt, Bridge,
                   SendingAccount, RewardAccount, otherChainDst, SigningAccount, PublicKey, Signature)
                  VALUES
                  (:txnId, :lgrSeq, :createCount, :success, :amt, :rewardAmt, :bridge,
                   :sendingAccount, :rewardAccount, :otherChainDst, :signingAccount, :pk, :sig);
            )sql",
            fmt::arg("table_name", tblName));

        *session << sql, soci::use(txnIdHex), soci::use(e.ledgerSeq_),
            soci::use(e.createCount_), soci::use(success), soci::use(amtBlob),
            soci::use(rewardAmtBlob), soci::use(bridgeBlob),
            soci::use(sendingAccountBlob), soci::use(rewardAccountBlob),
            soci::use(otherChainDstBlob), soci::use(signingAccountBlob),
            soci::use(publicKeyBlob), soci::use(signatureBlob);
    }
    {
        auto session = app_.getXChainTxnDB().checkoutDb();
        auto const sql = fmt::format(
            R"sql(UPDATE {table_name} SET TransID = :tx_hash WHERE ChainType = :chain_type;
            )sql",
            fmt::arg("table_name", db_init::xChainSyncTable));
        auto const chainType = static_cast<std::uint32_t>(ct);
        *session << sql, soci::use(txnIdHex), soci::use(chainType);
    }

    // The attestation will be created by the other chain
    if (autoSubmit_[oct] && createOpt)
    {
        bool processNow = e.ledgerBoundary_ || !e.rpcOrder_;
        pushAtt(e.bridge_, std::move(*createOpt), oct, processNow);
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
    JLOGV(j_.trace(), "onEvent HeartbeatTimer", jv("event", e.toJson()));
}

void
Federator::onEvent(event::EndOfHistory const& e)
{
    JLOGV(j_.debug(), "onEvent EndOfHistory", jv("event", e.toJson()));
    auto const ct = e.chainType_;
    if (initSync_[ct].syncing_)
    {
        initSync_[ct].historyDone_ = true;
        tryFinishInitSync(ct);
    }
}

#ifdef USE_BATCH_ATTESTATION

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
            commitAttests << std::hex;
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
            createAttests << std::hex;
            for (auto i = batchStart; i != batchEnd; ++i)
            {
                createAttests << ":" << i->createCount;
                createFunc(i->createCount);
            }
            return 0;
        });

    return {commitAttests.str(), createAttests.str()};
}

#endif

std::pair<std::string, std::string>
forAttestIDs(
    ripple::Attestations::AttestationClaim const& claim,
    std::function<void(std::uint64_t id)> commitFunc = [](std::uint64_t) {},
    std::function<void(std::uint64_t id)> createFunc = [](std::uint64_t) {})
{
    std::stringstream commitAttests;

    auto const& claimID(claim.claimID);
    commitAttests << ":" << std::hex << claimID;
    commitFunc(claimID);

    return {commitAttests.str(), std::string()};
}

std::pair<std::string, std::string>
forAttestIDs(
    ripple::Attestations::AttestationCreateAccount const& create,
    std::function<void(std::uint64_t id)> commitFunc = [](std::uint64_t) {},
    std::function<void(std::uint64_t id)> createFunc = [](std::uint64_t) {})
{
    std::stringstream createAttests;

    auto const& createCount(create.createCount);
    createAttests << ":" << std::hex << createCount;
    createFunc(createCount);

    return {std::string(), createAttests.str()};
}

static std::unordered_set<ripple::TERUnderlyingType> SkippableTxnResult(
    {ripple::tesSUCCESS,
     ripple::tecXCHAIN_NO_CLAIM_ID,
     ripple::tecXCHAIN_SENDING_ACCOUNT_MISMATCH,
     ripple::tecXCHAIN_ACCOUNT_CREATE_PAST,
     ripple::tecXCHAIN_WRONG_CHAIN,
     ripple::tecXCHAIN_PROOF_UNKNOWN_KEY,
     ripple::tecXCHAIN_NO_SIGNERS_LIST,
     ripple::tecXCHAIN_BAD_TRANSFER_ISSUE,
     ripple::tecINSUFFICIENT_RESERVE,
     ripple::tecNO_DST_INSUF_XRP});

void
Federator::onEvent(event::XChainAttestsResult const& e)
{
    auto const ct = e.chainType_;
    auto const oct = otherChain(ct);

    JLOGV(j_.debug(), "onEvent XChainAttestsResult", jv("event", e.toJson()));

    if (!autoSubmit_[ct])
        return;

    // will resubmit after txn ttl (i.e. TxnTTLLedgers = 4) ledgers
    // may also get here during init sync.
    if (!SkippableTxnResult.contains(TERtoInt(e.ter_)))
        return;

    // Can be several attestations with the same ClaimID
    std::vector<SubmissionPtr> subToDelete;
    subToDelete.reserve(2);  // no more expected
    {
        std::lock_guard l{txnsMutex_};
        auto& subs = submitted_[ct];
        std::list<SubmissionPtr>::iterator it = subs.begin();
        do
        {
            if (it = std::find_if(
                    it,
                    subs.end(),
                    [&](auto const& i) {
                        return e.isFinal_
                            ? i->checkID(e.claimID_, e.createCount_)
                            : i->accountSqn_ == e.accountSqn_;
                    });
                it != subs.end())
            {
                subToDelete.push_back(std::move(*it));
                it = subs.erase(it);
            }
        } while (e.isFinal_ && (it != subs.end()));
    }

    for (auto& sub : subToDelete)
    {
        // Deleting events from the opposite side of the attestations
        auto const attestedIDs = sub->forAttestIDs(
            [&](std::uint64_t id) { deleteFromDB(oct, id, false); },
            [&](std::uint64_t id) { deleteFromDB(oct, id, true); });
        JLOGV(
            j_.trace(),
            "XChainAttestsResult processed",
            jv("chainType", to_string(ct)),
            jv("accountSqn", e.accountSqn_),
            jv("result", e.ter_),
            jv("commitAttests", attestedIDs.first),
            jv("createAttests", attestedIDs.second));
    }

    if (e.isHistory_)
    {
        // save latest attestation
        initSync_[ct].attestedTx_.insert(AttestedHistoryTx{
            e.type_, e.src_, e.dst_, e.createCount_, e.claimID_});

        JLOGV(
            j_.debug(),
            "XChainAttestsResult add attestation",
            jv("chainType", to_string(ct)),
            jv("type", static_cast<int>(e.type_)),
            jv("src", e.src_),
            jv("dst", e.dst_),
            jv("createCount",
               e.createCount_ ? fmt::format("{:x}", *e.createCount_)
                              : std::string("0")),
            jv("claimID",
               e.claimID_ ? fmt::format("{:x}", *e.claimID_)
                          : std::string("0")));
    }
}

void
Federator::onEvent(event::NewLedger const& e)
{
    auto const ct = e.chainType_;
    unsigned const doorLedgerIndex =
        chains_[ct].listener_->getDoorProcessedLedger();
    unsigned const submitLedgerIndex =
        chains_[ct].listener_->getSubmitProcessedLedger();

    JLOGV(
        j_.debug(),
        "onEvent NewLedger",
        jv("processedDoorLedger", doorLedgerIndex),
        jv("processedSubmitLedger", submitLedgerIndex),
        jv("event", e.toJson()));

    auto const prevIdx = ledgerIndexes_[ct].exchange(e.ledgerIndex_);
    ledgerFees_[ct].store(e.fee_);

    // Fix TTL for tx from DB
    if (!prevIdx)
    {
        std::lock_guard l{txnsMutex_};
        auto& subs = submitted_[ct];
        for (auto& s : subs)
            if (!s->lastLedgerSeq_)
                s->lastLedgerSeq_ = e.ledgerIndex_ + TxnTTLLedgers;
    }

    if (!autoSubmit_[ct] || isSyncing())
        return;

    checkExpired(ct, submitLedgerIndex);
}

void
Federator::checkExpired(ChainType ct, std::uint32_t ledger)
{
    bool notify = false;
    {
        std::lock_guard l{txnsMutex_};
        auto& subs = submitted_[ct];

        // add expired txn to errored_ for resubmit
        auto firstFresh =
            std::find_if(subs.begin(), subs.end(), [ledger](auto const& s) {
                return s->lastLedgerSeq_ > ledger;
            });
        while (subs.begin() != firstFresh)
        {
            assert(!initSync_[ct].syncing_);
            auto& front = subs.front();
            if (front->retriesAllowed_ > 0)
            {
                JLOGV(
                    j_.warn(),
                    "Ledger TTL expired, move to errored",
                    jv("chainType", to_string(ct)),
                    jv("retries",
                       static_cast<unsigned>(front->retriesAllowed_)),
                    jv("accSqn", front->accountSqn_),
                    jv("lastLedger", front->lastLedgerSeq_),
                    jv("tx", front->getJson()));

                front->retriesAllowed_--;
                front->accountSqn_ = 0;
                front->lastLedgerSeq_ = 0;
                errored_[ct].emplace_back(std::move(front));
            }
            else
            {
                auto const attestedIDs = front->forAttestIDs();
                JLOGV(
                    j_.warn(),
                    "Giving up after repeated retries",
                    jv("chainType", to_string(ct)),
                    jv("commitAttests", attestedIDs.first),
                    jv("createAttests", attestedIDs.second),
                    jv(front->getLogName(), front->getJson()));
            }
            subs.pop_front();
        }
        notify = !errored_[ct].empty();
    }
    if (notify)
    {
        std::lock_guard l(cvMutexes_[lt_txnSubmit]);
        cvs_[lt_txnSubmit].notify_one();
    }
    else
    {
        std::lock_guard bl{batchMutex_};
        if (curClaimAtts_[ct].size() + curCreateAtts_[ct].size() > 0)
            pushAttOnSubmitTxn(bridge_, ct);
    }
}

void
Federator::updateSignerListStatus(ChainType const chainType)
{
    auto const signingAcc =
        signingAccount_ ? *signingAccount_ : calcAccountID(signingPK_);
    auto& signerListInfo(signerListsInfo_[chainType]);

    // check signer list
    signerListInfo.status_ = signerListInfo.presentInSignerList_
        ? SignerListInfo::present
        : SignerListInfo::absent;

    // check master key
    if ((signerListInfo.status_ != SignerListInfo::present) &&
        !signerListInfo.disableMaster_)
    {
        auto const& masterDoorID = bridge_.door(chainType);
        if (masterDoorID == signingAcc)
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
    auto const signingAcc =
        signingAccount_ ? *signingAccount_ : calcAccountID(signingPK_);
    auto& signerListInfo(signerListsInfo_[e.chainType_]);

    signerListInfo.presentInSignerList_ =
        e.signerList_.find(signingAcc) != e.signerList_.end();
    updateSignerListStatus(e.chainType_);

    JLOGV(
        j_.info(),
        "onEvent XChainSignerListSet",
        jv("SigningAcc", ripple::toBase58(signingAcc)),
        jv("DoorID", ripple::toBase58(e.masterDoorID_)),
        jv("ChainType", to_string(e.chainType_)),
        jv("SignerListInfo", signerListInfo.toJson()));
}

void
Federator::onEvent(event::XChainSetRegularKey const& e)
{
    auto const signingAcc =
        signingAccount_ ? *signingAccount_ : calcAccountID(signingPK_);
    auto& signerListInfo(signerListsInfo_[e.chainType_]);

    signerListInfo.regularDoorID_ = e.regularDoorID_;
    updateSignerListStatus(e.chainType_);

    JLOGV(
        j_.info(),
        "onEvent XChainSetRegularKey",
        jv("SigningAcc", ripple::toBase58(signingAcc)),
        jv("DoorID", ripple::toBase58(e.masterDoorID_)),
        jv("ChainType", to_string(e.chainType_)),
        jv("SignerListInfo", signerListInfo.toJson()));
}

void
Federator::onEvent(event::XChainAccountSet const& e)
{
    auto const signingAcc =
        signingAccount_ ? *signingAccount_ : calcAccountID(signingPK_);
    auto& signerListInfo(signerListsInfo_[e.chainType_]);

    signerListInfo.disableMaster_ = e.disableMaster_;
    updateSignerListStatus(e.chainType_);

    JLOGV(
        j_.info(),
        "onEvent XChainAccountSet",
        jv("SigningAcc", ripple::toBase58(signingAcc)),
        jv("DoorID", ripple::toBase58(e.masterDoorID_)),
        jv("ChainType", to_string(e.chainType_)),
        jv("SignerListInfo", signerListInfo.toJson()));
}

void
Federator::pushAttOnSubmitTxn(
    ripple::STXChainBridge const& bridge,
    ChainType chainType)
{
    // batch mutex must already be held
    bool notify = false;
    auto const& signerListInfo(signerListsInfo_[chainType]);
    if (signerListInfo.ignoreSignerList_ ||
        (signerListInfo.status_ != SignerListInfo::absent))
    {
        JLOGV(
            j_.debug(),
            "In the signer list, the attestations are proceed.",
            jv("ChainType", to_string(chainType)));

        std::lock_guard tl{txnsMutex_};
        notify = txns_[ChainType::locking].empty() &&
            txns_[ChainType::issuing].empty();

        if (useBatch_)
        {
#ifdef USE_BATCH_ATTESTATION
            auto p = SubmissionPtr(new SubmissionBatch(
                0,
                0,
                ripple::STXChainAttestationBatch{
                    bridge,
                    curClaimAtts_[chainType].begin(),
                    curClaimAtts_[chainType].end(),
                    curCreateAtts_[chainType].begin(),
                    curCreateAtts_[chainType].end()}));

            txns_[chainType].emplace_back(std::move(p));
#else
            throw std::runtime_error(
                "Please compile with USE_BATCH_ATTESTATION to use Batch "
                "Attestations");
#endif
        }
        else
        {
            // lastLedgerSeq, accountSqn will be updated right before sending
            for (auto const& claim : curClaimAtts_[chainType])
            {
                auto p = SubmissionPtr(new SubmissionClaim(
                    0, 0, networkID_[chainType], bridge, claim));
                txns_[chainType].emplace_back(std::move(p));
            }

            for (auto const& create : curCreateAtts_[chainType])
            {
                auto p = SubmissionPtr(new SubmissionCreateAccount(
                    0, 0, networkID_[chainType], bridge, create));
                txns_[chainType].emplace_back(std::move(p));
            }
        }
    }
    else
    {
        JLOGV(
            j_.info(),
            "Not in the signer list, the attestations has been dropped.",
            jv("ChainType", to_string(chainType)));
    }

    curClaimAtts_[chainType].clear();
    curCreateAtts_[chainType].clear();

    if (notify)
    {
        std::lock_guard l(cvMutexes_[lt_txnSubmit]);
        cvs_[lt_txnSubmit].notify_one();
    }
}

void
Federator::pushAtt(
    ripple::STXChainBridge const& bridge,
    ripple::Attestations::AttestationClaim&& att,
    ChainType chainType,
    bool ledgerBoundary)
{
    std::lock_guard bl{batchMutex_};
    curClaimAtts_[chainType].emplace_back(std::move(att));
    auto const attSize =
        curClaimAtts_[chainType].size() + curCreateAtts_[chainType].size();
    assert(attSize <= maxAttests());
    if (ledgerBoundary || attSize >= maxAttests())
        pushAttOnSubmitTxn(bridge, chainType);
}

void
Federator::pushAtt(
    ripple::STXChainBridge const& bridge,
    ripple::Attestations::AttestationCreateAccount&& att,
    ChainType chainType,
    bool ledgerBoundary)
{
    std::lock_guard bl{batchMutex_};
    curCreateAtts_[chainType].emplace_back(std::move(att));
    auto const attSize =
        curClaimAtts_[chainType].size() + curCreateAtts_[chainType].size();
    assert(attSize <= maxAttests());
    if (ledgerBoundary || attSize >= maxAttests())
        pushAttOnSubmitTxn(bridge, chainType);
}

void
Federator::submitTxn(SubmissionPtr&& submission, ChainType ct)
{
    if (submission->numAttestations() == 0)
        return;

    // already verified txnSubmit before call submitTxn()
    config::TxnSubmit const& txnSubmit = *chains_[ct].txnSubmit_;
    ripple::XRPAmount fee{ledgerFees_[ct].load() + FeeExtraDrops};
    ripple::STTx const toSubmit = submission->getSignedTxn(txnSubmit, fee, j_);

    auto const attestedIDs = submission->forAttestIDs();
    JLOGV(
        j_.trace(),
        "Submitting transaction",
        jv("chainType", to_string(ct)),
        jv("commitAttests", attestedIDs.first),
        jv("createAttests", attestedIDs.second),
        jv("lastLedgerSeq", submission->lastLedgerSeq_),
        jv("accountSqn", submission->accountSqn_),
        jv("hash",
           to_string(toSubmit.getHash(ripple::HashPrefix::transactionID))),
        jv(submission->getLogName(), submission->getJson()));

    Json::Value const request = [&] {
        Json::Value r;
        r[ripple::jss::tx_blob] =
            ripple::strHex(toSubmit.getSerializer().peekData());
        return r;
    }();

    auto callback = [this, ct](Json::Value const& v) {
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
                            auto& subs = submitted_[ct];
                            if (auto it = std::find_if(
                                    subs.begin(),
                                    subs.end(),
                                    [&](auto const& i) {
                                        return i->accountSqn_ == sqn;
                                    });
                                it != subs.end())
                            {
                                auto const attestedIDs = (*it)->forAttestIDs();
                                JLOGV(
                                    j_.warn(),
                                    "Tem txn submit result, removing "
                                    "submission",
                                    jv("account sequence", sqn),
                                    jv("chainType", to_string(ct)),
                                    jv("commitAttests", attestedIDs.first),
                                    jv("createAttests", attestedIDs.second));
                                subs.erase(it);
                            }
                        }
                    }
                }
            }
        }
    };

    {
        std::lock_guard tl{txnsMutex_};
        submitted_[ct].emplace_back(std::move(submission));
    }
    chains_[ct].listener_->send("submit", request, callback);
    // JLOG(j_.trace()) << "txn submitted";  // the listener logs as well
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
            if (accountInfo.isMember(ripple::jss::result) &&
                accountInfo[ripple::jss::result].isMember("account_data"))
            {
                auto const& ad =
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

    decltype(txns_)::type localTxns;
    ChainType submitChain = ChainType::locking;
    while (!requestStop_)
    {
        {
            std::lock_guard l{txnsMutex_};
            assert(localTxns.empty());

            for (auto const ct : {ChainType::locking, ChainType::issuing})
            {
                submitChain = ct;
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
            auto const lastLedgerSeq =
                ledgerIndexes_[submitChain].load() + TxnTTLLedgers;
            txn->lastLedgerSeq_ = lastLedgerSeq;
            txn->accountSqn_ = accountSqns_[submitChain]++;
            {
                // TODO move out of submit loop
                auto session = app_.getXChainTxnDB().checkoutDb();
                auto const sql = fmt::format(
                    R"sql(UPDATE {table_name} SET LedgerSeq = :ledger_sqn WHERE ChainType = :chain_type;
            )sql",
                    fmt::arg("table_name", db_init::xChainSyncTable));
                auto const chainType = static_cast<std::uint32_t>(submitChain);
                *session << sql, soci::use(lastLedgerSeq), soci::use(chainType);
                JLOGV(
                    j_.trace(),
                    "syncDB update ledgerSqn txnSubmitLoop",
                    jv("chainType", to_string(submitChain)),
                    jv("ledgerSqn", lastLedgerSeq));
            }
            submitTxn(std::move(txn), submitChain);
        }
        localTxns.clear();
    }
}

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
        side["initiating"] = initSync_[ct].syncing_ ? "True" : "False";
        side["ledger_index"] = ledgerIndexes_[ct].load();
        side["fee"] = ledgerFees_[ct].load();

        int commitCount = 0;
        int createCount = 0;
        Json::Value commitAttests{Json::arrayValue};
        Json::Value createAttests{Json::arrayValue};
        auto getAttests = [&](auto const& submissions) {
            commitCount = 0;
            createCount = 0;
            commitAttests.clear();
            createAttests.clear();
            for (auto const& txn : submissions)
            {
                txn->forAttestIDs(
                    [&](std::uint64_t id) {
                        assert(
                            id <= (std::uint64_t)
                                      std::numeric_limits<std::uint32_t>::max);
                        commitAttests.append((std::uint32_t)id);
                        ++commitCount;
                    },
                    [&](std::uint64_t id) {
                        assert(
                            id <= (std::uint64_t)
                                      std::numeric_limits<std::uint32_t>::max);
                        createAttests.append((std::uint32_t)id);
                        ++createCount;
                    });
            }
        };

        {
            std::lock_guard l{txnsMutex_};
            getAttests(submitted_[ct]);
            Json::Value submitted{Json::objectValue};
            submitted["commit_attests_size"] = commitCount;
            if (commitCount)
                submitted["commit_attests"] = commitAttests;
            submitted["create_account_attests_size"] = createCount;
            if (createCount)
                submitted["create_account_attests"] = createAttests;
            side["submitted"] = submitted;

            getAttests(errored_[ct]);
            Json::Value errored{Json::objectValue};
            errored["commit_attests_size"] = commitCount;
            if (commitCount > 0)
                errored["commit_attests"] = commitAttests;
            errored["create_account_attests_size"] = createCount;
            if (createCount > 0)
                errored["create_account_attests"] = createAttests;
            side["errored"] = errored;

            getAttests(txns_[ct]);
        }
        {
            std::lock_guard l{batchMutex_};
            for (auto const& a : curClaimAtts_[ct])
            {
                commitAttests.append((int)a.claimID);
                ++commitCount;
            }
            for (auto const& a : curCreateAtts_[ct])
            {
                createAttests.append((int)a.createCount);
                ++createCount;
            }
        }
        Json::Value pending{Json::objectValue};
        pending["commit_attests_size"] = commitCount;
        if (commitCount > 0)
            pending["commit_attests"] = commitAttests;
        pending["create_account_attests_size"] = createCount;
        if (createCount > 0)
            pending["create_account_attests"] = createAttests;
        side["pending"] = pending;

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
            return db_init::xChainCreateAccountTableName(ct);
        else
            return db_init::xChainTableName(ct);
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
    // TODO multi bridge
    if (isSyncing())
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

std::size_t
Federator::maxAttests() const
{
#ifdef USE_BATCH_ATTESTATION
    return useBatch_ ? ripple::AttestationBatch::maxAttestations : 1;
#else
    return 1;
#endif
}

void
Federator::checkSigningKey(
    ChainType const ct,
    bool const masterDisabled,
    std::optional<ripple::AccountID> const& regularAcc)
{
    auto const signingAcc = ripple::calcAccountID(signingPK_);

    if ((signingAcc == signingAccount_) && masterDisabled)
    {
        JLOGV(
            j_.fatal(),
            "Masterkey disabled for signing account",
            jv("chainType", to_string(ct)),
            jv("config acc", ripple::toBase58(*signingAccount_)),
            jv("config pk acc", ripple::toBase58(signingAcc)),
            jv("info regular acc",
               regularAcc ? ripple::toBase58(*regularAcc) : std::string()));
        throw std::runtime_error("Masterkey disabled for signing account");
    }

    if ((signingAcc != signingAccount_) && (regularAcc != signingAcc))
    {
        JLOGV(
            j_.fatal(),
            "Invalid signing account regular key used",
            jv("chainType", to_string(ct)),
            jv("config acc", ripple::toBase58(*signingAccount_)),
            jv("config pk acc", ripple::toBase58(signingAcc)),
            jv("info regular acc",
               regularAcc ? ripple::toBase58(*regularAcc) : std::string()));

        throw std::runtime_error("Invalid signing account key used");
    }

    if (signingAcc == signingAccount_)
    {
        JLOGV(
            j_.trace(),
            "Signing account master key used",
            jv("acc", ripple::toBase58(*signingAccount_)));
    }
    else
    {
        JLOGV(
            j_.trace(),
            "Signing account regular key used",
            jv("master acc", ripple::toBase58(*signingAccount_)),
            jv("regular acc",
               regularAcc ? ripple::toBase58(*regularAcc) : std::string()));
    }
}

void
Federator::setNetworkID(std::uint32_t networkID, ChainType ct)
{
    if (networkID_[ct] != networkID)
    {
        networkID_[ct] = networkID;

        // Fix NetworkID for tx from DB
        std::lock_guard tl{txnsMutex_};
        auto& subs(submitted_[ct]);
        for (auto& s : subs)
            s->networkID_ = networkID;
    }
}

Submission::Submission(
    std::uint32_t lastLedgerSeq,
    std::uint32_t accountSqn,
    std::uint32_t networkID,
    std::string_view const logName)
    : lastLedgerSeq_(lastLedgerSeq)
    , accountSqn_(accountSqn)
    , networkID_(networkID)
    , logName_(logName)
{
}

std::string const&
Submission::getLogName() const
{
    return logName_;
}

#ifdef USE_BATCH_ATTESTATION
SubmissionBatch::SubmissionBatch(
    uint32_t lastLedgerSeq,
    uint32_t accountSqn,
    ripple::STXChainAttestationBatch const& batch)
    : Submission(lastLedgerSeq, accountSqn, "batch"), batch_(batch)
{
}

std::pair<std::string, std::string>
SubmissionBatch::forAttestIDs(
    std::function<void(std::uint64_t id)> commitFunc,
    std::function<void(std::uint64_t id)> createFunc) const
{
    return xbwd::forAttestIDs(batch_, commitFunc, createFunc);
}

Json::Value
SubmissionBatch::getJson(ripple::JsonOptions const opt) const
{
    return batch_.getJson(opt);
}

std::size_t
SubmissionBatch::numAttestations() const
{
    return batch_.numAttestations();
}

ripple::STTx
SubmissionBatch::getSignedTxn(
    config::TxnSubmit const& txn,
    ripple::XRPAmount const& fee,
    beast::Journal j) const
{
    return xbwd::txn::getSignedTxn(
        txn.submittingAccount,
        batch_,
        ripple::jss::XChainAddAttestations,
        batch_.getFName().getJsonName(),
        accountSqn_,
        lastLedgerSeq_,
        fee,
        txn.keypair,
        j);
}

#endif

SubmissionClaim::SubmissionClaim(
    std::uint32_t lastLedgerSeq,
    std::uint32_t accountSqn,
    std::uint32_t networkID,
    ripple::STXChainBridge const& bridge,
    ripple::Attestations::AttestationClaim const& claim)
    : Submission(lastLedgerSeq, accountSqn, networkID, "claim")
    , bridge_(bridge)
    , claim_(claim)
{
}

std::pair<std::string, std::string>
SubmissionClaim::forAttestIDs(
    std::function<void(std::uint64_t id)> commitFunc,
    std::function<void(std::uint64_t id)> createFunc) const
{
    return xbwd::forAttestIDs(claim_, commitFunc, createFunc);
}

Json::Value
SubmissionClaim::getJson(ripple::JsonOptions const opt) const
{
    Json::Value j = claim_.toSTObject().getJson(opt);
    j[ripple::sfOtherChainSource.getJsonName()] = j[ripple::jss::Account];
    j.removeMember(ripple::jss::Account);
    j[bridge_.getFName().getJsonName()] = bridge_.getJson(opt);
    return j;
}

std::size_t
SubmissionClaim::numAttestations() const
{
    return 1;
}

ripple::STTx
SubmissionClaim::getSignedTxn(
    config::TxnSubmit const& txn,
    ripple::XRPAmount const& fee,
    beast::Journal j) const
{
    return xbwd::txn::getSignedTxn(
        txn.submittingAccount,
        *this,
        ripple::jss::XChainAddClaimAttestation,
        Json::StaticString(nullptr),
        accountSqn_,
        lastLedgerSeq_,
        networkID_,
        fee,
        txn.keypair,
        j);
}

bool
SubmissionClaim::checkID(
    std::optional<std::uint32_t> const& claim,
    std::optional<std::uint32_t> const&)
{
    return claim == claim_.claimID;
}

SubmissionCreateAccount::SubmissionCreateAccount(
    std::uint32_t lastLedgerSeq,
    std::uint32_t accountSqn,
    std::uint32_t networkID,
    ripple::STXChainBridge const& bridge,
    ripple::Attestations::AttestationCreateAccount const& create)
    : Submission(lastLedgerSeq, accountSqn, networkID, "createAccount")
    , bridge_(bridge)
    , create_(create)
{
}

std::pair<std::string, std::string>
SubmissionCreateAccount::forAttestIDs(
    std::function<void(std::uint64_t id)> commitFunc,
    std::function<void(std::uint64_t id)> createFunc) const
{
    return xbwd::forAttestIDs(create_, commitFunc, createFunc);
}

Json::Value
SubmissionCreateAccount::getJson(ripple::JsonOptions const opt) const
{
    Json::Value j = create_.toSTObject().getJson(opt);
    j[ripple::sfOtherChainSource.getJsonName()] = j[ripple::jss::Account];
    j.removeMember(ripple::jss::Account);
    j[bridge_.getFName().getJsonName()] = bridge_.getJson(opt);
    return j;
}

std::size_t
SubmissionCreateAccount::numAttestations() const
{
    return 1;
}

ripple::STTx
SubmissionCreateAccount::getSignedTxn(
    config::TxnSubmit const& txn,
    ripple::XRPAmount const& fee,
    beast::Journal j) const
{
    return xbwd::txn::getSignedTxn(
        txn.submittingAccount,
        *this,
        ripple::jss::XChainAddAccountCreateAttestation,
        Json::StaticString(nullptr),
        accountSqn_,
        lastLedgerSeq_,
        networkID_,
        fee,
        txn.keypair,
        j);
}

bool
SubmissionCreateAccount::checkID(
    std::optional<std::uint32_t> const&,
    std::optional<std::uint32_t> const& create)
{
    return create == create_.createCount;
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
