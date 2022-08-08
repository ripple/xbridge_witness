#pragma once

#include <ripple/beast/net/IPEndpoint.h>
#include <ripple/json/json_value.h>
#include <ripple/server/Port.h>

namespace xbwd {
class App;
namespace rpc {

void
doCommand(
    App& app,
    beast::IP::Endpoint const& remoteIPAddress,
    Json::Value const& in,
    Json::Value& result);

}  // namespace rpc
}  // namespace xbwd
