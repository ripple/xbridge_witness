#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xbwd {
namespace db_init {

// Ledger database holds ledgers and ledger confirmations
std::string const&
xChainDBName();
std::string const&
xChainMainToSideTableName();
std::string const&
xChainSideToMainTableName();

std::vector<std::string> const&
xChainDBPragma();

std::vector<std::string> const&
xChainDBInit();

}  // namespace db_init
}  // namespace xbwd
