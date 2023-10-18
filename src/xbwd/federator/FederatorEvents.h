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

#include <xbwd/basics/ChainTypes.h>
#include <xbwd/client/RpcResultParse.h>

#include <ripple/json/json_value.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/TER.h>

#include <optional>
#include <variant>

namespace xbwd {
namespace event {

// A cross chain transfer was detected on this federator
struct XChainCommitDetected
{
    // Source of the event
    ChainType chainType_;
    // Src account on the src chain
    ripple::AccountID src_;
    ripple::STXChainBridge bridge_;
    std::optional<ripple::STAmount> deliveredAmt_;
    std::uint64_t claimID_;
    std::optional<ripple::AccountID> otherChainDst_;

    std::uint32_t ledgerSeq_;
    ripple::uint256 txnHash_;
    ripple::TER status_;
    std::optional<std::int32_t> rpcOrder_;
    bool ledgerBoundary_;

    Json::Value
    toJson() const;
};

// A cross chain account create was detected on this federator
struct XChainAccountCreateCommitDetected
{
    // Source of the event
    ChainType chainType_;
    // Src account on the src chain
    ripple::AccountID src_;
    ripple::STXChainBridge bridge_;
    std::optional<ripple::STAmount> deliveredAmt_;
    ripple::STAmount rewardAmt_;
    std::uint64_t createCount_;
    ripple::AccountID otherChainDst_;

    std::uint32_t ledgerSeq_;
    ripple::uint256 txnHash_;
    ripple::TER status_;
    std::optional<std::int32_t> rpcOrder_;
    bool ledgerBoundary_;

    Json::Value
    toJson() const;
};

struct HeartbeatTimer
{
    ChainType chainType_;
    Json::Value
    toJson() const;
};

struct XChainTransferResult
{
    // Source of the event
    ChainType chainType_;
    ripple::AccountID dst_;
    std::optional<ripple::STAmount> deliveredAmt_;
    std::uint64_t claimID_;
    std::uint32_t ledgerSeq_;
    // Txn hash transaction on the dst chain
    ripple::uint256 txnHash_;
    ripple::TER ter_;
    std::optional<std::int32_t> rpcOrder_;

    Json::Value
    toJson() const;
};

struct XChainAttestsResult
{
    ChainType chainType_ = ChainType::locking;
    std::uint32_t accountSqn_ = 0;
    ripple::uint256 txnHash_;
    ripple::TER ter_;

    bool isHistory_ = false;
    bool isFinal_ = false;
    xbwd::XChainTxnType type_ = xbwd::XChainTxnType::xChainAddClaimAttestation;
    ripple::AccountID src_, dst_;
    std::optional<std::uint64_t> createCount_, claimID_;

    Json::Value
    toJson() const;
};

struct NewLedger
{
    ChainType chainType_;
    std::uint32_t ledgerIndex_;
    std::uint32_t fee_;

    Json::Value
    toJson() const;
};

struct EndOfHistory
{
    ChainType chainType_;

    Json::Value
    toJson() const;
};

// Signer list changed on chain account
struct XChainSignerListSet
{
    ChainType chainType_ = ChainType::locking;
    ripple::AccountID masterDoorID_;
    std::unordered_set<ripple::AccountID> signerList_;

    Json::Value
    toJson() const;
};

struct XChainSetRegularKey
{
    ChainType chainType_ = ChainType::locking;
    ripple::AccountID masterDoorID_;
    ripple::AccountID regularDoorID_;

    Json::Value
    toJson() const;
};

struct XChainAccountSet
{
    ChainType chainType_ = ChainType::locking;
    ripple::AccountID masterDoorID_;
    bool disableMaster_ = false;

    Json::Value
    toJson() const;
};

}  // namespace event

using FederatorEvent = std::variant<
    event::XChainCommitDetected,
    event::XChainAccountCreateCommitDetected,
    event::HeartbeatTimer,
    event::XChainTransferResult,
    event::XChainAttestsResult,
    event::NewLedger,
    event::XChainSignerListSet,
    event::XChainSetRegularKey,
    event::XChainAccountSet,
    event::EndOfHistory>;

Json::Value
toJson(FederatorEvent const& event);

}  // namespace xbwd
