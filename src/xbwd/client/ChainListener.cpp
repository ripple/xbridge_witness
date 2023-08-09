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

#include <xbwd/client/ChainListener.h>

#include <xbwd/basics/StructuredLog.h>
#include <xbwd/client/RpcResultParse.h>
#include <xbwd/client/WebsocketClient.h>
#include <xbwd/federator/Federator.h>
#include <xbwd/federator/FederatorEvents.h>

#include <ripple/basics/Log.h>
#include <ripple/basics/RangeSet.h>
#include <ripple/basics/XRPAmount.h>
#include <ripple/basics/strHex.h>
#include <ripple/json/Output.h>
#include <ripple/json/json_get_or_throw.h>
#include <ripple/json/json_writer.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/LedgerFormats.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>

#include <charconv>
#include <type_traits>

namespace xbwd {

class Federator;

ChainListener::ChainListener(
    ChainType chainType,
    ripple::STXChainBridge const sidechain,
    std::optional<ripple::AccountID> submitAccountOpt,
    std::weak_ptr<Federator>&& federator,
    std::optional<ripple::AccountID> signingAccount,
    beast::Journal j)
    : chainType_{chainType}
    , bridge_{sidechain}
    , witnessAccountStr_(
          submitAccountOpt ? ripple::toBase58(*submitAccountOpt)
                           : std::string{})
    , federator_{std::move(federator)}
    , signingAccount_(signingAccount)
    , j_{j}
{
}

// destructor must be defined after WebsocketClient size is known (i.e. it can
// not be defaulted in the header or the unique_ptr declaration of
// WebsocketClient won't work)
ChainListener::~ChainListener() = default;

void
ChainListener::init(boost::asio::io_service& ios, beast::IP::Endpoint const& ip)
{
    wsClient_ = std::make_shared<WebsocketClient>(
        [self = shared_from_this()](Json::Value const& msg) {
            self->onMessage(msg);
        },
        [self = shared_from_this()]() { self->onConnect(); },
        ios,
        ip,
        /*headers*/ std::unordered_map<std::string, std::string>{},
        j_);

    wsClient_->connect();
}

void
ChainListener::onConnect()
{
    auto const doorAccStr = ripple::toBase58(bridge_.door(chainType_));

    auto doorAccInfoCb = [self = shared_from_this(),
                          doorAccStr](Json::Value const& msg) {
        self->processAccountInfo(msg);
        Json::Value params;
        params[ripple::jss::streams] = Json::arrayValue;
        params[ripple::jss::streams].append("ledger");
        self->send("subscribe", params);
    };

    auto serverInfoCb = [self = shared_from_this(), doorAccStr, doorAccInfoCb](
                            Json::Value const& msg) {
        self->processServerInfo(msg);
        Json::Value params;
        params[ripple::jss::account] = doorAccStr;
        params[ripple::jss::signer_lists] = true;
        self->send("account_info", params, doorAccInfoCb);
    };

    auto mainFlow = [self = shared_from_this(), doorAccStr, serverInfoCb]() {
        Json::Value params;
        self->send("server_info", params, serverInfoCb);
    };

    auto signAccInfoCb = [self = shared_from_this(),
                          mainFlow](Json::Value const& msg) {
        self->processSigningAccountInfo(msg);
        mainFlow();
    };

    inRequest_ = false;
    ledgerReqMax_ = 0;
    prevLedgerIndex_ = 0;
    txnHistoryIndex_ = 0;
    hp_.clear();

    if (signingAccount_)
    {
        Json::Value params;
        params[ripple::jss::account] = ripple::toBase58(*signingAccount_);
        send("account_info", params, signAccInfoCb);
    }
    else
    {
        mainFlow();
    }
}

void
ChainListener::shutdown()
{
    if (wsClient_)
        wsClient_->shutdown();
}

std::uint32_t
ChainListener::send(std::string const& cmd, Json::Value const& params) const
{
    auto const chainName = to_string(chainType_);
    return wsClient_->send(cmd, params, chainName);
}

void
ChainListener::stopHistoricalTxns()
{
    hp_.stopHistory_ = true;
}

void
ChainListener::send(
    std::string const& cmd,
    Json::Value const& params,
    RpcCallback onResponse)
{
    auto const chainName = to_string(chainType_);
    // JLOGV(
    //     j_.trace(),
    //     "ChainListener send",
    //     jv("chain_name", chainName),
    //     jv("command", cmd),
    //     jv("params", params));

    auto id = wsClient_->send(cmd, params, chainName);
    // JLOGV(j_.trace(), "ChainListener send id", jv("id", id));

    std::lock_guard lock(callbacksMtx_);
    callbacks_.emplace(id, onResponse);
}

template <class E>
void
ChainListener::pushEvent(E&& e) const
{
    static_assert(std::is_rvalue_reference_v<decltype(e)>, "");

    if (auto f = federator_.lock())
    {
        f->push(std::move(e));
    }
}

void
ChainListener::onMessage(Json::Value const& msg)
{
    auto callbackOpt = [&]() -> std::optional<RpcCallback> {
        if (msg.isMember(ripple::jss::id) && msg[ripple::jss::id].isIntegral())
        {
            auto callbackId = msg[ripple::jss::id].asUInt();
            std::lock_guard lock(callbacksMtx_);
            auto i = callbacks_.find(callbackId);
            if (i != callbacks_.end())
            {
                auto cb = i->second;
                callbacks_.erase(i);
                return cb;
            }
        }
        return {};
    }();

    if (callbackOpt)
    {
        JLOGV(
            j_.trace(),
            "ChainListener onMessage, reply to a callback",
            jv("chain_name", to_string(chainType_)),
            jv("msg", msg.toStyledString()));
        (*callbackOpt)(msg);
    }
    else
    {
        processMessage(msg);
    }
}

namespace {

bool
isDeletedClaimId(Json::Value const& meta, std::uint64_t claimID)
{
    if (!meta.isMember("AffectedNodes"))
        return false;

    for (auto const& an : meta["AffectedNodes"])
    {
        if (!an.isMember("DeletedNode"))
            continue;
        auto const& dn = an["DeletedNode"];
        if (!dn.isMember("FinalFields"))
            continue;
        auto const& ff = dn["FinalFields"];
        auto const optClaimId =
            Json::getOptional<std::uint64_t>(ff, ripple::sfXChainClaimID);

        if (optClaimId == claimID)
            return true;
    }

    return false;
}

bool
isDeletedAccCnt(Json::Value const& meta, std::uint64_t createCnt)
{
    if (!meta.isMember("AffectedNodes"))
        return false;

    for (auto const& an : meta["AffectedNodes"])
    {
        if (!an.isMember("DeletedNode"))
            continue;
        auto const& dn = an["DeletedNode"];
        if (!dn.isMember("FinalFields"))
            continue;
        auto const& ff = dn["FinalFields"];
        auto const optCreateCnt = Json::getOptional<std::uint64_t>(
            ff, ripple::sfXChainAccountCreateCount);

        if (optCreateCnt == createCnt)
            return true;
    }

    return false;
}

std::optional<event::NewLedger>
checkLedger(ChainType chainType, Json::Value const& msg)
{
    auto checkLedgerHlp =
        [&](Json::Value const& msg) -> std::optional<event::NewLedger> {
        if (msg.isMember(ripple::jss::fee_base) &&
            msg[ripple::jss::fee_base].isIntegral() &&
            msg.isMember(ripple::jss::ledger_index) &&
            msg[ripple::jss::ledger_index].isIntegral() &&
            msg.isMember(ripple::jss::reserve_base) &&
            msg.isMember(ripple::jss::reserve_inc) &&
            msg.isMember(ripple::jss::fee_ref) &&
            msg.isMember(ripple::jss::validated_ledgers))
        {
            return event::NewLedger{
                chainType,
                msg[ripple::jss::ledger_index].asUInt(),
                msg[ripple::jss::fee_base].asUInt()};
        }
        return {};
    };
    return msg.isMember(ripple::jss::result)
        ? checkLedgerHlp(msg[ripple::jss::result])
        : checkLedgerHlp(msg);
}

}  // namespace

void
ChainListener::processMessage(Json::Value const& msg)
{
    auto const chainName = to_string(chainType_);

    auto ignoreRet = [&](std::string_view reason, auto&&... v) {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            jv("chain_name", chainName),
            jv("reason", reason),
            std::forward<decltype(v)>(v)...);
    };

