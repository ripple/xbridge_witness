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
xChainLockingToIssuingTableName()
{
    static std::string const r{"XChainTxnLockingToIssuing"};
    return r;
}

std::string const&
xChainIssuingToLockingTableName()
{
    static std::string const r{"XChainTxnIssuingToLocking"};
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
        // Success is a bool (but soci complains about using bools)

        auto const tblFmtStr = R"sql(
            CREATE TABLE IF NOT EXISTS {table_name} (
                TransID           CHARACTER(64) PRIMARY KEY,
                LedgerSeq         BIGINT UNSIGNED,
                ClaimID           BIGINT UNSIGNED,
                Success           UNSIGNED,
                DeliveredAmt      BLOB,
                Bridge            BLOB,
                SendingAccount    BLOB,
                RewardAccount     BLOB,
                OtherChainAccount BLOB,
                PublicKey         BLOB,
                Signature         BLOB);
        )sql";
        auto const idxFmtStr = R"sql(
            CREATE INDEX IF NOT EXISTS {table_name}XSeqIdx ON {table_name}(ClaimID);",
        )sql";
        r.push_back(fmt::format(
            tblFmtStr,
            fmt::arg("table_name", xChainLockingToIssuingTableName())));
        r.push_back(fmt::format(
            idxFmtStr,
            fmt::arg("table_name", xChainLockingToIssuingTableName())));
        r.push_back(fmt::format(
            tblFmtStr,
            fmt::arg("table_name", xChainIssuingToLockingTableName())));
        r.push_back(fmt::format(
            idxFmtStr,
            fmt::arg("table_name", xChainIssuingToLockingTableName())));

        r.push_back("END TRANSACTION;");
        return r;
    }();
    return result;
}

}  // namespace db_init
}  // namespace xbwd
