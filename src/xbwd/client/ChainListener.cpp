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

#include <xbwd/client/WebsocketClient.h>
#include <xbwd/federator/Federator.h>
#include <xbwd/federator/FederatorEvents.h>

#include <ripple/basics/Log.h>
#include <ripple/basics/XRPAmount.h>
#include <ripple/basics/strHex.h>
#include <ripple/json/Output.h>
#include <ripple/json/json_get_or_throw.h>
#include <ripple/json/json_writer.h>
#include <ripple/protocol/AccountID.h>
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
    beast::Journal j)
    : chainType_{chainType}
    , bridge_{sidechain}
    , witnessAccountStr_(
          submitAccountOpt ? ripple::toBase58(*submitAccountOpt)
                           : std::string{})
    , federator_{std::move(federator)}
    , j_{j}
{
}

// destructor must be defined after WebsocketClient size is known (i.e. it can
// not be defaulted in the header or the unique_ptr declration of
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
    const auto doorAccStr = ripple::toBase58(
        ChainType::locking == chainType_ ? bridge_.lockingChainDoor()
                                         : bridge_.issuingChainDoor());

    Json::Value params;
    params[ripple::jss::account] = doorAccStr;
    params[ripple::jss::signer_lists] = true;

    send(
        "account_info",
        params,
        [self = shared_from_this(), doorAccStr](Json::Value const& v) {
            self->processAccountInfo(v);

            Json::Value params;
            params[ripple::jss::account_history_tx_stream] = Json::objectValue;
            params[ripple::jss::account_history_tx_stream]
                  [ripple::jss::account] = doorAccStr;

            params[ripple::jss::streams] = Json::arrayValue;
            params[ripple::jss::streams].append("ledger");
            if (!self->witnessAccountStr_.empty())
            {
                params[ripple::jss::accounts] = Json::arrayValue;
                params[ripple::jss::accounts].append(self->witnessAccountStr_);
            }
            self->send("subscribe", params);
        });
}

void
ChainListener::shutdown()
{
    if (wsClient_)
        wsClient_->shutdown();
}

std::uint32_t
ChainListener::send(std::string const& cmd, Json::Value const& params)
{
    return wsClient_->send(cmd, params);
}

void
ChainListener::stopHistoricalTxns()
{
    const auto doorAccStr = ripple::toBase58(
        ChainType::locking == chainType_ ? bridge_.lockingChainDoor()
                                         : bridge_.issuingChainDoor());

    Json::Value params;
    params[ripple::jss::account_history_tx_stream] = Json::objectValue;
    params[ripple::jss::account_history_tx_stream]
          [ripple::jss::stop_history_tx_only] = true;
    params[ripple::jss::account_history_tx_stream][ripple::jss::account] =
        doorAccStr;
    send("unsubscribe", params);
}

void
ChainListener::send(
    std::string const& cmd,
    Json::Value const& params,
    RpcCallback onResponse)
{
    JLOGV(
        j_.trace(),
        "ChainListener send",
        ripple::jv("command", cmd),
        ripple::jv("params", params));

    auto id = wsClient_->send(cmd, params);
    JLOGV(j_.trace(), "ChainListener send id", ripple::jv("id", id));

    std::lock_guard lock(callbacksMtx_);
    callbacks_.emplace(id, onResponse);
}

template <class E>
void
ChainListener::pushEvent(E&& e)
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
            ripple::jv("msg", msg.toStyledString()),
            ripple::jv("chain_name", to_string(chainType_)));
        (*callbackOpt)(msg);
    }
    else
    {
        processMessage(msg);
    }
}

