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

#include <ripple/protocol/SField.h>
#include <ripple/protocol/jss.h>
#include <xbwd/client/RpcResultParse.h>

namespace xbwd {

namespace rpcResultParse {
bool
fieldMatchesStr(Json::Value const& val, char const* field, char const* toMatch)
{
    if (!val.isMember(field))
        return false;
    auto const f = val[field];
    if (!f.isString())
        return false;
    return f.asString() == toMatch;
}

std::optional<std::uint64_t>
parseCreateCount(Json::Value const& meta)
{
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
                auto const& ff = node[ripple::sfFinalFields.getJsonName()];
                auto const& count =
                    ff[ripple::sfXChainAccountCreateCount.getJsonName()];
                if (count.isString())
                {
                    auto const s = count.asString();
                    std::uint64_t val;

                    // TODO: Confirm that this will be encoded as
                    // hex
                    auto [p, ec] =
                        std::from_chars(s.data(), s.data() + s.size(), val, 16);

                    if (ec != std::errc() || (p != s.data() + s.size()))
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
}

std::optional<ripple::STAmount>
parseRewardAmt(Json::Value const& transaction)
{
    if (transaction.isMember(ripple::sfSignatureReward.getJsonName()))
    {
        return amountFromJson(
            ripple::sfGeneric,
            transaction[ripple::sfSignatureReward.getJsonName()]);
    }
    return {};
}

std::optional<xbwd::XChainTxnType>
parseXChainTxnType(Json::Value const& transaction)
{
    using enum xbwd::XChainTxnType;
    static std::unordered_map<std::string, XChainTxnType> const candidates{
        {ripple::jss::XChainCommit.c_str(), xChainCommit},
        {ripple::jss::XChainClaim.c_str(), xChainClaim},
        {ripple::jss::XChainCreateBridge.c_str(), xChainCreateBridge},
        {ripple::jss::XChainAccountCreateCommit.c_str(),
         xChainAccountCreateCommit},
#ifdef USE_BATCH_ATTESTATION
        {ripple::jss::XChainAddAttestationBatch.c_str(),
         xChainAddAttestationBatch},
#endif
        {ripple::jss::XChainAddAccountCreateAttestation.c_str(),
         xChainAddAccountCreateAttestation},
        {ripple::jss::XChainAddClaimAttestation.c_str(),
         xChainAddClaimAttestation},
        {ripple::jss::SignerListSet.c_str(), SignerListSet},
        {ripple::jss::AccountSet.c_str(), AccountSet},
        {ripple::jss::SetRegularKey.c_str(), SetRegularKey},
    };

    if (!transaction.isMember(ripple::jss::TransactionType) ||
        !transaction[ripple::jss::TransactionType].isString())
        return {};

    if (auto const it = candidates.find(
            transaction[ripple::jss::TransactionType].asString());
        it != candidates.end())
    {
        return it->second;
    }
    return std::nullopt;
}

std::optional<ripple::AccountID>
parseDstAccount(Json::Value const& transaction, XChainTxnType txnType)
{
    try
    {
        switch (txnType)
        {
            case XChainTxnType::xChainAccountCreateCommit:
                [[fallthrough]];
            case XChainTxnType::xChainClaim:
                return ripple::parseBase58<ripple::AccountID>(
                    transaction[ripple::sfDestination.getJsonName()]
                        .asString());
            case XChainTxnType::xChainCommit:
                return ripple::parseBase58<ripple::AccountID>(
                    transaction[ripple::sfOtherChainDestination.getJsonName()]
                        .asString());
            default:
                break;
        }
    }
    catch (...)
    {
    }
    return {};
}

std::optional<ripple::AccountID>
parseSrcAccount(Json::Value const& transaction)
{
    try
    {
        return ripple::parseBase58<ripple::AccountID>(
            transaction[ripple::jss::Account].asString());
    }
    catch (...)
    {
    }
    return {};
}

std::optional<ripple::AccountID>
parseOtherSrcAccount(Json::Value const& transaction, XChainTxnType txnType)
{
    try
    {
        switch (txnType)
        {
            case XChainTxnType::xChainCommit:
                [[fallthrough]];
            case XChainTxnType::xChainAccountCreateCommit:
                return ripple::parseBase58<ripple::AccountID>(
                    transaction[ripple::jss::Account].asString());

            case XChainTxnType::xChainAddClaimAttestation:
                [[fallthrough]];
            case XChainTxnType::xChainAddAccountCreateAttestation:
                return ripple::parseBase58<ripple::AccountID>(
                    transaction[ripple::sfOtherChainSource.getJsonName()]
                        .asString());
            default:
                break;
        }
    }
    catch (...)
    {
    }
    return {};
}

std::optional<ripple::AccountID>
parseOtherDstAccount(Json::Value const& transaction, XChainTxnType txnType)
{
    try
    {
        switch (txnType)
        {
            case XChainTxnType::xChainAccountCreateCommit:
                [[fallthrough]];
            case XChainTxnType::xChainAddClaimAttestation:
                [[fallthrough]];
            case XChainTxnType::xChainAddAccountCreateAttestation:
                return ripple::parseBase58<ripple::AccountID>(
                    transaction[ripple::sfDestination.getJsonName()]
                        .asString());

            case XChainTxnType::xChainCommit:
                return ripple::parseBase58<ripple::AccountID>(
                    transaction[ripple::sfOtherChainDestination.getJsonName()]
                        .asString());
            default:
                break;
        }
    }
    catch (...)
    {
    }
    return {};
}

std::optional<ripple::STXChainBridge>
parseBridge(Json::Value const& transaction)
{
    try
    {
        if (!transaction.isMember(ripple::sfXChainBridge.getJsonName()))
            return {};
        return ripple::STXChainBridge(
            transaction[ripple::sfXChainBridge.getJsonName()]);
    }
    catch (...)
    {
    }
    return {};
}

std::optional<ripple::uint256>
parseTxHash(Json::Value const& transaction)
{
    try
    {
        ripple::uint256 result;
        if (result.parseHex(transaction[ripple::jss::hash].asString()))
            return result;
    }
    catch (...)
    {
    }
    return {};
}

std::optional<std::uint32_t>
parseTxSeq(Json::Value const& transaction)
{
    try
    {
        if (!transaction.isMember(ripple::jss::Sequence) ||
            !transaction[ripple::jss::Sequence].isIntegral())
            return {};
        return transaction[ripple::jss::Sequence].asUInt();
    }
    catch (...)
    {
        return {};
    }
}

std::optional<std::uint32_t>
parseLedgerSeq(Json::Value const& msg)
{
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
    return {};
}

std::optional<ripple::STAmount>
parseDeliveredAmt(Json::Value const& transaction, Json::Value const& meta)
{
    std::optional<ripple::STAmount> deliveredAmt;
    if (meta.isMember(ripple::jss::delivered_amount))
    {
        deliveredAmt = amountFromJson(
            ripple::sfGeneric, meta[ripple::jss::delivered_amount]);
    }
    // TODO: Add delivered amount to the txn data; for now override with amount
    if (transaction.isMember(ripple::jss::Amount))
    {
        deliveredAmt =
            amountFromJson(ripple::sfGeneric, transaction[ripple::jss::Amount]);
    }
    return deliveredAmt;
}
}  // namespace rpcResultParse
}  // namespace xbwd
