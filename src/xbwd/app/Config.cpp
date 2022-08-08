#include "ripple/protocol/AccountID.h"
#include "ripple/protocol/KeyType.h"
#include <xbwd/app/Config.h>

#include <xbwd/rpc/fromJSON.h>

namespace xbwd {
namespace config {

namespace {
ripple::KeyType
keyTypeFromJson(Json::Value const& jv, char const* key)
{
    using namespace std::literals;
    auto const v = jv[key];
    if (v.isNull())
        // default to secp256k1 if not specified
        return ripple::KeyType::secp256k1;

    auto const s = v.asString();
    if (s == "secp256k1"s)
        return ripple::KeyType::secp256k1;
    if (s == "ed25519"s)
        return ripple::KeyType::ed25519;

    throw std::runtime_error(
        "Unknown key type: "s + s + " while constructing a key type from json");
}
}  // namespace

Config::Config(Json::Value const& jv)
    : lockingchainIp{rpc::fromJson<beast::IP::Endpoint>(
          jv,
          "lockingchain_endpoint")}
    , issuingchainIp{rpc::fromJson<beast::IP::Endpoint>(
          jv,
          "issuingchain_endpoint")}
    , rpcEndpoint{rpc::fromJson<beast::IP::Endpoint>(jv, "rpc_endpoint")}
    , dataDir{rpc::fromJson<boost::filesystem::path>(jv, "db_dir")}
    , keyType{keyTypeFromJson(jv, "signing_key_keytype")}
    , signingKey{ripple::generateSecretKey(
          keyType,
          rpc::fromJson<ripple::Seed>(jv, "signing_key_seed"))}
    , bridge{rpc::fromJson<ripple::STXChainBridge>(jv, "bridge")}
    , lockingChainRewardAccount{rpc::fromJson<ripple::AccountID>(
          jv,
          "lockingchain_reward_account")}
    , issuingChainRewardAccount{
          rpc::fromJson<ripple::AccountID>(jv, "issuingchain_reward_account")}
{
}

}  // namespace config
}  // namespace xbwd
