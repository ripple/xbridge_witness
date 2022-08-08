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
    if (jv.isMember("Admin"))
    {
        auto const& admin = jv["Admin"];
        adminConf = [&]() -> std::optional<AdminConfig> {
            AdminConfig ac;
            if (admin.isMember("Username") || admin.isMember("Password"))
            {
                // must have none or both of "Username" and "Password"
                if (!admin.isMember("Username") ||
                    !admin.isMember("Password") ||
                    !admin["Username"].isString() ||
                    !admin["Password"].isString() ||
                    admin["Username"].asString().empty() ||
                    admin["Password"].asString().empty())
                {
                    throw std::runtime_error(
                        "Admin config wrong format:" +
                        admin.asString());
                }

                ac.pass.emplace(AdminConfig::PasswordAuth{
                    admin["Username"].asString(),
                    admin["Password"].asString()});
            }
            // may throw while parsing IPs or Subnets
            if (admin.isMember("IPs") && admin["IPs"].isArray())
            {
                for (auto const& s : admin["IPs"])
                {
                    ac.addresses.emplace(
                        boost::asio::ip::make_address(s.asString()));
                }
            }
            if (admin.isMember("Subnets") && admin["Subnets"].isArray())
            {
                for (auto const& s : admin["Subnets"])
                {
                    // First, see if it's an ipv4 subnet. If not, try ipv6.
                    // If that throws, then there's nothing we can do with
                    // the entry.
                    try
                    {
                        ac.netsV4.emplace_back(
                            boost::asio::ip::make_network_v4(s.asString()));
                    }
                    catch (boost::system::system_error const&)
                    {
                        ac.netsV6.emplace_back(
                            boost::asio::ip::make_network_v6(s.asString()));
                    }
                }
            }

            if (ac.pass || !ac.addresses.empty() || !ac.netsV4.empty() ||
                !ac.netsV6.empty())
            {
                return std::optional<AdminConfig>{ac};
            }
            else
            {
                throw std::runtime_error(
                    "Admin config wrong format:" + admin.asString());
            }
        }();
    }
}

}  // namespace config
}  // namespace xbwd
