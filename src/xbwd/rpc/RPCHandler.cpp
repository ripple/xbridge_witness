#include <xbwd/rpc/RPCHandler.h>

#include <xbwd/app/App.h>
#include <xbwd/app/DBInit.h>
#include <xbwd/federator/Federator.h>
#include <xbwd/rpc/fromJSON.h>

#include <ripple/json/json_value.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STArray.h>
#include <ripple/protocol/STBase.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/XChainAttestations.h>
#include <ripple/protocol/jss.h>

#include <fmt/core.h>
#include <openssl/crypto.h>
#include <soci/soci-backend.h>

#include <functional>
#include <unordered_map>

namespace xbwd {
namespace rpc {

namespace {

void
doStop(App& app, Json::Value const& in, Json::Value& result)
{
    // TODO: This is a privilated command.
    result[ripple::jss::request] = in;
    result[ripple::jss::status] = "stopping";
    app.signalStop();
}

void
doServerInfo(App& app, Json::Value const& in, Json::Value& result)
{
    result[ripple::jss::request] = in;
    auto const f = app.federator();
    if (!f)
    {
        result[ripple::jss::error] = "internalError";
        result[ripple::jss::error_message] = "internal federator error";
        return;
    }
    result["info"] = f->getInfo();
}

void
doSelectAll(
    App& app,
    Json::Value const& in,
    Json::Value& result,
    ChainDir const chainDir)

{
    // TODO: Remove me
    //

    result[ripple::jss::request] = in;

    auto const& tblName = db_init::xChainTableName(chainDir);

    {
        auto session = app.getXChainTxnDB().checkoutDb();
        soci::blob amtBlob(*session);
        soci::blob bridgeBlob(*session);
        soci::blob sendingAccountBlob(*session);
        soci::blob rewardAccountBlob(*session);
        soci::blob otherChainDstBlob(*session);
        soci::blob signingAccountBlob(*session);
        soci::blob publicKeyBlob(*session);
        soci::blob signatureBlob(*session);

        std::string transID;
        int ledgerSeq;
        int claimID;
        int success;

        auto sql = fmt::format(
            R"sql(SELECT TransID, LedgerSeq, ClaimID, Success, DeliveredAmt,
                         Bridge, SendingAccount, RewardAccount, OtherChainDst,
                         SigningAccount, PublicKey, Signature
                  FROM {table_name};
            )sql",
            fmt::arg("table_name", tblName));

        soci::indicator otherChainDstInd;
        soci::statement st =
            ((*session).prepare << sql,
             soci::into(transID),
             soci::into(ledgerSeq),
             soci::into(claimID),
             soci::into(success),
             soci::into(amtBlob),
             soci::into(bridgeBlob),
             soci::into(sendingAccountBlob),
             soci::into(rewardAccountBlob),
             soci::into(otherChainDstBlob, otherChainDstInd),
             soci::into(signingAccountBlob),
             soci::into(publicKeyBlob),
             soci::into(signatureBlob));
        st.execute();

        std::vector<ripple::Attestations::AttestationClaim> claims;
        ripple::STXChainBridge bridge;
        std::optional<ripple::STXChainBridge> firstBridge;
        while (st.fetch())
        {
            ripple::AccountID signingAccount;
            convert(signingAccountBlob, signingAccount);

            ripple::PublicKey signingPK;
            convert(publicKeyBlob, signingPK);

            ripple::Buffer sigBuf;
            convert(signatureBlob, sigBuf);

            ripple::STAmount sendingAmount;
            convert(amtBlob, sendingAmount, ripple::sfAmount);

            ripple::AccountID sendingAccount;
            convert(sendingAccountBlob, sendingAccount);

            ripple::AccountID rewardAccount;
            convert(rewardAccountBlob, rewardAccount);

            std::optional<ripple::AccountID> optDst;
            if (otherChainDstInd == soci::i_ok)
            {
                optDst.emplace();
                convert(otherChainDstBlob, *optDst);
            }

            convert(bridgeBlob, bridge, ripple::sfXChainBridge);
            if (!firstBridge)
            {
                firstBridge = bridge;
            }
            else
            {
                assert(bridge == *firstBridge);
            }

            claims.emplace_back(
                signingAccount,
                signingPK,
                sigBuf,
                sendingAccount,
                sendingAmount,
                rewardAccount,
                chainDir == ChainDir::lockingToIssuing,
                claimID,
                optDst);
        }

        auto const& config(app.config());
        if (config.useBatch)
        {
#ifdef USE_BATCH_ATTESTATION
            ripple::STXChainAttestationBatch batch{
                bridge, claims.begin(), claims.end()};
            result[ripple::sfXChainAttestationBatch.getJsonName()] =
                batch.getJson(ripple::JsonOptions::none);
#else
            throw std::runtime_error(
                "Please compile with USE_BATCH_ATTESTATION to use Batch "
                "Attestations");
#endif
        }
        else
        {
            auto& jclaims = (result["claims"] = Json::arrayValue);
            for (auto const& claim : claims)
            {
                SubmissionClaim sc(0, 0, 0, bridge, claim);
                jclaims.append(sc.getJson(ripple::JsonOptions::none));
            }
        }
    }
}

void
doSelectAllLocking(App& app, Json::Value const& in, Json::Value& result)
{
    return doSelectAll(app, in, result, ChainDir::lockingToIssuing);
}

void
doSelectAllIssuing(App& app, Json::Value const& in, Json::Value& result)
{
    return doSelectAll(app, in, result, ChainDir::issuingToLocking);
}

void
doWitness(App& app, Json::Value const& in, Json::Value& result)
{
    result[ripple::jss::request] = in;
    auto optBridge = optFromJson<ripple::STXChainBridge>(in, "bridge");
    auto optAmt = optFromJson<ripple::STAmount>(in, "sending_amount");
    auto optClaimID = optFromJson<std::uint64_t>(in, "claim_id");
    auto optDoor = optFromJson<ripple::AccountID>(in, "door");
    auto optSendingAccount =
        optFromJson<ripple::AccountID>(in, "sending_account");
    auto optDst = optFromJson<ripple::AccountID>(in, "destination");
    {
        auto const missingOrInvalidField = [&]() -> std::string {
            if (!optBridge)
                return "bridge";
            if (!optAmt)
                return "sending_amount";
            if (!optClaimID)
                return "claim_id";
            if (!optDoor)
                return "door";
            if (!optSendingAccount)
                return "sending_account";
            return {};
        }();
        if (!missingOrInvalidField.empty())
        {
            result[ripple::jss::error] = "invalidRequest";
            result[ripple::jss::error_message] = fmt::format(
                "Missing or invalid field: {}", missingOrInvalidField);
            return;
        }
    }

    auto const& door = *optDoor;
    auto const& sendingAccount = *optSendingAccount;
    auto const& bridge = *optBridge;
    auto const& sendingAmount = *optAmt;
    auto const& claimID = *optClaimID;

    ChainDir const chainDir = (*optDoor == optBridge->lockingChainDoor())
        ? ChainDir::lockingToIssuing
        : ChainDir::issuingToLocking;

    if (chainDir == ChainDir::issuingToLocking &&
        *optDoor != optBridge->issuingChainDoor())
    {
        // TODO: Write log message
        // put expected value in the error message?
        result[ripple::jss::error] = "invalidRequest";
        result[ripple::jss::error_message] = fmt::format(
            "Specified door account does not match any sidechain door "
            "account.");
        return;
    }

    auto const& tblName = db_init::xChainTableName(chainDir);

    {
        auto session = app.getXChainTxnDB().checkoutDb();
        soci::blob amtBlob(*session);
        soci::blob bridgeBlob(*session);
        soci::blob sendingAccountBlob(*session);
        soci::blob rewardAccountBlob(*session);
        soci::blob otherChainDstBlob(*session);
        soci::blob signingAccountBlob(*session);
        soci::blob publicKeyBlob(*session);
        soci::blob signatureBlob(*session);

        convert(sendingAmount, amtBlob);
        convert(bridge, bridgeBlob);
        convert(sendingAccount, sendingAccountBlob);
        soci::indicator sigInd;
        if (optDst)
        {
            convert(*optDst, otherChainDstBlob);

            auto sql = fmt::format(
                R"sql(SELECT SigningAccount, Signature, PublicKey, RewardAccount FROM {table_name}
                  WHERE ClaimID = :claimID and
                        Success = 1 and
                        DeliveredAmt = :amt and
                        Bridge = :bridge and
                        SendingAccount = :sendingAccount and
                        OtherChainDst = :otherChainDst;
            )sql",
                fmt::arg("table_name", tblName));

            *session << sql, soci::into(signingAccountBlob),
                soci::into(signatureBlob, sigInd), soci::into(publicKeyBlob),
                soci::into(rewardAccountBlob), soci::use(*optClaimID),
                soci::use(amtBlob), soci::use(bridgeBlob),
                soci::use(sendingAccountBlob), soci::use(otherChainDstBlob);
        }
        else
        {
            auto sql = fmt::format(
                R"sql(SELECT SigningAccount, Signature, PublicKey, RewardAccount, OtherChainDst FROM {table_name}
                  WHERE ClaimID = :claimID and
                        Success = 1 and
                        DeliveredAmt = :amt and
                        Bridge = :bridge and
                        SendingAccount = :sendingAccount;
            )sql",
                fmt::arg("table_name", tblName));

            soci::indicator otherChainDstInd;
            *session << sql, soci::into(signingAccountBlob),
                soci::into(signatureBlob, sigInd), soci::into(publicKeyBlob),
                soci::into(rewardAccountBlob),
                soci::into(otherChainDstBlob, otherChainDstInd),
                soci::use(*optClaimID), soci::use(amtBlob),
                soci::use(bridgeBlob), soci::use(sendingAccountBlob);

            if (otherChainDstInd == soci::i_ok)
            {
                optDst.emplace();
                convert(otherChainDstBlob, *optDst);
            }
        }

