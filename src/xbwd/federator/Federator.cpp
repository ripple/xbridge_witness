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
    beast::Journal j)
{
    auto r =
        std::make_shared<Federator>(Federator::PrivateTag{}, app, config, j);

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
            j);
    std::shared_ptr<ChainListener> sidechainListener =
        std::make_shared<ChainListener>(
            ChainType::issuing,
            config.bridge,
            getSubmitAccount(ChainType::issuing),
            r,
            j);
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
    , ignoreSignerList_(config.ignoreSignerList)
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
        pushAtt(e.bridge_, std::move(*claimOpt), dstChain, e.ledgerBoundary_);
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
        pushAtt(e.bridge_, std::move(*createOpt), dstChain, e.ledgerBoundary_);
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

static std::unordered_set<ripple::TERUnderlyingType> SkippableTec(
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
        j_.trace(),
        "XChainAttestsResult",
        ripple::jv("chain", to_string(e.chainType_)),
        ripple::jv("accountSqn", e.accountSqn_),
        ripple::jv("result", transHuman(e.ter_)));
    if (SkippableTec.find(TERtoInt(e.ter_)) != SkippableTec.end())
    {
        std::lock_guard l{txnsMutex_};
        auto& subs = submitted_[e.chainType_];
        if (auto i = std::find_if(
                subs.begin(),
                subs.end(),
                [&](auto const& i) { return i.accountSqn_ == e.accountSqn_; });
            i != subs.end())
        {
            subs.erase(i);
        }
    }
    // else, will resubmit after txn ttl (i.e. TxnTTLLedgers = 4) ledgers
}

void
Federator::onEvent(event::NewLedger const& e)
{
    JLOGV(
        j_.trace(),
        "NewLedger",
        ripple::jv("chain", to_string(e.chainType_)),
        ripple::jv("ledgerIndex", e.ledgerIndex_),
        ripple::jv("fee", e.fee_));
    ledgerIndexes_[e.chainType_].store(e.ledgerIndex_);
    ledgerFees_[e.chainType_].store(e.fee_);

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
                JLOGV(
                    j_.warn(),
                    "Giving up after repeated retries ",
                    ripple::jv(
                        "batch",
                        front.batch_.getJson(ripple::JsonOptions::none)));
            }
            submitted_[e.chainType_].pop_front();
        }
    }
    if (!errored_[ChainType::locking].empty() ||
        !errored_[ChainType::issuing].empty())
    {
        std::lock_guard l(cvMutexes_[lt_txnSubmit]);
        cvs_[lt_txnSubmit].notify_one();
    }
}

void
Federator::onEvent(event::XChainSignerListSet const& e)
{
    const auto signingAcc = calcAccountID(signingPK_);
    const auto ignoreSignerList(chains_[e.chainType_].ignoreSignerList_);
    auto& inSignerList = inSignerList_[e.chainType_];

    inSignerList = [&] {
        for (const auto& acc : e.entries_)
        {
            if (acc == signingAcc)
                return KeySignerListStatus::present;
        }
        return KeySignerListStatus::absent;
    }();

    JLOGV(
        j_.info(),
        "event::XChainSignerListSet",
        ripple::jv("SigningAcc", ripple::toBase58(signingAcc)),
        ripple::jv("DoorID", ripple::toBase58(e.account_)),
        ripple::jv("ChainType", to_string(e.chainType_)),
        ripple::jv("Active", inSignerList == KeySignerListStatus::present),
        ripple::jv("IgnoreSignerList", ignoreSignerList));
}

void
Federator::pushAttOnSubmitTxn(
    ripple::STXChainBridge const& bridge,
    ChainType chainType)
{
    // batch mutex must already be held
    bool notify = false;
    const auto inSignList = inSignerList_[chainType];
    if (chains_[chainType].ignoreSignerList_ ||
        (inSignList != KeySignerListStatus::absent))
    {
        std::lock_guard tl{txnsMutex_};
        notify = txns_[ChainType::locking].empty() &&
            txns_[ChainType::issuing].empty();
        txns_[chainType].emplace_back(
            0,
            0,
            ripple::STXChainAttestationBatch{
                bridge,
                curClaimAtts_[chainType].begin(),
                curClaimAtts_[chainType].end(),
                curCreateAtts_[chainType].begin(),
                curCreateAtts_[chainType].end()});
        curClaimAtts_[chainType].clear();
        curCreateAtts_[chainType].clear();
    }
    else
    {
        curClaimAtts_[chainType].clear();
        curCreateAtts_[chainType].clear();

        JLOGV(
            j_.info(),
            "not in signer list, atestations dropped",
            ripple::jv("ChainType", to_string(chainType)));
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
    ChainType chainType,
    bool ledgerBoundary)
{
    std::lock_guard bl{batchMutex_};
    curClaimAtts_[chainType].emplace_back(std::move(att));
    assert(
        curClaimAtts_[chainType].size() + curCreateAtts_[chainType].size() <=
        ripple::AttestationBatch::maxAttestations);
    if (ledgerBoundary ||
        curClaimAtts_[chainType].size() + curCreateAtts_[chainType].size() >=
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
    curCreateAtts_[chainType].emplace_back(std::move(att));
    assert(
        curClaimAtts_[chainType].size() + curCreateAtts_[chainType].size() <=
        ripple::AttestationBatch::maxAttestations);
    if (ledgerBoundary ||
        curClaimAtts_[chainType].size() + curCreateAtts_[chainType].size() >=
            ripple::AttestationBatch::maxAttestations)
        pushAttOnSubmitTxn(bridge, chainType);
}

void
Federator::submitTxn(Submission const& submission, ChainType dstChain)
{
    JLOGV(
        j_.trace(),
        "Submitting transaction",
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
                                JLOG(j_.trace())
                                    << "Tem txn submit result, removing "
                                       "submission with account sequence "
                                    << sqn;
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

        std::promise<Json::Value> promise;
        std::future<Json::Value> future = promise.get_future();
        auto callback = [&promise](Json::Value const& v) {
            promise.set_value(v);
        };
        Json::Value request;
        request[ripple::jss::account] = accountStrs[chain];
        request[ripple::jss::ledger_index] = "validated";
        chains_[chain].listener_->send("account_info", request, callback);
        Json::Value const accountInfo = future.get();
        JLOGV(
            j_.trace(),
            "txn submit account info",
            ripple::jv("accountInfo", accountInfo));
        if (accountInfo.isMember(ripple::jss::result) &&
            accountInfo[ripple::jss::result].isMember("account_data"))
        {
            auto const ad = accountInfo[ripple::jss::result]["account_data"];
            if (ad.isMember(ripple::jss::Sequence) &&
                ad[ripple::jss::Sequence].isIntegral())
            {
                accountSqns_[chain] = ad[ripple::jss::Sequence].asUInt();
                return true;
            }
        }

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
            submitTxn(txn, submitChain);
        }
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

Submission::Submission(
    uint32_t lastLedgerSeq,
    uint32_t accountSqn,
    ripple::STXChainAttestationBatch const& batch)
    : lastLedgerSeq_(lastLedgerSeq), accountSqn_(accountSqn), batch_(batch)
{
}

}  // namespace xbwd
