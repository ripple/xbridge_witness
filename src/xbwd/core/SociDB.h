#pragma once

//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2015 Ripple Labs Inc.

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

/** An embedded database wrapper with an intuitive, type-safe interface.

    This collection of classes let's you access embedded SQLite databases
    using C++ syntax that is very similar to regular SQL.

    This module requires the @ref beast_sqlite external module.
*/

#include <stdexcept>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated"
#endif

#include <ripple/basics/Buffer.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/base_uint.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/STXChainBridge.h>

#define SOCI_USE_BOOST
#include <cstdint>
#include <soci/soci.h>
#include <string>
#include <vector>

namespace sqlite_api {
struct sqlite3;
}

namespace xbwd {

/**
 *  Open a soci session.
 *
 *  @param s Session to open.
 *  @param beName Backend name.
 *  @param connectionString Connection string to forward to soci::open.
 *         see the soci::open documentation for how to use this.
 *
 */
void
open(
    soci::session& s,
    std::string const& beName,
    std::string const& connectionString);

std::uint32_t
getKBUsedAll(soci::session& s);
std::uint32_t
getKBUsedDB(soci::session& s);

template <class T>
T
convert(soci::blob&);

template <>
inline std::vector<std::uint8_t>
convert(soci::blob& from)
{
    std::vector<std::uint8_t> to;
    to.resize(from.get_len());
    if (to.empty())
        return to;
    from.read(0, reinterpret_cast<char*>(&to[0]), from.get_len());
    return to;
}

template <>
inline ripple::Buffer
convert(soci::blob& from)
{
    ripple::Buffer to;
    to.alloc(from.get_len());
    if (to.empty())
        return to;
    from.read(0, reinterpret_cast<char*>(to.data()), from.get_len());
    return to;
}

template <>
inline std::string
convert(soci::blob& from)
{
    auto tmp(convert<std::vector<std::uint8_t>>(from));
    return std::string(tmp.begin(), tmp.end());
}

template <>
inline ripple::PublicKey
convert(soci::blob& from)
{
    auto tmp(convert<std::vector<std::uint8_t>>(from));
    return ripple::PublicKey{ripple::makeSlice(tmp)};
}

template <>
inline ripple::STAmount
convert(soci::blob& from)
{
    auto tmp(convert<std::vector<std::uint8_t>>(from));
    ripple::SerialIter s(tmp.data(), tmp.size());
    return ripple::STAmount{s, ripple::sfAmount};
}

template <>
inline ripple::STXChainBridge
convert(soci::blob& from)
{
    auto tmp(convert<std::vector<std::uint8_t>>(from));
    ripple::SerialIter s(tmp.data(), tmp.size());
    return ripple::STXChainBridge{s, ripple::sfXChainBridge};
}

template <>
inline ripple::AccountID
convert(soci::blob& from)
{
    ripple::AccountID a;
    if (a.size() != from.get_len())
        throw std::runtime_error("Soci blob size mismatch");
    from.read(0, reinterpret_cast<char*>(a.data()), from.get_len());
    return a;
}

soci::blob
convert(std::vector<std::uint8_t> const& from, soci::session& s);
soci::blob
convert(ripple::Buffer const& from, soci::session& s);
soci::blob
convert(std::string const& from, soci::session& s);
soci::blob
convert(ripple::PublicKey const& from, soci::session& s);
soci::blob
convert(ripple::STAmount const& from, soci::session& s);
soci::blob
convert(ripple::STXChainBridge const& from, soci::session& s);
soci::blob
convert(ripple::AccountID const& from, soci::session& s);

}  // namespace xbwd

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