        // TODO: Check for multiple values
        if (sigInd == soci::i_ok && publicKeyBlob.get_len() > 0 &&
            rewardAccountBlob.get_len() > 0)
        {
            ripple::AccountID rewardAccount;
            convert(rewardAccountBlob, rewardAccount);
            ripple::AccountID signingAccount;
            convert(signingAccountBlob, signingAccount);
            ripple::PublicKey signingPK;
            convert(publicKeyBlob, signingPK);
            ripple::Buffer sigBuf;
            convert(signatureBlob, sigBuf);

            ripple::Attestations::AttestationClaim claim{
                signingAccount,
                signingPK,
                sigBuf,
                sendingAccount,
                sendingAmount,
                rewardAccount,
                chainDir == ChainDir::lockingToIssuing,
                claimID,
                optDst};

            auto const& config(app.config());
            if (config.useBatch)
            {
#ifdef USE_BATCH_ATTESTATION
                ripple::STXChainAttestationBatch batch{
                    bridge, &claim, &claim + 1};
                result[ripple::sfXChainAttestationBatch.getJsonName()] =
                    batch.getJson(ripple::JsonOptions::none);
#else
                throw std::runtime_error(
                    "Please compile with USE_BATCH_ATTESTATION to use Batch "
                    "Attestations");
#endif
            }
            else
            {
                SubmissionClaim sc(0, 0, 0, bridge, claim);
                result["claim"] = sc.getJson(ripple::JsonOptions::none);
            }
        }
        else
        {
            result[ripple::jss::error] = "invalidRequest";
            result[ripple::jss::error_message] = "No such transaction";
        }
    }
}

