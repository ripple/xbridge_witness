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

#ifndef RIPPLE_SIDECHAIN_FEDERATOR_EVENTS_H_INCLUDED
#define RIPPLE_SIDECHAIN_FEDERATOR_EVENTS_H_INCLUDED

#include <ripple/beast/utility/Journal.h>
#include <ripple/json/json_value.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Issue.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/TER.h>

#include <boost/format.hpp>

#include <optional>
#include <sstream>
#include <string>
#include <variant>

namespace ripple {
namespace sidechain {
namespace event {

enum class Dir { sideToMain, mainToSide };
enum class AccountFlagOp { set, clear };
static constexpr std::uint32_t MemoStringMax = 512;

enum class EventType {
    bootstrap,
    trigger,
    result,
    resultAndTrigger,
    heartbeat,
};

struct HeartbeatTimer
{
    EventType
    eventType() const;

    Json::Value
    toJson() const;
};

}  // namespace event


}  // namespace sidechain
}  // namespace ripple

#endif