    auto txnHistoryIndex = [&]() -> std::optional<std::int32_t> {
        // only history stream messages have the index
        if (!msg.isMember(ripple::jss::account_history_tx_index) ||
            !msg[ripple::jss::account_history_tx_index].isIntegral())
            return {};
        // values < 0 are historical txns. values >= 0 are new transactions.
        // Only the initial sync needs historical txns.
        return msg[ripple::jss::account_history_tx_index].asInt();
    }();
    bool const isHistory = txnHistoryIndex && (*txnHistoryIndex < 0);

    if (isHistory && hp_.stopHistory_)
        return ignoreRet(
            "stopped processing historical tx",
            jv(ripple::jss::account_history_tx_index.c_str(),
               *txnHistoryIndex));

    JLOGV(
        j_.trace(),
        "chain listener process message",
        jv("chain_name", chainName),
        jv("msg", msg.toStyledString()));

    auto newLedgerEv = checkLedger(chainType_, msg);
    if (newLedgerEv)
    {
        pushEvent(std::move(*newLedgerEv));
        processNewLedger(newLedgerEv->ledgerIndex_);
        return;
    }

    if (!msg.isMember(ripple::jss::validated) ||
        !msg[ripple::jss::validated].asBool())
        return ignoreRet("not validated");

    if (!msg.isMember(ripple::jss::transaction))
        return ignoreRet("no tx");

    auto const& transaction = msg[ripple::jss::transaction];

    if (!msg.isMember(ripple::jss::meta))
        return ignoreRet("no meta");

    auto const& meta = msg[ripple::jss::meta];

    if (!msg.isMember(ripple::jss::engine_result_code))
        return ignoreRet("no engine result code");

    ripple::TER const txnTER = [&msg] {
        return ripple::TER::fromInt(
            msg[ripple::jss::engine_result_code].asInt());
    }();
    bool const txnSuccess = ripple::isTesSuccess(txnTER);

    auto txnTypeOpt = rpcResultParse::parseXChainTxnType(transaction);
    if (!txnTypeOpt)
        return ignoreRet("not a sidechain transaction");

    auto const txnBridge = rpcResultParse::parseBridge(transaction);
    if (txnBridge && *txnBridge != bridge_)
    {
        // Only keep transactions to or from the door account.
        // Transactions to the account are initiated by users and are are cross
        // chain transactions. Transaction from the account are initiated by
        // federators and need to be monitored for errors. There are two types
        // of transactions that originate from the door account: the second half
        // of a cross chain payment and a refund of a failed cross chain
        // payment.

        // TODO: It is a mistake to filter out based on sidechain.
        // This server should support multiple sidechains
        // Note: the federator stores a hard-coded sidechain in the
        // database, if we remove this filter we need to remove
        // sidechain from the app and listener as well
        return ignoreRet("Sidechain mismatch");
    }

    auto const txnHash = rpcResultParse::parseTxHash(transaction);
    if (!txnHash)
        return ignoreRet("no tx hash");

    auto const txnSeq = rpcResultParse::parseTxSeq(transaction);
    if (!txnSeq)
        return ignoreRet("no txnSeq");

    auto const lgrSeq = rpcResultParse::parseLedgerSeq(msg);
    if (!lgrSeq)
        return ignoreRet("no lgrSeq");

    auto const src = rpcResultParse::parseSrcAccount(transaction);
    if (!src)
        return ignoreRet("no account src");

    auto const dst = rpcResultParse::parseDstAccount(transaction, *txnTypeOpt);

    auto const ledgerBoundary = [&]() -> bool {
        if (msg.isMember(ripple::jss::account_history_boundary) &&
            msg[ripple::jss::account_history_boundary].isBool() &&
            msg[ripple::jss::account_history_boundary].asBool())
        {
            JLOGV(
                j_.trace(),
                "ledger boundary",
                jv("seq", *lgrSeq),
                jv("chain_name", chainName));
            return true;
        }
        return false;
    }();

    std::optional<ripple::STAmount> deliveredAmt =
        rpcResultParse::parseDeliveredAmt(transaction, meta);

    auto const [chainDir, oppositeChainDir] =
        [&]() -> std::pair<ChainDir, ChainDir> {
        using enum ChainDir;
        if (chainType_ == ChainType::locking)
            return {issuingToLocking, lockingToIssuing};
        return {lockingToIssuing, issuingToLocking};
    }();