void
doWitnessAccountCreate(App& app, Json::Value const& in, Json::Value& result)
{
    result[ripple::jss::request] = in;
    auto optBridge = optFromJson<ripple::STXChainBridge>(in, "bridge");
    auto optAmt = optFromJson<ripple::STAmount>(in, "sending_amount");
    auto optRewardAmt = optFromJson<ripple::STAmount>(in, "reward_amount");
    auto optCreateCount = optFromJson<std::uint64_t>(in, "create_count");
    auto optDoor = optFromJson<ripple::AccountID>(in, "door");
    auto optSendingAccount =
        optFromJson<ripple::AccountID>(in, "sending_account");
    auto optRewardAccount =
        optFromJson<ripple::AccountID>(in, "reward_account");
    auto optDst = optFromJson<ripple::AccountID>(in, "destination");
    {
        auto const missingOrInvalidField = [&]() -> std::string {
            if (!optBridge)
                return "bridge";
            if (!optAmt)
                return "sending_amount";
            if (!optCreateCount)
                return "create_count";
            if (!optRewardAmt)
                return "reward_amount";
            if (!optDoor)
                return "door";
            if (!optSendingAccount)
                return "sending_account";
            if (!optRewardAccount)
                return "reward_account";
            if (!optDst)
                return "destination";
            return {};
        }();
        if (!missingOrInvalidField.empty())
        {
            result[ripple::jss::error] = "invalidRequest";
            result[ripple::jss::error_message] = fmt::format(
                "Missing or invalid field: {}", missingOrInvalidField);
            return;
        }
    }

    auto const& door = *optDoor;
    auto const& sendingAccount = *optSendingAccount;
    auto const& bridge = *optBridge;
    auto const& sendingAmount = *optAmt;
    auto const& rewardAmount = *optRewardAmt;
    auto const& rewardAccount = *optRewardAccount;
    auto const& createCount = *optCreateCount;
    auto const& dst = *optDst;

    ChainDir const chainDir = (*optDoor == optBridge->lockingChainDoor())
        ? ChainDir::lockingToIssuing
        : ChainDir::issuingToLocking;
    if (chainDir == ChainDir::issuingToLocking &&
        *optDoor != optBridge->issuingChainDoor())
    {
        // TODO: Write log message
        // put expected value in the error message?
        result[ripple::jss::error] = "invalidRequest";
        result[ripple::jss::error] = fmt::format(
            "Specified door account does not match any sidechain door "
            "account.");
        return;
    }

    auto const& tblName = db_init::xChainCreateAccountTableName(chainDir);

    std::vector<std::uint8_t> const encodedBridge = [&] {
        ripple::Serializer s;
        bridge.add(s);
        return std::move(s.modData());
    }();

    auto const encodedAmt = [&]() -> std::vector<std::uint8_t> {
        ripple::Serializer s;
        sendingAmount.add(s);
        return std::move(s.modData());
    }();
    auto const encodedRewardAmt = [&]() -> std::vector<std::uint8_t> {
        ripple::Serializer s;
        rewardAmount.add(s);
        return std::move(s.modData());
    }();
    {
        auto session = app.getXChainTxnDB().checkoutDb();

        soci::blob amtBlob(*session);
        convert(encodedAmt, amtBlob);

        soci::blob rewardAmtBlob(*session);
        convert(encodedRewardAmt, rewardAmtBlob);

        soci::blob bridgeBlob(*session);
        convert(encodedBridge, bridgeBlob);

        soci::blob sendingAccountBlob(*session);
        convert(sendingAccount, sendingAccountBlob);

        soci::blob otherChainDstBlob(*session);
        convert(dst, otherChainDstBlob);

        soci::blob rewardAccountBlob(*session);
        soci::blob signingAccountBlob(*session);
        soci::blob publicKeyBlob(*session);
        soci::blob signatureBlob(*session);

        auto sql = fmt::format(
            R"sql(SELECT SigningAccount, Signature, PublicKey, RewardAccount FROM {table_name}
                  WHERE CreateCount = :createCount and
                        Success = 1 and
                        DeliveredAmt = :amt and
                        RewardAmt = :rewardAmt and
                        Bridge = :bridge and
                        SendingAccount = :sendingAccount and
                        OtherChainDst = :otherChainDst;
            )sql",
            fmt::arg("table_name", tblName));

        *session << sql, soci::into(signingAccountBlob),
            soci::into(signatureBlob), soci::into(publicKeyBlob),
            soci::into(rewardAccountBlob), soci::use(createCount),
            soci::use(amtBlob), soci::use(rewardAmtBlob), soci::use(bridgeBlob),
            soci::use(sendingAccountBlob), soci::use(otherChainDstBlob);

        // TODO: Check for multiple values
        if (signatureBlob.get_len() > 0 && publicKeyBlob.get_len() > 0 &&
            rewardAccountBlob.get_len() > 0)
        {
            ripple::AccountID rewardAccount;
            convert(rewardAccountBlob, rewardAccount);
            ripple::AccountID signingAccount;
            convert(signingAccountBlob, signingAccount);
            ripple::PublicKey signingPK;
            convert(publicKeyBlob, signingPK);
            ripple::Buffer sigBuf;
            convert(signatureBlob, sigBuf);

            ripple::Attestations::AttestationCreateAccount createAccount{
                signingAccount,
                signingPK,
                sigBuf,
                sendingAccount,
                sendingAmount,
                rewardAmount,
                rewardAccount,
                chainDir == ChainDir::lockingToIssuing,
                createCount,
                dst};

            auto const& config(app.config());
            if (config.useBatch)
            {
#ifdef USE_BATCH_ATTESTATION
                ripple::AttestationBatch::AttestationClaim* nullClaim = nullptr;
                ripple::STXChainAttestationBatch batch{
                    bridge,
                    nullClaim,
                    nullClaim,
                    &createAccount,
                    &createAccount + 1};
                result[ripple::sfXChainAttestationBatch.getJsonName()] =
                    batch.getJson(ripple::JsonOptions::none);
#else
                throw std::runtime_error(
                    "Please compile with USE_BATCH_ATTESTATION to use Batch "
                    "Attestations");
#endif
            }
            else
            {
                SubmissionCreateAccount ca(0, 0, 0, bridge, createAccount);
                result["createAccount"] = ca.getJson(ripple::JsonOptions::none);
            }
        }
        else
        {
            result[ripple::jss::error] = "invalidRequest";
            result[ripple::jss::error_message] = "No such transaction";
        }
    }
}

