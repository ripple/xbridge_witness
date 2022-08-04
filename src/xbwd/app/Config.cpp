#include "ripple/protocol/AccountID.h"
#include <xbwd/app/Config.h>

#include <xbwd/rpc/fromJSON.h>

namespace xbwd {
namespace config {

Config::Config(Json::Value const& jv)
    : mainchainIp{rpc::fromJson<beast::IP::Endpoint>(jv, "mainchain_endpoint")}
    , sidechainIp{rpc::fromJson<beast::IP::Endpoint>(jv, "sidechain_endpoint")}
    , rpcEndpoint{rpc::fromJson<beast::IP::Endpoint>(jv, "rpc_endpoint")}
    , dataDir{rpc::fromJson<boost::filesystem::path>(jv, "db_dir")}
    // TODO: Add a field for keytype in the config
    , keyType{ripple::KeyType::ed25519}
    , signingKey{ripple::generateSecretKey(
          keyType,
          rpc::fromJson<ripple::Seed>(jv, "signing_key_seed"))}
    , sidechain{rpc::fromJson<ripple::STXChainBridge>(jv, "sidechain")}
    , lockingChainRewardAccount{rpc::fromJson<ripple::AccountID>(
          jv,
          "lockingchain_reward_account")}
    , issuingChainRewardAccount{
          rpc::fromJson<ripple::AccountID>(jv, "issuingchain_reward_account")}
{
}

}  // namespace config
}  // namespace xbwd