    switch (*txnTypeOpt)
    {
        case XChainTxnType::xChainClaim: {
            auto const claimID = Json::getOptional<std::uint64_t>(
                transaction, ripple::sfXChainClaimID);

            if (!claimID)
                return ignoreRet("no claimID");
            if (!dst)
                return ignoreRet("no dst in xchain claim");

            using namespace event;
            XChainTransferResult e{
                chainDir,
                *dst,
                deliveredAmt,
                *claimID,
                *lgrSeq,
                *txnHash,
                txnTER,
                txnHistoryIndex};
            pushEvent(std::move(e));
        }
        break;
        case XChainTxnType::xChainCommit: {
            auto const claimID = Json::getOptional<std::uint64_t>(
                transaction, ripple::sfXChainClaimID);

            if (!claimID)
                return ignoreRet("no claimID");
            if (!txnBridge)
                return ignoreRet("no bridge in xchain commit");

            using namespace event;
            XChainCommitDetected e{
                oppositeChainDir,
                *src,
                *txnBridge,
                deliveredAmt,
                *claimID,
                dst,
                *lgrSeq,
                *txnHash,
                txnTER,
                txnHistoryIndex,
                ledgerBoundary};
            pushEvent(std::move(e));
        }
        break;
        case XChainTxnType::xChainAccountCreateCommit: {
            auto const createCount = rpcResultParse::parseCreateCount(meta);
            if (!createCount)
                return ignoreRet("no createCount");
            if (!txnBridge)
                return ignoreRet("no bridge in xchain commit");

            auto const rewardAmt = rpcResultParse::parseRewardAmt(transaction);
            if (!rewardAmt)
                return ignoreRet("no reward amt in xchain create account");

            if (!dst)
                return ignoreRet("no dst in xchain create account");

            using namespace event;
            XChainAccountCreateCommitDetected e{
                oppositeChainDir,
                *src,
                *txnBridge,
                deliveredAmt,
                *rewardAmt,
                *createCount,
                *dst,
                *lgrSeq,
                *txnHash,
                txnTER,
                txnHistoryIndex,
                ledgerBoundary};
            pushEvent(std::move(e));
        }
        break;
        case XChainTxnType::xChainCreateBridge: {
            if (!txnBridge)
                return ignoreRet("no bridge in xChainCreateBridge");
            if (isHistory)
            {
                pushEvent(event::EndOfHistory{chainType_});
                hp_.stopHistory_ = true;
            }
        }
        break;
#ifdef USE_BATCH_ATTESTATION
        case XChainTxnType::xChainAddAttestationBatch: {
            if (rpcResultParse::fieldMatchesStr(
                    transaction,
                    ripple::jss::Account,
                    witnessAccountStr_.c_str()) &&
                txnSeq)
            {
                pushEvent(
                    event::XChainAttestsResult{chainType_, *txnSeq, txnTER});
                return;
            }
            else
                return ignore_ret("not an attestation sent from this server");
        }
        break;
#endif
        case XChainTxnType::xChainAddAccountCreateAttestation:
        case XChainTxnType::xChainAddClaimAttestation: {
            bool const isOwn = rpcResultParse::fieldMatchesStr(
                transaction, ripple::jss::Account, witnessAccountStr_.c_str());
            if ((isHistory || isOwn) && txnSeq)
            {
                JLOGV(
                    j_.trace(),
                    "Attestation processing",
                    jv("chain_name", chainName),
                    jv("src", *src),
                    jv("dst",
                       !dst || !*dst ? std::string() : ripple::toBase58(*dst)),
                    jv("witnessAccountStr_", witnessAccountStr_));

                auto osrc = rpcResultParse::parseOtherSrcAccount(
                    transaction, *txnTypeOpt);
                auto odst = rpcResultParse::parseOtherDstAccount(
                    transaction, *txnTypeOpt);
                if (!osrc ||
                    ((txnTypeOpt ==
                      XChainTxnType::xChainAddAccountCreateAttestation) &&
                     !odst))
                    return ignoreRet(
                        "osrc/odst account missing",
                        jv("witnessAccountStr_", witnessAccountStr_));

                std::optional<std::uint64_t> claimID, accountCreateCount;

                if (txnTypeOpt == XChainTxnType::xChainAddClaimAttestation)
                {
                    claimID = Json::getOptional<std::uint64_t>(
                        transaction, ripple::sfXChainClaimID);
                    if (!claimID)
                        return ignoreRet("no claimID");

                    if (!isOwn && !isDeletedClaimId(meta, *claimID))
                        return ignoreRet("claimID not in DeletedNode");
                }

                if (txnTypeOpt ==
                    XChainTxnType::xChainAddAccountCreateAttestation)
                {
                    accountCreateCount = Json::getOptional<std::uint64_t>(
                        transaction, ripple::sfXChainAccountCreateCount);
                    if (!accountCreateCount)
                        return ignoreRet("no accountCreateCount");

                    if (!isOwn && !isDeletedAccCnt(meta, *accountCreateCount))
                        return ignoreRet(
                            "accountCreateCount not in DeletedNode");
                }

                pushEvent(event::XChainAttestsResult{
                    chainType_,
                    *txnSeq,
                    *txnHash,
                    txnTER,
                    isHistory,
                    *txnTypeOpt,
                    *osrc,
                    odst ? *odst : ripple::AccountID(),
                    accountCreateCount,
                    claimID});
                return;
            }
            else
                return ignoreRet(
                    "not an attestation sent from this server",
                    jv("witnessAccountStr_", witnessAccountStr_));
        }
        break;
        case XChainTxnType::SignerListSet: {
            if (txnSuccess && !isHistory)
                processSignerListSet(transaction);
            return;
        }
        break;
        case XChainTxnType::AccountSet: {
            if (txnSuccess && !isHistory)
                processAccountSet(transaction);
            return;
        }
        break;
        case XChainTxnType::SetRegularKey: {
            if (txnSuccess && !isHistory)
                processSetRegularKey(transaction);
            return;
        }
        break;
    }
}

namespace {
std::optional<std::unordered_set<ripple::AccountID>>
processSignerListSetGeneral(
    Json::Value const& msg,
    std::string_view const chainName,
    std::string_view const errTopic,
    beast::Journal j)
{
    auto warn_ret = [&](std::string_view reason) {
        JLOGV(
            j.warn(),
            errTopic,
            jv("reason", reason),
            jv("chain_name", chainName),
            jv("msg", msg));
        return std::optional<std::unordered_set<ripple::AccountID>>();
    };

    if (msg.isMember("SignerQuorum"))
    {
        unsigned const signerQuorum = msg["SignerQuorum"].asUInt();
        if (!signerQuorum)
            return warn_ret("'SignerQuorum' is null");
    }
    else
        return warn_ret("'SignerQuorum' missed");

    if (!msg.isMember("SignerEntries"))
        return warn_ret("'SignerEntries' missed");
    auto const& signerEntries = msg["SignerEntries"];
    if (!signerEntries.isArray())
        return warn_ret("'SignerEntries' is not an array");

    std::unordered_set<ripple::AccountID> entries;
    for (auto const& superEntry : signerEntries)
    {
        if (!superEntry.isMember("SignerEntry"))
            return warn_ret("'SignerEntry' missed");

        auto const& entry = superEntry["SignerEntry"];
        if (!entry.isMember(ripple::jss::Account))
            return warn_ret("'Account' missed");

        auto const& jAcc = entry[ripple::jss::Account];
        auto parsed = ripple::parseBase58<ripple::AccountID>(jAcc.asString());
        if (!parsed)
            return warn_ret("invalid 'Account'");

        entries.insert(parsed.value());
    }

    return {std::move(entries)};
}

}  // namespace

