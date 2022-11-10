#pragma once
//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <xbwd/core/SociDB.h>

#include <ripple/app/main/DBInit.h>
#include <ripple/core/Config.h>

#include <boost/filesystem/path.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace soci {
class session;
}

namespace xbwd {

class LockedSociSession
{
public:
    using mutex = std::recursive_mutex;

private:
    std::shared_ptr<soci::session> session_;
    std::unique_lock<mutex> lock_;

public:
    LockedSociSession(std::shared_ptr<soci::session> it, mutex& m)
        : session_(std::move(it)), lock_(m)
    {
    }
    LockedSociSession(LockedSociSession&& rhs) noexcept
        : session_(std::move(rhs.session_)), lock_(std::move(rhs.lock_))
    {
    }
    LockedSociSession() = delete;
    LockedSociSession(LockedSociSession const& rhs) = delete;
    LockedSociSession&
    operator=(LockedSociSession const& rhs) = delete;

    soci::session*
    get()
    {
        return session_.get();
    }
    soci::session&
    operator*()
    {
        return *session_;
    }
    soci::session*
    operator->()
    {
        return session_.get();
    }
    explicit operator bool() const
    {
        return bool(session_);
    }
};

class DatabaseCon
{
public:
    DatabaseCon(
        boost::filesystem::path const& dataDir,
        std::string const& dbName,
        std::vector<std::string> const& pragma,
        std::vector<std::string> const& initSQL,
        beast::Journal j)
        : DatabaseCon(dataDir / dbName, nullptr, pragma, initSQL, j)
    {
    }

    ~DatabaseCon() = default;

    soci::session&
    getSession()
    {
        return *session_;
    }

    LockedSociSession
    checkoutDb()
    {
        return LockedSociSession(session_, lock_);
    }

private:
    DatabaseCon(
        boost::filesystem::path const& pPath,
        std::vector<std::string> const* commonPragma,
        std::vector<std::string> const& pragma,
        std::vector<std::string> const& initSQL,
        beast::Journal j);

    LockedSociSession::mutex lock_;

    // checkpointer may outlive the DatabaseCon when the checkpointer jobQueue
    // callback locks a weak pointer and the DatabaseCon is then destroyed. In
    // this case, the checkpointer needs to make sure it doesn't use an already
    // destroyed session. Thus this class keeps a shared_ptr to the session (so
    // the checkpointer can keep a weak_ptr) and the checkpointer is a
    // shared_ptr in this class. session_ will never be null.
    std::shared_ptr<soci::session> const session_;

    beast::Journal j_;
};

}  // namespace xbwd
