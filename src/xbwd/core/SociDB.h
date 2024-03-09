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

struct convert_t
{
    soci::blob& blob_;
    ripple::SField const* f_ = nullptr;

    convert_t(soci::blob&, ripple::SField const* f_ = nullptr);

    operator std::vector<std::uint8_t>() const;
    operator ripple::Buffer() const;
    operator std::string() const;
    operator ripple::PublicKey() const;
    operator ripple::STAmount() const;
    operator ripple::STXChainBridge() const;

    template <std::size_t Bits, class Tag = void>
    operator ripple::base_uint<Bits, Tag>()
    {
        ripple::base_uint<Bits, Tag> a;
        if (a.size() != blob_.get_len())
            throw std::runtime_error("Soci blob size mismatch");
        blob_.read(0, reinterpret_cast<char*>(a.data()), blob_.get_len());
        return a;
    }

    static soci::blob
    convert(std::vector<std::uint8_t> const& from, soci::session& s);
    static soci::blob
    convert(ripple::Buffer const& from, soci::session& s);
    static soci::blob
    convert(std::string const& from, soci::session& s);
    static soci::blob
    convert(ripple::PublicKey const& from, soci::session& s);
    static soci::blob
    convert(ripple::STAmount const& from, soci::session& s);
    static soci::blob
    convert(ripple::STXChainBridge const& from, soci::session& s);
    template <std::size_t Bits, class Tag = void>
    static soci::blob
    convert(ripple::base_uint<Bits, Tag> const& from, soci::session& s)
    {
        soci::blob to(s);
        to.write(0, reinterpret_cast<char const*>(from.data()), from.size());
        return to;
    }
};

}  // namespace xbwd

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