void
ChainListener::processAccountInfo(Json::Value const& msg) const noexcept
{
    std::string const chainName = to_string(chainType_);
    std::string_view const errTopic = "ignoring account_info message";

    auto warn_ret = [&, this](std::string_view reason) {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("reason", reason),
            jv("chain_name", chainName),
            jv("msg", msg));
    };

    try
    {
        if (!msg.isMember(ripple::jss::result))
            return warn_ret("'result' missed");

        auto const& jres = msg[ripple::jss::result];
        if (!jres.isMember(ripple::jss::account_data))
            return warn_ret("'account_data' missed");

        auto const& jaccData = jres[ripple::jss::account_data];
        if (!jaccData.isMember(ripple::jss::Account))
            return warn_ret("'Account' missed");

        auto const& jAcc = jaccData[ripple::jss::Account];
        auto const parsedAcc =
            ripple::parseBase58<ripple::AccountID>(jAcc.asString());
        if (!parsedAcc)
            return warn_ret("invalid 'Account'");

        // check disable master key
        {
            auto const fDisableMaster = jaccData.isMember(ripple::jss::Flags)
                ? static_cast<bool>(
                      jaccData[ripple::jss::Flags].asUInt() &
                      ripple::lsfDisableMaster)
                : false;

            pushEvent(event::XChainAccountSet{
                chainType_, *parsedAcc, fDisableMaster});
        }

        // check regular key
        {
            if (jaccData.isMember("RegularKey"))
            {
                std::string const regularKeyStr =
                    jaccData["RegularKey"].asString();
                auto opRegularDoorId =
                    ripple::parseBase58<ripple::AccountID>(regularKeyStr);

                pushEvent(event::XChainSetRegularKey{
                    chainType_,
                    *parsedAcc,
                    opRegularDoorId ? std::move(*opRegularDoorId)
                                    : ripple::AccountID()});
            }
        }

        // check signer list
        {
            if (!jaccData.isMember(ripple::jss::signer_lists))
                return warn_ret("'signer_lists' missed");
            auto const& jslArray = jaccData[ripple::jss::signer_lists];
            if (!jslArray.isArray() || jslArray.size() != 1)
                return warn_ret("'signer_lists'  isn't array of size 1");

            auto opEntries = processSignerListSetGeneral(
                jslArray[0u], chainName, errTopic, j_);
            if (!opEntries)
                return;

            pushEvent(event::XChainSignerListSet{
                chainType_, *parsedAcc, std::move(*opEntries)});
        }
    }
    catch (std::exception const& e)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("exception", e.what()),
            jv("chain_name", chainName),
            jv("msg", msg));
    }
    catch (...)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("exception", "unknown exception"),
            jv("chain_name", chainName),
            jv("msg", msg));
    }
}

void
ChainListener::processServerInfo(Json::Value const& msg) noexcept
{
    std::string const chainName = to_string(chainType_);
    std::string_view const errTopic = "ignoring server_info message";

    auto warn_ret = [&, this](std::string_view reason) {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("reason", reason),
            jv("chain_name", chainName));
    };

    try
    {
        if (!msg.isMember(ripple::jss::result))
            return warn_ret("'result' missed");

        auto const& jres = msg[ripple::jss::result];
        if (!jres.isMember(ripple::jss::info))
            return warn_ret("'info' missed");

        auto const& jinfo = jres[ripple::jss::info];

        std::uint32_t networkID = 0;
        auto checkNetworkID = [&jinfo, &networkID, this, warn_ret]() {
            if (!jinfo.isMember(ripple::jss::network_id))
                return warn_ret("'network_id' missed");

            auto const& jnetID = jinfo[ripple::jss::network_id];
            if (!jnetID.isIntegral())
                return warn_ret("'network_id' invalid type");

            networkID = jnetID.asUInt();
            auto fed = federator_.lock();
            if (fed)
                fed->setNetwordID(networkID, chainType_);
        };
        checkNetworkID();

        auto checkCompleteLedgers = [&jinfo, this, warn_ret, chainName]() {
            if (!jinfo.isMember(ripple::jss::complete_ledgers))
                return warn_ret("'complete_ledgers' missed");

            auto const& jledgers = jinfo[ripple::jss::complete_ledgers];
            if (!jledgers.isString())
                return warn_ret("'complete_ledgers' invalid type");

            std::string const ledgers = jledgers.asString();

            ripple::RangeSet<std::uint32_t> rs;
            if (!ripple::from_string(rs, ledgers) || rs.empty())
                return warn_ret("'complete_ledgers' invalid value");

            auto const& interval = *rs.rbegin();
            auto const m = interval.lower();
            if (!hp_.minValidatedLedger_ || (m < hp_.minValidatedLedger_))
                hp_.minValidatedLedger_ = m;
        };
        checkCompleteLedgers();

        JLOGV(
            j_.info(),
            "server_info",
            jv("chain_name", chainName),
            jv("minValidatedLedger", hp_.minValidatedLedger_),
            jv("networkID", networkID));
    }
    catch (std::exception const& e)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("exception", e.what()),
            jv("chain_name", chainName),
            jv("msg", msg));
    }
    catch (...)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("exception", "unknown exception"),
            jv("chain_name", chainName),
            jv("msg", msg));
    }
}