void
doAttestTx(App& app, Json::Value const& in, Json::Value& result)
{
    result[ripple::jss::request] = in;
    auto const f = app.federator();
    if (!f)
    {
        result[ripple::jss::error] = "internalError";
        result[ripple::jss::error_message] = "internal federator error";
        return;
    }

    auto optBridge = optFromJson<ripple::STXChainBridge>(in, "bridge");
    auto optChainType = optFromJson<ChainType>(in, "chain_type");
    auto optTxHash = optFromJson<ripple::uint256>(in, "tx_hash");
    {
        auto const missingOrInvalidField = [&]() -> std::string {
            if (!optBridge)
                return "bridge";
            if (!optChainType)
                return "chain_type";
            if (!optTxHash)
                return "tx_hash";
            return {};
        }();
        if (!missingOrInvalidField.empty())
        {
            result[ripple::jss::error] = "invalidRequest";
            result[ripple::jss::error_message] = fmt::format(
                "Missing or invalid field: {}", missingOrInvalidField);
            return;
        }
    }

    f->pullAndAttestTx(*optBridge, *optChainType, *optTxHash, result);
}

enum class Role { USER, ADMIN };

struct CmdFun
{
    std::function<void(App&, Json::Value const&, Json::Value&)> func;
    Role role;
};

