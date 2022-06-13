#pragma once

#include <cstdint>
#include <string>

namespace xbwd {

/** Versioning information for this build. */
namespace build_info {

inline char const* const serverName = "xbridge_witnessd";
inline char const* const serverNameUC = "XBRIDGE_WITNESSD";

/** Server version.
    Follows the Semantic Versioning Specification:
    http://semver.org/
*/
std::string const&
getVersionString();

/** Full server version string.
    This includes the name of the server. It is used in the peer
    protocol hello message and also the headers of some HTTP replies.
*/
std::string const&
getFullVersionString();

}  // namespace build_info

}  // namespace xbwd