void
ChainListener::processMessage(Json::Value const& msg)
{
    const auto chainName = to_string(chainType_);

    // Even though this lock has a large scope, this function does very little
    // processing and should run relatively quickly
    std::lock_guard l{m_};

    JLOGV(
        j_.trace(),
        "chain listener process message",
        ripple::jv("msg", msg.toStyledString()),
        ripple::jv("chain_name", chainName));

    // TODO two formats, consider only use 2nd
    /*
    {"msg": "{
       "id" : 0,
       "jsonrpc" : "2.0",
       "result" : {
          "fee_base" : 10,
          "fee_ref" : 10,
          "ledger_hash" :
    "ABEF4002A5656F61AD2AF8C324DFB88993A2707EF8F2680065306A3AE1DD5572",
          "ledger_index" : 24,
          "ledger_time" : 718263410,
          "reserve_base" : 5000000,
          "reserve_inc" : 1000000,
          "validated_ledgers" : "2-24",
          "warning" : "account_history_tx_stream is an experimental feature and
    likely to be removed in the future"
       },
       "ripplerpc" : "2.0",
       "status" : "success",
       "type" : "response"
    }
    ", "isMainchain": true, "jlogId": 213}

     {"msg": "{
       "fee_base" : 10,
       "fee_ref" : 10,
       "ledger_hash" :
    "9DE7A38E33F87566C7E120D3A3D327E4F792FB8730E792DEB39E3D3D4F2B2406",
       "ledger_index" : 25,
       "ledger_time" : 718263950,
       "reserve_base" : 5000000,
       "reserve_inc" : 1000000,
       "txn_count" : 1,
       "type" : "ledgerClosed", //not always have
       "validated_ledgers" : "2-25"
    }
    ", "isMainchain": true, "jlogId": 213}
    */

    auto tryPushNewLedgerEvent = [&](Json::Value const& result) -> bool {
        if (result.isMember(ripple::jss::fee_base) &&
            result[ripple::jss::fee_base].isIntegral() &&
            result.isMember(ripple::jss::ledger_index) &&
            result[ripple::jss::ledger_index].isIntegral() &&
            result.isMember(ripple::jss::reserve_base) &&
            result.isMember(ripple::jss::reserve_inc) &&
            result.isMember(ripple::jss::fee_ref) &&
            result.isMember(ripple::jss::validated_ledgers))
        {
            using namespace event;
            NewLedger e{
                chainType_,
                result[ripple::jss::ledger_index].asUInt(),
                result[ripple::jss::fee_base].asUInt()};
            pushEvent(std::move(e));
            return true;
        }
        return false;
    };

    auto fieldMatchesStr =
        [](Json::Value const& val, char const* field, char const* toMatch) {
            if (!val.isMember(field))
                return false;
            auto const f = val[field];
            if (!f.isString())
                return false;
            return f.asString() == toMatch;
        };

    if (msg.isMember(ripple::jss::result) &&
        tryPushNewLedgerEvent(msg[ripple::jss::result]))
        return;
    else if (tryPushNewLedgerEvent(msg))
        return;

    if (!msg.isMember(ripple::jss::validated) ||
        !msg[ripple::jss::validated].asBool())
    {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("reason", "not validated"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName));
        return;
    }

    if (!msg.isMember(ripple::jss::engine_result_code))
    {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("reason", "no engine result code"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName));
        return;
    }

    if (!msg.isMember(ripple::jss::meta))
    {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("reason", "tx meta"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName));
        return;
    }

    ripple::TER const txnTER = [&msg] {
        return ripple::TER::fromInt(
            msg[ripple::jss::engine_result_code].asInt());
    }();

    bool const txnSuccess = ripple::isTesSuccess(txnTER);

    if (fieldMatchesStr(msg, ripple::jss::type, ripple::jss::transaction))
    {
        auto const txn = msg[ripple::jss::transaction];
        if (fieldMatchesStr(
                txn,
                ripple::jss::TransactionType,
                ripple::jss::XChainAddAttestation) &&
            fieldMatchesStr(
                txn, ripple::jss::Account, witnessAccountStr_.c_str()) &&
            txn.isMember(ripple::jss::Sequence) &&
            txn[ripple::jss::Sequence].isIntegral())
        {
            auto const txnSeq = txn[ripple::jss::Sequence].asUInt();
            using namespace event;
            XChainAttestsResult e{chainType_, txnSeq, txnTER};
            pushEvent(std::move(e));
            return;
        }
    }

    if (!msg.isMember(ripple::jss::account_history_tx_index))
    {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("reason", "no account history tx index"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName));
        return;
    }

    // values < 0 are historical txns. values >= 0 are new transactions. Only
    // the initial sync needs historical txns.
    int const txnHistoryIndex =
        msg[ripple::jss::account_history_tx_index].asInt();

    if (txnSuccess && (txnHistoryIndex >= 0) &&
        fieldMatchesStr(msg, ripple::jss::type, ripple::jss::transaction))
    {
        auto const& txn = msg[ripple::jss::transaction];
        if (fieldMatchesStr(
                txn, ripple::jss::TransactionType, ripple::jss::SignerListSet))
        {
            processSignerListSet(txn);
        }
    }

    auto const meta = msg[ripple::jss::meta];

    auto const txnBridge = [&]() -> std::optional<ripple::STXChainBridge> {
        try
        {
            if (!msg.isMember(ripple::jss::transaction))
                return {};
            auto const txn = msg[ripple::jss::transaction];
            if (!txn.isMember(ripple::jss::XChainBridge))
                return {};
            return ripple::STXChainBridge(txn[ripple::jss::XChainBridge]);
        }
        catch (...)
        {
        }
        return {};
    }();

    enum class TxnType { xChainCommit, xChainClaim, xChainCreateAccount };
    auto txnTypeOpt = [&]() -> std::optional<TxnType> {
        // Only keep transactions to or from the door account.
        // Transactions to the account are initiated by users and are are cross
        // chain transactions. Transaction from the account are initiated by
        // federators and need to be monitored for errors. There are two types
        // of transactions that originate from the door account: the second half
        // of a cross chain payment and a refund of a failed cross chain
        // payment.

        if (!fieldMatchesStr(msg, ripple::jss::type, ripple::jss::transaction))
            return {};

        if (!msg.isMember(ripple::jss::transaction))
            return {};
        auto const txn = msg[ripple::jss::transaction];

        auto const isXChainCommit = fieldMatchesStr(
            txn, ripple::jss::TransactionType, ripple::jss::XChainCommit);

        auto const isXChainClaim = !isXChainCommit &&
            fieldMatchesStr(
                txn, ripple::jss::TransactionType, ripple::jss::XChainClaim);

        auto const isXChainCreateAccount = !isXChainCommit && !isXChainClaim &&
            fieldMatchesStr(
                txn,
                ripple::jss::TransactionType,
                ripple::jss::XChainAccountCreateCommit);

        if (!isXChainCommit && !isXChainClaim && !isXChainCreateAccount)
            return {};

        if (!txnBridge)
        {
            JLOGV(
                j_.trace(),
                "ignoring listener message",
                ripple::jv("reason", "invalid txn: Missing bridge"),
                ripple::jv("msg", msg),
                ripple::jv("chain_name", chainName));
            return {};
        }

        if (*txnBridge != bridge_)
        {
            // TODO: It is a mistake to filter out based on sidechain.
            // This server should support multiple sidechains
            // Note: the federator stores a hard-coded sidechain in the
            // database, if we remove this filter we need to remove sidechain
            // from the app and listener as well
            JLOGV(
                j_.trace(),
                "ignoring listener message",
                ripple::jv("reason", "Sidechain mismatch"),
                ripple::jv("msg", msg),
                ripple::jv("chain_name", chainName));
            return {};
        }

        if (isXChainClaim)
            return TxnType::xChainClaim;
        else if (isXChainCommit)
            return TxnType::xChainCommit;
        else if (isXChainCreateAccount)
            return TxnType::xChainCreateAccount;
        return {};
    }();

    if (!txnTypeOpt)
    {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("reason", "not a sidechain transaction"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName));
        return;
    }

    auto const txnHash = [&]() -> std::optional<ripple::uint256> {
        try
        {
            ripple::uint256 result;
            if (result.parseHex(msg[ripple::jss::transaction][ripple::jss::hash]
                                    .asString()))
                return result;
        }
        catch (...)
        {
        }
        // TODO: this is an insane input stream
        // Detect and connect to another server
        return {};
    }();
    if (!txnHash)
    {
        JLOGV(
            j_.warn(),
            "ignoring listener message",
            ripple::jv("reason", "no tx hash"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName));
        return;
    }

    auto const txnSeq = [&]() -> std::optional<std::uint32_t> {
        try
        {
            return msg[ripple::jss::transaction][ripple::jss::Sequence]
                .asUInt();
        }
        catch (...)
        {
            // TODO: this is an insane input stream
            // Detect and connect to another server
            return {};
        }
    }();
    if (!txnSeq)
    {
        JLOGV(
            j_.warn(),
            "ignoring listener message",
            ripple::jv("reason", "no txnSeq"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName));
        return;
    }
    auto const lgrSeq = [&]() -> std::optional<std::uint32_t> {
        try
        {
            if (msg.isMember(ripple::jss::ledger_index))
            {
                return msg[ripple::jss::ledger_index].asUInt();
            }
        }
        catch (...)
        {
        }
        // TODO: this is an insane input stream
        // Detect and connect to another server
        return {};
    }();
    if (!lgrSeq)
    {
        JLOGV(
            j_.warn(),
            "ignoring listener message",
            ripple::jv("reason", "no lgrSeq"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName));
        return;
    }
    TxnType const txnType = *txnTypeOpt;

    std::optional<ripple::STAmount> deliveredAmt;
    if (meta.isMember(ripple::jss::delivered_amount))
    {
        deliveredAmt = amountFromJson(
            ripple::sfGeneric,
            msg[ripple::jss::transaction][ripple::jss::delivered_amount]);
    }
    // TODO: Add delivered amount to the txn data; for now override with amount
    if (msg[ripple::jss::transaction].isMember(ripple::jss::Amount))
    {
        deliveredAmt = amountFromJson(
            ripple::sfGeneric,
            msg[ripple::jss::transaction][ripple::jss::Amount]);
    }

    auto const src = [&]() -> std::optional<ripple::AccountID> {
        try
        {
            return ripple::parseBase58<ripple::AccountID>(
                msg[ripple::jss::transaction][ripple::jss::Account].asString());
        }
        catch (...)
        {
        }
        // TODO: this is an insane input stream
        // Detect and connect to another server
        return {};
    }();
    if (!src)
    {
        JLOGV(
            j_.warn(),
            "ignoring listener message",
            ripple::jv("reason", "no account src"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName));
        return;
    }
    auto const dst = [&]() -> std::optional<ripple::AccountID> {
        try
        {
            switch (txnType)
            {
                case TxnType::xChainCreateAccount:
                    [[fallthrough]];
                case TxnType::xChainClaim:
                    return ripple::parseBase58<ripple::AccountID>(
                        msg[ripple::jss::transaction]
                           [ripple::sfDestination.getJsonName()]
                               .asString());
                case TxnType::xChainCommit:
                    return ripple::parseBase58<ripple::AccountID>(
                        msg[ripple::jss::transaction]
                           [ripple::sfOtherChainDestination.getJsonName()]
                               .asString());
            }
        }
        catch (...)
        {
        }
        return {};
    }();

    auto const ledgerBoundary = [&]() -> bool {
        if (msg.isMember(ripple::jss::account_history_ledger_boundary) &&
            msg[ripple::jss::account_history_ledger_boundary].isBool() &&
            msg[ripple::jss::account_history_ledger_boundary].asBool())
        {
            JLOGV(
                j_.trace(),
                "ledger boundary",
                ripple::jv("seq", *lgrSeq),
                ripple::jv("chain_name", chainName));
            return true;
        }
        return false;
    }();

    auto const [chainDir, oppositeChainDir] =
        [&]() -> std::pair<ChainDir, ChainDir> {
        using enum ChainDir;
        if (chainType_ == ChainType::locking)
            return {issuingToLocking, lockingToIssuing};
        return {lockingToIssuing, issuingToLocking};
    }();

    switch (txnType)
    {
        case TxnType::xChainClaim: {
            auto const claimID = Json::getOptional<std::uint64_t>(
                msg[ripple::jss::transaction], ripple::sfXChainClaimID);

            if (!claimID)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv("reason", "no xChainSeq"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName));
                return;
            }
            if (!dst)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv("reason", "no dst in xchain claim"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName));
                return;
            }
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
        case TxnType::xChainCommit: {
            auto const claimID = Json::getOptional<std::uint64_t>(
                msg[ripple::jss::transaction], ripple::sfXChainClaimID);

            if (!claimID)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv("reason", "no xChainSeq"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName));
                return;
            }
            if (!txnBridge)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv("reason", "no bridge in xchain commit"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName));
                return;
            }
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
        case TxnType::xChainCreateAccount: {
            auto const createCount = [&]() -> std::optional<std::uint64_t> {
                try
                {
                    auto af = meta[ripple::sfAffectedNodes.getJsonName()];
                    for (auto const& outerNode : af)
                    {
                        try
                        {
                            auto const node =
                                outerNode[ripple::sfModifiedNode.getJsonName()];
                            if (node[ripple::sfLedgerEntryType.getJsonName()] !=
                                ripple::jss::Bridge)
                                continue;
                            auto const& ff =
                                node[ripple::sfFinalFields.getJsonName()];
                            auto const& count =
                                ff[ripple::sfXChainAccountCreateCount
                                       .getJsonName()];
                            if (count.isString())
                            {
                                auto const s = count.asString();
                                std::uint64_t val;

                                // TODO: Confirm that this will be encoded as
                                // hex
                                auto [p, ec] = std::from_chars(
                                    s.data(), s.data() + s.size(), val, 16);

                                if (ec != std::errc() ||
                                    (p != s.data() + s.size()))
                                    return {};
                                return val;
                            }
                            return count.asUInt();
                        }
                        catch (...)
                        {
                        }
                    }
                }
                catch (...)
                {
                }
                return {};
            }();

            if (!createCount)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv("reason", "no createCount"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName));
                return;
            }
            if (!txnBridge)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv("reason", "no bridge in xchain commit"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName));
                return;
            }
            auto const rewardAmt = [&]() -> std::optional<ripple::STAmount> {
                if (msg[ripple::jss::transaction].isMember(
                        ripple::sfSignatureReward.getJsonName()))
                {
                    return amountFromJson(
                        ripple::sfGeneric,
                        msg[ripple::jss::transaction]
                           [ripple::sfSignatureReward.getJsonName()]);
                }
                return {};
            }();
            if (!rewardAmt)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv(
                        "reason", "no reward amt in xchain create account"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName));
                return;
            }
            if (!dst)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv("reason", "no dst in xchain create account"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName));
                return;
            }
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
    }
}

