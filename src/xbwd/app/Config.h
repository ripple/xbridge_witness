#pragma once

#include <ripple/basics/hardened_hash.h>
#include <ripple/beast/net/IPEndpoint.h>
#include <ripple/json/json_value.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/KeyType.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/SecretKey.h>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/network_v4.hpp>
#include <boost/asio/ip/network_v6.hpp>
#include <boost/filesystem.hpp>

#include <string>

template <>
struct std::hash<ripple::STXChainBridge>
{
    std::size_t
    operator()(ripple::STXChainBridge const& b) const noexcept
    {
        beast::xxhasher hasher;
        beast::hash_append(hasher, b.lockingChainDoor());
        ripple::hash_append(hasher, b.lockingChainIssue());
        beast::hash_append(hasher, b.issuingChainDoor());
        ripple::hash_append(hasher, b.issuingChainIssue());
        const std::size_t h = static_cast<std::size_t>(hasher);
        return h;
    }
};

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

    static std::optional<AdminConfig>
    make(Json::Value const& jv);
};

struct TxnSubmit
{
    ripple::KeyType keyType;

    std::pair<ripple::PublicKey, ripple::SecretKey> keypair;
    ripple::AccountID submittingAccount;
    bool shouldSubmit{true};

    explicit TxnSubmit(Json::Value const& jv);
};

struct ChainConfig
{
    beast::IP::Endpoint chainIp;
    ripple::AccountID rewardAccount;
    std::optional<TxnSubmit> txnSubmit;
    bool ignoreSignerList = false;
    std::optional<ripple::uint256> lastAttestedCommitTx;
    explicit ChainConfig(Json::Value const& jv);
};

struct Config
{
public:
    ChainConfig lockingChainConfig;
    ChainConfig issuingChainConfig;
    beast::IP::Endpoint rpcEndpoint;
    boost::filesystem::path dataDir;
    ripple::KeyType keyType;
    ripple::SecretKey signingKey;
    std::unordered_set<ripple::STXChainBridge> bridges;
    std::optional<AdminConfig> adminConfig;

    std::string logFile;
    std::string logLevel;
    bool logSilent;

    explicit Config(Json::Value const& jv);
};

}  // namespace config
}  // namespace xbwd
