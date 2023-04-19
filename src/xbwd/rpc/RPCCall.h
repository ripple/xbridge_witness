#pragma once

#include <xbwd/rpc/fromJSON.h>

#include <ripple/beast/net/IPEndpoint.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/json/json_value.h>

#include <boost/asio/io_service.hpp>

namespace xbwd {

namespace config {
class Config;
}

namespace rpc_call {

static const unsigned apiMaximumSupportedVersion = 1;

beast::IP::Endpoint
addrToEndpoint(boost::asio::io_service& io, rpc::AddrEndpoint const& ae);

std::pair<int, Json::Value>
fromCommandLine(
    config::Config const& config,
    Json::Value const& request,
    beast::severities::Severity logLevel);

}  // namespace rpc_call
}  // namespace xbwd