namespace {

void
logBadMsg(
    beast::Journal& j,
    const std::string_view reason,
    Json::Value const& msg,
    const std::string_view chainName)
{
    JLOGV(
        j.warn(),
        "ignoring signer list message",
        ripple::jv("reason", reason),
        ripple::jv("msg", msg),
        ripple::jv("chain_name", chainName));
}

std::optional<event::XChainSignerListSet>
processSignerListSetGeneral(
    Json::Value const& msg,
    beast::Journal& j,
    ChainType chainType)
{
    const std::string chainName = to_string(chainType);

    if (msg.isMember("SignerQuorum"))
    {
        const unsigned signerQuorum = msg["SignerQuorum"].asUInt();
        if (!signerQuorum)
        {
            logBadMsg(j, "'SignerQuorum' is null", msg, chainName);
            return {};
        }
    }
    else
    {
        logBadMsg(j, "'SignerQuorum' missed", msg, chainName);
        return {};
    }

    if (!msg.isMember("SignerEntries"))
    {
        logBadMsg(j, "'SignerEntries' missed", msg, chainName);
        return {};
    }
    const auto& signerEntries = msg["SignerEntries"];
    if (!signerEntries.isArray())
    {
        logBadMsg(j, "'SignerEntries' is not an array", msg, chainName);
        return {};
    }

    event::XChainSignerListSet e;
    e.entries_.reserve(signerEntries.size());
    for (const auto& superEntry : signerEntries)
    {
        if (!superEntry.isMember("SignerEntry"))
        {
            logBadMsg(j, "'SignerEntry' missed", msg, chainName);
            return {};
        }
        const auto& entry = superEntry["SignerEntry"];

        if (!entry.isMember(ripple::jss::Account))
        {
            logBadMsg(j, "'Account' missed", msg, chainName);
            return {};
        }
        const auto& jAcc = entry[ripple::jss::Account];
        auto parsed = ripple::parseBase58<ripple::AccountID>(jAcc.asString());
        if (!parsed)
        {
            logBadMsg(j, "invalid 'Account'", msg, chainName);
            return {};
        }

        e.entries_.push_back(parsed.value());
    }

    return e;
}

}  // namespace

