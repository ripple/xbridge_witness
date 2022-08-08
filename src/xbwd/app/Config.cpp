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
          "LockingChainEndpoint")}
    , issuingchainIp{rpc::fromJson<beast::IP::Endpoint>(
          jv,
          "IssuingChainEndpoint")}
    , rpcEndpoint{rpc::fromJson<beast::IP::Endpoint>(jv, "RPCEndpoint")}
    , dataDir{rpc::fromJson<boost::filesystem::path>(jv, "DBDir")}
    , keyType{keyTypeFromJson(jv, "SigningKeyKeyType")}
    , signingKey{ripple::generateSecretKey(
          keyType,
          rpc::fromJson<ripple::Seed>(jv, "SigningKeySeed"))}
    , bridge{rpc::fromJson<ripple::STXChainBridge>(jv, "XChainBridge")}
    , lockingChainRewardAccount{rpc::fromJson<ripple::AccountID>(
          jv,
          "LockingChainRewardAccount")}
    , issuingChainRewardAccount{
          rpc::fromJson<ripple::AccountID>(jv, "IssuingChainRewardAccount")}
{
}

}  // namespace config
}  // namespace xbwd
