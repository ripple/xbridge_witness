//------------------------------------------------------------------------------
/*
    Copyright (c) 2022 Ripple Labs Inc.

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

#include <ChainListener.h>

// #include <ripple/app/sidechain/Federator.h>
#include <AttnServer.h>
#include <FederatorEvents.h>
#include <WebsocketClient.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/XRPAmount.h>
#include <ripple/basics/strHex.h>
#include <ripple/json/Output.h>
#include <ripple/json/json_writer.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>

#include <type_traits>

// todo: temporary hack as these are not defined in the current branch
// -------------------------------------------------------------------
namespace ripple {
namespace jss {

#define JSS(x) constexpr ::Json::StaticString x(#x)
    
JSS(Memo);                   // txn common field
JSS(Memos);                  // txn common field
JSS(MemoType);               // txn common field
JSS(MemoData);               // txn common field

#undef JSS
}
}

// todo: temporary hack as log macros have changed
// -----------------------------------------------
#undef JLOGV
#undef jss
#define JLOGV(a,...)
#define jss(a,b) a


namespace ripple {
namespace sidechain {

class AttnServer;

ChainListener::ChainListener(
    ChainType chain_type,
    AccountID const& doorAccount,
    AttnServer& attn_server,
    beast::Journal j)
    : chain_type_{chain_type}
    , doorAccount_{doorAccount}
    , doorAccountStr_{toBase58(doorAccount)}
    , attn_server_{attn_server}
    , j_{j}
{
}

// destructor must be defined after WebsocketClient size is known (i.e. it can
// not be defaulted in the header or the unique_ptr declration of
// WebsocketClient won't work)
ChainListener::~ChainListener() = default;

std::string const& ChainListener::chainName() const
{
    // Note: If this function is ever changed to return a value instead of a
    // ref, review the code to ensure the "jv" functions don't bind to temps
    static const std::string m("Mainchain");
    static const std::string s("Sidechain");
    return isMainchain() ? m : s;
}

void ChainListener::processMessage(Json::Value const& msg)
{
    // Even though this lock has a large scope, this function does very little
    // processing and should run relatively quickly
    std::lock_guard l{m_};

    JLOGV(j_.trace(), "chain listener message", jv("msg", msg), jv("isMainchain", isMainchain()));

    std::array<std::pair<bool, char const *>, 4> checks {{
        { msg.isMember(jss::validated) && msg[jss::validated].asBool(),  "not validated" },
        { msg.isMember(jss::engine_result_code),  "no engine result code" },
        { msg.isMember(jss::account_history_tx_index),  "no account history tx index" },
        { msg.isMember(jss::meta),  "tx meta" }}};
    
    for ( auto check : checks) {
        if (!check.first) {
            JLOGV(
                j_.trace(),
                "ignoring listener message",
                jv("reason", check.second),
                jv("msg", msg),
                jv("chain_name", chainName()));
            return;
        }
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

    TER const txnTER = [&msg] {
        return TER::fromInt(msg[jss::engine_result_code].asInt());
    }();

    bool const txnSuccess = (txnTER == tesSUCCESS);

    // values < 0 are historical txns. values >= 0 are new transactions. Only
    // the initial sync needs historical txns.
    int const txnHistoryIndex = msg[jss::account_history_tx_index].asInt();

    auto const meta = msg[jss::meta];

    // There are two payment types of interest:
    // 1. User initiated payments on this chain that trigger a transaction on
    // the other chain.
    // 2. Federated initated payments on this chain whose status needs to be
    // checked.
    enum class PaymentType { user, federator };
    auto paymentTypeOpt = [&]() -> std::optional<PaymentType> {
        // Only keep transactions to or from the door account.
        // Transactions to the account are initiated by users and are are cross
        // chain transactions. Transaction from the account are initiated by
        // attn_servers and need to be monitored for errors. There are two types
        // of transactions that originate from the door account: the second half
        // of a cross chain payment and a refund of a failed cross chain
        // payment.

        if (!fieldMatchesStr(msg, jss::type, jss::transaction))
            return {};

        if (!msg.isMember(jss::transaction))
            return {};
        
        auto const txn = msg[jss::transaction];

        if (!fieldMatchesStr(txn, jss::TransactionType, "Payment"))
            return {};

        bool const accIsSrc = fieldMatchesStr(txn, jss::Account, doorAccountStr_.c_str());
        bool const accIsDst = fieldMatchesStr(txn, jss::Destination, doorAccountStr_.c_str());

        if (accIsSrc == accIsDst)
        {
            // either account is not involved, or self send
            return {};
        }

        if (accIsSrc)
            return PaymentType::federator;
        return PaymentType::user;
    }();

    // There are four types of messages used to control the attn_server accounts:
    // 1. AccountSet without modifying account settings. These txns are used to
    // trigger TicketCreate txns.
    // 2. TicketCreate to issue tickets.
    // 3. AccountSet that changes the depositAuth setting of accounts.
    // 4. SignerListSet to update the signerList of accounts.
    // 5. AccoutSet that disables the master key. All transactions before this
    // are used for setup only and should be ignored. This transaction is also
    // used to help set the initial transaction sequence numbers
    enum class AccountControlType {
        trigger,
        ticket,
        depositAuth,
        signerList,
        disableMasterKey
    };
    auto accountControlTypeOpt = [&]() -> std::optional<AccountControlType> {
        if (!fieldMatchesStr(msg, jss::type, jss::transaction))
            return {};

        if (!msg.isMember(jss::transaction))
            return {};
        auto const txn = msg[jss::transaction];

        if (fieldMatchesStr(txn, jss::TransactionType, "AccountSet"))
        {
            if (!(txn.isMember(jss::SetFlag) || txn.isMember(jss::ClearFlag)))
            {
                return AccountControlType::trigger;
            }
            else
            {
                // Get the flags value at the key. If the key is not present,
                // return 0.
                auto getFlags =
                    [&txn](Json::StaticString const& key) -> std::uint32_t {
                    if (txn.isMember(key))
                    {
                        auto const val = txn[key];
                        try
                        {
                            return val.asUInt();
                        }
                        catch (...)
                        {
                        }
                    }
                    return 0;
                };

                std::uint32_t const setFlags = getFlags(jss::SetFlag);
                std::uint32_t const clearFlags = getFlags(jss::ClearFlag);

                if (setFlags == asfDepositAuth || clearFlags == asfDepositAuth)
                    return AccountControlType::depositAuth;

                if (setFlags == asfDisableMaster)
                    return AccountControlType::disableMasterKey;
            }
        }
        if (fieldMatchesStr(txn, jss::TransactionType, "TicketCreate"))
            return AccountControlType::ticket;
        if (fieldMatchesStr(txn, jss::TransactionType, "SignerListSet"))
            return AccountControlType::signerList;

        return {};
    }();

    if (!paymentTypeOpt && !accountControlTypeOpt)
    {
        JLOGV(
            j_.warn(),
            "ignoring listener message",
            jv("reason", "wrong type, not payment nor account control tx"),
            jv("msg", msg),
            jv("chain_name", chainName()));
        return;
    }
    assert(!paymentTypeOpt || !accountControlTypeOpt);

    auto const txnHash = [&]() -> std::optional<uint256> {
        try
        {
            uint256 result;
            if (result.parseHex(msg[jss::transaction][jss::hash].asString()))
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
        JLOG(j_.warn()) << "ignoring listener message, no tx hash";
        return;
    }

    auto const seq = [&]() -> std::optional<std::uint32_t> {
        try
        {
            return msg[jss::transaction][jss::Sequence].asUInt();
        }
        catch (...)
        {
            // TODO: this is an insane input stream
            // Detect and connect to another server
            return {};
        }
    }();
    if (!seq)
    {
        JLOG(j_.warn()) << "ignoring listener message, no tx seq";
        return;
    }

    if (paymentTypeOpt)
    {
        PaymentType const paymentType = *paymentTypeOpt;

        std::optional<STAmount> deliveredAmt;
        if (meta.isMember(jss::delivered_amount))
        {
            deliveredAmt =
                amountFromJson(sfGeneric, meta[jss::delivered_amount]);
        }

        auto const src = [&]() -> std::optional<AccountID> {
            try
            {
                return parseBase58<AccountID>(
                    msg[jss::transaction][jss::Account].asString());
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
            // TODO: handle the error
            return;
        }

        auto const dst = [&]() -> std::optional<AccountID> {
            try
            {
                switch (paymentType)
                {
                    case PaymentType::user: {
                        // This is the destination of the "other chain"
                        // transfer, which is specified as a memo.
                        if (!msg.isMember(jss::transaction))
                        {
                            return std::nullopt;
                        }
                        try
                        {
                            // the memo data is a hex encoded version of the
                            // base58 encoded address. This was chosen for ease
                            // of encoding by clients.
                            auto const hexData =
                                msg[jss::transaction][jss::Memos][0u][jss::Memo]
                                   [jss::MemoData]
                                       .asString();
                            if ((hexData.size() > 100) || (hexData.size() % 2))
                                return std::nullopt;

                            auto const asciiData = [&]() -> std::string {
                                std::string result;
                                result.reserve(40);
                                auto d = hexData.data();
                                for (int i = 0; i < hexData.size(); i += 2)
                                {
                                    auto highNibble = charUnHex(d[i]);
                                    auto lowNibble = charUnHex(d[i + 1]);
                                    if (highNibble < 0 || lowNibble < 0)
                                        return {};
                                    char c = (highNibble << 4) | lowNibble;
                                    result.push_back(c);
                                }
                                return result;
                            }();
                            return parseBase58<AccountID>(asciiData);
                        }
                        catch (...)
                        {
                            // User did not specify a destination address in a
                            // memo
                            return std::nullopt;
                        }
                    }
                    case PaymentType::federator:
                        return parseBase58<AccountID>(
                            msg[jss::transaction][jss::Destination].asString());
                }
            }
            catch (...)
            {
            }
            // TODO: this is an insane input stream
            // Detect and connect to another server
            return {};
        }();
        if (!dst)
        {
            // TODO: handle the error
            return;
        }

        switch (paymentType)
        {
            case PaymentType::user: {
                if (!txnSuccess)
                    return;

                if (!deliveredAmt)
                    return;

                // todo - store in db
            }
            break;
        }
    }

    // Note: Handling "last in history" is done through the lambda given
    // to `make_scope` earlier in the function
}

}  // namespace sidechain
}  // namespace ripple