void
ChainListener::processAccountInfo(Json::Value const& msg) noexcept
{
    const std::string chainName = to_string(chainType_);

    try
    {
        if (!msg.isMember(ripple::jss::result))
        {
            logBadMsg(j_, "'result' missed", msg, chainName);
            return;
        }
        const auto& jres = msg[ripple::jss::result];

        if (!jres.isMember(ripple::jss::account_data))
        {
            logBadMsg(j_, "'accoujnt_data' missed", msg, chainName);
            return;
        }
        const auto& jaccData = jres[ripple::jss::account_data];

        if (!jaccData.isMember(ripple::jss::Account))
        {
            logBadMsg(j_, "'Account' missed", msg, chainName);
            return;
        }
        const auto& jAcc = jaccData[ripple::jss::Account];
        auto parsed = ripple::parseBase58<ripple::AccountID>(jAcc.asString());
        if (!parsed)
        {
            logBadMsg(j_, "invalid 'Account'", msg, chainName);
            return;
        }

        if (!jaccData.isMember(ripple::jss::signer_lists))
        {
            logBadMsg(j_, "'signer_lists' missed", msg, chainName);
            return;
        }
        const auto& jslArray = jaccData[ripple::jss::signer_lists];

        if (!jslArray.isArray() || jslArray.size() != 1)
        {
            logBadMsg(
                j_, "'signer_lists'  isn't array of size 1", msg, chainName);
            return;
        }

        const auto& jsl = jslArray[0u];
        auto opEv = processSignerListSetGeneral(jsl, j_, chainType_);
        if (opEv)
        {
            event::XChainSignerListSet& evSignSet(*opEv);
            evSignSet.chainType_ = chainType_;
            evSignSet.account_ = parsed.value();
            pushEvent(std::move(evSignSet));
        }
    }
    catch (const std::exception& e)
    {
        JLOGV(
            j_.warn(),
            "ignoring signer list message",
            ripple::jv("exception", e.what()),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName));
    }
    catch (...)
    {
        logBadMsg(j_, "unknown exception", msg, chainName);
    }
}