void
ChainListener::processSigningAccountInfo(Json::Value const& msg) const noexcept
{
    std::string const chainName = to_string(chainType_);
    std::string_view const errTopic = "ignoring signing account_info message";

    auto warn_ret = [&, this](std::string_view reason) {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("reason", reason),
            jv("chain_name", chainName),
            jv("msg", msg));
    };

    try
    {
        if (!msg.isMember(ripple::jss::result))
            return warn_ret("'result' missed");

        auto const& jres = msg[ripple::jss::result];
        if (!jres.isMember(ripple::jss::account_data))
            return warn_ret("'account_data' missed");

        auto const& jaccData = jres[ripple::jss::account_data];
        if (!jaccData.isMember(ripple::jss::Account))
            return warn_ret("'Account' missed");

        auto const& jAcc = jaccData[ripple::jss::Account];
        auto const parsedAcc =
            ripple::parseBase58<ripple::AccountID>(jAcc.asString());
        if (!parsedAcc)
            return warn_ret("invalid 'Account'");

        bool const fDisableMaster = jaccData.isMember(ripple::jss::Flags)
            ? static_cast<bool>(
                  jaccData[ripple::jss::Flags].asUInt() &
                  ripple::lsfDisableMaster)
            : false;

        std::optional<ripple::AccountID> regularAcc;
        if (jaccData.isMember("RegularKey"))
        {
            std::string const regularKeyStr = jaccData["RegularKey"].asString();
            regularAcc = ripple::parseBase58<ripple::AccountID>(regularKeyStr);
        }

        auto f = federator_.lock();
        if (!f)
            return warn_ret("federator not available");

        f->checkSigningKey(chainType_, fDisableMaster, regularAcc);
    }
    catch (std::exception const& e)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("exception", e.what()),
            jv("chain_name", chainName),
            jv("msg", msg));
    }
    catch (...)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("exception", "unknown exception"),
            jv("chain_name", chainName),
            jv("msg", msg));
    }
}

void
ChainListener::processSignerListSet(Json::Value const& msg) const noexcept
{
    std::string const chainName = to_string(chainType_);
    std::string_view const errTopic = "ignoring SignerListSet message";

    auto warn_ret = [&, this](std::string_view reason) {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("reason", reason),
            jv("chain_name", chainName),
            jv("msg", msg));
    };

    try
    {
        auto const lockingDoorStr =
            ripple::toBase58(bridge_.lockingChainDoor());
        auto const issuingDoorStr =
            ripple::toBase58(bridge_.issuingChainDoor());

        if (!msg.isMember(ripple::jss::Account))
            return warn_ret("'Account' missed");

        auto const txAccStr = msg[ripple::jss::Account].asString();
        if ((txAccStr != lockingDoorStr) && (txAccStr != issuingDoorStr))
            return warn_ret("unknown tx account");

        auto const parsedAcc = ripple::parseBase58<ripple::AccountID>(txAccStr);
        if (!parsedAcc)
            return warn_ret("invalid 'Account'");

        auto opEntries =
            processSignerListSetGeneral(msg, chainName, errTopic, j_);
        if (!opEntries)
            return;

        event::XChainSignerListSet evSignSet{
            .chainType_ = txAccStr == lockingDoorStr ? ChainType::locking
                                                     : ChainType::issuing,
            .masterDoorID_ = *parsedAcc,
            .signerList_ = std::move(*opEntries)};
        if (evSignSet.chainType_ != chainType_)
        {
            // This is strange but it is processed well by rippled
            // so we can proceed
            JLOGV(
                j_.warn(),
                "processing signer list message",
                jv("warning", "Door account type mismatch"),
                jv("chain_type", to_string(chainType_)),
                jv("tx_type", to_string(evSignSet.chainType_)));
        }

        pushEvent(std::move(evSignSet));
    }
    catch (std::exception const& e)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("exception", e.what()),
            jv("chain_name", chainName),
            jv("msg", msg));
    }
    catch (...)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("exception", "unknown exception"),
            jv("chain_name", chainName),
            jv("msg", msg));
    }
}

void
ChainListener::processAccountSet(Json::Value const& msg) const noexcept
{
    std::string const chainName = to_string(chainType_);
    std::string_view const errTopic = "ignoring AccountSet message";

    auto warn_ret = [&, this](std::string_view reason) {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("reason", reason),
            jv("chain_name", chainName),
            jv("msg", msg));
    };

    try
    {
        auto const lockingDoorStr =
            ripple::toBase58(bridge_.lockingChainDoor());
        auto const issuingDoorStr =
            ripple::toBase58(bridge_.issuingChainDoor());

        if (!msg.isMember(ripple::jss::Account))
            return warn_ret("'Account' missed");

        auto const txAccStr = msg[ripple::jss::Account].asString();
        if ((txAccStr != lockingDoorStr) && (txAccStr != issuingDoorStr))
            return warn_ret("unknown tx account");

        auto const parsedAcc = ripple::parseBase58<ripple::AccountID>(txAccStr);
        if (!parsedAcc)
            return warn_ret("invalid 'Account'");

        if (!msg.isMember(ripple::jss::SetFlag) &&
            !msg.isMember(ripple::jss::ClearFlag))
            return warn_ret("'XXXFlag' missed");

        bool const setFlag = msg.isMember(ripple::jss::SetFlag);
        unsigned const flag = setFlag ? msg[ripple::jss::SetFlag].asUInt()
                                      : msg[ripple::jss::ClearFlag].asUInt();
        if (flag != ripple::asfDisableMaster)
            return warn_ret("not 'asfDisableMaster' flag");

        event::XChainAccountSet evAccSet{chainType_, *parsedAcc, setFlag};
        if (evAccSet.chainType_ != chainType_)
        {
            // This is strange but it is processed well by rippled
            // so we can proceed
            JLOGV(
                j_.warn(),
                "processing account set",
                jv("warning", "Door account type mismatch"),
                jv("chain_name", chainName),
                jv("tx_type", to_string(evAccSet.chainType_)));
        }
        pushEvent(std::move(evAccSet));
    }
    catch (std::exception const& e)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("exception", e.what()),
            jv("chain_name", chainName),
            jv("msg", msg));
    }
    catch (...)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("exception", "unknown exception"),
            jv("chain_name", chainName),
            jv("msg", msg));
    }
}

void
ChainListener::processSetRegularKey(Json::Value const& msg) const noexcept
{
    std::string const chainName = to_string(chainType_);
    std::string_view const errTopic = "ignoring SetRegularKey message";

    auto warn_ret = [&, this](std::string_view reason) {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("reason", reason),
            jv("chain_name", chainName),
            jv("msg", msg));
    };

    try
    {
        auto const lockingDoorStr =
            ripple::toBase58(bridge_.lockingChainDoor());
        auto const issuingDoorStr =
            ripple::toBase58(bridge_.issuingChainDoor());

        if (!msg.isMember(ripple::jss::Account))
            return warn_ret("'Account' missed");

        auto const txAccStr = msg[ripple::jss::Account].asString();
        if ((txAccStr != lockingDoorStr) && (txAccStr != issuingDoorStr))
            return warn_ret("unknown tx account");

        auto const parsedAcc = ripple::parseBase58<ripple::AccountID>(txAccStr);
        if (!parsedAcc)
            return warn_ret("invalid 'Account'");

        event::XChainSetRegularKey evSetKey{
            .chainType_ = txAccStr == lockingDoorStr ? ChainType::locking
                                                     : ChainType::issuing,
            .masterDoorID_ = *parsedAcc};

        if (evSetKey.chainType_ != chainType_)
        {
            // This is strange but it is processed well by rippled
            // so we can proceed
            JLOGV(
                j_.warn(),
                "processing account set",
                jv("warning", "Door account type mismatch"),
                jv("chain_name", chainName),
                jv("tx_type", to_string(evSetKey.chainType_)));
        }

        std::string const regularKeyStr = msg.isMember("RegularKey")
            ? msg["RegularKey"].asString()
            : std::string();
        if (!regularKeyStr.empty())
        {
            auto opRegularDoorId =
                ripple::parseBase58<ripple::AccountID>(regularKeyStr);
            if (!opRegularDoorId)
                return warn_ret("invalid 'RegularKey'");
            evSetKey.regularDoorID_ = std::move(*opRegularDoorId);
        }

        pushEvent(std::move(evSetKey));
    }
    catch (std::exception const& e)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("exception", e.what()),
            jv("chain_name", chainName),
            jv("msg", msg));
    }
    catch (...)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("exception", "unknown exception"),
            jv("chain_name", chainName),
            jv("msg", msg));
    }
}

