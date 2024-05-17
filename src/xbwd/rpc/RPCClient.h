#pragma once

#include <xbwd/rpc/RPCCall.h>

#include <ripple/beast/utility/Journal.h>
#include <ripple/json/json_value.h>

#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <unordered_map>

namespace xbwd {

namespace rpc_call {

class RPCClient
{
    boost::asio::io_service& io_;

    std::string const host_;
    boost::asio::ip::tcp::endpoint ep_;
    boost::asio::ip::tcp::socket s_;

    beast::Journal j_;

public:
    RPCClient(
        boost::asio::io_service& ioc,
        rpc::AddrEndpoint const& ae,
        beast::Journal j);
    ~RPCClient();

    std::pair<int, Json::Value>
    post(Json::Value const& jreq);
};

}  // namespace rpc_call
}  // namespace xbwd