void
ChainListener::processSignerListSet(Json::Value const& msg) noexcept
{
    const std::string chainName = to_string(chainType_);

    try
    {
        const auto encodedLockingDoorAccId =
            ripple::toBase58(bridge_.lockingChainDoor());
        const auto encodedIssuingDoorAccId =
            ripple::toBase58(bridge_.issuingChainDoor());

        if (!msg.isMember(ripple::jss::Account))
        {
            logBadMsg(j_, "'Account' missed", msg, chainName);
            return;
        }
        const auto& jAcc = msg[ripple::jss::Account];
        const auto txAccStr = jAcc.asString();

        if ((txAccStr != encodedLockingDoorAccId) &&
            (txAccStr != encodedIssuingDoorAccId))
        {
            logBadMsg(j_, "unknown tx account", msg, chainName);
            return;
        }

        auto parsed = ripple::parseBase58<ripple::AccountID>(txAccStr);
        if (!parsed)
        {
            logBadMsg(j_, "invalid 'Account'", msg, chainName);
            return;
        }

        auto opEv = processSignerListSetGeneral(msg, j_, chainType_);
        if (opEv)
        {
            event::XChainSignerListSet& evSignSet(*opEv);
            evSignSet.chainType_ = txAccStr == encodedLockingDoorAccId
                ? ChainType::locking
                : ChainType::issuing;

            evSignSet.account_ = parsed.value();
            if (evSignSet.chainType_ != chainType_)
            {
                // This is strange but it is processed well by rippled
                // so we can proceed
                JLOGV(
                    j_.warn(),
                    "processing signer list",
                    ripple::jv("warning", "Door account type mismatch"),
                    ripple::jv("chain_type", chainName),
                    ripple::jv("tx_type", to_string(evSignSet.chainType_)));
            }
            pushEvent(std::move(evSignSet));
        }
    }
    catch (const std::exception& e)
    {
        JLOGV(
            j_.warn(),
            "ignoring signer list message",
            ripple::jv("exception", e.what()),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName));
    }
    catch (...)
    {
        logBadMsg(j_, "unknown exception", msg, chainName);
    }
}

Json::Value
ChainListener::getInfo() const
{
    std::lock_guard l{m_};

    // TODO
    Json::Value ret{Json::objectValue};
    return ret;
}

}  // namespace xbwd
