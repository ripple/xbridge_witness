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
#include <ripple/json/json_writer.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>

#include <type_traits>

namespace xbwd {

class Federator;

ChainListener::ChainListener(
    IsMainchain isMainchain,
    ripple::STXChainBridge const sidechain,
    std::weak_ptr<Federator>&& federator,
    beast::Journal j)
    : isMainchain_{isMainchain == IsMainchain::yes}
    , sidechain_{sidechain}
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
    wsClient_ = std::make_unique<WebsocketClient>(
        [self = shared_from_this()](Json::Value const& msg) {
            self->onMessage(msg);
        },
        ios,
        ip,
        /*headers*/ std::unordered_map<std::string, std::string>{},
        j_);

    Json::Value params;
    params[ripple::jss::account_history_tx_stream] = Json::objectValue;
    params[ripple::jss::account_history_tx_stream][ripple::jss::account] =
        ripple::toBase58(
            isMainchain_ ? sidechain_.lockingChainDoor()
                         : sidechain_.issuingChainDoor());
    send("subscribe", params);
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
    Json::Value params;
    params[ripple::jss::account_history_tx_stream] = Json::objectValue;
    params[ripple::jss::account_history_tx_stream]
          [ripple::jss::stop_history_tx_only] = true;
    params[ripple::jss::account_history_tx_stream][ripple::jss::account] =
        ripple::toBase58(
            isMainchain_ ? sidechain_.lockingChainDoor()
                         : sidechain_.issuingChainDoor());
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
    std::lock_guard lock(callbacksMtx_);
    callbacks_.emplace(id, onResponse);
}

std::string const&
ChainListener::chainName() const
{
    // Note: If this function is ever changed to return a value instead of a
    // ref, review the code to ensure the "jv" functions don't bind to temps
    static const std::string m("Mainchain");
    static const std::string s("Sidechain");
    return isMainchain_ ? m : s;
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
            ripple::jv("msg", msg));
        assert(msg.isMember(ripple::jss::result));
        (*callbackOpt)(msg[ripple::jss::result]);
    }
    else
    {
        processMessage(msg);
    }
}

void
ChainListener::processMessage(Json::Value const& msg)
{
    // Even though this lock has a large scope, this function does very little
    // processing and should run relatively quickly
    std::lock_guard l{m_};

    JLOGV(
        j_.trace(),
        "chain listener message",
        ripple::jv("msg", msg),
        ripple::jv("isMainchain", isMainchain_));

    if (!msg.isMember(ripple::jss::validated) ||
        !msg[ripple::jss::validated].asBool())
    {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("reason", "not validated"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName()));
        return;
    }

    if (!msg.isMember(ripple::jss::engine_result_code))
    {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("reason", "no engine result code"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName()));
        return;
    }

    if (!msg.isMember(ripple::jss::account_history_tx_index))
    {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("reason", "no account history tx index"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName()));
        return;
    }

    if (!msg.isMember(ripple::jss::meta))
    {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("reason", "tx meta"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName()));
        return;
    }

    auto fieldMatchesStr =
        [](Json::Value const& val, char const* field, char const* toMatch) {
            if (!val.isMember(field))
                return false;
            auto const f = val[field];
            if (!f.isString())
                return false;
            return f.asString() == toMatch;
        };

    ripple::TER const txnTER = [&msg] {
        return ripple::TER::fromInt(
            msg[ripple::jss::engine_result_code].asInt());
    }();

    bool const txnSuccess = ripple::isTesSuccess(txnTER);

    // values < 0 are historical txns. values >= 0 are new transactions. Only
    // the initial sync needs historical txns.
    int const txnHistoryIndex =
        msg[ripple::jss::account_history_tx_index].asInt();

    auto const meta = msg[ripple::jss::meta];

    // There are two payment types of interest:
    // 1. User initiated payments on this chain that trigger a transaction on
    // the other chain.
    // 2. Federated initated payments on this chain whose status needs to be
    // checked.
    enum class TxnType { xChainTransfer, xChainClaim };
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

        auto const isXChainTransfer = fieldMatchesStr(
            txn, ripple::jss::TransactionType, ripple::jss::XChainCommit);

        auto const isXChainClaim = !isXChainTransfer &&
            fieldMatchesStr(
                txn, ripple::jss::TransactionType, ripple::jss::XChainClaim);

        if (!isXChainTransfer && !isXChainClaim)
            return {};

            // TODO: Fix bug where sidechain field is empty
#if 0
        auto const txnSidechain = [&]() -> std::optional<ripple::STXChainBridge> {
            if (msg.isMember(ripple::jss::Sidechain))
            {
                try
                {
                    return ripple::STXChainBridgeFromJson(
                        ripple::sfSidechain, txn[ripple::jss::Sidechain]);
                }
                catch (...)
                {
                }
            }
            return std::nullopt;
        }();

        if (!txnSidechain)
        {
            JLOGV(
                j_.trace(),
                "ignoring listener message",
                ripple::jv("reason", "invalid txn: Missing sidechain"),
                ripple::jv("msg", msg),
                ripple::jv("chain_name", chainName()));
            return {};
        }

        if (*txnSidechain != sidechain_)
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
                ripple::jv("chain_name", chainName()));
            return {};
        }
#endif

        if (isXChainClaim)
            return TxnType::xChainClaim;
        else if (isXChainTransfer)
            return TxnType::xChainTransfer;
        return {};
    }();

    if (!txnTypeOpt)
    {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("reason", "not a sidechain transaction"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName()));
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
            ripple::jv("chain_name", chainName()));
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
            ripple::jv("chain_name", chainName()));
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
            ripple::jv("chain_name", chainName()));
        return;
    }
    auto const xChainSeq = [&]() -> std::optional<std::uint32_t> {
        try
        {
            if (msg[ripple::jss::transaction].isMember(
                    ripple::sfXChainClaimID.getName()))
            {
                return msg[ripple::jss::transaction]
                          [ripple::sfXChainClaimID.getName()]
                              .asUInt();
            }
        }
        catch (...)
        {
        }
        // TODO: this is an insane input stream
        // Detect and connect to another server
        return {};
    }();
    if (!xChainSeq)
    {
        JLOGV(
            j_.warn(),
            "ignoring listener message",
            ripple::jv("reason", "no xChainSeq"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName()));
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
            ripple::jv("chain_name", chainName()));
        return;
    }
    auto const dst = [&]() -> std::optional<ripple::AccountID> {
        if (txnType == TxnType::xChainClaim)
        {
            try
            {
                return ripple::parseBase58<ripple::AccountID>(
                    msg[ripple::jss::transaction][ripple::jss::Destination]
                        .asString());
            }
            catch (...)
            {
            }
        }
        return {};
    }();

    switch (txnType)
    {
        case TxnType::xChainClaim: {
            if (!dst)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv("reason", "no dst in xchain claim"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName()));
                return;
            }
            using namespace event;
            XChainTransferResult e{
                isMainchain_ ? Dir::sideToMain : Dir::mainToSide,
                *dst,
                deliveredAmt,
                *xChainSeq,
                *lgrSeq,
                *txnHash,
                txnTER,
                txnHistoryIndex};
            pushEvent(std::move(e));
        }
        break;
        case TxnType::xChainTransfer: {
            using namespace event;
            XChainTransferDetected e{
                isMainchain_ ? Dir::mainToSide : Dir::sideToMain,
                *src,
                deliveredAmt,
                *xChainSeq,
                *lgrSeq,
                *txnHash,
                txnTER,
                txnHistoryIndex};
            pushEvent(std::move(e));
        }
        break;
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
