//------------------------------------------------------------------------------
/*
    Copyright (c) 2022 Ripple Labs Inc.

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

#ifndef RIPPLE_SIDECHAIN_IMPL_CHAINLISTENER_H_INCLUDED
#define RIPPLE_SIDECHAIN_IMPL_CHAINLISTENER_H_INCLUDED

#include <ripple/protocol/AccountID.h>

#include <ripple/beast/utility/Journal.h>
#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/address.hpp>

#include <memory>
#include <mutex>
#include <string>

#include <AttnServer.h>

namespace ripple {
namespace sidechain {

class AttnServer;
class WebsocketClient;

class ChainListener
{
protected:
    ChainType chain_type_;
    std::string doorAccountStr_;
    AccountID doorAccount_;
    AttnServer& attn_server_;
    mutable std::mutex m_;
    
    beast::Journal j_;

    ChainListener(
        ChainType chain_type,
        AccountID const& doorAccount,
        AttnServer& attn_server,
        beast::Journal j);

    virtual ~ChainListener();

    std::string const& chainName() const;

    void processMessage(Json::Value const& msg) EXCLUDES(m_);
    bool isMainchain() const { return chain_type_== mainChain; }
};

}  // namespace sidechain
}  // namespace ripple

#endif
