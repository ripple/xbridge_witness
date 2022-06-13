#include <xbwd/app/DBInit.h>

#include <fmt/core.h>

namespace xbwd {
namespace db_init {

std::string const&
xChainDBName()
{
    static std::string const r{"xchain_txns.db"};
    return r;
}

std::string const&
xChainMainToSideTableName()
{
    static std::string const r{"XChainTxnMainToSide"};
    return r;
}

std::string const&
xChainSideToMainTableName()
{
    static std::string const r{"XChainTxnSideToMain"};
    return r;
}

std::vector<std::string> const&
xChainDBPragma()
{
    static std::vector<std::string> const result = [] {
        std::vector<std::string> r;
        r.push_back("PRAGMA journal_size_limit=1582080;");
        return r;
    }();

    return result;
};

std::vector<std::string> const&
xChainDBInit()
{
    static std::vector<std::string> result = [] {
        std::vector<std::string> r;
        r.push_back("BEGIN TRANSACTION;");

        // DeliveredAmt is encoded as a serialized STAmount
        //              this is raw data - no encoded.
        // Signature is hex encoded (without a leading 0x)
        // Success is a bool (but soci complains about using bools)
        // Sidechain is encoded as a serialized STSidechain

        auto const tblFmtStr = R"sql(
            CREATE TABLE IF NOT EXISTS {table_name} (
                TransID      CHARACTER(64) PRIMARY KEY,
                LedgerSeq    BIGINT UNSIGNED,
                XChainSeq    BIGINT UNSIGNED,
                Success      UNSIGNED,
                DeliveredAmt BLOB,
                Sidechain    BLOB,
                PublicKey    TEXT,
                Signature    TEXT);
        )sql";
        auto const idxFmtStr = R"sql(
            CREATE INDEX IF NOT EXISTS {table_name}XSeqIdx ON {table_name}(XChainSeq);",
        )sql";
        r.push_back(fmt::format(
            tblFmtStr, fmt::arg("table_name", xChainMainToSideTableName())));
        r.push_back(fmt::format(
            idxFmtStr, fmt::arg("table_name", xChainMainToSideTableName())));
        r.push_back(fmt::format(
            tblFmtStr, fmt::arg("table_name", xChainSideToMainTableName())));
        r.push_back(fmt::format(
            idxFmtStr, fmt::arg("table_name", xChainSideToMainTableName())));

        r.push_back("END TRANSACTION;");
        return r;
    }();
    return result;
}

}  // namespace db_init
}  // namespace xbwd
