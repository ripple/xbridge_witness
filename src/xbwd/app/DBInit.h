#pragma once

#include <xbwd/basics/ChainTypes.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xbwd {
namespace db_init {

std::string const&
xChainDBName();

std::string const&
xChainTableName(ChainDir dir);

std::string const&
xChainCreateAccountTableName(ChainDir dir);

std::vector<std::string> const&
xChainDBPragma();

std::vector<std::string> const&
xChainDBInit();

}  // namespace db_init
}  // namespace xbwd
