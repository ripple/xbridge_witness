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

#include <xbwd/federator/FederatorEvents.h>

#include <fmt/core.h>

#include <string_view>
#include <type_traits>

namespace xbwd {
namespace event {

namespace {

std::string
to_hex(std::uint64_t i)
{
    return fmt::format("{:016x}", i);
}

}  // namespace

Json::Value
XChainCommitDetected::toJson() const
{
    Json::Value result{Json::objectValue};
    result["eventType"] = "XChainCommitDetected";
    result["src"] = toBase58(src_);
    if (otherChainDst_)
        result["otherChainDst"] = toBase58(*otherChainDst_);
    if (deliveredAmt_)
        result["deliveredAmt"] =
            deliveredAmt_->getJson(ripple::JsonOptions::none);
    result["claimID"] = to_hex(claimID_);
    result["txnHash"] = to_string(txnHash_);
    if (rpcOrder_)
        result["rpcOrder"] = *rpcOrder_;
    return result;
}

Json::Value
XChainAccountCreateCommitDetected::toJson() const
{
    Json::Value result{Json::objectValue};
    result["eventType"] = "XChainAccountCreateCommitDetected";
    result["src"] = toBase58(src_);
    result["otherChainDst"] = toBase58(otherChainDst_);
    if (deliveredAmt_)
        result["deliveredAmt"] =
            deliveredAmt_->getJson(ripple::JsonOptions::none);
    result["rewardAmt"] = rewardAmt_.getJson(ripple::JsonOptions::none);
    result["createCount"] = to_hex(createCount_);
    result["txnHash"] = to_string(txnHash_);
    if (rpcOrder_)
        result["rpcOrder"] = *rpcOrder_;
    return result;
}

Json::Value
HeartbeatTimer::toJson() const
{
    Json::Value result{Json::objectValue};
    result["eventType"] = "HeartbeatTimer";
    return result;
}

Json::Value
XChainTransferResult::toJson() const
{
    Json::Value result{Json::objectValue};
    result["eventType"] = "XChainTransferResult";
    result["dir"] = to_string(dir_);
    result["dst"] = toBase58(dst_);
    if (deliveredAmt_)
        result["deliveredAmt"] =
            deliveredAmt_->getJson(ripple::JsonOptions::none);
    result["claimID"] = to_hex(claimID_);
    result["txnHash"] = to_string(txnHash_);
    result["ter"] = transHuman(ter_);
    if (rpcOrder_)
        result["rpcOrder"] = *rpcOrder_;
    return result;
}

Json::Value
XChainAttestsResult::toJson() const
{
    Json::Value result{Json::objectValue};
    result["eventType"] = "XChainAttestsResult";
    result["chainType"] = to_string(chainType_);
    result["accountSequence"] = accountSqn_;
    result["txnHash"] = to_string(txnHash_);
    result["ter"] = transHuman(ter_);

    result["isHistory"] = isHistory_;
    result["type"] = static_cast<int>(type_);
    result["src"] = toBase58(src_);
    result["dst"] = toBase58(dst_);
    if (createCount_)
        result["createCount"] = to_hex(*createCount_);
    if (claimID_)
        result["claimID"] = to_hex(*claimID_);
    return result;
}

Json::Value
NewLedger::toJson() const
{
    Json::Value result{Json::objectValue};
    result["eventType"] = "NewLedger";
    result["chainType"] = to_string(chainType_);
    result["ledgerIndex"] = ledgerIndex_;
    result["fee"] = fee_;
    return result;
}

Json::Value
EndOfHistory::toJson() const
{
    Json::Value result{Json::objectValue};
    result["eventType"] = "EndOfHistory";
    result["chainType"] = to_string(chainType_);
    return result;
}

Json::Value
XChainSignerListSet::toJson() const
{
    Json::Value result{Json::objectValue};
    result["eventType"] = "XChainSignerListSet";
    result["masterDoorID"] = masterDoorID_.isNonZero()
        ? ripple::toBase58(masterDoorID_)
        : std::string();
    auto& jEntries = (result["entries"] = Json::arrayValue);
    for (auto const& acc : signerList_)
        jEntries.append(toBase58(acc));

    return result;
}

Json::Value
XChainSetRegularKey::toJson() const
{
    Json::Value result{Json::objectValue};
    result["eventType"] = "XChainSetRegularKey";
    result["masterDoorID"] = masterDoorID_.isNonZero()
        ? ripple::toBase58(masterDoorID_)
        : std::string();
    result["regularDoorID"] = regularDoorID_.isNonZero()
        ? ripple::toBase58(regularDoorID_)
        : std::string();

    return result;
}

Json::Value
XChainAccountSet::toJson() const
{
    Json::Value result{Json::objectValue};
    result["eventType"] = "XChainAccountSet";
    result["masterDoorID"] = masterDoorID_.isNonZero()
        ? ripple::toBase58(masterDoorID_)
        : std::string();
    result["disableMaster"] = disableMaster_;

    return result;
}

}  // namespace event

Json::Value
toJson(FederatorEvent const& event)
{
    return std::visit([](auto const& e) { return e.toJson(); }, event);
}

}  // namespace xbwd
