#include <xbwd/rpc/RPCClient.h>

#include <xbwd/app/Config.h>
#include <xbwd/basics/StructuredLog.h>

#include <boost/asio/connect.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <ripple/basics/Log.h>
#include <ripple/json/json_reader.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/jss.h>

namespace xbwd {
namespace rpc_call {

using atcp = boost::asio::ip::tcp;
namespace bhttp = boost::beast::http;

beast::IP::Endpoint
addrToEndpoint(boost::asio::io_service& io, rpc::AddrEndpoint const& ae)
{
    try
    {
        auto addr = boost::asio::ip::make_address(ae.host);
        return beast::IP::Endpoint(std::move(addr), ae.port);
    }
    catch (...)
    {
    }

    atcp::resolver resolver{io};
    auto const results = resolver.resolve(ae.host, std::to_string(ae.port));
    auto const ep = results.begin()->endpoint();
    return beast::IP::Endpoint(ep.address(), ep.port());
}

std::pair<int, Json::Value>
fromCommandLine(
    config::Config const& config,
    Json::Value const& jreq,
    beast::severities::Severity logLevel)
{
    ripple::Logs logs = logLevel;
    beast::Journal j = [&]() {
        if (!config.logFile.empty())
        {
            if (!logs.open(config.logFile))
                std::cerr << "Can't open log file " << config.logFile
                          << std::endl;
        }
        logs.silent(config.logSilent);
        return logs.journal("CmdLine");
    }();

    boost::asio::io_service ioc;
    RPCClient rc(ioc, config.addrRpcEndpoint, j);
    auto r = rc.post(jreq);
    return r;
}

RPCClient::RPCClient(
    boost::asio::io_service& ioc,
    rpc::AddrEndpoint const& ae,
    beast::Journal j)
    : io_(ioc), host_(ae.host), s_(io_), j_(j)
{
    try
    {
        auto addr = boost::asio::ip::make_address(ae.host);
        ep_ = atcp::endpoint(std::move(addr), ae.port);
    }
    catch (...)
    {
    }

    {
        atcp::resolver resolver{io_};
        auto const results = resolver.resolve(ae.host, std::to_string(ae.port));
        ep_ = results.begin()->endpoint();
    }

    s_.connect(ep_);
    JLOGV(
        j_.trace(),
        "Client connected",
        jv("Host", ep_.address().to_string()),
        jv("Port", ep_.port()));
}

RPCClient::~RPCClient()
{
    boost::system::error_code ec;
    s_.shutdown(atcp::socket::shutdown_both, ec);

    if (ec && ec != boost::system::errc::not_connected)
    {
        JLOGV(
            j_.error(),
            "Error shutdown connection",
            jv("message", ec.message()));
    }
}

std::pair<int, Json::Value>
RPCClient::post(const Json::Value& jreq)
{
    bhttp::request<bhttp::string_body> req{bhttp::verb::post, "/", 11};
    req.set(bhttp::field::host, host_);
    req.set(bhttp::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(bhttp::field::content_type, "application/json");
    auto const body = jreq.toStyledString();
    req.body() = body;
    req.prepare_payload();

    JLOGV(j_.info(), "Sending", jv("msg", jreq.toStyledString()));

    Json::Value jres;

    try
    {
        bhttp::write(s_, req);

        boost::beast::flat_buffer buffer;
        bhttp::response<bhttp::string_body> res;
        bhttp::read(s_, buffer, res);
        std::string const& strBody = res.body();

        Json::Reader jr;
        jr.parse(strBody, jres);

        JLOGV(j_.info(), "Response", jv("msg", jres.toStyledString()));
    }
    catch (std::exception const& e)
    {
        JLOGV(j_.error(), "Exception in post", jv("message", e.what()));
        jres["exception"] = e.what();
        return {-1, jres};
    }

    return {0, jres};
}

}  // namespace rpc_call
}  // namespace xbwd
