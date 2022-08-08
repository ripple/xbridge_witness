#pragma once

#include <ripple/beast/net/IPEndpoint.h>
#include <ripple/json/json_value.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/SecretKey.h>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/network_v4.hpp>
#include <boost/asio/ip/network_v6.hpp>
#include <boost/filesystem.hpp>

#include "ripple/protocol/KeyType.h"
#include <string>

namespace xbwd {
namespace config {

struct AdminConfig
{
    struct PasswordAuth
    {
        std::string user;
        std::string password;
    };

    // At least one of the following (including pass) should not be empty.
    std::set<boost::asio::ip::address> addresses;
    std::vector<boost::asio::ip::network_v4> netsV4;
    std::vector<boost::asio::ip::network_v6> netsV6;
    // If the pass is set, it will be checked in addition to address
    // verification, if any.
    std::optional<PasswordAuth> pass;
};

struct Config
{
public:
    beast::IP::Endpoint lockingchainIp;
    beast::IP::Endpoint issuingchainIp;
    beast::IP::Endpoint rpcEndpoint;
    boost::filesystem::path dataDir;
    ripple::KeyType keyType;
    ripple::SecretKey signingKey;
    ripple::STXChainBridge bridge;
    ripple::AccountID lockingChainRewardAccount;
    ripple::AccountID issuingChainRewardAccount;
    std::optional<AdminConfig> adminConf;

    explicit Config(Json::Value const& jv);
};

}  // namespace config
}  // namespace xbwd
