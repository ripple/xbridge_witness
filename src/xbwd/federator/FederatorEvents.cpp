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

std::string const&
to_string(Dir dir)
{
    switch (dir)
    {
        case Dir::lockingToIssuing: {
            static std::string const r("locking");
            return r;
        }
        case Dir::issuingToLocking: {
            static std::string const r("issuing");
            return r;
        }
    }

    // Some compilers will warn about not returning from all control paths
    // without this, but this code will never execute.
    assert(0);
    static std::string const error("error");
    return error;
}

}  // namespace

Json::Value
XChainCommitDetected::toJson() const
{
    Json::Value result{Json::objectValue};
    result["eventType"] = "XChainTransferDetected";
    result["src"] = toBase58(src_);
    if (otherChainAccount_)
        result["otherChainAccount"] = toBase58(*otherChainAccount_);
    if (deliveredAmt_)
        result["deliveredAmt"] =
            deliveredAmt_->getJson(ripple::JsonOptions::none);
    result["claimID"] = to_hex(claimID_);
    result["txnHash"] = to_string(txnHash_);
    result["rpcOrder"] = rpcOrder_;
    return result;
}

Json::Value
XChainAccountCreateCommitDetected::toJson() const
{
    Json::Value result{Json::objectValue};
    result["eventType"] = "XChainAccountCreateCommitDetected";
    result["src"] = toBase58(src_);
    result["otherChainAccount"] = toBase58(otherChainAccount_);
    if (deliveredAmt_)
        result["deliveredAmt"] =
            deliveredAmt_->getJson(ripple::JsonOptions::none);
    result["rewardAmt"] = rewardAmt_.getJson(ripple::JsonOptions::none);
    result["createCount"] = to_hex(createCount_);
    result["txnHash"] = to_string(txnHash_);
    result["rpcOrder"] = rpcOrder_;
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
    result["rpcOrder"] = rpcOrder_;
    return result;
}

}  // namespace event

Json::Value
toJson(FederatorEvent const& event)
{
    return std::visit([](auto const& e) { return e.toJson(); }, event);
}

}  // namespace xbwd