void
ChainListener::processTx(Json::Value const& v) const noexcept
{
    std::string const chainName = to_string(chainType_);
    std::string_view const errTopic = "ignoring tx RPC response";

    auto warn_ret = [&, this](std::string_view reason) {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("reason", reason),
            jv("chain_name", chainName),
            jv("msg", v));
    };

    try
    {
        if (!v.isMember(ripple::jss::result))
            return warn_ret("missing result field");

        auto const& msg = v[ripple::jss::result];

        if (!msg.isMember(ripple::jss::validated) ||
            !msg[ripple::jss::validated].asBool())
            return warn_ret("not validated");

        if (!msg.isMember(ripple::jss::meta))
            return warn_ret("missing meta field");

        auto const& meta = msg[ripple::jss::meta];

        if (!(meta.isMember("TransactionResult") &&
              meta["TransactionResult"].isString() &&
              meta["TransactionResult"].asString() == "tesSUCCESS"))
            return warn_ret("missing or bad TransactionResult");

        auto txnTypeOpt = rpcResultParse::parseXChainTxnType(msg);
        if (!txnTypeOpt)
            return warn_ret("missing or bad tx type");

        auto const txnHash = rpcResultParse::parseTxHash(msg);
        if (!txnHash)
            return warn_ret("missing or bad tx hash");

        auto const txnBridge = rpcResultParse::parseBridge(msg);
        if (!txnBridge)  // TODO check bridge match
            return warn_ret("missing or bad bridge");

        auto const txnSeq = rpcResultParse::parseTxSeq(msg);
        if (!txnSeq)
            return warn_ret("missing or bad tx sequence");

        auto const lgrSeq = rpcResultParse::parseLedgerSeq(msg);
        if (!lgrSeq)
            return warn_ret("missing or bad ledger sequence");

        auto const src = rpcResultParse::parseSrcAccount(msg);
        if (!src)
            return warn_ret("missing or bad source account");

        auto const dst = rpcResultParse::parseDstAccount(msg, *txnTypeOpt);

        std::optional<ripple::STAmount> deliveredAmt =
            rpcResultParse::parseDeliveredAmt(msg, meta);

        auto const oppositeChainDir = chainType_ == ChainType::locking
            ? ChainDir::lockingToIssuing
            : ChainDir::issuingToLocking;

        switch (*txnTypeOpt)
        {
            case XChainTxnType::xChainCommit: {
                auto const claimID = Json::getOptional<std::uint64_t>(
                    msg, ripple::sfXChainClaimID);
                if (!claimID)
                    return warn_ret("no claimID");

                using namespace event;
                XChainCommitDetected e{
                    oppositeChainDir,
                    *src,
                    *txnBridge,
                    deliveredAmt,
                    *claimID,
                    dst,
                    *lgrSeq,
                    *txnHash,
                    ripple::tesSUCCESS,
                    {},
                    false};
                pushEvent(std::move(e));
            }
            break;
            case XChainTxnType::xChainAccountCreateCommit: {
                auto const createCount = rpcResultParse::parseCreateCount(meta);
                if (!createCount)
                    return warn_ret("missing or bad createCount");

                auto const rewardAmt = rpcResultParse::parseRewardAmt(msg);
                if (!rewardAmt)
                    return warn_ret("missing or bad rewardAmount");

                if (!dst)
                    return warn_ret("missing or bad destination account");

                using namespace event;
                XChainAccountCreateCommitDetected e{
                    oppositeChainDir,
                    *src,
                    *txnBridge,
                    deliveredAmt,
                    *rewardAmt,
                    *createCount,
                    *dst,
                    *lgrSeq,
                    *txnHash,
                    ripple::tesSUCCESS,
                    {},
                    false};
                pushEvent(std::move(e));
            }
            break;
            default:
                return warn_ret("wrong transaction type");
        }
    }
    catch (std::exception const& e)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("exception", e.what()),
            jv("chain_name", chainName),
            jv("msg", v));
    }
    catch (...)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("exception", "unknown exception"),
            jv("chain_name", chainName),
            jv("msg", v));
    }
}

void
ChainListener::processAccountTx(Json::Value const& msg)
{
    bool const requestContinue = processAccountTxHlp(msg);

    if (hp_.state_ != HistoryProcessor::FINISHED)
    {
        if (hp_.stopHistory_)
        {
            inRequest_ = false;
            txnHistoryIndex_ = 0;
            hp_.state_ = HistoryProcessor::FINISHED;
            JLOGV(
                j_.info(),
                "History mode off",
                jv("chain_name", to_string(chainType_)));
        }
        else if (!requestContinue)
            requestLedgers();
    }
    else if (!requestContinue)
        inRequest_ = false;
}

