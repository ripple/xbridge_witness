#pragma once

#include <ripple/json/json_value.h>

namespace xbwd {
class App;
namespace rpc {

void
doCommand(App& app, Json::Value const& in, Json::Value& result);

}  // namespace rpc
}  // namespace xbwd
