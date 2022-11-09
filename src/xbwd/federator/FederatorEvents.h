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
    ChainDir dir_;
    // Src account on the src chain
    ripple::AccountID src_;
    ripple::STXChainBridge bridge_;
    std::optional<ripple::STAmount> deliveredAmt_;
    std::uint64_t claimID_;
    std::optional<ripple::AccountID> otherChainDst_;

    std::uint32_t ledgerSeq_;
    ripple::uint256 txnHash_;
    ripple::TER status_;
    std::int32_t rpcOrder_;
    bool ledgerBoundary_;

    Json::Value
    toJson() const;
};

// A cross chain account create was detected on this federator
struct XChainAccountCreateCommitDetected
{
    ChainDir dir_;
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
    std::int32_t rpcOrder_;
    bool ledgerBoundary_;

    Json::Value
    toJson() const;
};

struct HeartbeatTimer
{
    Json::Value
    toJson() const;
};

struct XChainTransferResult
{
    // direction is the direction of the triggering transaction.
    // I.e. A "mainToSide" transfer result is a transaction that
    // happens on the sidechain (the triggering transaction happended on the
    // mainchain)
    ChainDir dir_;
    ripple::AccountID dst_;
    std::optional<ripple::STAmount> deliveredAmt_;
    std::uint64_t claimID_;
    std::uint32_t ledgerSeq_;
    // Txn hash transaction on the dst chain
    ripple::uint256 txnHash_;
    ripple::TER ter_;
    std::int32_t rpcOrder_;

    Json::Value
    toJson() const;
};

struct XChainAttestsResult
{
    ChainType chainType_;
    std::uint32_t accountSqn_;
    ripple::TER ter_;

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

// Signer list changed on chain account
struct XChainSignerListSet
{
    ChainType chainType_ = ChainType::locking;
    ripple::AccountID account_;
    std::vector<ripple::AccountID> entries_;

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
    event::XChainSignerListSet>;

Json::Value
toJson(FederatorEvent const& event);

}  // namespace xbwd
