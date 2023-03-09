#include "ripple/protocol/SecretKey.h"
#include <xbwd/app/Config.h>

#include <xbwd/rpc/fromJSON.h>

#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/KeyType.h>

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

std::optional<AdminConfig>
AdminConfig::make(Json::Value const& jv)
{
    AdminConfig ac;

    if (jv.isMember("Username") || jv.isMember("Password"))
    {
        // must have none or both of "Username" and "Password"
        if (!jv.isMember("Username") || !jv.isMember("Password") ||
            !jv["Username"].isString() || !jv["Password"].isString() ||
            jv["Username"].asString().empty() ||
            jv["Password"].asString().empty())
        {
            throw std::runtime_error("Admin config wrong format");
        }

        ac.pass.emplace(AdminConfig::PasswordAuth{
            jv["Username"].asString(), jv["Password"].asString()});
    }
    // may throw while parsing IPs or Subnets
    if (jv.isMember("IPs") && jv["IPs"].isArray())
    {
        for (auto const& s : jv["IPs"])
        {
            ac.addresses.emplace(boost::asio::ip::make_address(s.asString()));
        }
    }
    if (jv.isMember("Subnets") && jv["Subnets"].isArray())
    {
        for (auto const& s : jv["Subnets"])
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
        throw std::runtime_error("Admin config wrong format:" + jv.asString());
    }
}

TxnSubmit::TxnSubmit(Json::Value const& jv)
    : keyType{keyTypeFromJson(jv, "SigningKeyType")}
    , keypair{ripple::generateKeyPair(
          keyType,
          rpc::fromJson<ripple::Seed>(jv, "SigningKeySeed"))}
    , submittingAccount{
          rpc::fromJson<ripple::AccountID>(jv, "SubmittingAccount")}
{
    if (jv.isMember("ShouldSubmit"))
    {
        if (jv["ShouldSubmit"].isBool())
            shouldSubmit = jv["ShouldSubmit"].asBool();
        else
            throw std::runtime_error("WitnessSubmit config wrong format");
    }
}

ChainConfig::ChainConfig(Json::Value const& jv)
    : addrChainIp{rpc::fromJson<rpc::AddrEndpoint>(jv, "Endpoint")}
    , rewardAccount{rpc::fromJson<ripple::AccountID>(jv, "RewardAccount")}
{
    if (jv.isMember("TxnSubmit"))
    {
        txnSubmit.emplace(jv["TxnSubmit"]);
    }
    if (jv.isMember("LastAttestedCommitTx"))
    {
        lastAttestedCommitTx.emplace(
            rpc::fromJson<ripple::uint256>(jv, "LastAttestedCommitTx"));
    }
    if (jv.isMember("IgnoreSignerList"))
        ignoreSignerList = jv["IgnoreSignerList"].asBool();
}

Config::Config(Json::Value const& jv)
    : lockingChainConfig(jv["LockingChain"])
    , issuingChainConfig(jv["IssuingChain"])
    , addrRpcEndpoint{rpc::fromJson<rpc::AddrEndpoint>(jv, "RPCEndpoint")}
    , dataDir{rpc::fromJson<boost::filesystem::path>(jv, "DBDir")}
    , keyType{keyTypeFromJson(jv, "SigningKeyType")}
    , signingKey{ripple::generateKeyPair(
                     keyType,
                     rpc::fromJson<ripple::Seed>(jv, "SigningKeySeed"))
                     .second}
    , bridge{rpc::fromJson<ripple::STXChainBridge>(jv, "XChainBridge")}
    , adminConfig{jv.isMember("Admin") ? AdminConfig::make(jv["Admin"]) : std::nullopt}
    , logFile(jv.isMember("LogFile") ? jv["LogFile"].asString() : std::string())
    , logLevel(
          jv.isMember("LogLevel") ? jv["LogLevel"].asString() : std::string())
    , logSilent(jv.isMember("LogSilent") ? jv["LogSilent"].asBool() : false)
    , logSizeToRotateMb(
          jv.isMember("LogSizeToRotateMb") ? jv["LogSizeToRotateMb"].asUInt()
                                           : 0)
    , logFilesToKeep(
          jv.isMember("LogFilesToKeep") ? jv["LogFilesToKeep"].asUInt() : 0)
    , useBatch(jv.isMember("UseBatch") ? jv["UseBatch"].asBool() : false)
{
#ifndef USE_BATCH_ATTESTATION
    if (useBatch)
        throw std::runtime_error(
            "Please compile with USE_BATCH_ATTESTATION to use Batch "
            "Attestations");
#endif
}

}  // namespace config
}  // namespace xbwd
