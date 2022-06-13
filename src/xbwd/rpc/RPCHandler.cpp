#include <xbwd/rpc/RPCHandler.h>

#include <xbwd/app/App.h>
#include <xbwd/app/DBInit.h>
#include <xbwd/rpc/fromJSON.h>

#include <ripple/json/json_value.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/STArray.h>
#include <ripple/protocol/STBase.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/jss.h>

#include <fmt/core.h>

#include <functional>
#include <unordered_map>

namespace xbwd {
namespace rpc {

namespace {

void
doStop(App& app, Json::Value const& in, Json::Value& result)
{
    // TODO: This is a privilated command.
    result["request"] = in;
    result["result"] = "stopping";
    app.signalStop();
}

void
doServerInfo(App& app, Json::Value const& in, Json::Value& result)
{
    result["request"] = in;
    result["result"] = "normal";
}

void
doWitness(App& app, Json::Value const& in, Json::Value& result)
{
    result["request"] = in;
    auto optSidechain = optFromJson<ripple::STSidechain>(in, "sidechain");
    auto optAmt = optFromJson<ripple::STAmount>(in, "amount");
    auto optXChainSeq =
        optFromJson<std::uint32_t>(in, "xchain_sequence_number");
    auto optDoor = optFromJson<ripple::AccountID>(in, "dst_door");
    {
        auto const missingOrInvalidField = [&]() -> std::string {
            if (!optSidechain)
                return "sidechain";
            if (!optAmt)
                return "amount";
            if (!optXChainSeq)
                return "xchain_sequence_number";
            if (!optDoor)
                return "dst_door";
            return {};
        }();
        if (!missingOrInvalidField.empty())
        {
            result["error"] = fmt::format(
                "Missing or invalid field: {}", missingOrInvalidField);
            return;
        }
    }

    bool const wasSrcChainSend = (*optDoor == optSidechain->srcChainDoor());
    if (!wasSrcChainSend && *optDoor != optSidechain->dstChainDoor())
    {
        // TODO: Write log message
        // put expected value in the error message?
        result["error"] = fmt::format(
            "Specified door account does not match any sidechain door "
            "account.");
        return;
    }

    auto const& tblName = wasSrcChainSend
        ? db_init::xChainMainToSideTableName()
        : db_init::xChainSideToMainTableName();

    std::vector<std::uint8_t> const encodedSidechain = [&] {
        ripple::Serializer s;
        optSidechain->add(s);
        return std::move(s.modData());
    }();

    auto const encodedAmt = [&]() -> std::vector<std::uint8_t> {
        ripple::Serializer s;
        optAmt->add(s);
        return std::move(s.modData());
    }();
    {
        auto session = app.getXChainTxnDB().checkoutDb();
        // Soci blob does not play well with optional. Store an empty blob when
        // missing delivered amount
        soci::blob amtBlob{*session};
        soci::blob sidechainBlob(*session);
        convert(encodedAmt, amtBlob);
        convert(encodedSidechain, sidechainBlob);

        boost::optional<std::string> hexSignature;
        boost::optional<std::string> publicKey;

        auto sql = fmt::format(
            R"sql(SELECT Signature, PublicKey FROM {table_name}
                  WHERE XChainSeq = :xChainSeq and
                        DeliveredAmt = :amt and
                        Sidechain = :sidechain;
            )sql",
            fmt::arg("table_name", tblName));

        *session << sql, soci::into(hexSignature), soci::into(publicKey),
            soci::use(*optXChainSeq), soci::use(amtBlob),
            soci::use(sidechainBlob);

        // TODO: Check for multiple values
        if (hexSignature && publicKey)
        {
            Json::Value proof;
            proof["signatures"] = Json::arrayValue;
            auto& sigs = proof["signatures"];
            Json::Value sig = Json::objectValue;
            sig["signing_key"] = *publicKey;
            sig["signature"] = *hexSignature;
            sigs.append(sig);
            proof["amount"] = optAmt->getJson(ripple::JsonOptions::none);
            // TODO: use decoded sidechain
            proof["sidechain"] = in["sidechain"];
            proof["was_src_chain_send"] = wasSrcChainSend;
            proof["xchain_seq"] = *optXChainSeq;
            result["result"]["proof"] = proof;
        }
        else
        {
            result["error"] = "No such transaction";
        }
    }
}

using CmdFun = std::function<void(App&, Json::Value const&, Json::Value&)>;

std::unordered_map<std::string, CmdFun> const handlers = [] {
    using namespace std::literals;
    std::unordered_map<std::string, CmdFun> r;
    r.emplace("stop"s, doStop);
    r.emplace("server_info"s, doServerInfo);
    r.emplace("witness"s, doWitness);
    return r;
}();
}  // namespace

void
doCommand(App& app, Json::Value const& in, Json::Value& result)
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
        result["error"] = fmt::format("No such method: {}", cmd);
        return;
    }
    return it->second(app, in, result);
}

}  // namespace rpc
}  // namespace xbwd
