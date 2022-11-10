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

#include <xbwd/core/DatabaseCon.h>

#include <xbwd/core/SociDB.h>

#include <ripple/basics/Log.h>
#include <ripple/basics/contract.h>

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>

#include <memory>
#include <unordered_map>

namespace xbwd {

DatabaseCon::DatabaseCon(
    boost::filesystem::path const& pPath,
    std::vector<std::string> const* commonPragma,
    std::vector<std::string> const& pragma,
    std::vector<std::string> const& initSQL,
    beast::Journal j)
    : session_(std::make_shared<soci::session>()), j_(j)
{
    const auto pParent = pPath.parent_path();
    boost::system::error_code ec;
    if (!boost::filesystem::exists(pParent, ec))
    {
        boost::filesystem::create_directories(pParent, ec);
        if (ec)
        {
            JLOGV(
                j_.fatal(),
                "can't create db path",
                ripple::jv("error", ec.message()),
                ripple::jv("path", pParent.string()));

            throw std::runtime_error(
                "can't create db path, error: " + ec.message());
        }
    }

    open(*session_, "sqlite", pPath.string());

    if (commonPragma)
    {
        for (auto const& p : *commonPragma)
        {
            soci::statement st = session_->prepare << p;
            st.execute(true);
        }
    }
    for (auto const& p : pragma)
    {
        soci::statement st = session_->prepare << p;
        st.execute(true);
    }
    for (auto const& sql : initSQL)
    {
        soci::statement st = session_->prepare << sql;
        st.execute(true);
    }
}
}  // namespace xbwd
