#pragma once

#include <ripple/beast/net/IPEndpoint.h>
#include <ripple/json/json_value.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/SecretKey.h>

#include <boost/asio/ip/address.hpp>
#include <boost/filesystem.hpp>

#include "ripple/protocol/KeyType.h"
#include <string>

namespace xbwd {
namespace config {

struct Config
{
public:
    beast::IP::Endpoint mainchainIp;
    beast::IP::Endpoint sidechainIp;
    beast::IP::Endpoint rpcEndpoint;
    boost::filesystem::path dataDir;
    ripple::KeyType keyType;
    ripple::SecretKey signingKey;
    ripple::STXChainBridge sidechain;

    explicit Config(Json::Value const& jv);
};

}  // namespace config
}  // namespace xbwd
