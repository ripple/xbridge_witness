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

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated"
#endif

#include <xbwd/core/SociDB.h>

#include <ripple/basics/ByteUtilities.h>
#include <ripple/basics/Slice.h>
#include <ripple/basics/contract.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/Serializer.h>

#include <boost/filesystem.hpp>
#include <soci/sqlite3/soci-sqlite3.h>

#include <memory>

namespace xbwd {

static auto checkpointPageCount = 1000;

namespace detail {

std::string
getSociSqliteInit(
    std::string const& name,
    std::string const& dir,
    std::string const& ext)
{
    if (name.empty())
    {
        throw std::runtime_error(
            "Sqlite databases must specify a dir and a name. Name: " + name +
            " Dir: " + dir);
    }
    boost::filesystem::path file(dir);
    if (is_directory(file))
        file /= name + ext;
    return file.string();
}

}  // namespace detail

void
open(
    soci::session& s,
    std::string const& beName,
    std::string const& connectionString)
{
    if (beName == "sqlite")
        s.open(soci::sqlite3, connectionString);
    else
        throw std::runtime_error("Unsupported soci backend: " + beName);
}

static sqlite_api::sqlite3*
getConnection(soci::session& s)
{
    sqlite_api::sqlite3* result = nullptr;
    auto be = s.get_backend();
    if (auto b = dynamic_cast<soci::sqlite3_session_backend*>(be))
        result = b->conn_;

    if (!result)
        throw std::logic_error("Didn't get a database connection.");

    return result;
}

std::uint32_t
getKBUsedAll(soci::session& s)
{
    if (!getConnection(s))
        throw std::logic_error("No connection found.");
    return static_cast<size_t>(
        sqlite_api::sqlite3_memory_used() / ripple::kilobytes(1));
}

std::uint32_t
getKBUsedDB(soci::session& s)
{
    // This function will have to be customized when other backends are added
    if (auto conn = getConnection(s))
    {
        int cur = 0, hiw = 0;
        sqlite_api::sqlite3_db_status(
            conn, SQLITE_DBSTATUS_CACHE_USED, &cur, &hiw, 0);
        return cur / ripple::kilobytes(1);
    }
    throw std::logic_error("");
    return 0;  // Silence compiler warning.
}

soci::blob
convert(std::vector<std::uint8_t> const& from, soci::session& s)
{
    soci::blob to(s);
    if (!from.empty())
        to.write(0, reinterpret_cast<char const*>(&from[0]), from.size());
    else
        to.trim(0);
    return to;
}

soci::blob
convert(ripple::Buffer const& from, soci::session& s)
{
    soci::blob to(s);
    if (!from.empty())
        to.write(0, reinterpret_cast<char const*>(from.data()), from.size());
    else
        to.trim(0);
    return to;
}

soci::blob
convert(std::string const& from, soci::session& s)
{
    soci::blob to(s);
    if (!from.empty())
        to.write(0, from.data(), from.size());
    else
        to.trim(0);
    return to;
}

soci::blob
convert(ripple::PublicKey const& from, soci::session& s)
{
    soci::blob to(s);
    to.write(0, reinterpret_cast<char const*>(from.data()), from.size());
    return to;
}

soci::blob
convert(ripple::STAmount const& from, soci::session& s)
{
    soci::blob to(s);
    ripple::Serializer r;
    from.add(r);
    to.write(0, reinterpret_cast<char const*>(r.data()), r.size());
    return to;
}

soci::blob
convert(ripple::STXChainBridge const& from, soci::session& s)
{
    soci::blob to(s);
    ripple::Serializer r;
    from.add(r);
    to.write(0, reinterpret_cast<char const*>(r.data()), r.size());
    return to;
}

soci::blob
convert(ripple::AccountID const& from, soci::session& s)
{
    soci::blob to(s);
    to.write(0, reinterpret_cast<char const*>(from.data()), from.size());
    return to;
}

}  // namespace xbwd

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
