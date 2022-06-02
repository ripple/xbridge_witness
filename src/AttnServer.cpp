#include <AttnServer.h>
#include <RPCServer.h>

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/version.hpp>
using boost::format;

#include <boost/iostreams/stream.hpp>

#include <fstream>
#include <iostream>
#include <streambuf>
#include <string>

#include <ripple/json/Writer.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_value.h>
#include <ripple/json/json_writer.h>
#include <ripple/protocol/jss.h>

#if 0 && (BOOST_VERSION / 100 % 1000) > 74
#define HAVE_BOOST_JSON
#endif

#ifdef HAVE_BOOST_JSON
#include <boost/json.hpp>
#include <boost/json/src.hpp>
#else
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#endif

namespace ripple {
namespace sidechain {

AttnServer::AttnServer(std::string const& fname)
{
    loadConfig(fname);
}

#ifndef HAVE_BOOST_JSON
template <class PT_ROOT>
static void
loadChainConfig(
    std::string const& fname,
    ChainType chaintype,
    ChainConfig& cfg,
    PT_ROOT const& root)
{
    cfg.chaintype = chaintype;
    cfg.ip = root.template get<std::string>("ip");
    cfg.port = root.template get<uint16_t>("port_ws");
    cfg.door_account_str = root.template get<std::string>("door_account");
    auto acct = parseBase58<AccountID>(cfg.door_account_str);
    if (acct)
        cfg.door_account = *acct;
    else
    {
        auto err =
            str(format(": %1% - Invalid door_account value \"%2%\"") % fname %
                cfg.door_account_str);
        throw std::runtime_error(err);
    }
}

template <class PT_ROOT>
static void
loadServerConfig(
    std::string const& fname,
    AttnServerConfig& cfg,
    PT_ROOT const& root)
{
    cfg.our_public_key_str = root.template get<std::string>("public_key");
    auto pk = parseBase58<PublicKey>(
        TokenType::AccountPublic, cfg.our_public_key_str);

    if (pk)
    {
        cfg.our_public_key = *pk;
    }
    else
    {
        auto err = str(format(": %1% - invalid public key") % fname);
        throw std::runtime_error(err);
    }

    auto key_str = root.template get<std::string>("secret_key");
    auto sk = parseBase58<SecretKey>(TokenType::AccountSecret, key_str);

    if (!sk)
    {
        // std::optional<Seed> seed = parseRippleLibSeed(key_str);
        std::optional<Seed> seed = parseGenericSeed(key_str);
        if (!seed)
            seed = parseBase58<Seed>(key_str);
        if (seed)
        {
            // this doesn't seem right... we should probably derive the public
            // key from the secret key?
            // TODO: we don't know the key type
            sk = generateKeyPair(KeyType::ed25519, *seed).second;
        }
    }

    if (sk)
    {
        cfg.our_secret_key = *sk;
    }
    else
    {
        auto err = str(format(": %1% - invalid secret key") % fname);
        throw std::runtime_error(err);
    }

    cfg.port_peer = root.template get<uint16_t>("port_peer");
    cfg.port_ws = root.template get<uint16_t>("port_ws");

    cfg.ssk_keys_str = root.template get<std::string>("__ssl_key");
    cfg.ssk_cert_str = root.template get<std::string>("__ssl_cert");
}

template <class PT_ROOT>
static void
loadSNTPConfig(
    std::string const& fname,
    SNTPServerConfig& cfg,
    PT_ROOT const& root)
{
}

#endif

std::string
AttnServer::process_rpc_request(std::string_view sv)
{
    std::string error_msg;
    uint32_t error_code = 0;
    Json::Value request;
    Json::Value result;
    Buffer signature;

    try
    {
        Json::Reader reader;

        reader.parse(sv.data(), sv.data() + sv.size(), request);
        auto req = request[jss::request];

        // parse dst door
        std::string dst_door_str = req[jss::dst_chain_door].asString();
        auto dst_door = parseBase58<AccountID>(dst_door_str);

        // parse sidechain
        auto sidechain = STSidechainFromJson(sfSidechain, req[jss::sidechain]);

        // parse amount
        STAmount amount = amountFromJson(sfAmount, req[jss::amount]);

        // parse xchain_sequence_number
        auto xChainSeqNum = req[jss::xchain_seq].asUInt();

        bool wasSrcChainSend = (dst_door == sidechain.srcChainDoor());

        // todo: get all the info from the tx

        auto const toSign = ChainClaimProofMessage(
            sidechain, amount, xChainSeqNum, wasSrcChainSend);

        // sign tx (for now we'll just sign the hash)
        signature =
            sign(cfg_.our_public_key, cfg_.our_secret_key, makeSlice(toSign));
    }
    catch (std::exception const& e)
    {
        error_code = 1;
        error_msg = e.what();
    }

    result[jss::error_code] = error_code;
    result[jss::error_message] = error_msg;
    result[jss::request] = request[jss::request];

    if (signature.size())
    {
        std::string signature_str{(char*)signature.data(), signature.size()};
        result[jss::signature] = strHex(signature_str);
        result[jss::signing_key] = cfg_.our_public_key_str;
    }

    std::string output;

    {
        Json::Writer writer(Json::stringOutput(output));
        writer.startRoot(Json::Writer::object);
        writer.set("result", result);
        writer.finish();
    }
    return output;
}

void
AttnServer::loadConfig(std::string const& fname)
{
#if defined(HAVE_BOOST_JSON)
    std::ifstream t(fname);
    std::string const input(std::istreambuf_iterator<char>(t), {});

    namespace json = boost::json;
    boost::json::error_code ec;
    auto doc = boost::json::parse(input, ec);
    if (ec || !doc.is_object())
    {
        auto err =
            str(format("error parsing config file: %1% - %2%") % fname %
                ec.message());
        throw std::runtime_error(err);
    }

    auto const& obj = doc.get_object();
    if (obj.contains("attn_server"))
    {
    }

#else
    namespace pt = boost::property_tree;

    pt::ptree root;
    pt::read_json(fname, root);
    auto const& att_root = root.get_child("attn_server");

    loadChainConfig(
        fname,
        ChainType::mainChain,
        cfg_.mainchain,
        att_root.get_child("mainchain"));
    loadChainConfig(
        fname,
        ChainType::sideChain,
        cfg_.sidechain,
        att_root.get_child("sidechain"));
    loadServerConfig(fname, cfg_, att_root.get_child("server"));
    loadSNTPConfig(
        fname, cfg_.sntp_servers, att_root.get_child("sntp_servers"));
#endif
}

void
AttnServer::mainLoop()
{
    const int num_threads = 4;
    boost::asio::io_context ioc(num_threads);

    RPCServer rpc_server(ioc, *this);

    std::vector<std::thread> v;
    for (int i = 1; i < num_threads; ++i)
        v.emplace_back([&ioc]() { ioc.run(); });
    ioc.run();
}

}  // namespace sidechain
}  // namespace ripple
