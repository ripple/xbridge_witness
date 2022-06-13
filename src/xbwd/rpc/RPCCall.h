#pragma once

#include <ripple/json/json_value.h>

namespace xbwd {

namespace config {
class Config;
}

namespace rpc_call {

inline int
fromCommandLine(config::Config const& config, Json::Value const& request)
{
    // TODO
    return 0;
}

}  // namespace rpc_call
}  // namespace xbwd