std::unordered_map<std::string, CmdFun> const handlers = [] {
    using namespace std::literals;
    std::unordered_map<std::string, CmdFun> r;
    r.emplace("stop"s, CmdFun{doStop, Role::ADMIN});
    r.emplace("server_info"s, CmdFun{doServerInfo, Role::ADMIN});
    r.emplace("witness"s, CmdFun{doWitness, Role::ADMIN});
    r.emplace(
        "witness_account_create"s, CmdFun{doWitnessAccountCreate, Role::ADMIN});
    r.emplace("select_all_locking"s, CmdFun{doSelectAllLocking, Role::ADMIN});
    r.emplace("select_all_issuing"s, CmdFun{doSelectAllIssuing, Role::ADMIN});
    r.emplace("attest_tx"s, CmdFun{doAttestTx, Role::ADMIN});
    return r;
}();
}  // namespace

bool
isAdmin(
    std::optional<config::AdminConfig> const& adminConf,
    Json::Value const& params,
    std::optional<std::string> const& passwordOp,
    boost::asio::ip::address const& remoteIp)
{
    if (!adminConf)
    {
        // allow admin RPCs if admin config was not set
        return true;
    }

    auto const& ac = *adminConf;
    // at least one of them should be set or not empty
    assert(
        ac.pass || !ac.addresses.empty() || !ac.netsV4.empty() ||
        !ac.netsV6.empty());

    // If the pass is set, it will be checked in addition to address
    // verification, if any.
    if (ac.pass)
    {
        std::string const paramUser =
            params.isMember("Username") && params["Username"].isString()
            ? params["Username"].asString()
            : std::string();

        unsigned const origAuthSize =
            ac.pass->user.size() + ac.pass->password.size();
        unsigned const inAuthSize =
            paramUser.size() + (passwordOp ? passwordOp->size() : 0);
        unsigned const toAllocateSize =
            std::max(origAuthSize, inAuthSize) + 1;  // +1 for \n separator

        std::string inAuth(toAllocateSize, char()),
            origAuth(toAllocateSize, char());
        inAuth = paramUser + '\n' + (passwordOp ? *passwordOp : std::string());
        origAuth = ac.pass->user + '\n' + ac.pass->password;

        // +2 == +1 for \n separator and +1 for \0 comparison
        bool const dataMatch =
            !CRYPTO_memcmp(inAuth.c_str(), origAuth.c_str(), origAuthSize + 2);
        if (!dataMatch)
            return false;
    }

    // return true if no need to check IP
    if (ac.addresses.empty() && ac.netsV4.empty() && ac.netsV6.empty())
        return true;

    if (ac.addresses.count(remoteIp) != 0)
        return true;

    if (remoteIp.is_v4())
    {
        for (auto const& net : ac.netsV4)
        {
            if (net.canonical().address() ==
                boost::asio::ip::network_v4(
                    remoteIp.to_v4(), net.prefix_length())
                    .canonical()
                    .address())
                return true;
        }
    }
    else
    {
        for (auto const& net : ac.netsV6)
        {
            if (net.canonical().address() ==
                boost::asio::ip::network_v6(
                    remoteIp.to_v6(), net.prefix_length())
                    .canonical()
                    .address())
                return true;
        }
    }

    // return false if need to check IP but none matched
    return false;
}

void
doCommand(
    App& app,
    beast::IP::Endpoint const& remoteIPAddress,
    Json::Value const& in,
    std::optional<std::string> const& passwordOp,
    Json::Value& result)
{
    auto const cmd = [&]() -> std::string {
        auto const cmd = in[ripple::jss::command];
        if (!cmd.isString())
        {
            return {};
        }
        return cmd.asString();
    }();
    auto it = handlers.find(cmd);
    if (it == handlers.end())
    {
        // TODO: regularize error handling
        result[ripple::jss::error] = "invalidRequest";
        result[ripple::jss::error_message] =
            fmt::format("No such method: {}", cmd);
        return;
    }

    if (it->second.role == Role::ADMIN &&
        !isAdmin(
            app.config().adminConfig,
            in,
            passwordOp,
            remoteIPAddress.address()))
    {
        result[ripple::jss::error] = "notAuthorized";
        result[ripple::jss::error_message] = fmt::format(
            "{} method requires ADMIN privilege. Request authentication "
            "failed.",
            cmd);
        return;
    }

    it->second.func(app, in, result);
}

}  // namespace rpc
}  // namespace xbwd
