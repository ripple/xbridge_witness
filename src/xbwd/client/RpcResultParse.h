#pragma once
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

#include <ripple/json/json_value.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/STXChainBridge.h>

#include <optional>

namespace xbwd {
enum class XChainTxnType {
    xChainCommit,
    xChainClaim,
    xChainCreateAccount,
#ifdef USE_BATCH_ATTESTATION
    xChainAttestationBatch,
#endif
    xChainAccountCreateAttestation,
    xChainClaimAttestation,
    SignerListSet,
    AccountSet,
    SetRegularKey
};

namespace rpcResultParse {
bool
fieldMatchesStr(Json::Value const& val, char const* field, char const* toMatch);

std::optional<std::uint64_t>
parseCreateCount(Json::Value const& meta);

std::optional<ripple::STAmount>
parseRewardAmt(Json::Value const& transaction);

std::optional<XChainTxnType>
parseXChainTxnType(Json::Value const& transaction);

std::optional<ripple::AccountID>
parseSrcAccount(Json::Value const& transaction);

std::optional<ripple::AccountID>
parseDstAccount(Json::Value const& transaction, XChainTxnType txnType);

std::optional<ripple::STXChainBridge>
parseBridge(Json::Value const& transaction);

std::optional<ripple::uint256>
parseTxHash(Json::Value const& transaction);

std::optional<std::uint32_t>
parseTxSeq(Json::Value const& transaction);

std::optional<std::uint32_t>
parseLedgerSeq(Json::Value const& msg);

std::optional<ripple::STAmount>
parseDeliveredAmt(Json::Value const& transaction, Json::Value const& meta);
}  // namespace rpcResultParse
}  // namespace xbwd