bool
ChainListener::processAccountTxHlp(Json::Value const& msg)
{
    static std::string const errTopic = "ignoring account_tx response";

    auto const chainName = to_string(chainType_);

    auto warnMsg = [&, this](std::string_view reason, auto&&... ts) {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("reason", reason),
            jv("chain_name", chainName),
            jv("msg", msg),
            std::forward<decltype(ts)>(ts)...);
    };

    auto warnCont = [&, this](std::string_view reason, auto&&... ts) {
        JLOGV(
            j_.warn(),
            errTopic,
            jv("reason", reason),
            jv("chain_name", chainName),
            std::forward<decltype(ts)>(ts)...);
    };

    if (!msg.isMember(ripple::jss::result))
    {
        warnMsg("no result");
        throw std::runtime_error("processAccountTx no result");
    }

    auto const& result = msg[ripple::jss::result];

    if (!result.isMember(ripple::jss::ledger_index_max) ||
        !result[ripple::jss::ledger_index_max].isIntegral() ||
        !result.isMember(ripple::jss::ledger_index_min) ||
        !result[ripple::jss::ledger_index_min].isIntegral())
    {
        warnMsg("no ledger range");
        throw std::runtime_error("no ledger range");
    }

    // these should left the same during full account_tx + marker serie
    unsigned const ledgerMax = result[ripple::jss::ledger_index_max].asUInt();
    unsigned const ledgerMin = result[ripple::jss::ledger_index_min].asUInt();

    if ((hp_.state_ != HistoryProcessor::FINISHED) && ledgerMin &&
        (ledgerMin <= hp_.toRequestLedger_))
        hp_.toRequestLedger_ = ledgerMin - 1;

    if (hp_.stopHistory_ && (ledgerMax <= hp_.startupLedger_))
    {
        warnCont(
            "stopped processing historical request",
            jv("startupLedger", hp_.startupLedger_),
            jv("ledgerMax", ledgerMax));
        return false;
    }

    if (!result.isMember(ripple::jss::account) ||
        !result[ripple::jss::account].isString())
    {
        warnMsg("no account");
        throw std::runtime_error("processAccountTx no account");
    }

    if (!result.isMember(ripple::jss::transactions) ||
        !result[ripple::jss::transactions].isArray())
    {
        warnMsg("no transactions");
        throw std::runtime_error("processAccountTx no transactions");
    }

    auto const& transactions = result[ripple::jss::transactions];
    if (!transactions.size())
        return false;

    bool const isMarker = result.isMember("marker");

    unsigned cnt = 0;
    for (auto it = transactions.begin(); it != transactions.end(); ++it, ++cnt)
    {
        if ((hp_.state_ != HistoryProcessor::FINISHED) && hp_.stopHistory_)
            break;

        if (hp_.accoutTxProcessed_ &&
            (hp_.state_ == HistoryProcessor::CHECK_LEDGERS))
        {
            // Skip records from previous request if ledgers were not present at
            // that moment
            if (cnt < hp_.accoutTxProcessed_)
                continue;
            hp_.accoutTxProcessed_ = 0;
            warnCont("Skipping tx", jv("cnt", cnt));
        }

        auto const& entry(*it);

        auto next = it;
        ++next;
        bool const isLast = next == transactions.end();

        if (!entry.isMember(ripple::jss::meta))
        {
            warnCont("no meta", jv("entry", entry));
            throw std::runtime_error("processAccountTx no meta");
        }
        auto const& meta = entry[ripple::jss::meta];

        if (!entry.isMember(ripple::jss::tx))
        {
            warnCont("no tx", jv("entry", entry));
            throw std::runtime_error("processAccountTx no tx");
        }
        auto const& tx = entry[ripple::jss::tx];

        Json::Value history = Json::objectValue;

        if (!tx.isMember(ripple::jss::ledger_index) ||
            !tx[ripple::jss::ledger_index].isIntegral())
        {
            warnCont("no ledger_index", jv("entry", entry));
            throw std::runtime_error("processAccountTx no ledger_index");
        }
        std::uint32_t const ledgerIdx = tx[ripple::jss::ledger_index].asUInt();
        bool const isHistorical = hp_.startupLedger_ >= ledgerIdx;

        if (isHistorical && hp_.stopHistory_)
        {
            warnCont(
                "stopped processing historical tx",
                jv("startupLedger", hp_.startupLedger_),
                jv("txLedger", ledgerIdx));
            continue;
        }

        // if (!isMarker && isLast && isHistorical)
        //     history[ripple::jss::account_history_tx_first] = true;

        bool const lgrBdr = (hp_.state_ != HistoryProcessor::FINISHED)
            ? prevLedgerIndex_ != ledgerIdx
            : isLast;
        if (lgrBdr)
        {
            prevLedgerIndex_ = ledgerIdx;
            history[ripple::jss::account_history_boundary] = true;
        }
        history[ripple::jss::account_history_tx_index] =
            isHistorical ? --txnHistoryIndex_ : txnHistoryIndex_++;
        std::string const tr = meta["TransactionResult"].asString();
        history[ripple::jss::engine_result] = tr;
        auto const tc = ripple::transCode(tr);
        if (!tc)
        {
            warnCont("no TransactionResult", jv("entry", entry));
            throw std::runtime_error("processAccountTx no TransactionResult");
        }
        history[ripple::jss::engine_result_code] = *tc;
        history[ripple::jss::ledger_index] = ledgerIdx;
        history[ripple::jss::meta] = meta;
        history[ripple::jss::transaction] = tx;
        history[ripple::jss::validated] =
            entry[ripple::jss::validated].asBool();
        history[ripple::jss::type] = ripple::jss::transaction;

        processMessage(history);
    }

    if (hp_.accoutTxProcessed_ && (cnt >= hp_.accoutTxProcessed_) &&
        (hp_.state_ == HistoryProcessor::CHECK_LEDGERS))
        warnCont("Skipped tx", jv("cnt", cnt));

    if (!isMarker && !hp_.stopHistory_)
        hp_.accoutTxProcessed_ = cnt;

    if (isMarker &&
        ((hp_.state_ == HistoryProcessor::FINISHED) || !hp_.stopHistory_))
    {
        std::string const account = result[ripple::jss::account].asString();
        accountTx(account, ledgerMin, ledgerMax, result[ripple::jss::marker]);
        return true;
    }

    return false;
}

void
ChainListener::accountTx(
    std::string const& account,
    unsigned ledger_min,
    unsigned ledger_max,
    Json::Value const& marker)
{
    inRequest_ = true;
    if (hp_.state_ != HistoryProcessor::FINISHED)
        hp_.marker_ = marker;

    Json::Value txParams;
    txParams[ripple::jss::account] = account;

    if (ledger_min)
        txParams[ripple::jss::ledger_index_min] = ledger_min;
    else
        txParams[ripple::jss::ledger_index_min] = -1;

    if (ledger_max)
        txParams[ripple::jss::ledger_index_max] = ledger_max;
    else
        txParams[ripple::jss::ledger_index_max] = -1;

    txParams[ripple::jss::binary] = false;
    txParams[ripple::jss::limit] = txLimit_;
    txParams[ripple::jss::forward] = hp_.state_ == HistoryProcessor::FINISHED;
    if (!marker.isNull())
        txParams[ripple::jss::marker] = marker;
    send(
        "account_tx",
        txParams,
        [self = shared_from_this()](Json::Value const& msg) {
            self->processAccountTx(msg);
        });
}

void
ChainListener::requestLedgers()
{
    if ((hp_.state_ == HistoryProcessor::RETR_HISTORY) ||
        (hp_.state_ == HistoryProcessor::CHECK_LEDGERS))
    {
        hp_.state_ = HistoryProcessor::RETR_LEDGERS;
        sendLedgerReq(hp_.requestLedgerBatch_);
    }
}

void
ChainListener::sendLedgerReq(unsigned cnt)
{
    auto const chainName = to_string(chainType_);

    if (!cnt || (hp_.toRequestLedger_ < minUserLedger_))
    {
        hp_.state_ = HistoryProcessor::CHECK_LEDGERS;
        JLOGV(
            j_.info(),
            "Finished requesting ledgers",
            jv("chain_name", chainName),
            jv("finish_ledger", hp_.toRequestLedger_ + 1));
        return;
    }

    if (cnt == hp_.requestLedgerBatch_)
    {
        JLOGV(
            j_.info(),
            "History not completed, start requesting more ledgers",
            jv("chain_name", chainName),
            jv("start_ledger", hp_.toRequestLedger_));
    }

    auto ledgerReqCb = [self = shared_from_this(), cnt](Json::Value const&) {
        self->sendLedgerReq(cnt - 1);
    };
    Json::Value params;
    params[ripple::jss::ledger_index] = hp_.toRequestLedger_--;
    send("ledger_request", params, ledgerReqCb);
}

void
ChainListener::processNewLedger(unsigned ledgerIdx)
{
    auto const doorAccStr = ripple::toBase58(bridge_.door(chainType_));

    if (!hp_.startupLedger_)
    {
        hp_.startupLedger_ = ledgerIdx;
        hp_.toRequestLedger_ = ledgerIdx - 1;
        JLOGV(
            j_.info(),
            "Init startup ledger",
            jv("chain_name", to_string(chainType_)),
            jv("startup_ledger", hp_.startupLedger_));

        if (!ledgerIdx)
        {
            JLOGV(
                j_.error(),
                "New ledger invalid idx",
                jv("chain_name", to_string(chainType_)),
                jv("ledgerIdx", ledgerIdx));
            throw std::runtime_error("New ledger invalid idx");
        }
    }

    if (hp_.state_ == HistoryProcessor::FINISHED)
    {
        if (inRequest_)
            return;

        if (ledgerIdx > ledgerReqMax_)
        {
            auto const ledgerReqMin =
                ledgerReqMax_ ? ledgerReqMax_ + 1 : hp_.startupLedger_ + 1;
            ledgerReqMax_ = ledgerIdx;
            accountTx(doorAccStr, ledgerReqMin, ledgerReqMax_);
            if (!witnessAccountStr_.empty())
                accountTx(witnessAccountStr_, ledgerReqMin, ledgerReqMax_);
        }

        return;
    }

    // History mode
    switch (hp_.state_)
    {
        case HistoryProcessor::CHECK_BRIDGE: {
            auto checkBridgeCb = [self = shared_from_this(),
                                  doorAccStr](Json::Value const& msg) {
                if (!self->processBridgeReq(msg))
                {
                    JLOGV(
                        self->j_.info(),
                        "History mode off, no bridge",
                        jv("chain_name", to_string(self->chainType_)));
                    self->hp_.state_ = HistoryProcessor::FINISHED;
                    self->pushEvent(event::EndOfHistory{self->chainType_});
                    return;
                }
                self->hp_.state_ = HistoryProcessor::RETR_HISTORY;
                self->accountTx(doorAccStr, 0, self->hp_.startupLedger_);
            };

            hp_.state_ = HistoryProcessor::WAIT_CB;
            Json::Value params;
            params[ripple::jss::bridge_account] = doorAccStr;
            send("ledger_entry", params, checkBridgeCb);

            break;
        }
        case HistoryProcessor::CHECK_LEDGERS: {
            JLOGV(
                j_.warn(),
                "Witness NOT initialized, waiting for ledgers",
                jv("chain_name", to_string(chainType_)));

            auto serverInfoCb = [self = shared_from_this(),
                                 doorAccStr](Json::Value const& msg) {
                auto const currentMinLedger = self->hp_.minValidatedLedger_;
                self->processServerInfo(msg);
                // check if ledgers were retrieved
                if (self->hp_.minValidatedLedger_ < currentMinLedger)
                    self->accountTx(
                        doorAccStr,
                        0,
                        self->hp_.startupLedger_,
                        self->hp_.marker_);
            };
            Json::Value params;
            send("server_info", params, serverInfoCb);

            break;
        }
        case HistoryProcessor::RETR_LEDGERS:
            [[fallthrough]];
        case HistoryProcessor::RETR_HISTORY:
            [[fallthrough]];
        case HistoryProcessor::WAIT_CB:
            [[fallthrough]];
        default:
            break;
    }
}

bool
ChainListener::processBridgeReq(Json::Value const& msg)
{
    auto const doorAccStr = ripple::toBase58(bridge_.door(chainType_));
    std::string const doorName = chainType_ == ChainType::locking
        ? "LockingChainDoor"
        : "IssuingChainDoor";

    if (msg.isMember(ripple::jss::error) && !msg[ripple::jss::error].isNull())
        return false;

    if (!msg.isMember(ripple::jss::status) ||
        !msg[ripple::jss::status].isString() ||
        (msg[ripple::jss::status].asString() != "success"))
        return false;

    if (!msg.isMember(ripple::jss::result))
        return false;
    auto const& res = msg[ripple::jss::result];

    if (!res.isMember(ripple::jss::ledger_current_index) ||
        !res[ripple::jss::ledger_current_index].isIntegral())
        return false;

    if (!res.isMember(ripple::jss::node))
        return false;
    auto const& node = res[ripple::jss::node];
    if (!node.isMember("XChainBridge"))
        return false;
    if (!node.isMember("PreviousTxnLgrSeq") ||
        !node["PreviousTxnLgrSeq"].isIntegral())
        return false;

    auto const& bridge = node["XChainBridge"];
    std::string const doorStr =
        bridge.isMember(doorName) && bridge[doorName].isString()
        ? bridge[doorName].asString()
        : std::string();

    return doorAccStr == doorStr;
}

Json::Value
ChainListener::getInfo() const
{
    // TODO
    Json::Value ret{Json::objectValue};
    return ret;
}

void
HistoryProcessor::clear()
{
    state_ = CHECK_BRIDGE;
    stopHistory_ = false;
    marker_.clear();
    accoutTxProcessed_ = 0;
    startupLedger_ = 0;
    toRequestLedger_ = 0;
    minValidatedLedger_ = 0;
}

}  // namespace xbwd
